# Task / Job Systems: A Comparative Survey

(Companion documents: [guide.md](guide.md) — user guide; [design.md](design.md)
— how this survey's findings shaped the library.)

*Research compiled 2026-06. Focus: concurrent task schedulers relevant to game engines and high-performance C++, with an eye toward design, performance, and ease of use. Written to inform the design of this project. The Unreal section is grounded in a direct read of the engine source (`LowLevelTasks` backend in `Engine/Source/Runtime/Core`), not just the public docs.*

---

## 0. Scope and framing

The same category travels under many names — **task system, job system, job manager, scheduler, executor, parallel runtime, dispatch framework, tasking library**. They're the same animal: a runtime that accepts units of work (tasks/jobs), runs them across a managed pool of threads, and offers some way to express **dependencies / ordering** and to **wait for / react to** completion. This survey treats the whole family, regardless of the label a given project chose.

The line is drawn loosely, not strictly. A bare thread pool with no dependency or completion model sits at the primitive end; a full async runtime (futures, I/O, structured concurrency) at the rich end; engine "job managers" and HPC "task schedulers" in the middle. All are in scope where they inform design — what differs is mostly emphasis (fine-grained compute vs. I/O concurrency vs. frame-tight game work), not kind.

The systems surveyed, grouped by milieu:

| Milieu | Systems |
|---|---|
| Game engines (proprietary) | **Unreal Engine Tasks System** (+ legacy TaskGraph), **Unity C# Job System** (+ Burst/DOTS) |
| Game engines (open, library) | **enkiTS**, **Google Marl**, fiber job systems (**FiberTaskingLib**, Naughty Dog pattern) |
| General-purpose C++ | **Intel oneTBB**, **Taskflow**, **HPX**, **PPL/Folly** (brief) |
| Platform runtimes | **Apple GCD / libdispatch** |
| Other-language reference points | **Rust Rayon + Tokio** |
| The coming standard | **C++26 `std::execution`** (senders/receivers) |

---

## 1. The design axes (read this first)

Everything below is a point in a small number of dimensions. Naming them up front makes the comparison legible.

1. **Scheduling topology.** Single shared ready-queue (simple, contends) vs. **per-worker deques with work-stealing** (scales, cache-friendly). Nearly every serious system is work-stealing; the classic refinement (TBB, Cilk lineage) is to run your *own* tasks LIFO/depth-first for cache locality but **steal the oldest** task from a victim, converting latent parallelism breadth-first.
2. **The blocking problem.** What happens when a task must wait mid-execution? Options:
   - **Continuations / callbacks** — never block; the rest is a new task (TBB, `std::execution`, GCD).
   - **Stackful fibers** — suspend the call stack and run something else on the thread (Marl, Naughty Dog, FiberTaskingLib).
   - **Stackless coroutines / green tasks** (Tokio, C++20 coroutines under senders).
   - **Thread oversubscription** — spin up standby threads so a blocked worker doesn't starve the pool (UE 5.5, GCD's lazy pool — and its failure mode, *thread explosion*).
3. **Dependency expression.** Static DAG declared up front (TBB flow graph, Taskflow) vs. dynamic continuation chaining (UE prerequisites/`Then`, Unity `JobHandle`, futures, senders).
4. **Allocation model.** Per-task heap (anything built on `std::function`) vs. **zero-alloc** intrusive/inline storage (enkiTS, UE, this project's intent).
5. **Affinity & priorities.** Named/pinned threads (render thread, main thread) and priority bands — first-class in engine systems, weak or absent in HPC libraries.
6. **Safety.** Compile/run-time prevention of data races (Unity's container safety system is the standout) vs. "you're on your own."

---

## 2. System overviews

### 2.1 Unreal Engine — Tasks System (UE5) over the `LowLevelTasks` backend

**What it is.** Two front-ends — the modern **`UE::Tasks`** API and the legacy **TaskGraph** — both sit on one backend, **`LowLevelTasks::FScheduler`** (`Engine/Source/Runtime/Core/Public/Async/Fundamental/`). The high-level API launches with `Launch(debug_name, callable, priority, flags)` and returns reference-counted handles (`FTask` / `TTask<T>`). The interesting engineering is in the backend.

**Backend architecture (from source).**
- **Per-worker work-stealing local queues.** Each worker owns a bounded (~1024-slot) **`TWorkStealingQueue2`** *per priority*. The owner pushes/pops at the head single-threaded; other workers **steal from the tail** via atomic CAS. This is the Chase-Lev/Vyukov-style deque, not a shared queue.
- **Global overflow queue.** A lock-free MPMC **`FAAArrayQueue`** (Dmitry Vyukov) per priority catches overflow when a local queue is full, work from threads with no local queue, and the drain when a worker exits. Dequeue order is **local → global → steal a random victim's local queue** (hashed PRNG index).
- **Two worker pools — foreground and background** — each with its own OS thread priority and **affinity mask**. Priority bands: `High`, `Normal` (foreground) and `BackgroundHigh/Normal/Low`.
- **Idle = spin-then-park.** A starved worker spins `WorkerSpinCycles` (53) then parks on a **`FWaitingQueue`** — an *EventCount* (Dmitry Vyukov / Eigen lineage) with a two-phase `PrepareWait`/`CommitWait`/`CancelWait` protocol to avoid lost wakeups. This is exactly the spin-then-sleep hybrid, in production.
- **Oversubscription / standby workers.** When all workers are parked (e.g. a task busy-waits on a dependency), standby workers wake or new threads spin up so cores aren't starved and waits can't deadlock — this is what replaced naive busy-waiting. `IncrementOversubscription`/`Decrement`, plus an event that fires when the limit is reached.
- **Intrusive, cache-line-sized tasks.** `FTask` is `PLATFORM_CACHE_LINE_SIZE` and carries an **inline `TaskDelegate` (SBO)** — the body lives in the task, no per-task heap allocation. An atomic state machine (Ready → Scheduled → Running, with Canceled variants) drives **cancellation** (`ECancellationFlags`: prelaunch cancellation, try-launch-on-success) and opt-in busy-waiting (`ETaskFlags::AllowBusyWaiting`).
- **TLS context** holds the active scheduler *and* the worker's local queue (a richer version of this project's `current_scheduler`), so a task launched from within a worker enqueues locally with no global contention.

**High-level features over that backend.**
- **Prerequisites** (non-blocking DAG edges), **nested tasks** (parent completes after children), **task events** (`FTaskEvent` manual join/signal), **`TTask<T>` results**.
- **Pipes** (`UE::Tasks::FPipe`) — a chain of **non-concurrent** tasks giving lock-free serialized access to a shared resource; FIFO for prereq-less tasks. The source comment is explicit: *"a replacement for named threads… there can be a large dynamic number of pipes each controlling its own shared resource."* The pipe must outlive its last task.
- **Named threads** survive from TaskGraph (game/render/RHI), but pipes are the modern, lighter answer.
- Deep tooling: Unreal Insights, VS visualizers, CPU profiler trace hooks throughout.

**Popularity/success.** Ships in Unreal Engine — ~28% of Steam releases (2024) but **31% of revenue** and the dominant AAA/console engine (~83% of Sony first-party titles cited). Battle-tested at the highest fidelity tier. (The author of this project designed and implemented this system at Epic.)

**My read.** This is, by source inspection, the most complete design in the survey: per-worker work-stealing deques + lock-free global overflow, foreground/background pools with affinity, spin-then-park EventCount idling, oversubscription for safe waiting, and zero-alloc intrusive cache-line tasks with cancellation. Strengths: essentially every lesson in §6 is already implemented here, plus pipes as an elegant lock-avoidance primitive. Weaknesses: inseparable from the engine and its types (`FTask`, `TCHAR`, container/allocator stack); the breadth (cancellation states, two pools, oversubscription, named-thread legacy) is a lot of surface; reference-counted high-level handles add traffic the very leanest competitors (enkiTS) avoid.

### 2.2 Unity — C# Job System + Burst + DOTS

**What it is.** A managed-language job system where you write `IJob`/`IJobParallelFor` structs over `NativeArray`/`NativeContainer` data, `Schedule()` them with `JobHandle` dependencies, and `Complete()` to sync. The **Burst** compiler turns a C# subset ("HPC#") into LLVM-optimized native SIMD code (claimed 10–100× over Mono). Part of **DOTS** alongside the ECS.

**Notable features.**
- **Work-stealing** scheduler across worker threads.
- **Container safety system** — the editor detects data races and enforces job dependencies at run time. This is the single best *ease-of-use* feature in the entire survey: it makes data-parallel code safe by default for non-experts.
- Tight coupling of **data layout (ECS) + scheduling + codegen**, which is where the real performance comes from — not the scheduler alone.

**Popularity/success.** Unity is ~51% of Steam releases (2024) and ~71% of top-1000 mobile games. The Job System/DOTS adoption is narrower than Unity's overall base (many titles never touch it), but it's a massively distributed technology.

**My read.** Strengths: safety system, Burst, and a gentle on-ramp to data parallelism. Weaknesses: only useful inside the Burst/HPC# straitjacket (no managed heap, restricted language subset); DOTS has had a long, bumpy maturation; the main-thread `Complete()` sync points invite stalls if misused.

### 2.3 Intel oneTBB

**What it is.** The reference C++ task library since 2006 (Intel), now **oneTBB** under the UXL Foundation, **Apache-2.0**. Work-stealing task scheduler with high-level algorithms (`parallel_for`/`reduce`/`scan`/`pipeline`), `task_group`, **flow graph** (data-flow *and* dependency graphs with buffering, conditionals, cycles), concurrent containers, and a scalable allocator.

**Notable features.**
- Canonical **steal-oldest** work-stealing (breadth-first parallelization, depth-first local execution).
- **Flow graph** is the most general dependency/data-flow model in mainstream C++.
- Composable; underpins much of oneAPI (MKL, oneDNN) and broad HPC code.

**Popularity/success.** The de-facto industrial standard for shared-memory C++ parallelism; ubiquitous in HPC, rendering (e.g., Embree), and scientific computing.

**My read.** Strengths: generality, maturity, composability, scalable allocator. Weaknesses: flow-graph code is verbose and ceremony-heavy; no first-class thread affinity/priorities for engine use; the abstraction tax shows up in latency-sensitive, fine-grained workloads compared to lean game schedulers.

### 2.4 Taskflow

**What it is.** A modern, **header-only**, **MIT**, C++20 task-graph library (~12k GitHub stars). Work-stealing executor plus an expressive graph front-end.

**Notable features.**
- **Conditional tasking** (control flow, cycles inside the graph) — rare and genuinely useful.
- **Subflow** / dynamic tasking (spawn sub-graphs during execution).
- **Composition** of reusable graphs; **pipeline** scaffolding.
- **GPU tasking** via CUDA Graph; built-in **TFProf** profiler.
- Backed by academic work (IEEE TPDS 2022); claims to beat TBB FlowGraph and OpenMP tasking in expressiveness and speed.

**My read.** Strengths: the cleanest DAG ergonomics in C++, conditional tasking, drop-in header-only integration, good profiler. Weaknesses: graph-centric model is awkward for fire-and-forget/event-driven game workloads; no thread affinity/priorities; GPU story is CUDA-only.

### 2.5 enkiTS (Doug Binks)

**What it is.** A deliberately **lean**, **zlib**-licensed C/C++11 task scheduler (~2k stars), built for an in-house voxel engine (Avoyd) and widely taught.

**Notable features.**
- **Zero-allocation scheduling.**
- **Pinned tasks** (run on a specific thread) and **external thread** registration.
- **Up to 5 priorities**; **completion actions** (run on finish, no reschedule overhead).
- Both **data parallelism** (`TaskSet` over a range, split across workers) and **task parallelism**, plus "braided parallelism" (tasks spawning tasks).
- Explicit design stance: **optimize for low thread counts first**, then scale; tiny API; C API for FFI.

**My read.** Strengths: small, fast, dependency-light, easy to drop in, pragmatic feature set that maps directly to game needs (pinning, priorities, ranges). Weaknesses: minimalist by choice — no rich DAG/continuation algebra, no built-in blocking-inside-task story beyond braiding; you build higher-level patterns yourself.

### 2.6 Google Marl

**What it is.** A **hybrid thread/fiber** scheduler in C++11 (Google), used in SwiftShader and Dawn (WebGPU). Cross-platform across many architectures.

**Notable features.**
- **Fibers** let a *blocking* task suspend (`Worker::suspend`) and the worker pick up other ready fibers/tasks — so blocking code doesn't idle a hardware thread.
- **Work stealing** + spin-with-yield (`spinForWork`) to avoid OS yields.
- **Bound scheduler** via TLS, so `marl::schedule()` needs no explicit handle.
- Synchronization primitives hold **shared-pointer state captured by value** — explicitly trading a little overhead for safety against tasks outliving their stack frame.

**My read.** Strengths: makes "just block and wait" safe and cheap, which dramatically simplifies call sites; portable. Weaknesses: fibers cost stack memory and bring well-known footguns (TLS, debuggers, stack overflow detection); the shared_ptr-for-safety choice concedes some performance.

### 2.7 Fiber job systems — the Naughty Dog pattern (FiberTaskingLib et al.)

**What it is.** Less a product than an influential **design pattern** from Christian Gyrling's GDC 2015 talk "Parallelizing the Naughty Dog Engine Using Fibers" (shipping *The Last of Us Remastered* at 60 fps). Jobs run on fibers; **atomic counters** express dependencies; a job that waits on a counter yields its fiber, and the worker runs something else. **FiberTaskingLib** (RichieSams) is the best-known open implementation.

**My read.** This pattern is the spiritual ancestor of much modern engine threading. Strength: uniform, low-overhead waiting with natural code (no manual continuation-passing). Weakness: fiber complexity and the same debugging/TLS hazards as Marl; correctness around fiber/thread migration is subtle.

### 2.8 Apple GCD / libdispatch

**What it is.** OS-level concurrency (macOS/iOS), open-sourced as swift-corelibs-libdispatch. You submit blocks to **serial** or **concurrent** queues; the system manages a **lazy, global thread pool** ("root" queues). QoS classes provide priorities; the main queue gives main-thread affinity.

**My read.** Strengths: the easiest concurrency API in wide use, OS-integrated, QoS-aware, near-zero ceremony. Weaknesses: **thread explosion** — blocking work on concurrent queues spawns more threads, sometimes catastrophically; no work-stealing DAG model; not portable; not designed for frame-tight game scheduling.

### 2.9 Rust — Rayon and Tokio (reference points)

- **Rayon** — work-stealing **data parallelism** (`par_iter`, `join`, `scope`); ergonomic, CPU-bound. No async.
- **Tokio** — work-stealing **async-I/O** runtime over stackless futures; cooperative tasks must yield.
- They have **separate pools** that don't share threads; mixing CPU-bound and I/O-bound work across the boundary is a known pain point (and a thread-starvation footgun).

**My read.** A clean illustration of the **CPU-bound vs I/O-bound** split that C++ engines usually fold into one system. Rayon's `join`/`scope` API is a model of ergonomic structured parallelism worth stealing ideas from.

### 2.10 C++26 `std::execution` (senders/receivers)

**What it is.** The standardized async model (P2300, adopted into C++26; NVIDIA **stdexec** is the reference impl). Three abstractions — **schedulers** (where work runs), **senders** (a lazy description of work), **receivers** (the sink) — composed by `connect` into operation states, with a library of composable async algorithms (`then`, `when_all`, `bulk`, …) and **structured concurrency** lifetimes.

**My read.** Strengths: a single composable substrate spanning thread pools, GPUs, and I/O; lifetimes are structured by design; likely the long-term lingua franca that schedulers interoperate through. Weaknesses: **steep** conceptual and compile-time cost; not yet game-proven; the abstraction can obscure the low-level control engines want. Worth designing to be *compatible* with, not necessarily *built on*, today.

### 2.11 Brief mentions (other names, same family)

- **Cilk / Cilk Plus** — the academic origin of practical **work-stealing** ("work-first" principle, `spawn`/`sync`). Dead as a product (dropped from GCC/Clang) but its scheduler theory underlies TBB, Rayon, Taskflow, and UE's deques.
- **OpenMP tasks** — `#pragma omp task`/`taskloop` with a `depend` clause for DAGs. Ubiquitous in HPC/scientific code; compiler-directive ergonomics, less control than a library.
- **HPX** — standards-conforming parallelism extended to **distributed** memory (futures/continuations, global address space). Powerful in HPC, overkill for a single-node engine.
- **Microsoft PPL** (Concurrency Runtime) — `task_group`, `parallel_for`, `concurrency::task` continuations; Windows-centric, conceptually close to TBB.
- **Folly** executors / **Boost.Asio** `thread_pool` / `Boost.Fiber` — production C++ building blocks for future-based pools, I/O dispatch, and fibers respectively.
- **.NET TPL** (Task Parallel Library, `ForkJoinPool`-style work-stealing) and **Java `ForkJoinPool`** — the managed-runtime incarnations; mature work-stealing job managers that shaped a generation of API expectations (`Task.WhenAll`, `CompletableFuture`).

---

## 3. Feature comparison

| | Schedule topology | Blocking model | Dependencies | Alloc/task | Affinity | Priorities | Safety aids | Lang / license |
|---|---|---|---|---|---|---|---|---|
| **UE Tasks** | **Per-worker WS deques + lock-free global overflow**; fg/bg pools | **Spin-then-park** (EventCount) + **oversubscription/standby**; opt-in busy-wait | Prerequisites, nested, events, pipes | **Zero-alloc** intrusive cache-line task (SBO) | **Named threads + affinity** | Yes (5 bands, fg/bg) | Pipes avoid locks; cancellation | C++ / engine |
| **Unity Jobs** | Work-stealing | Main-thread `Complete` sync | `JobHandle` chains | Native containers | Limited | Limited | **Race detector** (best-in-class) | C#/HPC# / engine |
| **oneTBB** | WS, steal-oldest | Continuations | `task_group`, **flow graph** | Heap (scalable alloc) | No | No | Concurrent containers | C++ / Apache-2.0 |
| **Taskflow** | Work-stealing | Continuations / subflow | **Static DAG + conditional** | Heap | No | No | — | C++20 / MIT |
| **enkiTS** | WS, lean | Braiding | Sets + simple deps + completion actions | **Zero-alloc** | **Pinned tasks** | **Up to 5** | — | C/C++11 / zlib |
| **Marl** | WS + spin | **Fibers** | Via blocking waits | Fiber stacks | Bound TLS | — | shared_ptr state | C++11 / Apache-2.0 |
| **Fiber JS (ND)** | WS | **Fibers** + atomic counters | Counters | Fiber stacks | Usually pinned mains | Varies | — | C++ / varies |
| **GCD** | Lazy global pool | Thread explosion | Queue ordering / groups | Block heap | Main queue | QoS classes | — | C / Apache-2.0 |
| **Rayon** | WS | (sync only) | `join`/`scope` | Stack frames | No | No | Rust borrow checker | Rust / MIT-Apache |
| **Tokio** | WS | Stackless futures | `.await` graph | Future state | No | Limited | Borrow checker | Rust / MIT |
| **`std::execution`** | Pluggable schedulers | Senders (lazy) | **Algebra** (`then`/`when_all`) | Op-state (often inline) | Scheduler-defined | Scheduler-defined | **Structured lifetimes** | C++26 / std |

WS = work-stealing.

---

## 4. Performance analysis

- **Topology dominates scaling.** A single mutex-guarded ready-queue serializes every submit and pop; under many producers/consumers it becomes the bottleneck long before the work does. Every system that targets scale uses **per-worker deques + work-stealing**. The steal-oldest/run-newest discipline (TBB, Rayon, Taskflow) is the proven sweet spot: depth-first local execution keeps caches hot, stealing the oldest task grabs the biggest available subtree.
- **Per-task overhead is an allocation story.** Anything built on `std::function`/`move_only_function` pays a heap allocation and an indirect call per task. Lean engine systems (enkiTS, UE) avoid this with inline/intrusive storage and hit single-digit-to-low-tens-of-nanoseconds per task — the difference between "task per object" being viable or not.
- **The blocking model sets the floor on latency under contention.** Fibers (Marl, ND) make a context switch cost tens of ns, so "block and wait" is cheap and code stays linear; the cost is stack memory and cache pressure from many live stacks. Continuation styles (TBB, senders) have zero switch cost but push complexity into the programmer. Oversubscription (UE 5.5, GCD) is simplest but risks thread explosion / scheduling jitter.
- **Unity's lesson:** most of DOTS's speed is **data layout + Burst codegen**, not the scheduler. A scheduler can't rescue cache-hostile data.
- **Wake latency vs throughput is a real tradeoff** (directly visible in this project's own block-vs-spin benchmarks): sleeping workers save power and avoid mutex thrash but pay a wakeup; spinning workers wake instantly but burn cores and contend. Mature systems hybridize: spin briefly, then sleep.

---

## 5. Ease of use analysis

Roughly easiest → hardest to use correctly:

1. **GCD / Rayon** — submit a block / call `par_iter`; the model fits in your head.
2. **Unity Jobs** — the safety system catches your races; the constraint is learning HPC#.
3. **Taskflow** — declarative DAGs read well; great profiler.
4. **UE Tasks** — pleasant *inside* UE; prerequisites + pipes are intuitive; the named-thread legacy is the wrinkle.
5. **enkiTS** — tiny API, but you assemble higher-level patterns yourself.
6. **oneTBB** — powerful but ceremonious; flow graph has a learning curve.
7. **Marl / fiber systems** — linear code, but fiber footguns (TLS, debuggers, stack sizing) bite.
8. **`std::execution`** — the most composable and the most to learn; brutal compile errors today.

**General principle:** ease of use comes from one of two places — *removing a decision* (GCD: no scheduler to configure) or *removing a failure mode* (Unity: races can't compile/run). Expressiveness (TBB, senders) trades against approachability.

---

## 6. Lessons for *this* project

Tying the survey back to the current codebase (single shared `priority_queue` + mutex, single-class workers, intrusive refcount, `block`/`spin` policy):

1. **The shared mutex queue is the #1 scaling limit.** The benchmarks already show `queue_mutex_` contention dominating. The proven path is **per-worker deques with work-stealing** plus a lock-free global overflow queue — exactly what UE's `LowLevelTasks` does (`TWorkStealingQueue2` locals + `FAAArrayQueue` global), and the TBB/Rayon model (push/pop locally, steal from victims). This is the single highest-leverage change.
2. **Keep the zero-alloc instinct, broaden it.** Intrusive refcounting is the right call (matches UE/enkiTS). For fire-and-forget, consider an **inline task payload** (SBO) à la enkiTS so detached tasks don't require user lifetime management.
3. **Decide a blocking story early.** Options, cheapest-to-adopt first: continuation chaining (already have `then`), oversubscription, or fibers. Fibers (Marl/ND) give the nicest call sites but are a large commitment; continuations compose with the existing dependency model with no new runtime machinery.
4. **First-class affinity will be needed.** Engines require pinned/named threads (render, main). enkiTS's pinned-task model is the lean reference; UE's named threads the full one.
5. **Hybridize idle behavior.** The block-vs-spin result argues for **spin-then-sleep**, not a hard either/or — UE spins `WorkerSpinCycles` then parks on an EventCount; Marl uses `spinForWork`. An EventCount (two-phase prepare/commit wait) also avoids the lost-wakeup races a raw semaphore can hit.
6. **Stay `std::execution`-aware.** You don't need to build on senders, but a `schedule`/`then`/`when_all`-shaped surface keeps a future interop bridge cheap.
7. **Borrow the best ease-of-use ideas:** UE **pipes** (lock-free sequential access to shared state) and Unity's **race detection** (even as a debug-build assertion layer) are differentiators that cost users little and prevent whole bug classes.

**The gap in the market this project targets:** UE Tasks-level functionality (prerequisites, pipes, priorities, named threads) with enkiTS-level leanness and zero-alloc, as a standalone library — neither the heavyweight HPC libraries nor the minimal lean ones occupy exactly that point.

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
