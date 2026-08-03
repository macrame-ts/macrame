<!-- Project name undecided (placeholder "task_system"); see docs/naming.md. -->

# task_system

**A C++23 parallelisation framework built around controlled access to shared resources.**

A comprehensive task system — work-stealing scheduler, dependency graphs, coroutines, cancellation, data-parallel loops — with one thing no other C++ task system has at its centre: **you declare which shared resources each piece of work touches, and the framework derives safe parallelism from those declarations**, backed by a runtime harness that catches any violations.

Inspired by game engines — high performance, low latency, soft real-time frame budgets, many interacting subsystems sharing state — but built to fit any system with similar requirements. No external dependencies.

**New here? Start with the [Quick start](docs/quickstart.md).**

[![CI](https://github.com/Andriy06/task_system/actions/workflows/ci.yml/badge.svg)](https://github.com/Andriy06/task_system/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
<!-- NOTE: the CI badge URL embeds the owner/repo (Andriy06/task_system) -- refresh it if the repo is renamed or moved (see docs/going-public.md). -->

---

## Applications

This framework targets systems with **complex, coarse-grained concurrency**:

- Multiple **separate internal parts** — subsystems, "data islands" — each thread-unsafe on its own (think physics simulation, animation, audio, AI; or a server's services, a simulation's modules).
- **Intensive communication and data sharing** between those parts — not an embarrassingly-parallel loop over uniform data (though that kind of work *within* a part is supported too, via `parallel_for`).
- A need to give each part a **thread-safe API** so the rest of the system can drive it concurrently and correctly.
- Often **soft real-time**: a frame, a tick, a request budget — you want all cores saturated and latency bounded.

You can build this by hand with mutexes or an existing task library. Quite a daunting task even for a specialist. The goal here is to make it **safer and straightforward** — to remove a large, recurring class of data-race and ordering bugs, and the effort of policing them, from that work.

**The high-level-parallelism bet.** Express parallelism where the *surface area is smallest* — between coarse parts, not deep inside every algorithm. Once enough parallelism is expressed at that level to keep all CPU cores busy, low-level parallelisation stops paying off and you can skip it. Less low-level threading means far less complexity, less code to maintain, and fewer bugs. This framework is built to make high-level parallelism easy to express **and** safe.

**Probably not for you** if your problem is a single tight data-parallel kernel, one shared container, or fully independent jobs with no shared state — `std::for_each(par)` will serve you better and simpler.

---

## Why controlled access

Most task systems schedule *work* and leave *shared-data safety* to you — or offer only **task dependencies** to manage it. Both common answers have problems:

- **Locking** (mutexes around shared state) is the simplest and most popular approach, and it is **error-prone and inefficient**: a forgotten or mis-scoped lock races silently; correct locks serialise readers against writers and convoy under contention; correctness lives in scattered conventions that no tool verifies.
- **Task dependencies alone are the wrong tool for data sharing.** Dependencies exist to express *ordering*. Retrofitting them to protect shared data means hand-encoding, for every pair of tasks, which access conflicts with which — O(N²) bookkeeping that yields **rigid, over-serialised graphs** and breaks the moment access patterns change. Ordering and data-access governance are different concerns; conflating them is where these systems get brittle.

This framework separates them. You **declare the data each task accesses and how** — read-only or read/write — and the framework **derives** the ordering and exclusion needed for safety. A **runtime harness** then verifies, as work runs, that nothing touched shared state it did not declare, aborting with a stack trace instead of racing. Dependencies remain available for genuine ordering; they are no longer overloaded to mean "these tasks share data."

```cpp
// The whole model in one line: the parameter's const-ness IS the access declaration.
inventory.access([](Inventory& inv) { inv.add(sword); }); // T& -> read/write access, exclusive
inventory.access([](const Inventory& inv) { return inv.count(); }); // const T& -> read-only access, concurrent with other readers
```

Inspired by Rust, adapted to C++. 

---

## How it differs from existing task systems

- **Access orchestration is a first-class, novel feature.** `Guarded<T>` + the runtime harness give you a thread-safe API for a shared object with the safety *checked*, not merely conventional. To our knowledge no other C++ task system derives scheduling from declared data access and polices it at runtime.
- **It is also a comprehensive, state-of-the-art task system**, not a thin wrapper: efficient work-stealing scheduler, typed continuations and joins, cooperative cancellation, reusable, nested and inline tasks, priorities, coroutine integration, and data-parallel loops.
- **High-level patterns, built in** — command buffers (`Deferred`) and double buffering (`Versioned`): the state-sharing idioms every game engine reinvents, provided as primitives.
- **Some functionality is uncommon or unique**: **retraction** (a blocking wait runs not-yet-started work inline, so fork-join can't deadlock the pool); scheduler's configurable idle policies; object hand-off between graph nodes; and a `co_await`-a-guard model where holding an access guard across a suspension is *detected and fails fast*.

For a feature-by-feature comparison with Unreal Engine Tasks System, Taskflow, TBB, HPX, Folly, Go, and others, see [docs/task-systems-comparison.md](docs/task-systems-comparison.md).

---

## Examples

Just a glimpse at how it looks in practice.

### 1. A thread-safe API for one object

Wrap a thread-unsafe object in `Guarded<T>`; you never get a bare `T&` — you hand it a functor, and the parameter's const-ness declares your access (`T&` = write, exclusive; `const T&` = read, concurrent with other reads). Two verbs run that functor:

```cpp
ts::Guarded<Inventory> inventory;

// access -- runs on the calling thread if the object is free right now, otherwise it is
// scheduled. The no-lock fast path; best for the short functors typical of this API:
inventory.access([](Inventory& inv) { inv.add(sword); });                     // write
auto n = inventory.access([](const Inventory& inv) { return inv.count(); });  // read

// async -- always scheduled off the calling thread. For a heavy functor you don't want
// running inline (it would block the caller and hold the object longer):
inventory.async([](Inventory& inv) { inv.defragment(); });
```

`access` is *opportunistic*: when the object is uncontended it skips scheduling entirely and runs the functor right there — so it may briefly block the caller, which is the right trade for a short critical section. `async` is the explicit "not on my thread" form for expensive work. Both return a `ts::Task<R>`; both declare the same read/write access.

With coroutines, the same access reads as ordinary linear code — `co_await` suspends until access is granted, then hands you an RAII guard with direct, harness-checked access:

```cpp
ts::Task<void> loot(ItemId id)
{
    auto inv = co_await ts::read_write(inventory);   // suspend until EXCLUSIVE access; no thread blocked
    inv->add(id);   // direct Inventory& access, harness-checked
    inv->recompute_weight();
}   // access released at scope exit
```

No lock is written, taken, or forgotten; concurrent writes serialise, reads run together, and any code that touches the inventory without a grant faults.

### 2. A frame as a graph — edges derived from access

Each subsystem is its own `Guarded<T>`. Declare what each node reads and writes; `compile()` derives the schedule from the access conflicts — you add explicit ordering only for intent that data access alone doesn't capture:

```cpp
ts::Guarded<Physics> physics;
ts::Guarded<Animation> anim;
ts::Guarded<Audio> audio;
ts::Guarded<Renderer> renderer;

ts::Static_task_graph frame;

// A node declares, per argument, read (const T&) or write (T&) access to each Guarded it
// touches -- and may touch several at once.
frame.add_node([](Physics& p) { p.step(); }, physics);
frame.add_node([](const Physics& p, Animation& a) { a.pose(p); }, physics, anim);
auto sfx = frame.add_node([](const Physics& p, Audio& s) { s.mix(p); }, physics, audio);

// Reads physics AND anim, writes renderer:
auto render = frame.add_node(
    [](const Physics& p, const Animation& a, Renderer& r) { r.submit(p, a); },
    physics, anim, renderer);

render.after(sfx);   // explicit ordering, for intent that access alone doesn't capture

frame.compile();   // edges = access conflicts + explicit after()/before()

// Compile once, execute many: a run reuses the compiled nodes and allocates only its
// completion handle -- no per-node allocation.
for (int f = 0; f < frame_count; ++f)
    frame.execute().sync();
```

The two physics readers (`pose`, `mix`) run in parallel, both after `step` writes physics; `render` waits for both its inputs and for `sfx`. Change a node's access and the schedule changes with it — you never hand-maintain the derived edges.

### 3. Many producers, one atomic apply — `Deferred<T>`

`Deferred<T>` implements the **command buffer** pattern (familiar from game engines): instead of mutating shared state directly, each producer *records* its intended changes into a buffer, and a single later step applies them all at once. Recording takes no access to the target — so producers neither block each other nor block anyone reading it:

```cpp
ts::Guarded<World> world;
ts::Deferred<World> staged{ world };

// Producers record changes into the buffer -- no access taken on `world`, so they run in
// parallel and never hold up its readers. Each producer mints its own recorder:
ts::Recorder<World> rec = staged.recorder();
rec.stage([e](World& w) { w.apply_damage(e); });   // recorded, not applied yet

// Meanwhile other work reads `world` freely and concurrently -- recording holds nothing:
auto hp = world.async([](const World& w) { return w.health_of(player); });

// At a chosen point, the whole batch applies as one write, in a deterministic order:
staged.commit().sync();
```

### 4. Stable reads while the next version is built — `Versioned<T>`

`Versioned<T>` implements the **double buffer** pattern: keep two copies of the state — the *published* one that readers see, and a *next* one being prepared — and swap them at a defined point. Readers get a stable, consistent view for the whole frame; producers build the next version without ever holding readers up:

```cpp
ts::Versioned<Poses> poses;

// Producers build the NEXT version, all frame long, without taking access from readers:
ts::Recorder<Poses> rec = poses.recorder();
rec.stage([id, xf](Poses& p) { p.set(id, xf); });

// Readers see the LAST published version, all frame -- stable and never contended:
auto n = poses.read([](const Poses& p) { return p.count(); });

// Once per frame, publish: the next version becomes the one readers see. Deterministic by
// construction -- independent runs are bit-identical:
poses.publish().sync();
```

---

## What's in the box

Layered and composable — use as much as you need, and in a way that suits you best. Going from bottom-up:

- **Scheduler** — efficient work-stealing, configurable idle policies, priorities. Minimal API; usable independently of the rest.
- **Tasks** — `launch` work and compose it as coroutines: `co_await` is the one continuation/join/dependency mechanism, so pipelines read as straight-line code with typed results in scope. Cooperative cancellation (incl. mid-body early-out). Nested tasks and task scopes gate a parent on dynamic fan-out. Priorities. Awaiting frees the worker, so fork-join of any depth can't deadlock the pool; an in-task blocking wait is caught by the safety harness, and so is the suspended two-object deadlock (a waits-for cycle detector names both tasks and objects).
- **`parallel_for`** — for the data-parallel work that does live inside a part; caller-participating (nested-safe), with guided/balanced/unbalanced chunking. plus **`async_parallel_for`** for extra flexibility.
- **Coroutines** — `co_await` any task; `co_await ts::read_only/read_write(obj)` yields an RAII access guard; holding one across a suspension is detected and fails fast.
- **`Guarded<T>`** — a thread-safe API for a shared object: a per-object reader/writer queue (concurrent reads, exclusive writes, FIFO) reached via `access` (opportunistic — runs inline when free) or `async` (always scheduled), plus multi-object operations with deadlock-free ordered acquisition.
- **`Static_task_graph`** — build-once/run-many DAG whose edges are derived from access conflicts (plus explicit ordering where you want it); a re-run reuses the compiled nodes and allocates only its completion handle. Planned profiler-guided optimisation.
- **Design patterns** — `Deferred<T>` / `Versioned<T>` — staged writes: record grant-free from any thread, apply the batch atomically at a defined point; `Versioned` gives readers a whole-frame stable snapshot. Deterministic by construction.

**v0.1.0** — pre-1.0: the API is stable in shape but not frozen. Some areas are actively evolving (**WIP**): the allocation/performance campaign, a platform abstraction layer, an ambient (overridable) scheduler, and benchmark regression tracking. See [docs/TODO.md](docs/TODO.md) for the live roadmap.

---

## Verification

Concurrency claims need evidence, not assertions:

- A **comprehensive test suite**, plus subprocess **death tests** for every fatal path.
- **ThreadSanitizer** (Linux/clang) and **AddressSanitizer** kept clean.
- **Deterministic end-to-end samples** that hash-compare independent runs — a 17-system mock engine frame, and a physics module/extract fixture.
- A **forensic harness** for the scheduler's hardest race class.

---

## Requirements & building

C++23, no external dependencies, exceptions disabled project-wide (failures are fatal-by-design — see the docs). Compilers: MSVC and clang-cl on Windows; clang on Linux.

- **Visual Studio 2022+**: open `task_system.slnx` (x64).
- **CMake**: presets for `windows-msvc`, `windows-clang-cl`, and `linux-clang` (the ThreadSanitizer stress driver builds on Linux).

The driver runs everything; `--tests`, `--bench`, `--stress`, and `--help` isolate parts.

---

## Documentation

- **[docs/quickstart.md](docs/quickstart.md)** — zero to a running program: include, build, first task, first guarded object.
- **[docs/guide.md](docs/guide.md)** — the user guide: concepts, every layer with examples, patterns, when-to-use-what.
- **[docs/design.md](docs/design.md)** — design rationale: the decisions, the rejected alternatives, the references.
- **[docs/task-systems-comparison.md](docs/task-systems-comparison.md)** — how it compares to other task systems.
- Deep dives: [task-internals](docs/task-internals.md), [command-buffer-design](docs/command-buffer-design.md), [deferred-versioned-state](docs/deferred-versioned-state.md).

---

## License

MIT.
