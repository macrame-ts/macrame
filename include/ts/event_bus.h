#pragma once

// `Event_bus` - per-domain pub/sub over `Guarded` + `Deferred`: producers publish events
// now, one dispatch delivers them later, as a batch. The deferred-batch model of entt's
// `enqueue`/`update` and Qt's queued connections: the event struct itself is the topic -
// no delegate declaration, no named channel object per event kind. Handlers see events at
// the dispatch point, not at the publish; a reaction that must happen immediately is a
// command to a known target (`target.async`) or a plain observer inside one system, not a
// bus event.
//
// Access control is `Guarded`'s: `publish` and `subscribe` stage into a private journal
// (grant-free, from any thread or task), and delivery is one ordinary write on the board -
// run `dispatch_fn()` as a graph node declaring write on `state()`, or through a write
// access from the frame loop. Handlers run serialized under that grant and must not touch
// other guarded state directly - route outbound effects through `async`/`publish`/`stage`.
//
// Use:
//   struct Damage { int npc; int amount; };              // the event type is the channel
//   ts::Event_bus bus{ ts::Named{"gameplay"} };
//   bus.publish(Damage{ npc, 12 });                      // fire: any thread, grant-free
//   auto c = bus.subscribe(hud, &Hud::on_damage);        // pinned: owner kept alive per call
//   auto c2 = bus.subscribe([](const Damage& d) { });    // unpinned; event type deduced
//   g.add_node("events", bus.dispatch_fn(), bus.state()).after(producers...);
// Staging derives no edges, so `after(producers...)` is explicit intent - required for
// same-frame delivery. Graph-free, the dispatch is one write access per frame:
// `bus.state().access(bus.dispatch_fn()).sync()`. see events.cpp sample.
//
// Contract:
//  - Delivery is batched: a subscription observes every event in the first cut that
//    contains its install, so subscribe-then-publish on one thread with no suspension
//    between the two calls is always delivered (both land FIFO in the same staging lane).
//    A task that suspends between the subscribe and the publish may resume on a different
//    worker, splitting them across lanes whose relative apply order is arbitrary - stage
//    both before any co_await if same-cut delivery matters. Cross-producer order is
//    arbitrary (the journal contract); per-producer order is FIFO.
//  - Pinned subscriptions (`subscribe(owner, ...)`) weak-lock the owner around each call:
//    a dying owner is skipped and reaped - no unsubscribe needed. The handler receives the
//    locked owner as its first parameter. If the pin is the last reference, the owner
//    destructs on the dispatching thread (the usual `shared_ptr` caveat).
//  - `Connection` removal is an immediate-effect write on the board: `disconnect()` (or
//    the destructor) outside tasks only; a disconnect racing a still-staged install wins.
//    No sub/unsub from inside a handler.
//  - Each publish stages a closure (heap past the SBO) - sized for sparse notifications,
//    not per-entity firehoses; the typed-lane plan is in `docs/command-buffer-design.md`.
//
// Not the right tool for heavy flows whose consumers deserve their own schedule - those
// want a dedicated batch object and a conflict-derived edge (the `Damage_events` tier in
// the events.cpp sample). The journal and its ordering contract are `Deferred`'s; the
// board is one `Guarded` object the harness and any `Static_task_graph` treat like any other.

#include "ts/access.h"
#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/detail/ref_count.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ts
{

class Event_bus
{
private:
    // Shared between a `Connection` and its (possibly not-yet-applied) staged
    // install: `disconnect` tombstones it; the install checks it at apply, the
    // dispatch loop skips and reaps entries it marks. Intrusively ref-counted -
    // held with independent lifetimes by the Connection, the staged closure, and
    // the installed entry (the pre-apply-disconnect race needs it to outlive the
    // Connection), so shared ownership is required; `Ref_ptr` gives it in one
    // allocation with a one-pointer handle.
    struct Sub_control : detail::Ref_counted<Sub_control>
    {
        std::atomic<bool> dead{ false };
    };

    struct Lane_base
    {
        virtual ~Lane_base() = default;
        virtual void dispatch_and_clear() = 0;
        virtual void remove(std::uint64_t id) = 0;
    };

    template<typename E>
    struct Lane : Lane_base
    {
        struct Subscription
        {
            std::uint64_t id = 0;
            bool pinned = false;
            std::weak_ptr<void> owner;                    // pinned only
            std::function<void(void*, const E&)> invoke;  // void* = the locked owner (null when unpinned)
            detail::Ref_ptr<Sub_control> control;
        };

        std::vector<E> events;
        std::vector<Subscription> subs;

        void dispatch_and_clear() override
        {
            // Subscriber-major: each subscriber is checked (and, if pinned, its owner
            // locked) once for the whole cut, not once per event. Fewer weak_ptr locks
            // when a hot event type has many subscribers, and a subscriber sees all of
            // this cut's events or none - a pinned owner cannot expire mid-batch. Handlers
            // defer their effects (async/publish/stage), so this ordering is unobservable
            // to well-behaved handlers.
            for (Subscription& s : subs)
            {
                if (s.control->dead.load(std::memory_order_acquire))
                    continue;
                if (!s.pinned)
                {
                    for (const E& e : events)
                        s.invoke(nullptr, e);
                    continue;
                }
                if (std::shared_ptr<void> pin = s.owner.lock())  // owner alive for the whole batch
                {
                    for (const E& e : events)
                        s.invoke(pin.get(), e);
                }
            }
            std::erase_if(subs, [](const Subscription& s)
            {
                return s.control->dead.load(std::memory_order_relaxed) || (s.pinned && s.owner.expired());
            });
            events.clear();
        }

        void remove(std::uint64_t id) override
        {
            std::erase_if(subs, [id](const Subscription& s) { return s.id == id; });
        }
    };

    // Deduces the event type from a handler's parameter list, the same idea as
    // the access machinery's const-ness deduction: `(const E&)` names an
    // unpinned handler, `(Owner&, const E&)` a pinned one. Generic lambdas
    // cannot deduce; use a concrete parameter type.
    template<typename F>
    struct Handler_traits : Handler_traits<decltype(&F::operator())> {};

    template<typename C, typename E>
    struct Handler_traits<void (C::*)(const E&) const>
    {
        using Event = E;
    };

    template<typename C, typename O, typename E>
    struct Handler_traits<void (C::*)(O&, const E&) const>
    {
        using Event = E;
    };

public:
    // The guarded store: typed event lanes + their subscriber lists, mutated
    // only under the bus's grant (installs and appends apply at the cut,
    // dispatch runs in the dispatch node, removal goes through `access`).
    class Board
    {
    public:
        template<typename E>
        Lane<E>& lane()
        {
            TS_CHECK_ACCESS();
            auto [it, added] = index_.try_emplace(std::type_index(typeid(E)), lanes_.size());
            if (added)
                lanes_.push_back(std::make_unique<Lane<E>>());
            return static_cast<Lane<E>&>(*lanes_[it->second]);
        }

        void dispatch_all()
        {
            TS_CHECK_ACCESS();
            // Lane order = first-apply order (an install or a publish creating
            // the lane), stable within a run; cross-producer apply order is
            // arbitrary, so type dispatch order is not guaranteed across runs.
            for (auto& lane : lanes_)
                lane->dispatch_and_clear();
        }

        void remove(std::type_index type, std::uint64_t id)
        {
            TS_CHECK_ACCESS();
            if (auto it = index_.find(type); it != index_.end())
                lanes_[it->second]->remove(id);
        }

    private:
        std::unordered_map<std::type_index, std::size_t> index_;
        std::vector<std::unique_ptr<Lane_base>> lanes_;
    };

    // RAII subscription handle; destruction (or `disconnect`) removes the entry
    // immediately under the board's grant - call from a blue thread or setup
    // code (see the header comment). A pending staged install is killed by the
    // tombstone regardless of context.
    class Connection
    {
    public:
        Connection() = default;

        Connection(Connection&& other) noexcept
            : bus_(std::exchange(other.bus_, nullptr)), type_(other.type_), id_(other.id_),
              control_(std::move(other.control_))
        {
        }

        Connection& operator=(Connection&& other) noexcept
        {
            if (this != &other)
            {
                disconnect();
                bus_ = std::exchange(other.bus_, nullptr);
                type_ = other.type_;
                id_ = other.id_;
                control_ = std::move(other.control_);
            }
            return *this;
        }

        ~Connection() { disconnect(); }

        void disconnect()
        {
            if (Event_bus* bus = std::exchange(bus_, nullptr))
            {
                // Tombstone first: kills a not-yet-applied staged install (it
                // no-ops at apply) and makes the dispatch loop skip and reap an
                // installed entry even before the removal below lands.
                control_->dead.store(true, std::memory_order_release);
                // Strict removal of an already-installed entry, immediate-effect
                // under the board's grant. Removing an id whose install never
                // applied is a no-op.
                bus->board_.access([type = type_, id = id_](Board& b) { b.remove(type, id); }).sync();
                control_.reset();
            }
        }

    private:
        friend class Event_bus;

        Connection(Event_bus* bus, std::type_index type, std::uint64_t id, detail::Ref_ptr<Sub_control> control)
            : bus_(bus), type_(type), id_(id), control_(std::move(control))
        {
        }

        Event_bus* bus_ = nullptr;
        std::type_index type_{ typeid(void) };
        std::uint64_t id_ = 0;
        detail::Ref_ptr<Sub_control> control_;
    };

    explicit Event_bus(Named name)
        : board_{ std::move(name) }, staged_{ board_ }, publisher_{ staged_.parallel_recorder() }
    {
    }

    // Fire-and-forget: stages the append, grant-free, from any thread or task;
    // visible to handlers at the next dispatch.
    template<typename E>
    void publish(E e)
    {
        publisher_.stage([e = std::move(e)](Board& b) mutable { b.lane<E>().events.push_back(std::move(e)); });
    }

    // Unpinned: handler is `fn(const E&)`; its captures must outlive the connection.
    template<typename Fn>
    Connection subscribe(Fn fn)
    {
        using E = typename Handler_traits<Fn>::Event;
        return add_subscription<E>(false, {}, [fn = std::move(fn)](void*, const E& e) { fn(e); });
    }

    // Pinned, handler form: `fn(Owner& self, const E&)` - `self` is the locked
    // owner, valid for the whole invocation; no raw pointer in user code.
    template<typename O, typename Fn>
        requires (!std::is_member_function_pointer_v<std::remove_reference_t<Fn>>)
    Connection subscribe(const std::shared_ptr<O>& owner, Fn fn)
    {
        using E = typename Handler_traits<Fn>::Event;
        return add_subscription<E>(true, owner,
            [fn = std::move(fn)](void* p, const E& e) { fn(*static_cast<O*>(p), e); });
    }

    // Pinned, member-function form (the UE `AddSP` shape).
    template<typename O, typename E>
    Connection subscribe(const std::shared_ptr<O>& owner, void (O::*method)(const E&))
    {
        return add_subscription<E>(true, owner,
            [method](void* p, const E& e) { (static_cast<O*>(p)->*method)(e); });
    }

    // The per-domain delivery point, as a node body: applies the staged batch
    // (`commit()` under the held grant takes the inline arm), dispatches every
    // handler, clears the lanes. A named type rather than a lambda so a user can
    // store the body - `ts::Event_bus::Dispatch_fn dispatch_ = bus.dispatch_fn();`
    // - the same spelling `ts::publish_fn` gives with `Publish_fn<T>`.
    struct Dispatch_fn
    {
        Event_bus* bus;

        void operator()(Board& board) const;
    };

    // Run it as a graph node declaring write on `state()`, or through a write
    // access from the frame loop.
    Dispatch_fn dispatch_fn() { return Dispatch_fn{ this }; }

    Guarded<Board>& state() { return board_; }

private:
    template<typename E>
    Connection add_subscription(bool pinned, std::weak_ptr<void> owner, std::function<void(void*, const E&)> invoke)
    {
        std::uint64_t id = next_id_++;
        auto control = detail::make_ref<Sub_control>();
        // Installed like a publish: staged into the journal, applied at the
        // dispatch node's cut before handlers run - grant-free, legal from any
        // thread or task, no join. A disconnect that raced ahead already
        // tombstoned `control`, so the install applies as a no-op instead of
        // resurrecting a dead subscription.
        publisher_.stage([id, pinned, owner = std::move(owner), invoke = std::move(invoke), control](Board& b) mutable
        {
            if (control->dead.load(std::memory_order_acquire))
                return;
            b.lane<E>().subs.push_back({ id, pinned, std::move(owner), std::move(invoke), std::move(control) });
        });
        return Connection(this, std::type_index(typeid(E)), id, std::move(control));
    }

    Guarded<Board> board_;
    Deferred<Board> staged_;
    Parallel_recorder<Board> publisher_;
    std::atomic<std::uint64_t> next_id_{ 1 };
};

inline void Event_bus::Dispatch_fn::operator()(Board& board) const
{
    (void)bus->staged_.commit();   // holds the grant: applies inline, no handle to await
    board.dispatch_all();
}

} // namespace ts
