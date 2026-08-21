# Macrame
###### *Threads, knotted on purpose.*

Macrame is a C++23 high-level parallelisation framework built around controlled access to shared resources.

It's a comprehensive task system with a work-stealing scheduler, dependency graphs, coroutines, cancellation, and data-parallel loops. But most importantly the resources each task touches are declared explicitly, with the framework deriving safe parallelism from the declarations with a runtime harness catching any violations.

Inspired by game engines that need high performance, low latency, soft real-time frame budgets, and many interacting subsystems sharing state. Built to fit any system with similar requirements. No external dependencies.

#### How it differs from existing task systems

- Combining `Guarded<T>` with the runtime harness provides a thread-safe API for shared objects. Safety is actively checked at runtime rather than relying on convention.
- It is a fully featured task system rather than a thin wrapper, including an efficient work-stealing scheduler, coroutine-based composition, cooperative cancellation, task priorities, graph-internal inline dispatch, and data-parallel loops.
- Common state-sharing idioms are provided as built-in primitives. These include static task graphs, command lists (`Deferred`), and double buffering (`Versioned`).
- The runtime harness is designed to catch issues early. Data access violations fail fast, and deadlocks are detected at runtime.

For a feature-by-feature comparison with Unreal Engine Tasks System, Taskflow, TBB, HPX, Folly, Go, and others, see [docs/task-systems-comparison.md](docs/task-systems-comparison.md).

[![CI](https://github.com/macrame-ts/macrame/actions/workflows/ci.yml/badge.svg)](https://github.com/macrame-ts/macrame/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)

**[Quick start](docs/quickstart.md)**

---

## Applications

This framework targets soft real-time systems with **complex concurrency**:

- Multiple **separate internal parts** — subsystems, "data islands" — each thread-unsafe on its own (think physics simulation, animation, audio, AI; or a server's services, a simulation's modules).
- **Intensive communication and data sharing** between those parts.
- A need to give each part a **thread-safe API** so the rest of the system can drive it concurrently and correctly.

You can build this by hand with mutexes and an existing task library, but it is hard to get right and hard to keep right. Macrame's goal is to make it safe and straightforward: to remove a large, recurring class of data-race and ordering bugs, and the effort of policing them.

**High-level parallelism.** Express parallelism between coarse parts, not deep inside every algorithm. Once there is enough of it to keep all CPU cores busy, low-level parallelisation stops paying off — and less low-level multithreading means less complexity.

**Probably not for you** if your problem is a single tight data-parallel kernel, one shared container, or fully independent jobs with no shared state — `std::for_each(par)` will serve you better and simpler.

---

## Why controlled access

Most task systems schedule *work* and leave *shared-data safety* to you — or offer only **task dependencies** to manage it. Both common answers have problems:

- **Locking** (mutexes around shared state) is the simplest and most popular approach, and it is **error-prone and inefficient**: a forgotten or mis-scoped lock races silently; correct locks serialise access and convoy under contention; correctness lives in scattered conventions that no tool verifies. Many issues are timing-dependent and appear only outside the dev machine.
- **Task dependencies alone are the wrong tool for data sharing.** Dependencies exist to express *ordering*. Retrofitting them to protect shared data means hand-encoding, for every pair of tasks, which access conflicts with which — O(N²) bookkeeping that yields **rigid, over-serialised graphs** and breaks the moment access patterns change. Ordering and data-access governance are different concerns; conflating them is where these systems get brittle.

This framework separates them. You **declare the data your logic accesses and how** — read-only or read/write — and the framework **derives** the ordering and exclusion needed for safety. A **runtime harness** then verifies, as work runs, that nothing touched shared state it did not declare, aborting with a stack trace instead of racing. Dependencies remain available for genuine ordering; they are no longer overloaded to mean "these tasks share data."

```cpp
ts::Guarded<Inventory> inventory{ "inventory" };
// The whole model at a glance: the parameter's const-ness is the access declaration.
inventory.access([](Inventory& inv) { inv.add(sword); }).sync(); // T& -> read/write access, exclusive
auto n = inventory.access([](const Inventory& inv) { return inv.count(); }).sync(); // const T& -> read-only, concurrent with other readers
```

---

## Examples

A few examples.

### 1. A thread-safe API for one object

Wrap a thread-unsafe object in `Guarded<T>` — you hand it a functor, and the parameter's const-ness declares your access (`T&` = write, exclusive; `const T&` = read, concurrent with other readers):

```cpp
ts::Guarded<Inventory> inventory{ "inventory" };

// `access` - runs on the calling thread if the object is free right now, otherwise waits
// its turn. Zero-allocation; best for the short functors typical of this API:
inventory.access([](Inventory& inv) { inv.add(sword); }).sync(); // write
auto n = inventory.access([](const Inventory& inv) { return inv.count(); }).sync(); // read

// `async` - always scheduled off the calling thread, returns a Task you can await, sync,
// or drop (fire-and-forget). For a heavy functor you don't want running inline
// (it would block the caller and hold the object longer):
inventory.async([](Inventory& inv) { inv.defragment(); });
```

`access` is the *attended* verb: the caller stays for the result (`.sync()` from regular code, `co_await` from a coroutine). The whole operation lives in the returned caller-owned handle, so it **allocates nothing**. It is also *opportunistic* — when the object is uncontended it skips scheduling and runs the functor inline, at a cost comparable to an uncontended mutex lock. That can briefly block the caller, which is the right trade for a short critical section. `async` is the *detached* verb: always scheduled, never blocks the caller, for expensive work.

With coroutines, the same access reads as ordinary linear code — `co_await` suspends until access is granted, then hands you an RAII guard with direct, harness-checked access:

```cpp
ts::Task<void> loot(ItemId id)
{
    auto inventory = co_await ts::read_write(guarded_inventory); // suspend until exclusive access; no thread blocked
    inventory->add(id); // direct Inventory& access, harness-checked
    inventory->recompute_weight();
} // access released at scope exit
```

You write no locks. Concurrent writes serialise, reads run together, and any code that touches the inventory without a grant faults. The check is one macro line — `TS_CHECK_ACCESS()` at the top of each `Inventory` method, ~1 ns per call.

### 2. A game frame as a graph — edges derived from access

Each subsystem is its own `Guarded<T>`. Declare what each node reads and writes; `compile()` derives the schedule from the access conflicts — you add explicit ordering only for "business logic", not to avoid data races:

```cpp
ts::Guarded<Physics> physics{ "physics" };
ts::Guarded<Animation> anim{ "anim" };
ts::Guarded<Audio> audio{ "audio" };
ts::Guarded<Renderer> renderer{ "renderer" };

ts::Static_task_graph frame;

// A node declares, per argument, read (const T&) or write (T&) access to each Guarded it
// touches - and may touch several at once. The leading name is what diagnostics, graph
// dumps and traces print.
frame.add_node("step", [](Physics& p) { p.step(); }, physics);
frame.add_node("pose", [](const Physics& p, Animation& a) { a.pose(p); }, physics, anim);
auto sfx = frame.add_node("mix", [](const Physics& p, Audio& s) { s.mix(p); }, physics, audio);

// Reads physics and anim, writes renderer:
auto render = frame.add_node("render",
    [](const Physics& p, const Animation& a, Renderer& r) { r.submit(p, a); },
    physics, anim, renderer);

render.after(sfx); // explicit ordering, for intent that access alone doesn't capture

frame.compile(); // edges = access conflicts + explicit after()/before()

// Compile once, execute many: a run reuses the compiled nodes and allocates only its
// completion handle - no per-node allocation.
for (int f = 0; f < frame_count; ++f)
    frame.execute().sync();
```

The two physics readers (`pose`, `mix`) run in parallel. `render` waits for both its inputs and for `sfx`. Change a node's access and the schedule changes with it — you never hand-maintain the derived edges.

### 3. Many producers, one atomic apply — `Deferred<T>`

`Deferred<T>` implements the **command list** pattern (familiar from game engines): instead of mutating shared state directly, each producer *records* its intended changes into a buffer, and a single later step applies them all at once. Recording takes no access to the target — so producers neither block each other nor block anyone reading it:

```cpp
ts::Guarded<World> world{ "world" };
ts::Deferred<World> staged{ world };

// Producers record changes into the buffer - no access taken on `world`, so they run in
// parallel and never hold up its readers. Each producer mints its own recorder:
ts::Recorder<World> rec = staged.recorder();
rec.stage([e](World& w) { w.apply_damage(e); }); // recorded, not applied yet

// Meanwhile other work reads `world` freely and concurrently - recording holds nothing:
auto hp = world.async([](const World& w) { return w.health_of(player); }); // a Task

// At a chosen point, the whole batch applies as one write, in a deterministic order:
staged.commit().sync();
```

### 4. Stable reads while the next version is built — `Versioned<T>`

`Versioned<T>` implements the **double buffer** pattern: keep two copies of the state — the *published* one that readers see, and a *next* one being prepared — and swap them at a defined point. Readers get a stable, consistent view for the whole frame; producers build the next version without ever holding readers up:

```cpp
ts::Versioned<Poses> poses{ "poses" };

// Producers build the next version, all frame long, without blocking readers:
ts::Recorder<Poses> rec = poses.recorder();
rec.stage([id, xf](Poses& p) { p.set(id, xf); });

// Readers see the last published version, all frame - stable and never contended:
auto n = poses.read([](const Poses& p) { return p.count(); }).sync();

// Once per frame, publish: the next version becomes the one readers see. Deterministic by
// construction - independent runs are bit-identical:
poses.publish().sync();
```

### 5. Seeing the frame — the built-in trace

The graph profiles itself: `--trace` runs the bundled ~30-system mock game frame and renders the **average** run — bars packed into concurrency rows, the measured critical path, dead-time bands. <!-- NOTE: the interactive link below is the GitHub Pages URL (Pages serving /docs from
     master) - dead until Pages is enabled at the flip (docs/going-public.md). -->
The SVG is interactive — hover tooltips with per-node stats and access declarations, view-toggle buttons — but GitHub strips scripts from images, so the picture below is static. [Open the interactive version](https://macrame-ts.github.io/macrame/media/game_frame_trace.svg) in a browser (or, in a clone, open [the file](docs/media/game_frame_trace.svg) itself). Note that it's not a single frame but all frames aggregated, showing the entire session in one high-level picture.

![Traced game frame](docs/media/game_frame_trace.svg)

---

## What's in the box

Layered and composable — use as much as you need. From the bottom up:

- **Scheduler** — efficient work-stealing, configurable idle policies, priorities. Minimal API; usable independently of the rest.
- **Tasks** — `launch` work and compose it with coroutines: `co_await` is the one mechanism for continuation, join, and dependency, so pipelines read as straight-line code with typed results in scope. Awaiting frees the worker, so fork-join of any depth can't deadlock the pool. Also cooperative cancellation (including mid-body early-out) and priorities. The harness catches an in-task blocking wait, and a waits-for cycle detector catches the suspended two-object deadlock — naming both the tasks and the objects.
- **`parallel_for`** — for the data-parallel work that does live inside a part; caller-participating (nested-safe) and self-balancing. Plus **`async_parallel_for`** and **`parallel_for_colored`** for extra flexibility.
- **Coroutines** — `co_await` any task; `co_await ts::read_only/read_write(obj)` yields an RAII access guard; holding one across a suspension is detected and fails fast.
- **`Guarded<T>`** — a thread-safe API for a shared object: a per-object reader/writer queue (concurrent reads, exclusive writes, FIFO) reached via `access` (attended and allocation-free — runs inline when free) or `async` (always scheduled), plus multi-object operations with deadlock-free ordered acquisition.
- **`Static_task_graph`** — build-once/run-many DAG whose edges are derived from access conflicts (plus explicit ordering where you want it); a re-run reuses the compiled nodes and allocates only its completion handle. Profiling and visualisation are included, focused on parallelism metrics like "dead time", "critical path", and core utilisation — see the worked profiler-guided optimisation exercise in [docs/example-frame-optimization.md](docs/example-frame-optimization.md).
- **Design patterns** — `Deferred<T>` / `Versioned<T>` — staged writes: record grant-free from any thread, apply the batch atomically at a defined point; `Versioned` gives readers a whole-frame stable snapshot. Deterministic by construction. Plus `Event_bus`, a lightweight pub/sub built on the same staging machinery.

**v0.1.0** (unreleased) — pre-1.0: the API is stable in shape but not frozen. See [CHANGELOG.md](CHANGELOG.md) for what 0.1.0 contains. Some areas are actively evolving (**WIP**): the allocation/performance campaign, a platform abstraction layer, and benchmark regression tracking. See [docs/roadmap.md](docs/roadmap.md) for where it's going.

---

## Verification

The concurrency behaviour is checked several ways:

- A **comprehensive test suite**, plus subprocess **death tests** for every fatal path.
- **ThreadSanitizer** (Linux/clang) and **AddressSanitizer** kept clean.
- **Deterministic end-to-end samples** that hash-compare independent runs — a ~30-system mock engine frame, and a physics machine/extract fixture.
- A runtime harness that reports races and deadlocks with a stack trace, not a silent corruption.

---

## Requirements & building

C++23, no external dependencies, exceptions disabled project-wide (failures are fatal-by-design — see the docs). Compilers: MSVC and clang-cl on Windows; clang on Linux.

- **Visual Studio 2022+**: open `macrame.slnx` (x64).
- **CMake**: presets for `windows-msvc`, `windows-clang-cl`, `windows-shipping`, `linux-clang`, and `linux-tsan` (the ThreadSanitizer stress driver).

Running the built driver with no arguments runs everything; `--tests`, `--bench`, and `--stress` isolate parts (`--help` lists the rest).

---

## Documentation

- **[docs/quickstart.md](docs/quickstart.md)** — zero to a running program: include, build, first task, first guarded object.
- **[docs/guide.md](docs/guide.md)** — the user guide: concepts, every layer with examples, patterns, when-to-use-what.
- **[docs/design.md](docs/design.md)** — design rationale: the decisions, the rejected alternatives, the references.
- **[docs/task-systems-comparison.md](docs/task-systems-comparison.md)** — how it compares to other task systems.
- **[docs/roadmap.md](docs/roadmap.md)** — where the library is going, by theme and horizon.
- Deep dives: [task-internals](docs/task-internals.md), [command-buffer-design](docs/command-buffer-design.md), [deferred-versioned-state](docs/deferred-versioned-state.md).

---

## Contributing

Contributions of all sizes are welcome — bug reports, docs, tests, code. See [CONTRIBUTING.md](CONTRIBUTING.md).

---

## License

MIT.
