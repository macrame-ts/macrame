<!-- Name decided: Macrame (see docs/naming.md). Project/file names stay "task_system" until the repo moves to macrame-ts/macrame - the rollout list is in docs/going-public.md. -->

# Macrame
###### *Threads, knotted on purpose.*

Macrame is a C++23 high-level parallelisation framework built around controlled access to shared resources.

Its a comprehensive task system with a work-stealing scheduler, dependency graphs, coroutines, cancellation, and data-parallel loops. But most importantly the resources each task touches are declared explicitly, with the framework deriving safe parallelism from the declarations with a runtime harness catching any violations.

Inspired by game engines that need high performance, low latency, soft real-time frame budgets, and many interacting subsystems sharing state. Built to fit any system with similar requirements. No external dependencies.

#### How it differs from existing task systems

- Combining `Guarded<T>` with the runtime harness provides a thread-safe API for shared objects. Safety is actively checked at runtime rather than relying on convention.
- It is a fully featured task system rather than a thin wrapper, including an efficient work-stealing scheduler, coroutine-based composition, cooperative cancellation, task priorities, graph-internal inline dispatch, and data-parallel loops.
- Common state-sharing idioms are provided as built-in primitives. These include static task graphs, command lists (`Deferred`), and double buffering (`Versioned`).
- The runtime harness is designed to catch issues early. Data access violations fail fast, and deadlocks are detected at runtime.

For a feature-by-feature comparison with Unreal Engine Tasks System, Taskflow, TBB, HPX, Folly, Go, and others, see [docs/task-systems-comparison.md](docs/task-systems-comparison.md).

[![CI](https://github.com/Andriy06/task_system/actions/workflows/ci.yml/badge.svg)](https://github.com/Andriy06/task_system/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
<!-- NOTE: the CI badge URL embeds the owner/repo (Andriy06/task_system) -- refresh it if the repo is renamed or moved (see docs/going-public.md). -->

[Quick start](docs/quickstart.md)

---

## Applications

This framework targets soft real-time systems that handle complex concurrency. It is well suited for applications with:

*   Multiple separate internal parts, such as subsystems or data islands, that are thread-unsafe on their own (examples include physics simulation, animation, audio, AI, or a server's background services).
*   Intensive communication and data sharing between those parts.
*   A need to provide each part with a thread-safe API so the rest of the system can drive it concurrently and correctly.

Building this by hand with mutexes or a standard task library can be daunting even for specialists. The goal of Macrame is to make this process safe and straightforward. It removes a large class of data-race and ordering bugs, along with the effort required to police them.

The framework is built on the concept of high-level parallelism. It encourages expressing parallelism where the surface area is smallest, such as between coarse parts rather than deep inside every algorithm. Once enough parallelism is expressed at that level to keep all CPU cores busy, low-level parallelisation stops paying off. Reducing low-level threading leads to less complexity, less code to maintain, and fewer bugs.

This library is likely not the best choice if your problem involves a single tight data-parallel kernel, one shared container, or fully independent jobs with no shared state. In those cases, standard library algorithms like `std::for_each(par)` will serve you better.

---

## Why controlled access

Most task systems schedule work and leave shared data safety up to the user, or they offer only task dependencies to manage it. Both of these common approaches have limitations:

*   **Locking shared state with mutexes** is the simplest and most popular approach, but it is often error-prone and inefficient. A forgotten or mis-scoped lock can cause silent data races, while correct locks can serialise access and cause delays under contention. Furthermore, correctness relies on scattered conventions that no tool verifies, meaning many issues are timing-dependent and only appear in production environments.
*   **Relying on task dependencies alone** is problematic for data sharing. Dependencies are designed to express ordering. Using them to protect shared data requires manually encoding which accesses conflict for every pair of tasks. This creates complex bookkeeping that results in rigid graphs and breaks as soon as access patterns change. Ordering and data access governance are different concerns, and conflating them makes systems brittle.

Macrame separates these concerns. You declare the data your logic accesses and whether that access is read-only or read/write. The framework then derives the ordering and exclusion needed for safety. A runtime harness verifies that nothing touches shared state without declaring it first, aborting with a stack trace instead of allowing a race condition. Dependencies remain available for genuine ordering, but they are no longer overloaded to represent data sharing.

```cpp
ts::Guarded<Inventory> inventory{ "inventory" };
// The parameter's const-ness acts as the access declaration.
inventory.access([](Inventory& inv) { inv.add(sword); }).sync(); // T& provides exclusive read/write access.
auto n = inventory.access([](const Inventory& inv) { return inv.count(); }).sync(); // const T& provides read-only access, concurrent with other readers.
```

---

## Examples

Here is a brief look at how the framework operates in practice.

#### 1. A thread-safe API for one object

You can wrap a thread-unsafe object in `Guarded<T>`. You pass it a functor, and the parameter's const-ness declares your access type. A non-const reference requests exclusive write access, while a const reference requests concurrent read access.

```cpp
ts::Guarded<Inventory> inventory{ "inventory" };

// The access method runs on the calling thread if the object is free. Otherwise, it waits for its turn.
// It requires zero allocations and is best for short functors.
inventory.access([](Inventory& inv) { inv.add(sword); }).sync(); // Write access.
auto n = inventory.access([](const Inventory& inv) { return inv.count(); }).sync(); // Read access.

// The async method is always scheduled off the calling thread and returns a Task.
// It is ideal for heavy functors that you do not want running inline.
inventory.async([](Inventory& inv) { inv.defragment(); });
```

The `access` method expects the caller to wait for the result, meaning the whole operation lives in the returned caller-owned handle and allocates nothing. It is opportunistic, meaning it skips scheduling entirely and runs the functor inline when the object is uncontended. This may briefly block the caller, which is an acceptable trade-off for short critical sections. The `async` method is detached. It never blocks the caller and is designed for expensive work.

With coroutines, the same access reads as ordinary linear code. Awaiting the task suspends execution until access is granted, then provides an RAII guard with direct access that is checked by the harness.

```cpp
ts::Task<void> loot(ItemId id)
{
    auto inventory = co_await ts::read_write(guarded_inventory); // Suspends until exclusive access is granted without blocking the thread.
    inventory->add(id); // Provides direct, harness-checked access.
    inventory->recompute_weight();
} // Access is released at scope exit.
```

This eliminates the need to write or manage locks. Concurrent writes are serialised, reads run together, and any code attempting to touch the inventory without permission will fault. The check is a single macro line at the top of each object method, costing roughly one nanosecond per call.

#### 2. A game frame as a graph

Each subsystem is represented by its own `Guarded<T>`. You declare what each node reads and writes, and the compile step derives the schedule from the access conflicts. Explicit ordering is only needed for business logic, not to prevent data races.

```cpp
ts::Guarded<Physics> physics{ "physics" };
ts::Guarded<Animation> anim{ "anim" };
ts::Guarded<Audio> audio{ "audio" };
ts::Guarded<Renderer> renderer{ "renderer" };

ts::Static_task_graph frame;

// Nodes declare read or write access to each guarded object they touch based on argument types.
frame.add_node("step", [](Physics& p) { p.step(); }, physics);
frame.add_node("pose", [](const Physics& p, Animation& a) { a.pose(p); }, physics, anim);
auto sfx = frame.add_node("mix", [](const Physics& p, Audio& s) { s.mix(p); }, physics, audio);

auto render = frame.add_node("render",
    [](const Physics& p, const Animation& a, Renderer& r) { r.submit(p, a); },
    physics, anim, renderer);

// Explicit ordering can be added for intent that access alone does not capture.
render.after(sfx);

// Edges are derived from access conflicts and explicit ordering rules.
frame.compile();

// The graph is compiled once and executed many times. Re-runs reuse the compiled nodes.
for (int f = 0; f < frame_count; ++f)
    frame.execute().sync();
```

In this example, the two physics readers run in parallel while the render node waits for both its inputs and the audio mix. If a node's access changes, the schedule updates automatically without requiring manual edge maintenance.

#### 3. Many producers, one atomic apply

`Deferred<T>` implements the command list pattern. Instead of mutating shared state directly, each producer records its intended changes into a buffer. A single later step applies them all at once. Recording does not require access to the target, allowing producers to run in parallel without blocking each other or any readers.

```cpp
ts::Guarded<World> world{ "world" };
ts::Deferred<World> staged{ world };

// Producers record changes into the buffer. Each producer uses its own recorder.
ts::Recorder<World> rec = staged.recorder();
rec.stage([e](World& w) { w.apply_damage(e); });

// Other work can read the world concurrently since recording holds no locks.
auto hp = world.async([](const World& w) { return w.health_of(player); });

// The whole batch of changes is applied as one write in a deterministic order.
staged.commit().sync();
```

#### 4. Stable reads while the next version is built

`Versioned<T>` implements the double buffer pattern. It keeps two copies of the state: a published version for readers and a next version being prepared. Readers get a stable view for the whole frame, while producers build the next version without interrupting them.

```cpp
ts::Versioned<Poses> poses{ "poses" };

// Producers build the next version without blocking readers.
ts::Recorder<Poses> rec = poses.recorder();
rec.stage([id, xf](Poses& p) { p.set(id, xf); });

// Readers see the last published version, which remains stable and uncontended.
auto n = poses.read([](const Poses& p) { return p.count(); }).sync();

// The next version is published and becomes the one readers see.
poses.publish().sync();
```

#### 5. Seeing the frame

The graph includes self-profiling capabilities. Using the `--trace` flag runs a mock game frame and renders the average run. The measured critical path is pinned to the top lane, parallel work is packed into rows below, and dead-time bands show where the critical chain waited.

<!-- NOTE: the interactive link below is the FINAL GitHub Pages URL (macrame-ts/macrame, Pages serving
     /docs from master) - dead until the repo moves and Pages is enabled at the flip (docs/going-public.md). -->
The SVG output is interactive and includes hover tooltips with per-node statistics and access declarations. Because GitHub strips scripts from images, the picture below is static. You can open the [interactive version](https://macrame-ts.github.io/macrame/media/game_frame_trace.svg) in a browser or view [the file](docs/media/game_frame_trace.svg) locally. It displays an aggregation of all frames, providing a high-level picture of the entire session.

![Traced game frame](docs/media/game_frame_trace.svg)

---

## What's in the box

The framework is layered and composable. You can use as much or as little as you need. Features include:

*   Scheduler. An efficient work-stealing scheduler with configurable idle policies and priorities. It has a minimal API and can be used independently of the rest of the framework.
*   Tasks. You can launch work and compose it using coroutines. Awaiting tasks is the standard continuation mechanism, allowing pipelines to read as straight-line code. It includes cooperative cancellation and priority support. Suspended tasks free their worker, preventing deadlocks in deep fork-join scenarios.
*   Data-parallel loops. Functions like `parallel_for` handle data-parallel work safely. Additional utilities like `async_parallel_for` and `parallel_for_colored` provide extra flexibility.
*   Coroutines. You can await any task or request read and write access to objects. Holding an access guard across a suspension point is automatically detected and results in a fast failure.
*   Guarded objects. `Guarded<T>` provides a thread-safe API for a shared object. It manages a reader and writer queue with concurrent reads and exclusive writes. 
*   Static task graphs. These are directed acyclic graphs built once and run multiple times. Edges are derived from access conflicts, and executions reuse compiled nodes to minimise allocations. Visualisation and profiling tools are included to track parallelism metrics.
*   Design patterns. Utilities like `Deferred<T>` and `Versioned<T>` manage staged writes and double buffering. These ensure deterministic execution. An event bus is also provided for lightweight publish and subscribe functionality.

The current unreleased version (v0.1.0) has a stable API shape, though it is not completely frozen. The changelog outlines the contents of this version. Certain areas are actively evolving, including performance optimizations, a platform abstraction layer, and benchmark tracking. The roadmap provides more details on future plans.

---

## Verification

Concurrency claims require evidence. The framework is verified through several methods:

*   A comprehensive test suite that includes subprocess death tests for fatal paths.
*   Clean results from ThreadSanitizer and AddressSanitizer.
*   Deterministic end-to-end samples that compare independent runs for consistency.
*   A forensic harness to detect races and deadlocks.

---

## Requirements and building

The project requires C++23 and has no external dependencies. Exceptions are disabled project-wide, as failures are designed to be fatal. Supported compilers include MSVC and clang-cl on Windows, and clang on Linux.

For Visual Studio 2022 and later, you can open the `task_system.slnx` solution file. CMake presets are also provided for Windows MSVC, Windows clang-cl, Windows shipping, Linux clang, and Linux ThreadSanitizer.

Running the built driver without arguments will execute everything. You can use flags like `--tests`, `--bench`, and `--stress` to isolate specific parts. Running with `--help` lists all available options.

---

## Documentation

*   [docs/quickstart.md](docs/quickstart.md) explains how to go from zero to a running program.
*   [docs/guide.md](docs/guide.md) is the user guide covering concepts, layers, patterns, and use cases.
*   [docs/design.md](docs/design.md) outlines the design rationale and rejected alternatives.
*   [docs/task-systems-comparison.md](docs/task-systems-comparison.md) compares Macrame to other task systems.
*   [docs/roadmap.md](docs/roadmap.md) details where the library is heading.

There are also deep dives available for task internals, command buffer design, and deferred versioned state.

---

## Contributing

Contributions of all sizes are welcome. This includes bug reports, documentation updates, tests, and code. Please see [CONTRIBUTING.md](CONTRIBUTING.md) for more details.

---

## License

This project is licensed under the MIT License.
