#include "event_bus_tests.h"
#include "ts/event_bus.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"
#include "harness.h"

#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

using ts::test::run;

namespace
{

struct Ping
{
    int n = 0;
};

struct Pong
{
    int n = 0;
};

// --- compile-time contract --------------------------------------------------

static_assert(!std::is_copy_constructible_v<ts::Event_bus>);
static_assert(!std::is_copy_constructible_v<ts::Event_bus::Connection>);
static_assert(std::is_move_constructible_v<ts::Event_bus::Connection>);

// The graph-free delivery point used throughout: one write access.
void dispatch(ts::Event_bus& bus)
{
    bus.state().access(bus.dispatch_fn()).sync();
}

void test_publish_then_dispatch_delivers()
{
    ts::Event_bus bus{ ts::Named{} };
    int sum = 0;
    auto c = bus.subscribe([&sum](const Ping& p) { sum += p.n; });

    bus.publish(Ping{ 1 });
    bus.publish(Ping{ 2 });
    TS_CHECK(sum == 0);          // deferred: nothing delivered before the dispatch
    dispatch(bus);
    TS_CHECK(sum == 3);
    dispatch(bus);
    TS_CHECK(sum == 3);          // lanes cleared: no redelivery
}

void test_subscribe_then_publish_same_cut()
{
    ts::Event_bus bus{ ts::Named{} };
    int seen = 0;
    // Program order: the staged install cannot land in a later cut than the
    // publish staged after it -- first dispatch must deliver.
    auto c = bus.subscribe([&seen](const Ping&) { ++seen; });
    bus.publish(Ping{ 7 });
    dispatch(bus);
    TS_CHECK(seen == 1);
}

void test_multiple_types_and_subscribers()
{
    ts::Event_bus bus{ ts::Named{} };
    int pings = 0, pongs = 0, both = 0;
    auto c1 = bus.subscribe([&pings](const Ping&) { ++pings; });
    auto c2 = bus.subscribe([&pongs](const Pong&) { ++pongs; });
    auto c3 = bus.subscribe([&both](const Ping&) { ++both; });
    auto c4 = bus.subscribe([&both](const Pong&) { ++both; });

    bus.publish(Ping{});
    bus.publish(Pong{});
    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(pings == 2);
    TS_CHECK(pongs == 1);
    TS_CHECK(both == 3);
}

void test_publish_and_subscribe_from_task()
{
    ts::Event_bus bus{ ts::Named{} };
    int seen = 0;
    ts::Event_bus::Connection conn;
    // Both verbs are grant-free staging -- legal from inside a task, no join.
    ts::launch([&bus, &conn, &seen]
    {
        conn = bus.subscribe([&seen](const Ping& p) { seen += p.n; });
        bus.publish(Ping{ 5 });
    }).sync();
    dispatch(bus);
    TS_CHECK(seen == 5);
}

void test_unpinned_disconnect_stops_delivery()
{
    ts::Event_bus bus{ ts::Named{} };
    int seen = 0;
    auto c = bus.subscribe([&seen](const Ping&) { ++seen; });
    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(seen == 1);

    c.disconnect();
    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(seen == 1);         // removed: later batches skip it
}

void test_disconnect_races_pending_install()
{
    ts::Event_bus bus{ ts::Named{} };
    int seen = 0;
    {
        // Destroyed before any dispatch: the tombstone kills the still-staged
        // install at apply -- no zombie entry, no delivery.
        auto c = bus.subscribe([&seen](const Ping&) { ++seen; });
    }
    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(seen == 0);
}

void test_pinned_auto_disconnect()
{
    struct Listener
    {
        int seen = 0;
        void on_ping(const Ping&) { ++seen; }
    };

    ts::Event_bus bus{ ts::Named{} };
    auto owner = std::make_shared<Listener>();
    auto c = bus.subscribe(owner, &Listener::on_ping);   // pinned, member-fn form

    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(owner->seen == 1);

    owner.reset();               // no unsubscribe call anywhere
    bus.publish(Ping{});
    dispatch(bus);               // weak lock fails: skipped and reaped, no UAF
    // Connection dtor at scope end removes a reaped id -- a no-op.
}

void test_pinned_self_lambda_form()
{
    struct Listener
    {
        int total = 0;
    };

    ts::Event_bus bus{ ts::Named{} };
    auto owner = std::make_shared<Listener>();
    auto c = bus.subscribe(owner, [](Listener& self, const Ping& p) { self.total += p.n; });

    bus.publish(Ping{ 4 });
    bus.publish(Ping{ 6 });
    dispatch(bus);
    TS_CHECK(owner->total == 10);
}

void test_connection_move()
{
    ts::Event_bus bus{ ts::Named{} };
    int seen = 0;
    auto c1 = bus.subscribe([&seen](const Ping&) { ++seen; });
    ts::Event_bus::Connection c2 = std::move(c1);   // c1 empty; its dtor must be harmless

    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(seen == 1);

    c2.disconnect();
    bus.publish(Ping{});
    dispatch(bus);
    TS_CHECK(seen == 1);
}

void test_dispatch_as_graph_node()
{
    ts::Event_bus bus{ ts::Named{} };
    ts::Guarded<int> ticks{ ts::Named{} };
    int seen = 0;
    auto c = bus.subscribe([&seen](const Ping& p) { seen += p.n; });

    ts::Static_task_graph g;
    auto produce = g.add_node(ts::Named{}, [&bus](int& n) { bus.publish(Ping{ ++n }); }, ticks);
    auto deliver = g.add_node(ts::Named{}, bus.dispatch_fn(), bus.state());
    deliver.after(produce);      // staging derives no edges: explicit intent
    g.compile();

    for (int i = 0; i < 3; ++i)
        g.execute().sync();
    TS_CHECK(seen == 1 + 2 + 3);
}

} // namespace

void run_event_bus_tests()
{
    std::printf("\n[event_bus] tests\n");
    run("publish then dispatch delivers", test_publish_then_dispatch_delivers);
    run("subscribe then publish, same cut", test_subscribe_then_publish_same_cut);
    run("multiple types and subscribers", test_multiple_types_and_subscribers);
    run("publish + subscribe from a task", test_publish_and_subscribe_from_task);
    run("unpinned disconnect stops delivery", test_unpinned_disconnect_stops_delivery);
    run("disconnect races pending install", test_disconnect_races_pending_install);
    run("pinned auto-disconnect", test_pinned_auto_disconnect);
    run("pinned self-lambda form", test_pinned_self_lambda_form);
    run("connection move", test_connection_move);
    run("dispatch as graph node", test_dispatch_as_graph_node);
}
