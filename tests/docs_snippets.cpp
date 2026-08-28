// Compile-only mirrors of the code blocks in README.md and docs/quickstart.md: if a public
// doc's snippet stops compiling against the real headers, this TU breaks the build - in
// every CI job - instead of a reader's first copy-paste. Nothing here runs; the functions
// are never called (external linkage, so no unused-function warnings). Snippets are kept
// textually close to the docs; adaptations (stub types, a parameter instead of a global)
// are marked. When a doc snippet changes, change its mirror here in the same commit.
#include "ts/ts.h"

#include <cstdio>
#include <vector>

namespace docs_snippets
{

// Stub types standing in for the docs' illustrative ones.
struct Item {};
using Item_id = int;
struct Inventory
{
    void add(Item) {}
    void add(Item_id) {}
    int count() const { return 0; }
    void recompute_weight() {}
};
struct Physics { void step() {} };
struct Animation { void pose(const Physics&) {} };
struct Audio { void mix(const Physics&) {} };
struct Renderer { void submit(const Physics&, const Animation&) {} };

// README "Why controlled access" + example 1 (access verbs, async).
void readme_access_example()
{
    Item sword;
    ts::Guarded<Inventory> inventory{ ts::Named{ "inventory" } };
    inventory.access([](Inventory& inv) { inv.add(Item{}); }).sync();
    auto n = inventory.access([](const Inventory& inv) { return inv.count(); }).sync();
    (void)n;
    (void)sword;
    inventory.async([](Inventory& inv) { inv.recompute_weight(); });
}

// README example 1, the coroutine form (adapted: the guarded object is a parameter, not a
// global).
ts::Task<void> loot(ts::Guarded<Inventory>& guarded_inventory, Item_id id)
{
    auto inventory = co_await ts::read_write(guarded_inventory);
    inventory->add(id);
    inventory->recompute_weight();
}

// README example 2: the frame graph (built and compiled; execution shape compiles too).
void readme_graph_example(bool quit)
{
    ts::Guarded<Physics> physics{ ts::Named{ "physics" } };
    ts::Guarded<Animation> anim{ ts::Named{ "anim" } };
    ts::Guarded<Audio> audio{ ts::Named{ "audio" } };
    ts::Guarded<Renderer> renderer{ ts::Named{ "renderer" } };

    ts::Static_task_graph frame;

    frame.add_node("step", [](Physics& p) { p.step(); }, physics);
    frame.add_node("pose", [](const Physics& p, Animation& a) { a.pose(p); }, physics, anim);
    auto sfx = frame.add_node("mix", [](const Physics& p, Audio& s) { s.mix(p); }, physics, audio);

    auto render = frame.add_node("render",
        [](const Physics& p, const Animation& a, Renderer& r) { r.submit(p, a); },
        physics, anim, renderer);

    render.after(sfx);

    frame.compile();

    while (!quit)
        frame.execute().sync();
}

// quickstart "Hello, task" (adapted: a function, not main; the scheduler comes up and goes
// down so the snippet is self-contained).
void quickstart_hello_task()
{
    ts::create_scheduler();
    ts::Task<int> t = ts::launch([] { return 6 * 7; });
    std::printf("%d\n", t.sync());
    ts::destroy_scheduler();
}

// quickstart "Guard a shared object".
void quickstart_guard_object()
{
    ts::Guarded<std::vector<int>> numbers{ ts::Named{ "numbers" } };

    numbers.access([](std::vector<int>& v) { v.push_back(1); }).sync();

    size_t n = numbers.access([](const std::vector<int>& v)
    {
        return v.size();
    }).sync();

    std::printf("%zu\n", n);
}

// quickstart "Make it safe by construction".
class Checked_inventory
{
public:
    void add(Item i) { TS_CHECK_ACCESS(); items_.push_back(i); }
    size_t count() const { TS_CHECK_ACCESS(); return items_.size(); }
private:
    std::vector<Item> items_;
};

void quickstart_checked_type()
{
    ts::Guarded<Checked_inventory> inventory{ ts::Named{ "inventory" } };
    inventory.access([](Checked_inventory& inv) { inv.add(Item{}); }).sync();
}

} // namespace docs_snippets
