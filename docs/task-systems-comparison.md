# Task / Job Systems: A Comparative Survey

Companion documents: [guide.md](guide.md) is the user guide, and [design.md](design.md) describes
how this survey's findings shaped the library.

This survey covers concurrent task schedulers relevant to game engines and high-performance C++, with attention to design, performance, and ease of use. It was written to inform the design of this project.

---

## 0. Scope and framing

The same category is known under many names: task system, job system, job manager, scheduler, executor, parallel runtime, dispatch framework, tasking library. A runtime accepts units of work (tasks or jobs), runs them across a managed pool of threads, and offers some way to express dependencies and ordering, plus some way to wait for or react to completion. This survey treats the whole family, regardless of the label a given project chose.

The line is drawn loosely rather than strictly. A bare thread pool with no dependency or completion model sits at the primitive end, and a full async runtime with futures, I/O, and structured concurrency sits at the rich end. Engine "job managers" and HPC "task schedulers" fall in the middle. All are in scope where they inform design. What differs is mostly emphasis (fine-grained compute, I/O concurrency, or frame-tight game work), not kind.

The systems surveyed, grouped by domain:

| Domain | Systems |
|---|---|
| Game engines (proprietary) | Unreal Engine Tasks System (plus the legacy TaskGraph), Unity C# Job System (plus Burst/DOTS) |
| Game engines (open, library) | enkiTS, Google Marl, fiber job systems (FiberTaskingLib, the Naughty Dog pattern) |
| General-purpose C++ | Intel oneTBB, Taskflow, HPX, PPL/Folly (brief) |
| Platform runtimes | Apple GCD / libdispatch |
| Other-language reference points | Rust Rayon and Tokio |
| The upcoming standard | C++26 `std::execution` (senders/receivers) |

---

## 1. The design axes

Everything below is a point in a small number of dimensions. Naming them up front makes the comparison easier to follow.

1. Scheduling topology. A single shared ready queue is simple but contends; per-worker deques with work stealing scale well and are cache-friendly. Nearly every serious system uses work stealing. The classic refinement, from TBB and the Cilk lineage, is to run your own tasks LIFO and depth-first for cache locality but steal the oldest task from a victim, which converts latent parallelism breadth-first.
2. The blocking problem. What happens when a task must wait mid-execution? The options are:
   - Continuations and callbacks, which never block; the rest of the work becomes a new task (TBB, `std::execution`, GCD).
   - Stackful fibers, which suspend the call stack and run something else on the thread (Marl, Naughty Dog, FiberTaskingLib).
   - Stackless coroutines or green tasks (Tokio, C++20 coroutines under senders).
   - Thread oversubscription, which spins up standby threads so a blocked worker does not starve the pool (UE 5.5, GCD's lazy pool). Its failure mode is thread explosion.
3. Dependency expression. Some systems take a static DAG declared up front (TBB flow graph, Taskflow); others chain continuations dynamically (UE prerequisites and `Then`, Unity `JobHandle`, futures, senders).
4. Allocation model. Some systems pay a heap allocation per task (anything built on `std::function`); others use zero-allocation intrusive or inline storage (enkiTS, UE, and this project's intent).
5. Affinity and priorities. Named or pinned threads (render thread, main thread) and priority bands are first-class in engine systems, and weak or absent in HPC libraries.
6. Safety. Compile-time or run-time prevention of data races (Unity's container safety system is the standout) versus leaving the user on their own.

---

## 2. System overviews

### 2.1 Unreal Engine Tasks System (UE5) over the `LowLevelTasks` backend

Two front ends, the modern `UE::Tasks` API and the legacy TaskGraph, sit on one backend: `LowLevelTasks::FScheduler` (`Engine/Source/Runtime/Core/Public/Async/Fundamental/`). The high-level API launches with `Launch(debug_name, callable, priority, flags)` and returns reference-counted handles (`FTask` / `TTask<T>`). The interesting engineering is in the backend.

The backend architecture, from source:

- Per-worker work-stealing local queues. Each worker owns a bounded (~1024-slot) `TWorkStealingQueue2` per priority. The owner pushes and pops at the head single-threaded; other workers steal from the tail via atomic CAS. This is the Chase-Lev/Vyukov-style deque, not a shared queue.
- A global overflow queue. A lock-free MPMC `FAAArrayQueue` (Dmitry Vyukov) per priority catches overflow when a local queue is full, work from threads with no local queue, and the drain when a worker exits. Dequeue order is local first, then global, then stealing from a random victim's local queue (hashed PRNG index).
- Two worker pools, foreground and background, each with its own OS thread priority and affinity mask. The priority bands are `High` and `Normal` for the foreground pool and `BackgroundHigh/Normal/Low` for the background pool.
- Idle workers spin, then park. A starved worker spins `WorkerSpinCycles` (53) and then parks on a `FWaitingQueue`, an EventCount (Dmitry Vyukov / Eigen lineage) with a two-phase `PrepareWait`/`CommitWait`/`CancelWait` protocol to avoid lost wakeups. This is exactly the spin-then-sleep hybrid, in production.
- Oversubscription and standby workers. When all workers are parked, for example because a task busy-waits on a dependency, standby workers wake or new threads spin up so cores are not starved and waits cannot deadlock. This is what replaced naive busy-waiting. The mechanism is `IncrementOversubscription`/`Decrement`, plus an event that fires when the limit is reached.
- Intrusive, cache-line-sized tasks. `FTask` is `PLATFORM_CACHE_LINE_SIZE` and carries an inline `TaskDelegate` with small-buffer storage, so the body lives in the task and there is no per-task heap allocation. An atomic state machine (Ready → Scheduled → Running, with Canceled variants) drives cancellation (`ECancellationFlags`: prelaunch cancellation, try-launch-on-success) and opt-in busy-waiting (`ETaskFlags::AllowBusyWaiting`).
- A TLS context holds the active scheduler and the worker's local queue, so a task launched from within a worker enqueues locally with no global contention. (This is a richer version of this project's `current_worker_index`, which, with one process-wide pool, is all that is needed to route a worker's own submits to its local deque.)

High-level features over that backend:

- Prerequisites (non-blocking DAG edges), nested tasks (a parent completes after its children), task events (`FTaskEvent`, a manual join/signal), and `TTask<T>` results.
- Pipes (`UE::Tasks::FPipe`): a chain of non-concurrent tasks giving lock-free serialized access to a shared resource, FIFO for tasks without prerequisites. The source comment is explicit: "a replacement for named threads… there can be a large dynamic number of pipes each controlling its own shared resource." The pipe must outlive its last task.
- Named threads (game/render/RHI) survive from TaskGraph, but pipes are the modern, lighter answer.
- Deep tooling: Unreal Insights, VS visualizers, and CPU profiler trace hooks throughout.

Popularity and success: it ships in Unreal Engine, which accounts for roughly 28% of Steam releases (2024) but 31% of revenue, and is the dominant AAA/console engine (about 83% of Sony first-party titles cited). It is battle-tested at the highest fidelity tier.

My read: by source inspection, this is the most complete design in the survey. It combines per-worker work-stealing deques with a lock-free global overflow, foreground and background pools with affinity, spin-then-park EventCount idling, oversubscription for safe waiting, and zero-allocation intrusive cache-line tasks with cancellation. Its strength is that essentially every lesson in section 6 is already implemented here, plus pipes as a lock-avoidance primitive. Its weaknesses: it is inseparable from the engine and its types (`FTask`, `TCHAR`, the container and allocator stack); the breadth (cancellation states, two pools, oversubscription, the named-thread legacy) is a lot of surface; and the reference-counted high-level handles add traffic that the leanest competitors (enkiTS) avoid.

### 2.2 Unity C# Job System, Burst, and DOTS

Unity's is a managed-language job system. You write `IJob`/`IJobParallelFor` structs over `NativeArray`/`NativeContainer` data, `Schedule()` them with `JobHandle` dependencies, and call `Complete()` to sync. The Burst compiler turns a C# subset ("HPC#") into LLVM-optimized native SIMD code (claimed 10–100× over Mono). It is part of DOTS alongside the ECS.

Notable features:

- A work-stealing scheduler across worker threads.
- The container safety system: the editor detects data races and enforces job dependencies at run time. This is the single best ease-of-use feature in the entire survey, because it makes data-parallel code safe by default for non-experts.
- Tight coupling of data layout (ECS), scheduling, and codegen, which is where the real performance comes from. The scheduler alone does not provide it.

Popularity and success: Unity accounts for roughly 51% of Steam releases (2024) and about 71% of top-1000 mobile games. Job System/DOTS adoption is narrower than Unity's overall base, since many titles never touch it, but it is a very widely distributed technology.

My read: the strengths are the safety system, Burst, and a gentle on-ramp to data parallelism. The weaknesses: it is only useful inside the tight Burst/HPC# constraints (no managed heap, a restricted language subset); DOTS has had a long, bumpy maturation; and the main-thread `Complete()` sync points invite stalls if misused.

### 2.3 Intel oneTBB

Intel's TBB has been the reference C++ task library since 2006, and is now oneTBB under the UXL Foundation, licensed Apache-2.0. It pairs a work-stealing task scheduler with high-level algorithms (`parallel_for`/`reduce`/`scan`/`pipeline`), `task_group`, a flow graph (data-flow and dependency graphs with buffering, conditionals, and cycles), concurrent containers, and a scalable allocator.

Notable features:

- Canonical steal-oldest work stealing (breadth-first parallelization, depth-first local execution).
- The flow graph is the most general dependency and data-flow model in mainstream C++.
- It is composable, and underpins much of oneAPI (MKL, oneDNN) and broad HPC code.

Popularity and success: the de-facto industrial standard for shared-memory C++ parallelism, ubiquitous in HPC, rendering (for example Embree), and scientific computing.

My read: the strengths are generality, maturity, composability, and the scalable allocator. The weaknesses: flow-graph code is verbose and ceremony-heavy; there is no first-class thread affinity or priority support for engine use; and the abstraction tax shows up in latency-sensitive, fine-grained workloads compared to lean game schedulers.

### 2.4 Taskflow

Taskflow is a modern, header-only, MIT-licensed C++20 task-graph library (~12k GitHub stars). It pairs a work-stealing executor with an expressive graph front end.

Notable features:

- Conditional tasking (control flow and cycles inside the graph), which is rare and genuinely useful.
- Subflow, or dynamic tasking: sub-graphs can be spawned during execution.
- Composition of reusable graphs, and pipeline scaffolding.
- GPU tasking via CUDA Graph, and a built-in profiler, TFProf.
- It is backed by academic work (IEEE TPDS 2022) and claims to beat TBB FlowGraph and OpenMP tasking in expressiveness and speed.

My read: the strengths are the cleanest DAG ergonomics in C++, conditional tasking, drop-in header-only integration, and a good profiler. The weaknesses: the graph-centric model is awkward for fire-and-forget or event-driven game workloads; there is no thread affinity or priority support; and the GPU story is CUDA-only.

### 2.5 enkiTS (Doug Binks)

enkiTS is a deliberately lean, zlib-licensed C/C++11 task scheduler (~2k stars), built for an in-house voxel engine (Avoyd) and widely taught.

Notable features:

- Zero-allocation scheduling.
- Pinned tasks (run on a specific thread) and external thread registration.
- Up to 5 priorities, and completion actions that run on finish with no reschedule overhead.
- Both data parallelism (a `TaskSet` over a range, split across workers) and task parallelism, plus "braided parallelism" (tasks spawning tasks).
- An explicit design stance: optimize for low thread counts first, then scale; a tiny API; a C API for FFI.

My read: the strengths are that it is small, fast, dependency-light, easy to drop in, and has a pragmatic feature set that maps directly to game needs (pinning, priorities, ranges). The weakness is minimalism by choice: there is no rich DAG or continuation algebra and no built-in story for blocking inside a task beyond braiding, so you build higher-level patterns yourself.

### 2.6 Google Marl

Marl is a hybrid thread/fiber scheduler in C++11 from Google, used in SwiftShader and Dawn (WebGPU), and cross-platform across many architectures.

Notable features:

- Fibers let a blocking task suspend (`Worker::suspend`) while the worker picks up other ready fibers and tasks, so blocking code does not idle a hardware thread.
- Work stealing plus spin-with-yield (`spinForWork`) to avoid OS yields.
- A bound scheduler via TLS, so `marl::schedule()` needs no explicit handle.
- Synchronization primitives hold shared-pointer state captured by value, an explicit trade of a little overhead for safety against tasks outliving their stack frame.

My read: the strength is that it makes "just block and wait" safe and cheap, which greatly simplifies call sites, and it is portable. The weaknesses: fibers cost stack memory and bring well-known hazards (TLS, debuggers, stack overflow detection), and the shared_ptr-for-safety choice concedes some performance.

### 2.7 Fiber job systems: the Naughty Dog pattern (FiberTaskingLib et al.)

This is less a product than an influential design pattern from Christian Gyrling's GDC 2015 talk "Parallelizing the Naughty Dog Engine Using Fibers" (shipping *The Last of Us Remastered* at 60 fps). Jobs run on fibers, atomic counters express dependencies, and a job that waits on a counter yields its fiber so the worker can run something else. FiberTaskingLib (RichieSams) is the best-known open implementation.

My read: this pattern is the ancestor of much modern engine threading. Its strength is uniform, low-overhead waiting with natural code and no manual continuation passing. Its weakness is fiber complexity and the same debugging and TLS hazards as Marl; correctness around fiber-to-thread migration is subtle.

### 2.8 Apple GCD / libdispatch

GCD is OS-level concurrency on macOS and iOS, open-sourced as swift-corelibs-libdispatch. You submit blocks to serial or concurrent queues, and the system manages a lazy, global thread pool (the "root" queues). QoS classes provide priorities, and the main queue gives main-thread affinity.

My read: the strengths are that it is the easiest concurrency API in wide use, OS-integrated, QoS-aware, and nearly free of ceremony. The weaknesses: thread explosion (blocking work on concurrent queues spawns more threads, sometimes catastrophically); no work-stealing DAG model; no portability; and no design for frame-tight game scheduling.

### 2.9 Rust: Rayon and Tokio (reference points)

- Rayon provides work-stealing data parallelism (`par_iter`, `join`, `scope`). It is ergonomic and CPU-bound, with no async support.
- Tokio is a work-stealing async-I/O runtime over stackless futures; cooperative tasks must yield.
- The two have separate pools that do not share threads. Mixing CPU-bound and I/O-bound work across the boundary is a known pain point and a thread-starvation hazard.

My read: a clean illustration of the CPU-bound versus I/O-bound split that C++ engines usually fold into one system. Rayon's `join`/`scope` API is a model of ergonomic structured parallelism worth borrowing ideas from.

### 2.10 C++26 `std::execution` (senders/receivers)

This is the standardized async model (P2300, adopted into C++26; NVIDIA's stdexec is the reference implementation). It rests on three abstractions: schedulers say where work runs, senders are a lazy description of work, and receivers are the sink. They are composed by `connect` into operation states, with a library of composable async algorithms (`then`, `when_all`, `bulk`, and more) and structured-concurrency lifetimes.

My read: the strengths are a single composable substrate spanning thread pools, GPUs, and I/O; lifetimes that are structured by design; and a likely role as the long-term common language that schedulers interoperate through. The weaknesses: a steep conceptual and compile-time cost; no game-proven record yet; and abstraction that can obscure the low-level control engines want. It is worth designing to be compatible with, though not necessarily built on, today.

### 2.11 Brief mentions (other names, same family)

- Cilk / Cilk Plus is the academic origin of practical work stealing (the "work-first" principle, `spawn`/`sync`). It is dead as a product (dropped from GCC and Clang), but its scheduler theory underlies TBB, Rayon, Taskflow, and UE's deques.
- OpenMP tasks: `#pragma omp task`/`taskloop` with a `depend` clause for DAGs. It is ubiquitous in HPC and scientific code, with compiler-directive ergonomics and less control than a library.
- HPX extends standards-conforming parallelism to distributed memory (futures and continuations, a global address space). It is strong in HPC and overkill for a single-node engine.
- Microsoft PPL (the Concurrency Runtime) offers `task_group`, `parallel_for`, and `concurrency::task` continuations. It is Windows-centric and conceptually close to TBB.
- Folly executors, Boost.Asio's `thread_pool`, and Boost.Fiber are production C++ building blocks for future-based pools, I/O dispatch, and fibers respectively.
- .NET TPL (the Task Parallel Library, with `ForkJoinPool`-style work stealing) and Java's `ForkJoinPool` are the managed-runtime incarnations: mature work-stealing job managers that shaped a generation of API expectations (`Task.WhenAll`, `CompletableFuture`).

---

## 3. Feature comparison

| | Schedule topology | Blocking model | Dependencies | Alloc/task | Affinity | Priorities | Safety aids | Lang / license |
|---|---|---|---|---|---|---|---|---|
| **UE Tasks** | Per-worker WS deques + lock-free global overflow; fg/bg pools | Spin-then-park (EventCount) + oversubscription/standby; opt-in busy-wait | Prerequisites, nested, events, pipes | Zero-alloc intrusive cache-line task (SBO) | Named threads + affinity | Yes (5 bands, fg/bg) | Pipes avoid locks; cancellation | C++ / engine |
| **Unity Jobs** | Work-stealing | Main-thread `Complete` sync | `JobHandle` chains | Native containers | Limited | Limited | Race detector (best in class) | C#/HPC# / engine |
| **oneTBB** | WS, steal-oldest | Continuations | `task_group`, flow graph | Heap (scalable alloc) | No | No | Concurrent containers | C++ / Apache-2.0 |
| **Taskflow** | Work-stealing | Continuations / subflow | Static DAG + conditional | Heap | No | No | — | C++20 / MIT |
| **enkiTS** | WS, lean | Braiding | Sets + simple deps + completion actions | Zero-alloc | Pinned tasks | Up to 5 | — | C/C++11 / zlib |
| **Marl** | WS + spin | Fibers | Via blocking waits | Fiber stacks | Bound TLS | — | shared_ptr state | C++11 / Apache-2.0 |
| **Fiber JS (ND)** | WS | Fibers + atomic counters | Counters | Fiber stacks | Usually pinned mains | Varies | — | C++ / varies |
| **GCD** | Lazy global pool | Thread explosion | Queue ordering / groups | Block heap | Main queue | QoS classes | — | C / Apache-2.0 |
| **Rayon** | WS | (sync only) | `join`/`scope` | Stack frames | No | No | Rust borrow checker | Rust / MIT-Apache |
| **Tokio** | WS | Stackless futures | `.await` graph | Future state | No | Limited | Borrow checker | Rust / MIT |
| **`std::execution`** | Pluggable schedulers | Senders (lazy) | Algebra (`then`/`when_all`) | Op-state (often inline) | Scheduler-defined | Scheduler-defined | Structured lifetimes | C++26 / std |

WS = work-stealing.

---

## 4. Performance analysis

- Topology dominates scaling. A single mutex-guarded ready queue serializes every submit and pop; under many producers and consumers it becomes the bottleneck long before the work does. Every system that targets scale uses per-worker deques with work stealing. The steal-oldest/run-newest discipline (TBB, Rayon, Taskflow) is the proven sweet spot: depth-first local execution keeps caches hot, and stealing the oldest task grabs the biggest available subtree.
- Per-task overhead is an allocation story. Anything built on `std::function` or `move_only_function` pays a heap allocation and an indirect call per task. Lean engine systems (enkiTS, UE) avoid this with inline, intrusive storage and reach single-digit to low-tens-of-nanoseconds per task. That is the difference between "a task per object" being viable or not.
- The blocking model sets the floor on latency under contention. Fibers (Marl, ND) make a context switch cost tens of nanoseconds, so blocking and waiting is cheap and code stays linear; the cost is stack memory and cache pressure from many live stacks. Continuation styles (TBB, senders) have zero switch cost but push complexity onto the programmer. Oversubscription (UE 5.5, GCD) is the simplest but risks thread explosion and scheduling jitter.
- Unity's lesson is that most of DOTS's speed comes from data layout and Burst codegen, not the scheduler. A scheduler cannot rescue cache-hostile data.
- Wake latency versus throughput is a real tradeoff, directly visible in this project's own block-vs-spin benchmarks. Sleeping workers save power and avoid mutex thrash but pay a wakeup; spinning workers wake instantly but burn cores and contend. Mature systems hybridize: they spin briefly, then sleep.

---

## 5. Ease of use analysis

Roughly ordered from easiest to hardest to use correctly:

1. GCD / Rayon. Submit a block or call `par_iter`; the model fits in your head.
2. Unity Jobs. The safety system catches your races; the constraint is learning HPC#.
3. Taskflow. Declarative DAGs read well, and the profiler is good.
4. UE Tasks. Pleasant inside UE; prerequisites and pipes are intuitive, and the named-thread legacy is the wrinkle.
5. enkiTS. A tiny API, but you assemble higher-level patterns yourself.
6. oneTBB. Capable but ceremonious; the flow graph has a learning curve.
7. Marl and other fiber systems. The code is linear, but fiber hazards (TLS, debuggers, stack sizing) bite.
8. `std::execution`. The most composable and the most to learn, with cryptic compile errors today.

A general principle: ease of use comes from one of two places. Either a decision is removed (GCD has no scheduler to configure) or a failure mode is removed (Unity races cannot compile or run). Expressiveness (TBB, senders) trades against approachability.

---

## 6. Lessons for this project

These lessons tie the survey back to the codebase as it stood at the 2026-06 baseline: a single shared `priority_queue` behind a mutex, single-class workers, intrusive refcounting, and a `block`/`spin` idle policy. They drove the work that followed. Items 1, 3, and 5 are now implemented (per-priority lock-free MPMC queues plus per-worker Chase-Lev deques, the `co_await` blocking story, and spin-then-block idle), and item 2's zero-allocation instinct was broadened into monomorphic single-allocation task blocks and the zero-allocation `Access_op`; a tunable small-buffer callable for the residual heap cases is still open. See [design.md](design.md) §3. The list is kept as the research record that motivated the work.

1. The shared mutex queue is the number one scaling limit. The benchmarks already show `queue_mutex_` contention dominating. The proven path is per-worker deques with work stealing plus a lock-free global overflow queue. This is exactly what UE's `LowLevelTasks` does (`TWorkStealingQueue2` locals plus a `FAAArrayQueue` global) and what the TBB/Rayon model does (push and pop locally, steal from victims). It is the single highest-leverage change.
2. Keep the zero-allocation instinct and broaden it. Intrusive refcounting is the right call and matches UE and enkiTS. For fire-and-forget work, consider an inline task payload with small-buffer storage in the style of enkiTS, so detached tasks do not require user lifetime management.
3. Decide a blocking story early. The options, cheapest to adopt first, are continuation chaining (the `then` of the day), oversubscription, and fibers. Fibers (Marl/ND) give the nicest call sites but are a large commitment; continuations compose with the existing dependency model with no new runtime machinery. (This has since been settled: the coroutine-first transformation made `co_await` the one composition mechanism and made an in-task blocking `sync()` fatal; see design.md §4.3–4.4.)
4. First-class affinity will be needed. Engines require pinned or named threads (render, main). enkiTS's pinned-task model is the lean reference, and UE's named threads the full one.
5. Hybridize idle behavior. The block-vs-spin result argues for spin-then-sleep rather than a hard either/or. UE spins `WorkerSpinCycles` and then parks on an EventCount; Marl uses `spinForWork`. An EventCount (a two-phase prepare/commit wait) also avoids the lost-wakeup races a raw semaphore can hit.
6. Stay `std::execution`-aware. There is no need to build on senders, but a `schedule`/`then`/`when_all`-shaped surface keeps a future interop bridge cheap.
7. Borrow the best ease-of-use ideas. UE pipes (lock-free sequential access to shared state) and Unity's race detection (even as a debug-build assertion layer) are differentiators that cost users little and prevent whole bug classes.

The gap this project targets: UE Tasks-level functionality (prerequisites, pipes, priorities, named threads) with enkiTS-level leanness and zero allocation, as a standalone library. Neither the heavyweight HPC libraries nor the minimal lean ones occupy exactly that point.

---

## Sources

- [Tasks Systems in Unreal Engine — Epic Developer Documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-systems-in-unreal-engine)
- [Tasks Systems in Unreal Engine 5.3 — docs.unrealengine.com](https://docs.unrealengine.com/5.3/en-US/tasks-systems-in-unreal-engine/)
- [Task Graph Insights in Unreal Engine 5](https://dev.epicgames.com/documentation/en-us/unreal-engine/task-graph-insights-in-unreal-engine-5)
- [Unity — DOTS](https://unity.com/dots)
- [Unity Manual — Job system overview](https://docs.unity3d.com/6000.3/Documentation/Manual/job-system-overview.html)
- [Unity Burst and the kernel theory of game performance — Sebastian Schöner](https://blog.s-schoener.com/2024-12-12-burst-kernel-theory-game-performance/)
- [Threading Building Blocks — Wikipedia](https://en.wikipedia.org/wiki/Threading_Building_Blocks)
- [Parallelizing Data Flow and Dependency Graphs — Intel oneTBB docs](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/parallelizing-data-flow-and-dependency-graphs.html)
- [How Task Scheduler Works — Intel oneTBB docs](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/how-task-scheduler-works.html)
- [Work stealing — Wikipedia](https://en.wikipedia.org/wiki/Work_stealing)
- [Taskflow — GitHub](https://github.com/taskflow/taskflow)
- [Taskflow — project site](https://taskflow.github.io/)
- [enkiTS — GitHub](https://github.com/dougbinks/enkiTS)
- [Google Marl — GitHub](https://github.com/google/marl)
- [Marl scheduler design — docs/scheduler.md](https://github.com/google/marl/blob/main/docs/scheduler.md)
- [Parallelizing the Naughty Dog Engine Using Fibers (GDC 2015), Christian Gyrling — slides](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf)
- [FiberTaskingLib — GitHub](https://github.com/RichieSams/FiberTaskingLib)
- [NVIDIA stdexec (`std::execution` reference impl) — GitHub](https://github.com/nvidia/stdexec)
- [Senders/Receivers: An Introduction — isocpp.org](https://isocpp.org/blog/2025/01/senders-receivers-an-introduction-lucian-radu-teodorescu)
- [Execution control library (since C++26) — cppreference](https://en.cppreference.com/cpp/execution)
- [Intel oneAPI Threading Building Blocks (oneTBB) — Intel](https://www.intel.com/content/www/us/en/developer/tools/oneapi/onetbb.html)
- [Mixing rayon and tokio — Lobsters discussion](https://lobste.rs/s/mebxps/mixing_rayon_tokio_for_fun_hair_loss)
- [Grand Central Dispatch internals — newosxbook.com](https://newosxbook.com/articles/GCD.html)
- [swift-corelibs-libdispatch — GitHub](https://github.com/apple/swift-corelibs-libdispatch/blob/master/dispatch/queue.h)
- [The Big Game Engines Report 2025 — Sensor Tower](https://sensortower.com/blog/the-big-game-engines-report-of-2025)
- [Video Game Insights: Game Engines on Steam in 2025](https://gamedevreports.substack.com/p/video-game-insights-game-engines)
