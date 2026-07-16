<!-- DRAFT README. Project name undecided (placeholder "task_system"); see naming.md.
     Lives in docs/ until the name lands, then moves to repo root as README.md. -->

# task_system

**A C++23 parallelisation framework built around controlled access to shared resources.**

A comprehensive task system — work-stealing scheduler, dependency graphs, coroutines, cancellation, data-parallel loops — with one thing no other C++ task system has at its centre: **you declare which shared resources each piece of work touches, and the framework derives safe parallelism from those declarations**, backed by a runtime harness that catches any violations.

Inspired by game engines — high performance, low latency, soft real-time frame budgets, many interacting subsystems sharing state — but built to fit any system with similar requirements. No external dependencies.

<!-- badges (CI/license) go here once the repo is public -->

---

## Applications

This framework targets systems with **complex, coarse-grained concurrency**:

- Multiple **separate internal parts** — subsystems, "data islands" — each thread-unsafe on its own (think physics simulation, animation, audio, AI; or a server's services, a simulation's modules).
- **Intensive communication and data sharing** between those parts — though an embarrassingly-parallel loop over uniform data is supported too.
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

This framework separates them. You **declare the data each task accesses and how** — read-only or read/write — and the framework **derives** the ordering and exclusion needed for safety. A **runtime harness** then verifies, as work runs, that nothing touched shared state it did not declare, aborting with a stack trace instead of racing. Dependencies remain available for genuine ordering; they are no longer overloaded to mean "these tasks share data.".

```cpp
// The whole model in one line: the parameter's const-ness IS the access declaration.
inventory.async([](Inventory& inv) { inv.add(sword); }); // T& -> read/write access, exclusive
inventory.async([](const Inventory& inv) { return inv.count(); }); // const T& -> read-only access, concurrent with other readers
```

Inspired by Rust, adapted to C++. 

---

## How it differs from existing task systems

- **Access orchestration is a first-class, novel feature.** `Guarded<T>` + the runtime harness give you a thread-safe API for a shared object with the safety *checked*, not merely conventional. To our knowledge no other C++ task system derives scheduling from declared data access and polices it at runtime.
- **It is also a comprehensive, state-of-the-art task system**, not a thin wrapper: efficient work-stealing scheduler, typed continuations and joins, cooperative cancellation, reusable, nested and inline tasks, priorities, coroutine integration, and data-parallel loops.
- **High-level design patterns** reinvented by every game engine. To streamline efficient inter-system collaboration.
- **Some functionality is uncommon or unique**: **retraction** (a blocking wait runs not-yet-started work inline, so fork-join can't deadlock the pool); scheduler's configurable idle policies; object hand-off between graph nodes; and a `co_await`-a-guard model where holding an access guard across a suspension is *detected and fails fast*.

For a feature-by-feature comparison with Unreal Engine Tasks System, Taskflow, TBB, HPX, Folly, Go, and others, see [docs/task-systems-comparison.md](task-systems-comparison.md).

---

## Examples

Just a glimpse at how it looks in practice.

### 1. A thread-safe API for one object — with coroutines

Wrap a thread-unsafe object in `Guarded<T>`; reach it by declaring access mode. With coroutines, that declaration reads as ordinary linear code — `co_await` suspends until access is granted, then hands you a guard with direct, harness-checked access:

```cpp
ts::Guarded<Inventory> inventory;

ts::Task<void> loot(ItemId id)
{
    auto inv = co_await ts::read_write(inventory);   // suspend until EXCLUSIVE access; no thread blocked
    inv->add(id);                               // direct Inventory& access, harness-checked
    inv->recompute_weight();
}                                               // access released at scope exit

ts::Task<int> total_items()
{
    const auto inv = co_await ts::read_only(inventory);    // SHARED read access — concurrent with other readers
    co_return inv->count();
}
```

No lock is written, taken, or forgotten; concurrent `loot` calls serialise, reads run together, and any code that touches the inventory without a grant faults. (The same is available without coroutines as `inventory.async(fn)`.)

### 2. A frame composition as a graph — edges derived from access

Each subsystem is its own `Guarded<T>`. Declare what each node reads and writes; `compile()` derives the schedule from the access conflicts — no manual dependency wiring for shared data:

```cpp
ts::Guarded<Physics>   physics;
ts::Guarded<Animation> anim;
ts::Guarded<Renderer>  renderer;

ts::Static_task_graph frame;
frame.add_node([](Physics& p)                     { p.step(); },     physics);
frame.add_node([](const Physics& p, Animation& a) { a.pose(p); },    physics, anim);
frame.add_node([](const Physics& p, Renderer& r)  { r.submit(p); },  physics, renderer);

frame.compile();          // pose() and submit() both READ physics -> they run in parallel,
frame.execute().sync();   // both after step() (the writer). Build once, run every frame.
```

Change a node's access and the schedule changes with it — you never hand-maintain the edges.

### 3. Many producers, one atomic apply — `Deferred<T>`

When many parts contribute writes to one target, serialising them through the pipe is wasteful. `Deferred<T>` lets each producer **record** changes with no access grant at all (so producers never contend), then applies the whole batch as a single write at a point you choose:

```cpp
ts::Guarded<World>  world;
ts::Deferred<World> staged{ world };

// Each producer system mints one recorder and stages grant-free, in parallel, contention-free:
ts::Recorder<World> rec = staged.recorder();
rec.stage([e](World& w) { w.apply_damage(e); });   // records into private storage; never blocks

// At a chosen point, one write applies everything atomically, in a deterministic order:
staged.commit_async().sync();
```

### 4. Stable reads while the next version is built — `Versioned<T>`

Read-heavy shared state (transforms, poses, a blackboard) contends badly on the pipe: every reader waits behind the writer. `Versioned<T>` keeps two replicas behind one guarded front — readers see the last **published** version for the whole frame while producers stage the next one; `publish()` flips atomically:

```cpp
ts::Versioned<Poses> poses;
ts::Recorder<Poses>  rec = poses.recorder();

// Producers write the NEXT version all frame, grant-free:
rec.stage([id, xf](Poses& p) { p.set(id, xf); });

// Readers see the LAST published version all frame — stable, zero reader/writer contention:
auto n = poses.read([](const Poses& p) { return p.count(); });

// Once per frame, flip. Deterministic by construction — independent runs are bit-identical:
poses.publish().sync();
```

---

## What's in the box

Layered and composable — use as much as you need, and in a way that suits you best. Going from bottom-up:

- **Scheduler** — efficient work-stealing, configurable idle policies, priorities. Minimal API, easy to replace or to use independently from the rest.
- **Tasks** — `launch` work with prerequisites (`after`), continuations (`then`), typed joins (`when_all`), cooperative cancellation (incl. mid-body early-out). Reusable (no allocs). Nested and inline tasks. Priorities. Blocking waits run not-yet-started work inline (retraction), so fork-join can't deadlock the pool while touching only related work and thus avoiding ubiquitous "busy waiting" issues.
- **`parallel_for`** — for the data-parallel work that does live inside a part; caller-participating (nested-safe), with guided/balanced/unbalanced chunking. plus **`async_parallel_for`** for extra flexibility.
- **Coroutines** — `co_await` any task; `co_await ts::read/write(obj)` yields an RAII access guard; holding one across a suspension is detected and fails fast.
- **`Guarded<T>`** — a thread-safe API for a shared object: a per-object reader/writer task pipe (concurrent reads, exclusive writes, FIFO, non-blocking submit), plus multi-object operations with deadlock-free ordered acquisition.
- **`Static_task_graph`** — build-once/run-many DAG whose edges are derived from access conflicts (plus explicit ordering where you want it); re-runs are allocation-free. Planned profiler-guided optimisation.
- **Design patterns** — `Deferred<T>` / `Versioned<T>` — staged writes: record grant-free from any thread, apply the batch atomically at a defined point; `Versioned` gives readers a whole-frame stable snapshot. Deterministic by construction.

Some areas are actively evolving (**WIP**): the allocation/performance campaign, a platform abstraction layer, an ambient (overridable) scheduler, and benchmark regression tracking. See [docs/TODO.md](TODO.md) for the live roadmap.

---

## Verification

Concurrency claims need evidence, not assertions:

- A **comprehensive test suite**, plus subprocess **death tests** for every fatal path.
- **ThreadSanitizer** (Linux/clang) and **AddressSanitizer** kept clean.
- **Deterministic end-to-end samples** that hash-compare independent runs — a 17-system mock engine frame, and a physics module/extract fixture.
- A **forensic harness** for the scheduler's hardest race class.

---

## Requirements & building

C++23, no external dependencies, exceptions disabled project-wide (failures are fatal-by-design) - gamedev influence, reach out if this is a problem. Compilers: MSVC and clang-cl on Windows; clang on Linux.

- **Visual Studio 2022+**: open `task_system.slnx` (x64).
- **CMake**: presets for `windows-msvc`, `windows-clang-cl`, and `linux-clang` (the ThreadSanitizer stress driver builds on Linux).

The driver runs everything; `--tests`, `--bench`, `--stress`, and `--help` isolate parts.

---

## Documentation

- **[docs/guide.md](guide.md)** — the user guide: concepts, every layer with examples, patterns, when-to-use-what.
- **[docs/design.md](design.md)** — design rationale: the decisions, the rejected alternatives, the references.
- **[docs/task-systems-comparison.md](task-systems-comparison.md)** — how it compares to other task systems.
- Deep dives: [task-internals](task-internals.md), [command-buffer-design](command-buffer-design.md), [deferred-versioned-state](deferred-versioned-state.md).

---

## License

MIT.
