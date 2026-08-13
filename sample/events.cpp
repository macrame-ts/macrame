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
// 3. Lightweight notifications, many types: `ts::Event_bus` (`ts/event_bus.h`)
//    -- per-domain pub/sub, type-as-topic, deferred batch. Firing is
//    `bus.publish(Damage{...})`, one grant-free line from anywhere; subscribing
//    is one line, the event type deduced from the handler's parameter.
//    Delivery happens at the dispatch node, deferred by design; immediate
//    reaction is tier 1 or 2 by definition. Contracts in the header comment.
// 4. Heavyweight flows that should shape the schedule: a dedicated batch
//    object (`Damage_events`) written by the producer node and read by the
//    consumer node -- the conflict-derived edge orders them, the dependency is
//    visible in the DOT dump and the trace, and cost is O(events staged), not
//    O(population): thousands of NPCs, two hits a frame, two iterations.
//
// Ordering note the bus rests on: `publish`/`subscribe` stage grant-free, so no
// derived edge orders producers against the dispatch node --
// `dispatch.after(producers...)` is explicit intent, required for same-frame
// delivery. Cross-producer apply order is arbitrary per the journal contract
// (the sample's handlers only count, so the run-twice determinism check is
// order-blind by construction). Runs twice and checks the runs are identical.

#include "ts/access.h"
#include "ts/event_bus.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"

#include <cstdio>
#include <functional>
#include <memory>
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

// --- minimal setup reference ------------------------------------------------------
//
// The entire required ceremony, both flavors. Everything else in this file is
// demonstration; these two functions are the floor.

struct Ping
{
    int n = 0;
};

// With a static graph: one bus, one dispatch node ordered after the producers.
bool minimal_bus_with_graph()
{
    ts::Event_bus bus{ ts::Named{} };
    ts::Guarded<int> ticks{ ts::Named{} };

    int received = 0;
    auto conn = bus.subscribe([&received](const Ping& p) { received += p.n; });

    ts::Static_task_graph g;
    auto produce = g.add_node(ts::Named{}, [&bus](int& n) { bus.publish(Ping{ ++n }); }, ticks);
    auto dispatch = g.add_node(ts::Named{}, bus.dispatch_fn(), bus.state());
    dispatch.after(produce);   // staging derives no edges -- same-frame delivery is explicit intent
    g.compile();

    for (int frame = 0; frame < 3; ++frame)
        g.execute().sync();

    return received == 1 + 2 + 3;
}

// Without a graph: delivery is one write access per frame, from the frame
// loop. `access` runs it inline when the board is free (the usual case here).
bool minimal_bus_without_graph()
{
    ts::Event_bus bus{ ts::Named{} };

    int received = 0;
    auto conn = bus.subscribe([&received](const Ping& p) { received += p.n; });

    for (int frame = 1; frame <= 3; ++frame)
    {
        bus.publish(Ping{ frame });                      // from anywhere: nodes, tasks, this thread
        bus.state().access(bus.dispatch_fn()).sync();    // the delivery point, once per frame
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
// mutation happening inside dispatch handlers, which run one at a time under
// the bus's write grant. A subscriber that must touch guarded state still
// routes through `async`/`publish`/`stage` -- the one rule.
struct Hud
{
    int footsteps = 0;
    int pickups = 0;
    int kills = 0;

    void on_footstep(const Footstep&) { ++footsteps; }
    void on_kill(const Kill&) { ++kills; }
};

// Destroyed mid-run to demonstrate pinned auto-disconnect: once the owner
// dies, the dispatch loop's weak lock fails, the handler is skipped and the
// entry reaped -- no unsubscribe call anywhere.
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
    // the bus's board, before they die).
    ts::Event_bus bus{ ts::Named{} };
    ts::Guarded<Damage_events> damage_events{ ts::Named{} };
    ts::Guarded<Movement> movement{ ts::Named{} };
    ts::Guarded<Combat> combat{ ts::Named{} };
    ts::Guarded<Loot> loot{ ts::Named{} };
    ts::Guarded<Audio> audio{ ts::Named{} };

    auto hud = std::make_shared<Hud>();
    auto commander = std::make_shared<Commander>();
    int events_total = 0;   // written only by dispatch handlers (serialized); read after the final sync

    // Subscriptions: the event type is deduced from the handler's parameter.
    // Installs are staged like publishes -- active from the first dispatch's
    // cut that contains them, which here is frame 0's.
    std::vector<ts::Event_bus::Connection> connections;
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

    // The dispatch node. Producers left no derived edges (staging never
    // touched the board's pipe), so same-frame delivery is explicit intent:
    auto dispatch = g.add_node(ts::Named{}, bus.dispatch_fn(), bus.state());
    dispatch.after(move, fight, scavenge);

    g.compile();

    const int commander_retires_at = 60;
    int commander_kills = 0;
    for (int f = 0; f < frames; ++f)
    {
        if (f == commander_retires_at)
        {
            commander_kills = commander->kills_seen;
            commander.reset();   // pinned auto-disconnect: no unsubscribe call, dispatch reaps it
        }
        g.execute().sync();
    }
    if (commander)
        commander_kills = commander->kills_seen;

    // The handler's async stings were enqueued during the runs -- pipe FIFO
    // applies them before these reads; the pinned subscribers settled with the
    // last dispatch, so plain reads are safe here on the blue thread.
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
