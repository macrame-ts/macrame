// Inter-system events on the existing primitives -- the delegate/event recipe,
// executable. A UE-style delegate bundles two things this library keeps apart:
// the notification ("X happened") and the reaction ("do work in response"). The
// reaction runs at a sanctioned point -- a graph edge, a dispatch node, an
// enqueued task -- never inline under the publisher's grant, because a
// synchronous broadcast into foreign systems is exactly the completeness hazard
// the access model exists to prevent.
//
// The four tiers, cheapest ceremony first:
//
// 1. Intra-system: parts of one system, under one grant. A plain synchronous
//    delegate (`Combat::on_kill_` below) is sanctioned as-is -- inside a grant
//    window there is no concurrency, so ordinary observer code needs zero
//    library machinery. The one rule: the handler must not touch other guarded
//    state inline; outbound effects go through `async`/`publish`/`stage`
//    (grant-free submission), and the harness faults a handler that strays.
// 2. Command: one known target -- "play this sound" is not an event, it is a
//    command; `audio.async(fn)` is the whole pattern (the kill sting below).
// 3. Lightweight notifications, many types: `Event_bus` below -- a pub/sub
//    prototype in the entt-dispatcher shape (type-as-topic, deferred batch).
//    Firing is `bus.publish(Damage{...})` -- one line from anywhere, grant-free,
//    no declaration, no edge. Subscribing is one line, the event type deduced
//    from the handler's parameter. Delivery happens at the pump node, deferred
//    by design; immediate reaction is tier 1 or 2 by definition.
// 4. Heavyweight flows that should shape the schedule: a dedicated batch
//    object (`Damage_events`) written by the producer node and read by the
//    consumer node -- the conflict-derived edge orders them, the dependency is
//    visible in the DOT dump and the trace, and cost is O(events staged), not
//    O(population): thousands of NPCs, two hits a frame, two iterations.
//
// Ordering note the bus rests on: `publish` stages grant-free, so no derived
// edge orders producers against the pump -- `pump.after(producers...)` is
// explicit intent, required for same-frame delivery. Cross-producer apply
// order is arbitrary per the journal contract (the sample's handlers only
// count, so the run-twice determinism check is order-blind by construction).
// Runs twice and checks the two runs are identical.

#include "ts/access.h"
#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sample
{

namespace
{

// --- event types ------------------------------------------------------------------

struct Damage
{
    int npc = 0;
    int amount = 0;
};

struct Footstep
{
    int npc = 0;
};

struct Pickup
{
    int npc = 0;
    int item = 0;
};

struct Kill
{
    int npc = 0;
};

// Tier 4 transport: the per-flow batch. A plain guarded struct; the producer
// node clears and refills it (capacity is reused, so steady state allocates
// nothing), the consumer node reads it through the derived edge.
struct Damage_events
{
    std::vector<Damage> hits;
};

// --- Event_bus (tier 3 prototype) ---------------------------------------------------
//
// A per-domain pub/sub bus over `Guarded` + `Deferred`: the industry's
// deferred-batch shape (entt `enqueue`/`update`, Qt queued connections) built
// on the staging machinery. The event struct is the topic -- no delegate
// declaration, no named channel object per event kind.
//
//   bus.publish(Damage{npc, 12});                      // fire: any thread, grant-free
//   auto c = bus.subscribe(hud, &Hud::on_kill);        // pinned: owner kept alive per call
//   auto c2 = bus.subscribe([](const Footstep& f){}); // unpinned: captures must outlive c2
//   g.add_node("events", bus.pump_body(), bus.state()); // delivery point, once per domain
//
// Contracts, in terms of the library's existing rules only:
//  - `publish` stages into a `Parallel_recorder` (per-worker slots): safe from
//    any thread; cross-producer apply order is arbitrary (the journal
//    contract), per-producer order per-worker FIFO. Closure-tier staging
//    allocates per event past the SBO -- the typed-lane command tier is the
//    planned fix; this prototype documents the cost, not the endgame.
//  - the pump runs under the board's write grant: `commit()` takes the inline
//    arm, handlers run serialized, and a handler must not touch other guarded
//    state directly (route through `async`/`publish`/`stage` -- the one rule).
//  - pinned subscriptions (`subscribe(owner, ...)`) hold a weak handle and
//    lock it around each invocation: a dying owner is skipped and reaped
//    (auto-disconnect), and destruction of the owner needs no unsubscribe
//    ceremony. The handler receives the locked owner as its first parameter,
//    so no raw pointer appears in user code. If the last strong reference dies
//    elsewhere mid-pump, the owner's destructor runs on the pump's thread when
//    the pin releases -- the standard `shared_ptr` caveat.
//  - `subscribe` is staged like a publish (grant-free, any thread or task):
//    the install applies at the pump's cut, before dispatch, so a subscription
//    observes every event in the first cut that contains it -- and program
//    order makes subscribe-then-publish from one thread always delivered
//    (the install cannot land in a later cut than the publish). A disconnect
//    racing a not-yet-applied install wins via a tombstone the install checks
//    at apply, so no zombie entry can appear. `Connection` removal stays an
//    immediate-effect write on the board (`access` + `sync`) --
//    blue-thread-or-setup only in this prototype; the library version would
//    add the reentrant-inline arm (sub/unsub from inside a handler, deferred
//    past the dispatch loop to keep iteration valid) and the checked-fatal arm
//    for ungranted tasks. Here, don't subscribe or disconnect from handlers.
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
    // would need an explicit `subscribe<E>` tier, not provided by the prototype.
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
    // only under the bus's grant (apply at commit, dispatch in the pump,
    // sub/unsub through `access`).
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
            // the lane), stable within a run but cross-producer apply order is
            // arbitrary, so type dispatch order is not guaranteed across runs.
            // The sample's handlers only count, so the determinism check is
            // order-blind by construction.
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

    // RAII subscription handle; destruction (or `disconnect`) removes the
    // entry immediately under the board's grant. Prototype contract: call from
    // a blue thread or setup code only (see the class comment).
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
                // under the board's grant -- prototype contract: blue thread or
                // setup only. Removing an id whose install never applied is a
                // no-op.
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

    explicit Event_bus(ts::Named name)
        : board_{ std::move(name) }, staged_{ board_ }, publisher_{ staged_.parallel_recorder() }
    {
    }

    // Fire-and-forget: stages the append, grant-free, from any thread or task;
    // visible to handlers at the next pump.
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

    // Pinned, handler form: `fn(Owner& self, const E&)` -- `self` is the locked
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

    // The per-domain delivery point: run as a graph node declaring write on
    // `state()`. Applies the staged batch (`commit()` under the held grant
    // takes the inline arm), dispatches every handler, clears the lanes.
    auto pump_body()
    {
        return [this](Board& board)
        {
            (void)staged_.commit();
            board.dispatch_all();
        };
    }

    ts::Guarded<Board>& state() { return board_; }

private:
    template<typename E>
    Connection add_subscription(bool pinned, std::weak_ptr<void> owner, std::function<void(void*, const E&)> invoke)
    {
        std::uint64_t id = next_id_++;
        auto control = std::make_shared<Sub_control>();
        // Installed like a publish: staged into the journal, applied at the
        // pump's cut before dispatch -- grant-free, legal from any thread or
        // task, no join. Program order makes subscribe-then-publish from one
        // thread always delivered: the install cannot land in a later cut than
        // the publish. A disconnect that raced ahead already tombstoned
        // `control`, so the install applies as a no-op instead of resurrecting
        // a dead subscription.
        publisher_.stage([id, pinned, owner = std::move(owner), invoke = std::move(invoke), control](Board& b) mutable
        {
            if (control->dead.load(std::memory_order_acquire))
                return;
            b.lane<E>().subs.push_back({ id, pinned, std::move(owner), std::move(invoke), std::move(control) });
        });
        return Connection(this, std::type_index(typeid(E)), id, std::move(control));
    }

    ts::Guarded<Board> board_;
    ts::Deferred<Board> staged_;
    ts::Parallel_recorder<Board> publisher_;
    std::atomic<std::uint64_t> next_id_{ 1 };
};

// --- minimal setup reference ------------------------------------------------------
//
// The entire required ceremony, both flavors. Everything else in this file is
// demonstration; these two functions are the floor.

struct Ping
{
    int n = 0;
};

// With a static graph: one bus, one pump node ordered after the producers.
bool minimal_bus_with_graph()
{
    Event_bus bus{ ts::Named{} };
    ts::Guarded<int> ticks{ ts::Named{} };

    int received = 0;
    auto conn = bus.subscribe([&received](const Ping& p) { received += p.n; });

    ts::Static_task_graph g;
    auto produce = g.add_node(ts::Named{}, [&bus](int& n) { bus.publish(Ping{ ++n }); }, ticks);
    auto pump = g.add_node(ts::Named{}, bus.pump_body(), bus.state());
    pump.after(produce);   // staging derives no edges -- same-frame delivery is explicit intent
    g.compile();

    for (int frame = 0; frame < 3; ++frame)
        g.execute().sync();

    return received == 1 + 2 + 3;
}

// Without a graph: the pump is one write access per frame, from the frame
// loop. `access` runs it inline when the board is free (the usual case here).
bool minimal_bus_without_graph()
{
    Event_bus bus{ ts::Named{} };

    int received = 0;
    auto conn = bus.subscribe([&received](const Ping& p) { received += p.n; });

    for (int frame = 1; frame <= 3; ++frame)
    {
        bus.publish(Ping{ frame });                   // from anywhere: nodes, tasks, this thread
        bus.state().access(bus.pump_body()).sync();   // the delivery point, once per frame
    }

    return received == 1 + 2 + 3;
}

// --- toy systems ------------------------------------------------------------------

// Movement: many NPCs patrol; a few actually move each frame and report
// footsteps -- cost tracks the movers, not the population.
class Movement
{
public:
    std::vector<Footstep> tick(int frame, int movers)
    {
        TS_CHECK_ACCESS();
        std::vector<Footstep> out;
        for (int m = 0; m < movers; ++m)
            out.push_back({ (frame * 5 + m * 11) % npc_count });
        return out;
    }

    static constexpr int npc_count = 1000;
};

// Combat: resolves the frame's couple of hits. Carries the tier-1
// intra-system delegate: `on_kill_` fires synchronously, inline, under this
// node's grant -- plain observer code, no library involvement.
class Combat
{
public:
    void set_on_kill(std::move_only_function<void(int npc)> fn)
    {
        TS_CHECK_ACCESS();
        on_kill_ = std::move(fn);
    }

    void resolve(int frame, std::vector<Damage>& out)
    {
        TS_CHECK_ACCESS();
        out.push_back({ (frame * 7 + 3) % Movement::npc_count, 10 + frame % 3 });
        out.push_back({ (frame * 13 + 5) % Movement::npc_count, 10 });
        hit_count_ += 2;
        if (hit_count_ % 5 == 0)
        {
            ++kills_;                          // the intra-system reaction proper
            if (on_kill_)
                on_kill_(out.back().npc);      // tier 1: synchronous, under the grant
        }
    }

    int kills() const { TS_CHECK_ACCESS(); return kills_; }

private:
    std::move_only_function<void(int)> on_kill_;
    int hit_count_ = 0;
    int kills_ = 0;
};

// Loot: an occasional pickup -- another producer publishing on the bus.
class Loot
{
public:
    std::vector<Pickup> tick(int frame)
    {
        TS_CHECK_ACCESS();
        std::vector<Pickup> out;
        if (frame % 4 == 0)
            out.push_back({ (frame * 3) % Movement::npc_count, frame % 7 });
        return out;
    }
};

class Audio
{
public:
    void play_hit(const Damage&) { TS_CHECK_ACCESS(); ++hit_barks_; }
    void play_kill_sting() { TS_CHECK_ACCESS(); ++kill_stings_; }
    int hit_barks() const { TS_CHECK_ACCESS(); return hit_barks_; }
    int kill_stings() const { TS_CHECK_ACCESS(); return kill_stings_; }

private:
    int hit_barks_ = 0;
    int kill_stings_ = 0;
};

// Plain subscriber objects -- not `Guarded`. Pinning covers their memory
// lifetime (the weak lock per invocation); serialization comes from all
// mutation happening inside pump handlers, which run one at a time under the
// bus's write grant. A subscriber that must touch guarded state still routes
// through `async`/`publish`/`stage` -- the one rule.
struct Hud
{
    int footsteps = 0;
    int pickups = 0;
    int kills = 0;

    void on_footstep(const Footstep&) { ++footsteps; }
    void on_kill(const Kill&) { ++kills; }
};

// Destroyed mid-run to demonstrate pinned auto-disconnect: once the owner
// dies, the pump's weak lock fails, the handler is skipped and the entry
// reaped -- no unsubscribe call anywhere.
struct Commander
{
    int kills_seen = 0;

    void on_kill(const Kill&) { ++kills_seen; }
};

struct Events_stats
{
    int hit_barks = 0;
    int kill_stings = 0;
    int combat_kills = 0;
    int hud_footsteps = 0;
    int hud_pickups = 0;
    int hud_kills = 0;
    int commander_kills = 0;
    int events_total = 0;

    bool operator==(const Events_stats&) const = default;
};

Events_stats run_events_frames(int frames)
{
    // Declaration order is destruction order reversed: the bus first (its
    // board outlives every connection and the staged batches), systems and
    // subscribers next, connections after them (their disconnect touches the
    // bus), the graph last (a compiled graph releases its objects, including
    // the pump's board, before they die).
    Event_bus bus{ ts::Named{} };
    ts::Guarded<Damage_events> damage_events{ ts::Named{} };
    ts::Guarded<Movement> movement{ ts::Named{} };
    ts::Guarded<Combat> combat{ ts::Named{} };
    ts::Guarded<Loot> loot{ ts::Named{} };
    ts::Guarded<Audio> audio{ ts::Named{} };

    auto hud = std::make_shared<Hud>();
    auto commander = std::make_shared<Commander>();
    int events_total = 0;   // written only by pump handlers (serialized); read after the final sync

    // Subscriptions: the event type is deduced from the handler's parameter.
    // Installs are staged like publishes -- active from the first pump's cut
    // that contains them, which here is frame 0's.
    std::vector<Event_bus::Connection> connections;
    connections.push_back(bus.subscribe(hud, &Hud::on_footstep));            // pinned, member-function form
    connections.push_back(bus.subscribe(hud, &Hud::on_kill));
    connections.push_back(bus.subscribe(hud, [](Hud& self, const Pickup&)   // pinned, lambda form: `self` is
    {                                                                        // the locked owner, no raw pointer
        ++self.pickups;
    }));
    connections.push_back(bus.subscribe(commander, &Commander::on_kill));
    connections.push_back(bus.subscribe([&events_total](const Footstep&) { ++events_total; }));  // unpinned
    connections.push_back(bus.subscribe([&events_total](const Pickup&) { ++events_total; }));
    connections.push_back(bus.subscribe([&events_total](const Kill&) { ++events_total; }));

    // Tier 1 + tier 2 wiring: the kill handler reacts inside `Combat` (the
    // counter bump happens in `resolve`), publishes outward on the bus and
    // issues one command -- both grant-free, per the one rule for handlers.
    combat.access([&bus, &audio](Combat& c)
    {
        c.set_on_kill([&bus, &audio](int npc)
        {
            bus.publish(Kill{ npc });                                  // tier 3: one line, no ceremony
            audio.async([](Audio& a) { a.play_kill_sting(); });        // tier 2: command to a known target
        });
    }).sync();

    ts::Static_task_graph g;

    auto move = g.add_node(ts::Named{}, [frame = 0, &bus](Movement& m) mutable
    {
        for (const Footstep& f : m.tick(frame, 3))
            bus.publish(f);
        ++frame;
    }, movement);

    // Tier 4 producer: combat writes the damage batch under its declared
    // grant; `hits` is cleared and refilled, capacity reused.
    auto fight = g.add_node(ts::Named{}, [frame = 0](Combat& c, Damage_events& ev) mutable
    {
        ev.hits.clear();
        c.resolve(frame, ev.hits);
        ++frame;
    }, combat, damage_events);

    auto scavenge = g.add_node(ts::Named{}, [frame = 0, &bus](Loot& l) mutable
    {
        for (const Pickup& p : l.tick(frame))
            bus.publish(p);
        ++frame;
    }, loot);

    // Tier 4 consumer: the derived write->read edge on `damage_events` orders
    // this after `fight`; O(hits) work. The kill sting `async`s land on the
    // same pipe this node holds -- they queue behind the grant, never race it.
    g.add_node(ts::Named{}, [](const Damage_events& ev, Audio& a)
    {
        for (const Damage& d : ev.hits)
            a.play_hit(d);
    }, damage_events, audio);

    // The pump. Producers left no derived edges (`publish` never touched the
    // board's pipe), so same-frame delivery is explicit intent:
    auto pump = g.add_node(ts::Named{}, bus.pump_body(), bus.state());
    pump.after(move, fight, scavenge);

    g.compile();

    const int commander_retires_at = 60;
    int commander_kills = 0;
    for (int f = 0; f < frames; ++f)
    {
        if (f == commander_retires_at)
        {
            commander_kills = commander->kills_seen;
            commander.reset();   // pinned auto-disconnect: no unsubscribe call, the pump reaps it
        }
        g.execute().sync();
    }
    if (commander)
        commander_kills = commander->kills_seen;

    // The handler's async stings were enqueued during the runs -- pipe FIFO
    // applies them before these reads; the pinned subscribers settled with the
    // last pump, so plain reads are safe here on the blue thread.
    Events_stats st;
    st.hit_barks = audio.access([](const Audio& a) { return a.hit_barks(); }).sync();
    st.kill_stings = audio.access([](const Audio& a) { return a.kill_stings(); }).sync();
    st.combat_kills = combat.access([](const Combat& c) { return c.kills(); }).sync();
    st.hud_footsteps = hud->footsteps;
    st.hud_pickups = hud->pickups;
    st.hud_kills = hud->kills;
    st.commander_kills = commander_kills;
    st.events_total = events_total;
    return st;
}

} // namespace

void run_events_sample()
{
    bool minimal_ok = minimal_bus_with_graph() && minimal_bus_without_graph();

    constexpr int frames = 90;
    Events_stats a = run_events_frames(frames);
    Events_stats b = run_events_frames(frames);

    std::printf("events sample: %d frames, %d hit barks, %d kill stings, %d kills, "
                "hud %d/%d/%d steps/pickups/kills, commander saw %d, %d events dispatched, "
                "minimal setups %s, %s\n",
        frames, a.hit_barks, a.kill_stings, a.combat_kills,
        a.hud_footsteps, a.hud_pickups, a.hud_kills, a.commander_kills, a.events_total,
        minimal_ok ? "ok" : "BROKEN",
        a == b ? "deterministic across runs" : "NON-DETERMINISTIC (bug)");
}

} // namespace sample
