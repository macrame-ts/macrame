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
//    state inline; outbound effects go through `async`/`stage` (grant-free
//    submission), and the harness faults a handler that strays.
// 2. Command: one known target -- "play this sound" is not an event, it is a
//    command; `audio.async(fn)` is the whole pattern (the kill sting below).
// 3. Lightweight notifications, many types: the event board -- one
//    `Versioned<Event_board>` per domain with a typed lane per event kind.
//    Staging is grant-free, so firing is one line from anywhere with no
//    declaration, no edge, and no contention on the board; only the flip and
//    the readers touch the pipe, and readers overlap as readers. Adding an
//    event type costs a struct, a lane, and a loop in some consumer -- no new
//    objects or nodes. Deferred by design: consumers see events at the flip.
// 4. Heavyweight flows that should shape the schedule: a dedicated batch
//    object (`Damage_events`) written by the producer node and read by the
//    consumer node -- the conflict-derived edge orders them, the dependency is
//    visible in the DOT dump and the trace, and cost is O(events staged), not
//    O(population): thousands of NPCs, two hits a frame, two iterations.
//
// Ordering notes the recipe relies on: journal apply order is
// recorder-creation order (FIFO within a recorder), so the board's clear
// recorder -- minted first -- applies before any producer's events no matter
// when during the frame the clear was staged. Producers stage without grants,
// so no derived edge orders them against the flip: `flip.after(...)` is
// explicit intent, required for same-frame delivery. Cross-recorder order is
// arbitrary per the journal contract -- each lane here has one producer.
// Runs twice and checks the two runs are identical.

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "ts/versioned.h"

#include <cstdio>
#include <functional>
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

// Tier 3 transport: the per-domain board. One typed lane per event kind; the
// container half is domain code, the synchronization half is `Versioned`.
class Event_board
{
public:
    void push(Footstep e) { TS_CHECK_ACCESS(); footsteps_.push_back(e); }
    void push(Pickup e) { TS_CHECK_ACCESS(); pickups_.push_back(e); }
    void push(Kill e) { TS_CHECK_ACCESS(); kills_.push_back(e); }

    const std::vector<Footstep>& footsteps() const { TS_CHECK_ACCESS(); return footsteps_; }
    const std::vector<Pickup>& pickups() const { TS_CHECK_ACCESS(); return pickups_; }
    const std::vector<Kill>& kills() const { TS_CHECK_ACCESS(); return kills_; }

    void clear()
    {
        TS_CHECK_ACCESS();
        footsteps_.clear();
        pickups_.clear();
        kills_.clear();
    }

private:
    std::vector<Footstep> footsteps_;
    std::vector<Pickup> pickups_;
    std::vector<Kill> kills_;
};

// --- toy systems ------------------------------------------------------------------

// Movement: many NPCs patrol; a few actually move each frame and stage
// footsteps -- cost tracks the movers, not the population.
class Movement
{
public:
    void tick(int frame, int movers, std::vector<Footstep>& out)
    {
        TS_CHECK_ACCESS();
        for (int m = 0; m < movers; ++m)
            out.push_back({ (frame * 5 + m * 11) % npc_count });
    }

    static constexpr int npc_count = 1000;
};

// Combat: resolves the frame's couple of hits. Carries the tier-1
// intra-system delegate: `on_kill_` fires synchronously, inline, under this
// node's grant -- plain observer code, no library involvement.
class Combat
{
public:
    // `move_only_function`: the handler owns a move-only `Recorder`.
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

// Loot: an occasional pickup -- another board producer with its own lane.
class Loot
{
public:
    void tick(int frame, std::vector<Pickup>& out)
    {
        TS_CHECK_ACCESS();
        if (frame % 4 == 0)
            out.push_back({ (frame * 3) % Movement::npc_count, frame % 7 });
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

// Two board consumers so the readers overlap on the published version.
class Hud
{
public:
    void show(const Event_board& b)
    {
        TS_CHECK_ACCESS();
        footsteps_ += static_cast<int>(b.footsteps().size());
        pickups_ += static_cast<int>(b.pickups().size());
        kills_ += static_cast<int>(b.kills().size());
    }

    int footsteps() const { TS_CHECK_ACCESS(); return footsteps_; }
    int pickups() const { TS_CHECK_ACCESS(); return pickups_; }
    int kills() const { TS_CHECK_ACCESS(); return kills_; }

private:
    int footsteps_ = 0;
    int pickups_ = 0;
    int kills_ = 0;
};

class Stats
{
public:
    void accumulate(const Event_board& b)
    {
        TS_CHECK_ACCESS();
        events_ += static_cast<int>(b.footsteps().size() + b.pickups().size() + b.kills().size());
    }

    int events() const { TS_CHECK_ACCESS(); return events_; }

private:
    int events_ = 0;
};

struct Events_stats
{
    int hit_barks = 0;
    int kill_stings = 0;
    int combat_kills = 0;
    int hud_footsteps = 0;
    int hud_pickups = 0;
    int hud_kills = 0;
    int stats_events = 0;

    bool operator==(const Events_stats&) const = default;
};

Events_stats run_events_frames(int frames)
{
    // Declaration order is destruction order reversed: the board first (its
    // recorders live in node closures and the combat handler, and recorders
    // must not outlive their journal), the graph last (a compiled graph must
    // release its objects before they die).
    ts::Versioned<Event_board> board{ ts::Named{} };
    ts::Guarded<Damage_events> damage_events{ ts::Named{} };
    ts::Guarded<Movement> movement{ ts::Named{} };
    ts::Guarded<Combat> combat{ ts::Named{} };
    ts::Guarded<Loot> loot{ ts::Named{} };
    ts::Guarded<Audio> audio{ ts::Named{} };
    ts::Guarded<Hud> hud{ ts::Named{} };
    ts::Guarded<Stats> stats{ ts::Named{} };

    // Minted before any producer recorder, so the staged clear applies first
    // at every cut and readers see exactly one frame's events per version.
    auto clear_rec = board.recorder();

    // Tier 1 + tier 2 wiring: the kill handler reacts inside Combat (the
    // counter bump happens in `resolve`), publishes outward by staging a
    // `Kill` onto the board, and issues one command -- `audio.async` -- both
    // grant-free, per the one rule for handlers.
    combat.access([&audio, kill_rec = board.recorder()](Combat& c) mutable
    {
        c.set_on_kill([&audio, kill_rec = std::move(kill_rec)](int npc) mutable
        {
            kill_rec.stage([npc](Event_board& b) { b.push(Kill{ npc }); });
            audio.async([](Audio& a) { a.play_kill_sting(); });
        });
    }).sync();

    ts::Static_task_graph g;

    // Movement stages the frame's clear plus its footsteps. Staging declares
    // nothing on the board -- the node's declared access is Movement only.
    auto move = g.add_node(ts::Named{}, [frame = 0, clear = std::move(clear_rec),
        steps = board.recorder()](Movement& m) mutable
    {
        clear.stage([](Event_board& b) { b.clear(); });
        std::vector<Footstep> out;
        m.tick(frame, 3, out);
        steps.stage([out = std::move(out)](Event_board& b)
        {
            for (const Footstep& f : out)
                b.push(f);
        });
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

    auto scavenge = g.add_node(ts::Named{}, [frame = 0, drops = board.recorder()](Loot& l) mutable
    {
        std::vector<Pickup> out;
        l.tick(frame, out);
        if (!out.empty())
            drops.stage([out = std::move(out)](Event_board& b)
            {
                for (const Pickup& p : out)
                    b.push(p);
            });
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

    // The flip. Producers left no derived edges (they never touched the
    // board's pipe), so same-frame delivery is explicit intent:
    auto flip = g.add_node(ts::Named{}, ts::publish_body(board), board.state());
    flip.after(move, fight, scavenge);

    // Tier 3 consumers: two readers of the published version, overlapping.
    auto show = g.add_node(ts::Named{}, [](const Event_board& b, Hud& h) { h.show(b); }, board.state(), hud);
    show.after(flip);
    auto count = g.add_node(ts::Named{}, [](const Event_board& b, Stats& s) { s.accumulate(b); }, board.state(), stats);
    count.after(flip);

    g.compile();
    for (int f = 0; f < frames; ++f)
        g.execute().sync();

    // The handler's async stings were enqueued during the runs -- pipe FIFO
    // applies them before these reads.
    Events_stats st;
    st.hit_barks = audio.access([](const Audio& a) { return a.hit_barks(); }).sync();
    st.kill_stings = audio.access([](const Audio& a) { return a.kill_stings(); }).sync();
    st.combat_kills = combat.access([](const Combat& c) { return c.kills(); }).sync();
    st.hud_footsteps = hud.access([](const Hud& h) { return h.footsteps(); }).sync();
    st.hud_pickups = hud.access([](const Hud& h) { return h.pickups(); }).sync();
    st.hud_kills = hud.access([](const Hud& h) { return h.kills(); }).sync();
    st.stats_events = stats.access([](const Stats& s) { return s.events(); }).sync();
    return st;
}

} // namespace

void run_events_sample()
{
    constexpr int frames = 90;
    Events_stats a = run_events_frames(frames);
    Events_stats b = run_events_frames(frames);

    std::printf("events sample: %d frames, %d hit barks, %d kill stings, %d kills, "
                "hud %d/%d/%d steps/pickups/kills, %d events counted, %s\n",
        frames, a.hit_barks, a.kill_stings, a.combat_kills,
        a.hud_footsteps, a.hud_pickups, a.hud_kills, a.stats_events,
        a == b ? "deterministic across runs" : "NON-DETERMINISTIC (bug)");
}

} // namespace sample
