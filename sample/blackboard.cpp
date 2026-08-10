// The blackboard pattern on `Versioned<T>` - the pattern recipe of
// docs/command-buffer-design.md §7.4, executable. The pattern needs no library
// support beyond the existing primitives.
//
// A stealth-guard toy: the world ticks a patrolling player; a senses system
// stages facts onto a shared squad blackboard (grant-free); a flip publishes
// them; a dispatch node fires key-change subscriptions off the fresh snapshot;
// agents read the stable previous version all frame. Runs twice and checks the
// two runs are identical - staged writes + snapshot reads keep the whole
// thing deterministic.

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "ts/versioned.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace sample
{

namespace
{

struct Vec2
{
    float x = 0.0f, y = 0.0f;
    bool operator==(const Vec2&) const = default;
};

enum class Key { player_pos, player_visible, alert_level };

using Value = std::variant<int, bool, Vec2>;

// The shared board: a plain thread-unsafe typed KV store, instrumented like any
// guarded system. The container half of the pattern is domain code - the
// library only provides the synchronization half (`Versioned`).
class Blackboard
{
public:
    void set(Key k, Value v)
    {
        TS_CHECK_ACCESS();
        facts_[k] = std::move(v);
    }

    template<typename V>
    V get(Key k, V fallback = {}) const
    {
        TS_CHECK_ACCESS();
        auto it = facts_.find(k);
        if (it == facts_.end())
            return fallback;
        if (const V* v = std::get_if<V>(&it->second))
            return *v;
        return fallback;
    }

    bool has(Key k) const
    {
        TS_CHECK_ACCESS();
        return facts_.contains(k);
    }

    Value raw(Key k) const
    {
        TS_CHECK_ACCESS();
        return facts_.at(k);
    }

private:
    std::map<Key, Value> facts_;
};

// Key-change notification on top of snapshot publishes: each subscription
// caches the last value it saw; `dispatch` (a node ordered after the flip)
// re-reads and fires on change. Polling-diff, not push - with closure
// commands the journal cannot report which keys a batch touched; the typed
// tier's `Set_cmd` stream is the planned dirty-set-for-free (doc §7.3).
// Deterministic: fires in subscription order, off a published version.
class Subscriptions
{
public:
    using Callback = std::function<void(const Value&)>;

    void subscribe(Key k, Callback cb)
    {
        TS_CHECK_ACCESS();
        subs_.push_back({ k, std::move(cb), std::nullopt });
    }

    // Runs inside the dispatch node, under its grant - callbacks must not
    // touch other guarded state directly; side effects go through
    // `async`/`stage` (grant-free submission).
    void dispatch(const Blackboard& bb)
    {
        TS_CHECK_ACCESS();
        for (Sub& s : subs_)
        {
            if (!bb.has(s.key))
                continue;
            Value now = bb.raw(s.key);
            if (!s.last || !(*s.last == now))
            {
                s.cb(now);
                s.last = now;
                ++fired_;
            }
        }
    }

    int fired() const
    {
        TS_CHECK_ACCESS();
        return fired_;
    }

private:
    struct Sub
    {
        Key key;
        Callback cb;
        std::optional<Value> last;
    };

    std::vector<Sub> subs_;
    int fired_ = 0;
};

// --- toy systems ------------------------------------------------------------------

// The player patrols a triangle wave; the guards' vision zone is x in [8, 16).
class World
{
public:
    void tick()
    {
        TS_CHECK_ACCESS();
        pos_.x += dir_;
        if (pos_.x >= 24.0f || pos_.x <= 0.0f)
            dir_ = -dir_;
    }

    Vec2 player() const { TS_CHECK_ACCESS(); return pos_; }
    bool player_in_zone() const { TS_CHECK_ACCESS(); return pos_.x >= 8.0f && pos_.x < 16.0f; }

private:
    Vec2 pos_;
    float dir_ = 1.0f;
};

enum class Orders { patrol, attack };

class Squad
{
public:
    void set_orders(Orders o)
    {
        TS_CHECK_ACCESS();
        if (orders_ != o)
        {
            orders_ = o;
            ++changes_;
        }
    }

    int order_changes() const { TS_CHECK_ACCESS(); return changes_; }

private:
    Orders orders_ = Orders::patrol;
    int changes_ = 0;
};

class Audio
{
public:
    void bark() { TS_CHECK_ACCESS(); ++barks_; }
    int barks() const { TS_CHECK_ACCESS(); return barks_; }

private:
    int barks_ = 0;
};

class Agents
{
public:
    void observe(bool player_visible)
    {
        TS_CHECK_ACCESS();
        if (player_visible)
            ++seen_;
    }

    int seen() const { TS_CHECK_ACCESS(); return seen_; }

private:
    int seen_ = 0;
};

struct Blackboard_stats
{
    int notifications = 0;
    int barks = 0;
    int order_changes = 0;
    int agent_seen = 0;
    int final_alert = 0;

    bool operator==(const Blackboard_stats&) const = default;
};

Blackboard_stats run_blackboard_frames(int frames)
{
    // Destruction order: graph last (it holds the recorder), then systems, then
    // the board - callbacks capture squad/audio by reference, so those outlive
    // the Subscriptions holding the callbacks.
    ts::Guarded<World> world{ ts::Named{} };
    ts::Versioned<Blackboard> bb{ ts::Named{} };   // the shared board
    ts::Guarded<Squad> squad{ ts::Named{} };
    ts::Guarded<Audio> audio{ ts::Named{} };
    ts::Guarded<Agents> agents{ ts::Named{} };
    ts::Guarded<Subscriptions> subs{ ts::Named{} };

    // Register subscriptions. The callbacks fire inside the dispatch node; both
    // route their side effects through `async` - no grant needed to submit.
    // `async` (not `access`) is deliberate there: inside a node, `access` may run
    // the body inline while the node still holds its declared objects --
    // stretching the critical window (every successor waits) and stacking a
    // nested access scope; under contention it enqueues anyway, so the inline
    // win is unreliable. Out here at setup it's the opposite - short and
    // uncontended - so `access` runs the registration right on this thread.
    subs.access([&squad, &audio](Subscriptions& s)
    {
        s.subscribe(Key::player_visible, [&audio](const Value& v)
        {
            if (std::get<bool>(v))
                audio.async([](Audio& a) { a.bark(); });
        });
        s.subscribe(Key::alert_level, [&squad](const Value& v)
        {
            Orders o = std::get<int>(v) >= 2 ? Orders::attack : Orders::patrol;
            squad.async([o](Squad& sq) { sq.set_orders(o); });
        });
    }).sync();

    ts::Static_task_graph g;

    g.add_node(ts::Named{}, [](World& w) { w.tick(); }, world);

    // Senses: reads the world, stages facts - no grant on the board, so it
    // never contends with the thinkers. The alert decay is a read-modify-write
    // command: legal under `Resync::replay` (both replicas see identical
    // pre-states), and it owns its keys - the key-ownership convention that
    // the cross-recorder ordering contract enforces by making the alternative
    // meaningless.
    auto sense = g.add_node(ts::Named{}, [rec = bb.recorder()](const World& w) mutable
    {
        Vec2 p = w.player();
        bool vis = w.player_in_zone();
        rec.stage([p, vis](Blackboard& b)
        {
            b.set(Key::player_pos, Value{ p });
            b.set(Key::player_visible, Value{ vis });
            int alert = b.get<int>(Key::alert_level, 0);
            b.set(Key::alert_level, Value{ vis ? 2 : std::max(0, alert - 1) });
        });
    }, world);

    // Thinkers: read the stable previous version all frame (declared before the
    // flip -> derived read->write edge). Every agent sees the same facts --
    // frame-coherent decisions, one frame of stimulus latency by design.
    g.add_node(ts::Named{}, [](const Blackboard& b, Agents& ag)
    {
        ag.observe(b.get<bool>(Key::player_visible, false));
    }, bb.state(), agents);

    auto flip = g.add_node(ts::Named{}, ts::publish_body(bb), bb.state());
    flip.after(sense);

    // Notification: after the flip, diff-and-fire off the fresh version.
    auto notify = g.add_node(ts::Named{}, [](const Blackboard& b, Subscriptions& s)
    {
        s.dispatch(b);
    }, bb.state(), subs);
    notify.after(flip);

    g.compile();
    for (int f = 0; f < frames; ++f)
        g.execute().sync();

    // The callbacks' async writes were enqueued during the runs - pipe FIFO
    // applies them before these reads.
    Blackboard_stats st;
    st.notifications = subs.access([](const Subscriptions& s) { return s.fired(); }).sync();
    st.barks = audio.access([](const Audio& a) { return a.barks(); }).sync();
    st.order_changes = squad.access([](const Squad& sq) { return sq.order_changes(); }).sync();
    st.agent_seen = agents.access([](const Agents& a) { return a.seen(); }).sync();
    st.final_alert = bb.read([](const Blackboard& b) { return b.get<int>(Key::alert_level, 0); }).sync();
    return st;
}

} // namespace

void run_blackboard_sample()
{
    constexpr int frames = 120;
    Blackboard_stats a = run_blackboard_frames(frames);
    Blackboard_stats b = run_blackboard_frames(frames);

    std::printf("blackboard sample: %d frames, %d notifications, %d barks, %d order changes, "
                "%d agent sightings, final alert %d, %s\n",
        frames, a.notifications, a.barks, a.order_changes, a.agent_seen, a.final_alert,
        a == b ? "deterministic across runs" : "NON-DETERMINISTIC (bug)");
}

} // namespace sample
