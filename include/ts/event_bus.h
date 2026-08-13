#pragma once

// `Event_bus` - per-domain pub/sub over `Guarded` + `Deferred`, the deferred-batch
// shape (entt's `enqueue`/`update`, Qt's queued connections): the event struct is
// the topic - no delegate declaration, no named channel per event kind.
//
//   ts::Event_bus bus{ ts::Named{"gameplay"} };
//   bus.publish(Damage{ npc, 12 });                     // fire: any thread, grant-free
//   auto c = bus.subscribe(hud, &Hud::on_kill);         // pinned: owner kept alive per call
//   auto c2 = bus.subscribe([](const Footstep& f){});   // unpinned: captures must outlive c2
//   g.add_node("events", bus.dispatch_fn(), bus.state());   // delivery point, once per domain
//   // graph-free: bus.state().access(bus.dispatch_fn()).sync(); once per frame
//
// Delivery is deferred by design: handlers see events at the dispatch node, in
// batches. Immediate reaction is a command to a known target (`target.async`) or
// an intra-system observer under one grant - not the bus. Contracts, all
// instances of the library's existing rules:
//
//  - `publish` stages into a `Parallel_recorder` (per-worker slots): safe from
//    any thread or task; cross-producer apply order is arbitrary (the journal
//    contract), per-producer order is per-worker FIFO. Closure-tier staging
//    allocates per event past the SBO - the typed-lane command tier is the
//    planned fix; this v1 documents the cost.
//  - `subscribe` is staged like a publish (grant-free, any thread or task): the
//    install applies at the dispatch node's cut, before handlers run, so a
//    subscription observes every event in the first cut that contains it - and
//    program order makes subscribe-then-publish from one thread always
//    delivered. A disconnect racing a not-yet-applied install wins via a
//    tombstone the install checks at apply.
//  - the dispatch node runs under the board's write grant: handlers run
//    serialized, and a handler must not touch other guarded state directly -
//    route through `async`/`publish`/`stage` (undeclared access faults in
//    checked builds).
//  - pinned subscriptions (`subscribe(owner, ...)`) hold a weak handle and lock
//    it around each invocation: a dying owner is skipped and reaped
//    (auto-disconnect), so owner destruction needs no unsubscribe ceremony. The
//    handler receives the locked owner as its first parameter - no raw pointer
//    in user code. If the last strong reference dies elsewhere mid-dispatch,
//    the owner's destructor runs on the dispatching thread when the pin
//    releases - the standard `shared_ptr` caveat.
//  - `Connection` removal is an immediate-effect write on the board (`access` +
//    `sync`): call from a blue thread or setup code. Sub/unsub from inside a
//    handler is not supported in v1 (the reentrant-inline arm, deferred past
//    the dispatch loop, and the checked-fatal arm for ungranted tasks are the
//    planned hardening).
//
// The bus is for lightweight notification fan-out. A heavy flow whose consumers
// should be scheduled as their own nodes belongs to a dedicated batch object
// with a conflict-derived edge - ordinary graph data flow, not events.

#include "ts/access.h"
#include "ts/deferred.h"
#include "ts/guarded.h"

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
    // dispatch loop skips and reaps entries it marks.
    struct Sub_control
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
            std::shared_ptr<Sub_control> control;
        };

        std::vector<E> events;
        std::vector<Subscription> subs;

        void dispatch_and_clear() override
        {
            for (const E& e : events)
            {
                for (Subscription& s : subs)
                {
                    if (s.control->dead.load(std::memory_order_acquire))
                        continue;
                    if (!s.pinned)
                    {
                        s.invoke(nullptr, e);
                        continue;
                    }
                    if (std::shared_ptr<void> pin = s.owner.lock())  // owner alive for the whole call
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

        Connection(Event_bus* bus, std::type_index type, std::uint64_t id, std::shared_ptr<Sub_control> control)
            : bus_(bus), type_(type), id_(id), control_(std::move(control))
        {
        }

        Event_bus* bus_ = nullptr;
        std::type_index type_{ typeid(void) };
        std::uint64_t id_ = 0;
        std::shared_ptr<Sub_control> control_;
    };

    explicit Event_bus(Named name = {})
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

    // The per-domain delivery point, as a node body: run it as a graph node
    // declaring write on `state()` (or through a write access from the frame
    // loop). Applies the staged batch (`commit()` under the held grant takes
    // the inline arm), dispatches every handler, clears the lanes.
    auto dispatch_fn()
    {
        return [this](Board& board)
        {
            (void)staged_.commit();
            board.dispatch_all();
        };
    }

    Guarded<Board>& state() { return board_; }

private:
    template<typename E>
    Connection add_subscription(bool pinned, std::weak_ptr<void> owner, std::function<void(void*, const E&)> invoke)
    {
        std::uint64_t id = next_id_++;
        auto control = std::make_shared<Sub_control>();
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

} // namespace ts
