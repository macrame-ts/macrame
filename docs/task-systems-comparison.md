# Task / Job Systems: A Comparative Survey

Companion documents: [guide.md](guide.md) is the user guide, [design.md](design.md) describes how this survey's findings shaped the library, and [pattern-farming.md](pattern-farming.md) catalogues the per-feature idea mining that followed it.

This survey covers concurrent task schedulers and async runtimes relevant to game engines and high-performance C++. It was written to inform the design of this library and is revised as the field moves. Each system gets a short statement of what it focuses on, the substance of its design, and an assessment of how it has turned out, based on adoption, issue-tracker themes, and maintenance state. The assessment lens is the set of things this library optimises for: safety (bug classes prevented by design or caught at runtime), performance, high-level parallelism, and built-in support for common state-sharing patterns.

---

## 0. Scope and framing

The same category is known under many names: task system, job system, job manager, scheduler, executor, parallel runtime, dispatch framework, tasking library. A runtime accepts units of work (tasks or jobs), runs them across a managed pool of threads, and offers some way to express dependencies and ordering, plus some way to wait for or react to completion. This survey treats the whole family, regardless of the label a given project chose.

The line is drawn loosely rather than strictly. A bare thread pool with no dependency or completion model sits at the primitive end, and a full async runtime with futures, I/O, and structured concurrency sits at the rich end. Engine "job managers" and HPC "task schedulers" fall in the middle. All are in scope where they inform design. What differs is mostly emphasis (fine-grained compute, I/O concurrency, or frame-tight game work), not kind.

The systems surveyed, grouped by domain:

| Domain | Systems |
|---|---|
| Game engines (proprietary) | Unreal Engine Tasks System (plus the legacy TaskGraph), Unity C# Job System (plus Burst/DOTS) |
| Game engines (open) | Bevy (ECS scheduling and task pools), enkiTS, Google Marl, fiber job systems (FiberTaskingLib, the Naughty Dog pattern) |
| General-purpose C++ | Intel oneTBB, Taskflow (HPX, PPL, and Folly executors in brief) |
| C++ coroutine libraries | cppcoro, folly::coro, concurrencpp, libcoro, Boost.Asio and Boost.Cobalt, Seastar, QCoro |
| Platform runtimes | Apple GCD / libdispatch |
| Language runtimes (reference points) | Go, C# async/await over the .NET TPL, Rust Rayon and Tokio |
| The C++ standard | C++26 `std::execution` (senders/receivers), with its implementations stdexec and libunifex |

---

## 1. The design axes

Everything below is a point in a small number of dimensions. Naming them up front makes the comparison easier to follow.

1. Scheduling topology. A single shared ready queue is simple but contends; per-worker deques with work stealing scale well and are cache-friendly. Nearly every serious system uses work stealing. The classic refinement, from TBB and the Cilk lineage, is to run your own tasks LIFO and depth-first for cache locality but steal the oldest task from a victim, which converts latent parallelism breadth-first.
2. The waiting problem. What happens when a task must wait mid-execution? The mechanisms in use, which combine rather than exclude one another:
   - Continuations and callbacks. Nothing ever waits in place; the rest of the work becomes a new task chained on completion (TBB, GCD, classic futures).
   - Stackful suspension. Fibers or green threads park the whole call stack and let the worker run something else (Marl, the Naughty Dog pattern, FiberTaskingLib, Go's goroutines). Code stays linear, at the cost of stack memory and fiber-specific hazards.
   - Stackless coroutines. A language feature (C++20 `co_await`, C# `async`/`await`, Rust `async`) that compiles the function into a resumable state machine. The language provides only the transformation; a library or runtime supplies the task type and the scheduling around it. This library, Tokio, the .NET runtime, and the C++ coroutine libraries of section 2.14 all live here.
   - Sender composition. `std::execution` describes work as lazy sender pipelines composed before anything runs. This is a composition style, not a separate suspension mechanism: senders interoperate with stackless coroutines in both directions (a sender can be awaited from a coroutine, and a coroutine task is itself a sender), and C++26 ships a coroutine task type inside `std::execution`. Section 2.13 covers the layering.
   - Thread oversubscription. Standby threads wake so a blocked worker does not starve the pool (UE, GCD's lazy pool). Its failure mode is thread explosion.
3. Dependency expression. Some systems take a static DAG declared up front (TBB flow graph, Taskflow). Others chain continuations dynamically (UE prerequisites, futures, senders, `JobHandle` chains). A third family derives ordering from declared data access instead of asking for edges at all (Bevy's ECS scheduler, Unity's container dependencies, this library's conflict-derived graph edges).
4. Allocation model. Every system pays some per-task cost; the differences are where the memory comes from and how often the general-purpose heap is touched. Systems built on `std::function` pay a global-heap allocation and an indirect call per task. Engine systems reduce the cost with dedicated allocators: UE routes task storage through its custom allocator stack, which replaces general-purpose heap traffic with pooled small-block allocation without removing the allocation itself. The leanest designs take allocation off the hot path entirely with caller-owned operation state, intrusive single-allocation task blocks, and inline small-buffer bodies (enkiTS, this library's `access`).
5. Affinity and priorities. Named or pinned threads (render thread, main thread) and priority bands are first-class in engine systems, and weak or absent in HPC libraries and coroutine libraries.
6. Safety. Three postures exist. Leave shared data to the user (TBB, Taskflow, GCD, every C++ coroutine library). Prevent races by construction (Rust's borrow checker, Bevy's declared-access scheduler, research designs like Verona). Detect violations at runtime (Unity's container safety system, Go's race detector, this library's access harness). Structured concurrency, meaning the API makes leaked or orphaned work impossible rather than merely discouraged, belongs to this axis too. It has spread across the industry in the last decade (Trio's nurseries, Kotlin coroutines, Swift concurrency, Java's `StructuredTaskScope`, C++ `async_scope` proposals).
7. Built-in patterns. Whether the system ships the recurring state-sharing idioms (command buffers, double buffering, pub/sub, serialized access lanes) or leaves users to build them. Unity's `EntityCommandBuffer`, Bevy's `Commands`, UE's pipes, and this library's `Deferred`/`Versioned`/`Event_bus` are the pattern-bearing examples. Most general-purpose libraries ship none.

---

## 2. System overviews

### 2.1 Unreal Engine Tasks System (UE5)

A high-performance task system with rich functionality and deep tooling support, carrying the weight of a large legacy codebase.

Two front ends, the modern `UE::Tasks` API and the legacy TaskGraph, sit on one backend: `LowLevelTasks::FScheduler` (`Engine/Source/Runtime/Core/Public/Async/Fundamental/`). The high-level API launches with `Launch(debug_name, callable, priority, flags)` and returns reference-counted handles (`FTask` / `TTask<T>`).

The backend architecture, from source:

- Per-worker work-stealing local queues. Each worker owns a bounded (~1024-slot) `TWorkStealingQueue2` per priority. The owner pushes and pops at the head single-threaded; other workers steal from the tail via atomic CAS. This is the Chase-Lev/Vyukov-style deque, not a shared queue.
- A global overflow queue. A lock-free MPMC `FAAArrayQueue` (Dmitry Vyukov) per priority catches overflow when a local queue is full, work from threads with no local queue, and the drain when a worker exits. Dequeue order is local first, then global, then stealing from a random victim's local queue (hashed PRNG index).
- Two worker pools, foreground and background, each with its own OS thread priority and affinity mask. The priority bands are `High` and `Normal` for the foreground pool and `BackgroundHigh/Normal/Low` for the background pool.
- Idle workers spin, then park. A starved worker spins `WorkerSpinCycles` (53) and then parks on a `FWaitingQueue`, an EventCount (Dmitry Vyukov / Eigen lineage) with a two-phase `PrepareWait`/`CommitWait`/`CancelWait` protocol to avoid lost wakeups. This is the spin-then-sleep hybrid, in production.
- Oversubscription and standby workers. When all workers are parked, for example because a task busy-waits on a dependency, standby workers wake or new threads spin up so cores are not starved and waits cannot deadlock. The mechanism is `IncrementOversubscription`/`Decrement`, plus an event that fires when the limit is reached.
- Complicated allocation through custom allocators. The task carries an inline `TaskDelegate` with small-buffer storage for the body, but tasks are heap-constructed and handles are reference-counted, so allocation remains; it is routed through several of the engine's dedicated allocators rather than the general-purpose heap. The net effect is pooled small-block traffic, not zero allocation.
- An atomic state machine (Ready, Scheduled, Running, with Canceled variants) drives cancellation (`ECancellationFlags`: prelaunch cancellation, try-launch-on-success) and opt-in busy-waiting (`ETaskFlags::AllowBusyWaiting`).
- A TLS context holds the active scheduler and the worker's local queue, so a task launched from within a worker enqueues locally with no global contention.

High-level features over that backend:

- Prerequisites (non-blocking DAG edges), nested tasks (a parent completes after its children), task events (`FTaskEvent`, a manual join/signal), and `TTask<T>` results.
- Pipes (`UE::Tasks::FPipe`): a chain of non-concurrent tasks giving lock-free serialized access to a shared resource, FIFO for tasks without prerequisites. The source comment is explicit: "a replacement for named threads… there can be a large dynamic number of pipes each controlling its own shared resource." The pipe must outlive its last task, and the teardown handshake is delicate: a task completing concurrently with pipe destruction can touch freed pipe memory, an artifact of the lock-free completion notification (this library's pipe does its drain notification under the pipe mutex specifically to be structurally immune to that race).
- Named threads (game/render/RHI) survive from TaskGraph; pipes are the modern, lighter answer to the same need.
- Deep tooling: Unreal Insights, VS visualizers, and CPU profiler trace hooks throughout.

How it has turned out: it ships in Unreal Engine, which accounted for 28% of 2024 Steam releases and 31% of Steam revenue, out-earning Unity for the first time since 2018, with 42% of surveyed developers naming it their primary engine. By source inspection, essentially every scheduler-backend lesson in section 6 is implemented here, plus pipes as a lock-avoidance primitive. The costs sit on the user-facing side. The system is inseparable from the engine and its types (`FTask`, `TCHAR`, the container and allocator stack). Concurrency features accumulated across engine generations coexist rather than replace each other: named threads, the legacy TaskGraph, the modern Tasks API, and pipes are all current answers to overlapping problems, so multithreading logic ends up spread across several abstraction levels and the functionality reads as scattered. The reference-counted handles also add traffic that the leanest competitors avoid.

### 2.2 Unity C# Job System, Burst, and DOTS

A safety-first job system, designed so that developers who are not multithreading specialists can write parallel code that will not race, and it accepts costs in performance ceiling and flexibility to get there.

You write `IJob`/`IJobParallelFor` structs over `NativeArray`/`NativeContainer` data, `Schedule()` them with `JobHandle` dependencies, and call `Complete()` to sync. The Burst compiler turns a C# subset ("HPC#") into LLVM-optimized native SIMD code (claimed 10–100x over Mono). It is part of DOTS alongside the ECS.

Notable design points:

- A work-stealing scheduler across worker threads.
- The container safety system: the editor detects data races and enforces job dependencies at run time. This remains the best-known runtime race detector in a shipping engine, and it makes data-parallel code safe by default for non-experts. The enforcement is editor-only; player builds run without the checks.
- Dynamic acquisition is forbidden by design. Jobs cannot take new data mid-run, `Schedule()`/`Complete()` are main-thread-only, and jobs cannot block, all documented explicitly as deadlock prevention. The costs are no nested fork-join and mandatory main-thread sync points.
- The `EntityCommandBuffer` is the built-in command-list pattern: structural changes are recorded during parallel execution and played back at sync points.
- Tight coupling of data layout (ECS), scheduling, and codegen is where the real performance comes from. The scheduler alone does not provide it.

How it has turned out: Unity accounted for 51% of 2024 Steam releases. The job system itself is broadly used and liked, but the surrounding DOTS/ECS stack has had a long, bumpy maturation: community "ECS Stack Review" threads through 2025 document sustained friction over missing pieces and slow iteration, while Unity's ECS team continues incremental roadmap updates toward the next LTS. The recurring practical complaint about the job system proper is main-thread `Complete()` stalls when dependencies are structured badly. The clearest lesson it exports is that a runtime race detector plus a forbidden-by-construction acquisition model lets non-specialists write parallel code, which is the direction this library's harness generalises.

### 2.3 Bevy

An ECS-first engine whose scheduler derives parallelism from declared data access, with a design that makes the hold-and-wait deadlock shape unrepresentable.

Bevy systems declare their access sets through `SystemParam` at registration, and the multithreaded executor admits a system only when its whole access set is compatible with everything currently running: a conflicting system simply does not start (`conflicting_systems.is_disjoint(&running_systems)`). A system either starts with all of its access or none of it, so there is no state in which a system holds part of its access and waits for the rest. This is conservative two-phase locking, and it is the closest published analogue to this library's conflict-derived graph edges.

What Bevy gave up to get that, and the escape hatches it grew:

- `Commands` and `ApplyDeferred`: structural world changes are recorded during parallel execution and replayed at sync points, the command-buffer pattern as a core primitive.
- Exclusive systems: `&mut World` is the only way to acquire arbitrary data at runtime, and it stops the entire schedule while it runs.
- `ParamSet`: two conflicting parameters in one system are otherwise a runtime panic; `ParamSet` makes them mutually exclusive in time.
- Ambiguity detection: the default scheduler allows systems with conflicting access but no declared order to run in either sequence across frames, which produced real nondeterministic-order bugs. Bevy shipped `ambiguity_detection` diagnostics (warn or error per schedule) after its stageless scheduling rework because users hit this in practice.
- Task pools: a fixed 50/25/25 thread split between Compute, AsyncCompute, and IO pools underutilised cores for apps not doing async IO, and a consolidation into one pool was adopted in 2025 (PR #18163).

Its recorded deadlocks are all in the executor dimension, not the access dimension: blocking `scope()` calls on a one-thread pool (fixed by making the caller participate in the work, PR #892, the same fix as a caller-runs-inline `parallel_for`), and running a nested schedule inside a system, which still hangs in `block_on` (issue #10032). No deadlock caused by the access-declaration model itself appears in the tracker, which is evidence that all-or-nothing admission works.

How it has turned out: Bevy is the most popular Rust game engine by a wide margin (about 45k GitHub stars as of March 2026, with well over a thousand contributors), with real shipped indie titles (Tiny Glade most visibly) and no AAA adoption. The renderer and editor are still maturing, but the scheduling model is the part that has proven out, and it validates deriving the schedule from declared access rather than hand-wired edges.

### 2.4 Intel oneTBB

The reference general-purpose C++ task scheduler for two decades: broad, composable, and industrial, with ergonomics that show their age.

Intel's TBB has been the standard C++ task library since 2006, and is now oneTBB under the UXL Foundation (since 2023), licensed Apache-2.0. It pairs a work-stealing task scheduler with high-level algorithms (`parallel_for`/`reduce`/`scan`/`pipeline`), `task_group`, a flow graph (data-flow and dependency graphs with buffering, conditionals, and cycles), concurrent containers, and a scalable allocator.

Notable design points:

- Canonical steal-oldest work stealing (breadth-first parallelization, depth-first local execution).
- The flow graph is the most general dependency and data-flow model in mainstream C++.
- It is composable, and underpins much of oneAPI (MKL, oneDNN) and broad HPC code.
- Development continues through a public RFC process. Dynamic non-tree task DAGs on `task_group` via `task_handle` shipped as a preview feature, and NUMA support and flow-graph serializers are in the proposal pipeline.

How it has turned out: it remains the de-facto industrial standard for shared-memory C++ parallelism, ubiquitous in HPC, rendering (Embree), and scientific computing, and the move to foundation governance kept releases flowing after Intel's oneAPI reorganisation. Its weaknesses for engine use are unchanged: no first-class thread affinity or priorities, and an abstraction tax in latency-sensitive fine-grained workloads compared to lean game schedulers. The ergonomics are a real cost, not just a style complaint. Flow-graph code is verbose and ceremony-heavy, the scheduler's behaviour is hard to learn well enough to predict, and the library is easy to hold wrong, which in practice restricts confident use to specialists.

### 2.5 Taskflow

A task-graph front end first: the cleanest DAG ergonomics in C++, over an academically grounded work-stealing runtime.

Taskflow is a header-only, MIT-licensed C++20 task-graph library. It pairs a work-stealing executor with an expressive graph front end.

Notable design points:

- Conditional tasking (control flow and cycles inside the graph), which is rare and genuinely useful.
- Subflow, or dynamic tasking: sub-graphs can be spawned during execution.
- Composition of reusable graphs, pipeline scaffolding, and `corun`, which lets a caller execute queued work while waiting (busy waiting on the pool's own tasks rather than parking).
- The strong/weak dependency semantics around conditional tasking carry documented traps: a strong back-edge can deadlock, and mixed strong-plus-weak in-edges can double-schedule a task. The docs are explicit about both.
- GPU tasking via CUDA Graph, and a built-in profiler, TFProf.
- It is backed by sustained academic work (IEEE TPDS 2022, conference awards through 2025).

How it has turned out: adoption is real and growing (about 12k GitHub stars as of mid-2026, the project cites over 1.5M downloads, and it runs Google Summer of Code projects), with uptake concentrated in research, EDA, and compute tooling rather than games. The graph-centric model is awkward for fire-and-forget or event-driven workloads, there is no thread affinity or priority support, and the GPU story is CUDA-only. Within its niche, the declarative DAG plus profiler combination is the usability benchmark other graph libraries get measured against.

### 2.6 enkiTS

Deliberate minimalism: a small, fast, zero-allocation scheduler that provides the primitives and leaves higher-level patterns to the user.

enkiTS is a zlib-licensed C/C++11 task scheduler by Doug Binks, built for an in-house voxel engine (Avoyd) and widely taught and embedded.

Notable design points:

- Zero-allocation scheduling.
- Pinned tasks (run on a specific thread) and external thread registration (`RegisterExternalTaskThread`, `WaitForNewPinnedTasks`), which together answer the "who owns the waiting for outside completions" question that most lean schedulers ignore.
- Up to 5 priorities, and completion actions that run on finish with no reschedule overhead.
- Both data parallelism (a `TaskSet` over a range, split across workers) and task parallelism, plus "braided parallelism" (tasks spawning tasks).
- An explicit design stance: optimize for low thread counts first, then scale; a tiny API; a C API for FFI.

How it has turned out: it has held a steady couple of thousand GitHub stars with low-volume, single-author maintenance for a decade, and it keeps getting picked for projects that want a scheduler without a framework. The minimalism is the point and also the limit: there is no dependency algebra beyond simple chaining, and no waiting story beyond braiding, so anything like the patterns in section 1 axis 7 is the user's job.

### 2.7 Google Marl

A hybrid thread/fiber scheduler built to make blocking cheap; archived by Google in April 2026.

Marl is C++11, from Google, used in SwiftShader and Dawn (WebGPU), and cross-platform across many architectures.

Notable design points:

- Fibers let a blocking task suspend (`Worker::suspend`) while the worker picks up other ready fibers and tasks, so blocking code does not idle a hardware thread. `marl::blocking_call` shunts a genuinely blocking call onto a throwaway thread and suspends the calling fiber.
- Work stealing plus spin-with-yield (`spinForWork`) before parking.
- A bound scheduler via TLS, so `marl::schedule()` needs no explicit handle.
- `marl::Ticket` and `Ticket::Queue`: a turnstile that hands out ordered tickets so work can execute in acquisition order without holding a lock, decoupling ordering from data (a standalone cousin of a FIFO access pipe).
- Synchronization primitives hold shared-pointer state captured by value, an explicit trade of overhead for safety against tasks outliving their stack frames.

How it has turned out: the repository was archived on 2026-04-27 and is read-only. The design remains instructive (it made "just block and wait" safe and cheap, and it is one of the cleaner fiber schedulers to read), but the archive is the maintenance-risk lesson: infrastructure tied to a single corporate sponsor's internal needs stops when those needs do. Fibers also carry their standing costs: stack memory, TLS surprises, and debugger hostility.

### 2.8 Fiber job systems: the Naughty Dog pattern (FiberTaskingLib et al.)

The design that made fibers the default answer in AAA engine threading for a decade: jobs run on fibers, dependencies are atomic counters, and waiting yields the fiber instead of the thread.

This is less a product than an influential pattern from Christian Gyrling's GDC 2015 talk "Parallelizing the Naughty Dog Engine Using Fibers" (shipping *The Last of Us Remastered* at 60 fps). A job that waits on a counter yields its fiber so the worker can run something else. FiberTaskingLib (RichieSams) is the best-known open implementation, with a `WaitGroup` counter and a fiber-aware mutex (`Fibtex`).

How it has turned out: the pattern is the ancestor of much modern engine threading, and its atomic-counter wait groups reappear everywhere (Marl's `WaitGroup`, Go's `sync.WaitGroup`, this library's `Signal` is the same role). Its weaknesses are fiber complexity, TLS and debugging hazards, and subtle correctness around fiber-to-thread migration. Stackless coroutines now deliver the same linear waiting code without hand-rolled context switching, which is the direction newer designs, this library included, have taken.

### 2.9 Apple GCD / libdispatch

OS-integrated concurrency with the least ceremony in wide use, whose lazy thread pool made "just dispatch it" work until blocking made it explode.

GCD is OS-level concurrency on macOS and iOS, open-sourced as swift-corelibs-libdispatch. You submit blocks to serial or concurrent queues, and the system manages a lazy, global thread pool (the "root" queues). QoS classes provide priorities, and the main queue gives main-thread affinity. Serial queues are the built-in serialized-access lane, the same job pipes and this library's per-object pipe do with more structure.

How it has turned out: it remains the easiest concurrency API in wide deployment, and its failure mode is equally well documented: blocking work on concurrent queues spawns threads until the process thrashes (thread explosion), to the point that Apple's own guidance and WWDC sessions teach queue-hierarchy discipline to avoid it. Apple's forward direction is Swift concurrency (a cooperative fixed-width pool with actors and structured tasks), which is a direct correction of exactly that failure mode. There is no dependency-graph model and no portability story for engine-style scheduling.

### 2.10 Go

Concurrency as a language feature: goroutines make spawning nearly free, channels make communication the synchronization, and the runtime hides the scheduler entirely.

Go is the reference point for what full language integration buys. The runtime's G/M/P scheduler multiplexes goroutines (stackful green threads with small, growable stacks) over OS threads: each logical processor P owns a local run queue, idle Ps steal half a victim's queue, a global queue is polled periodically to prevent starvation, and since Go 1.14 long-running goroutines are preempted asynchronously via signals on top of cooperative yield points. A goroutine entering a blocking syscall hands its P to another thread, so blocking is safe by construction rather than by programmer discipline. The pattern vocabulary lives in libraries: `context` propagates deadlines and cancellation down call trees (a child can shorten the budget, never extend it), `errgroup` gives fail-fast sibling cancellation, `select` is the race/timeout combinator, and `sync.Pool` shards free lists per-P, the same TLS-keyed sharding this library uses for its journal lanes.

On the safety axes the picture is mixed. Data races remain possible and the race detector is opt-in. Structured concurrency was proposed for the language (issue #29011) and closed as unadoptable, so leaked goroutines are a real, tooling-acknowledged bug class (`goleak` exists for a reason), and libraries fill the structure gap. The runtime carries an internal lock-ranking checker (`lockrank`), and it is off by default. Nothing exposes priorities, affinity, or the scheduler itself.

How it has turned out: goroutines plus channels are probably the most successful concurrency ergonomics in industry, and the model dominates I/O-concurrent server software. It is not a fit for frame-tight compute (GC pauses, no priorities, opaque scheduling), which is why game engines borrow its ideas (the handoff idle policy in this library's scheduler is Go's spinner-handoff design) rather than its runtime. The exportable lessons are that ergonomics come from removing the scheduler from view, and that unstructured spawning, however cheap, creates a leak class that libraries then have to police.

### 2.11 C# async/await over the .NET TPL

The most widely deployed async model in industry: eager tasks and compiler-generated state machines, with a decade of documented sharp edges as the price of retrofitting async onto a synchronous ecosystem.

C# `async` methods are eager: they run synchronously until the first incomplete `await`, and awaiting an already-completed awaitable never suspends, the same eager discipline this library's `Task` follows. Beneath them, the Task Parallel Library provides the work-stealing thread pool that shaped a generation of API expectations (`Task.WhenAll`, continuations). Resumption is governed by a captured `SynchronizationContext`/`TaskScheduler`, and `ExecutionContext`/`AsyncLocal` state flows implicitly across suspensions, an implicit cousin of an explicit grant snapshot. The performance story matured well: `ValueTask<T>` gives a zero-allocation path for synchronously available results, and poolable `IValueTaskSource` backing extends that to suspending operations, with the corresponding contract that a `ValueTask` is awaited at most once. `System.Threading.Channels` ships bounded/unbounded channels with single-reader/single-writer fast paths, and `TaskCompletionSource` plays the manual-completion role of this library's `Signal`, with a famous footgun: by default, continuations run synchronously on the completing thread, so completing from an OS callback can hijack that thread into arbitrary user code, and `RunContinuationsAsynchronously` is near-universal best practice.

The sharp edges are as instructive as the successes. Blocking on async work (`.Result`, `.Wait()`) from a context the continuation needs deadlocks, the classic sync-over-async trap. Library code carries `ConfigureAwait(false)` boilerplate on every await to avoid context capture. `Task.WhenAny` does not cancel the losers, so abandoned tasks keep running and leak side effects. Cancellation is exception-based (`OperationCanceledException`) over cooperative tokens, and the async keyword colors every function it touches, spreading virally up call stacks.

How it has turned out: the model works at enormous scale and is the strongest evidence that eager tasks plus language-level await is a viable mainstream shape. The recurring failures cluster precisely at the boundary between sync and async code, which is evidence for making that boundary explicit and enforced rather than conventional. This library's rule that an in-task blocking `sync()` is fatal is the checked version of the discipline C# documents but cannot enforce.

### 2.12 Rust: Rayon and Tokio

Two runtimes for two problems: Rayon for CPU-bound structured data parallelism, Tokio for async I/O, with a well-documented pain seam between them.

- Rayon provides work-stealing data parallelism (`par_iter`, `join`, `scope`). It is ergonomic and CPU-bound, with no async support. Its `join`/`scope` API remains a model of structured parallelism.
- Tokio is a work-stealing async-I/O runtime over stackless futures; tasks are cooperative and must yield. `JoinSet` joins a dynamic set of tasks in completion order. The `select!` macro is the race combinator, and its documented cancellation-safety hazard is instructive: a losing branch's future is dropped mid-await, so state pulled out of a channel but not yet committed is silently lost. Cancellation semantics stay hard even with a borrow checker.
- The two have separate pools that do not share threads. Mixing CPU-bound and I/O-bound work across the boundary is a known pain point and a thread-starvation hazard.

How it has turned out: both are the settled defaults of their language niche, and Rust's compile-time data-race prevention remains the standout safety result in the whole survey. The split-runtime experience is a clean illustration of the CPU-bound versus I/O-bound divide that C++ engines usually fold into one system, and of the integration cost when the two worlds are separate products.

### 2.13 C++26 `std::execution`, stdexec, and libunifex

The standard's answer to async composition: a lazy sender algebra with structured lifetimes, adopted into C++26, still early in tooling, documentation, and ergonomics.

The layering matters, because the pieces are often conflated. Senders/receivers (P2300) are a composition vocabulary: schedulers say where work runs, senders are lazy descriptions of work, receivers are the sink, and `connect` produces caller-owned operation states, with algorithms (`then`, `when_all`, `bulk`) composing pipelines before anything runs. C++20 coroutines are a language feature at a different layer, and the two interoperate by design: a sender can be awaited inside a coroutine (`as_awaitable`), a coroutine task is itself a sender, and C++26 additionally ships `std::execution::task`, a coroutine type living inside the sender world. stdexec is NVIDIA's reference implementation of all of this; libunifex is Meta's earlier implementation of the same model, in heavy production use (as of 2023 it served the async machinery of Meta's rsys video-calling stack across billions of monthly calls). Two mechanical findings from source inspection are worth recording: stdexec resolves the inline-versus-async completion race with the same two-state atomic exchange handshake this library's resume trampoline uses, and ambient context (stop tokens, scheduler affinity) flows through the receiver's queryable environment rather than explicit parameters, so cancellation propagates into every awaited sender with zero plumbing at the call site.

How it has turned out: the standard was finalized with C++26 in March 2026, so the model has won the committee argument. Adoption evidence is thinner. stdexec sits at about 2.3k GitHub stars (2026), early-adopter reports praise the structured lifetimes and criticise the documentation gaps, compile times, and cryptic diagnostics, and the late-cycle `std::execution::task` drew formal design objections (P3801) about a feature landing after the freeze. There is no game-proven record. The practical posture for an engine-focused library is unchanged: stay shaped so an interop bridge is cheap, and do not build on it yet.

### 2.14 The C++ coroutine library landscape

C++20 shipped coroutines as a bare language feature with no library support, and a generation of libraries filled the gap. They differ mainly on three questions: eager or lazy tasks, how resumption finds an executor, and how cancellation is plumbed. None of them addresses shared-state safety; they compose completions, and protecting data remains the user's job.

- cppcoro (Lewis Baker) is the archetype the others define themselves against. Everything is lazy (`initial_suspend` suspends always), awaiting uses symmetric transfer in both directions, and the awaitable synchronization family (`async_manual_reset_event`, `async_auto_reset_event`, `async_latch`, `async_mutex`) established the canonical mechanic: one atomic word that is either "set" or the head of an intrusive waiter list whose nodes live in the coroutine frames, so a wait costs zero heap. Laziness buys safety (a constructed-and-dropped task never runs and cannot outlive its captures) at the cost of a mandatory suspend/resume round trip even when the result is already available. The library is effectively dormant: the author moved on to folly::coro and libunifex years ago, and the tracker's own issues ask whether anyone still maintains it.
- folly::coro is Meta's production coroutine library, graduated out of `folly/experimental`. Tasks are lazy and executor-sticky by design: a `Task` always resumes on the executor it was launched on, even when the awaited work completed elsewhere, and cannot be awaited without one (`co_withExecutor`). `collectAll` cancels siblings on the first failure, and its `collectAny` merges cancellation tokens so a parent's cancellation and the race's own loser-cancellation compose. It is battle-tested at Meta scale, and it brings all of folly with it as a dependency.
- concurrencpp (David Haim) pairs an eager `result<T>` with a lazy `lazy_result<T>` over a single-atomic producer/consumer rendezvous, so awaiting a ready result costs one failed CAS and no reschedule. A fused promise-plus-state single allocation (convergent with this library's frame/block fusion), a background `timer_queue` thread that bridges wall-clock time into executors, and a deliberately non-recursive `async_lock` round out a clean design. It remains a single-maintainer project with modest adoption.
- libcoro (Josh Baldwin) covers tasks, executors, synchronization, and networking, and is actively maintained (v0.15.0, June 2025), with modest adoption.
- Boost.Asio is the C++ I/O framework first and grew coroutine support onto its executor model. Its typed cancellation is the notable design point: three cancellation strengths (`terminal`, `partial`, `total`), where cancellation simply refuses to happen if the operation cannot honour the requested strength, trading silent unsafety for explicit no-ops. Boost.Cobalt is single-threaded-per-executor by design, a different safety basis (no data races because no parallelism within an executor), and its `race` first tries a non-destructive interrupt of the losers before escalating to cancellation.
- Seastar is the shard-per-core extreme: one shard owns its data, so its primitives are lock-free by architecture rather than by algorithm, coroutines are eager, and cooperative preemption runs on a task quota (~500 µs) checked at await points. It underpins ScyllaDB and Redpanda, production evidence that the shared-nothing shape works when the whole application commits to it.
- QCoro bridges coroutines into Qt. Its eager task resumes waiters by looping `resume()` with no symmetric transfer or trampoline, which risks stack growth on deep synchronous chains, a useful negative example for eager-task design.

How the landscape has turned out: lazy tasks won the numbers (cppcoro, folly::coro, Asio, Cobalt's `task`, every sender library), while the eager camp (concurrencpp's `result`, Cobalt's `promise`, Seastar, C#, this library) keeps being reinvented where fire-and-forget dispatch and never-suspending-on-settled-work matter more than construct-then-compose. The maintenance pattern is also a finding in itself: the field's foundational library went dormant, the production-grade options are welded to large frameworks (folly, Qt, Asio), and the independents are single-maintainer projects. A free-standing `async_mutex`, which most of these ship, re-opens exactly the manual lock-ordering deadlock class that ordered, declaration-derived access exists to remove.

### 2.15 Brief mentions (other names, same family)

- Cilk / Cilk Plus is the academic origin of practical work stealing (the "work-first" principle, `spawn`/`sync`). It is dead as a product (dropped from GCC and Clang), but its scheduler theory underlies TBB, Rayon, Taskflow, and UE's deques.
- OpenMP tasks: `#pragma omp task`/`taskloop` with a `depend` clause for DAGs. Ubiquitous in HPC and scientific code, with compiler-directive ergonomics and less control than a library.
- HPX extends standards-conforming parallelism to distributed memory (futures and continuations, a global address space, `hpx::dataflow` as the "invoke when inputs are ready" combinator). It is strong in HPC and overkill for a single-node engine. Its `async_rw_mutex` independently reinvents the reader/writer access pipe, one of several such convergences (Folly's `coro::SharedMutexFair`, PPL's `overwrite_buffer`) that validate the pattern.
- Microsoft PPL (the Concurrency Runtime) offers `task_group`, `parallel_for`, and `concurrency::task` continuations. Windows-centric, conceptually close to TBB, and effectively in maintenance.
- Boost.Fiber and Folly's executors are production building blocks for fibers and future-based pools respectively.
- The managed-runtime relatives beyond C#: Java's `ForkJoinPool` (the work-stealing ancestor of the TPL's pool), Project Loom's virtual threads (stackful green threads arriving twenty years later with structured `StructuredTaskScope` attached), and Kotlin coroutines, which made structured concurrency the default rather than an option. Swift concurrency (actors over a cooperative fixed-width pool) is Apple's correction of GCD's thread explosion. Together they show the industry converging on structure-by-default, the same conclusion this library's implicit task scope and nested-run containment encode.

---

## 3. Feature comparison

| | Schedule topology | Waiting model | Dependencies | Shared-state safety | Built-in patterns | Affinity / priorities |
|---|---|---|---|---|---|---|
| **UE Tasks** | Per-worker WS deques + lock-free global overflow; fg/bg pools | Spin-then-park EventCount + oversubscription; opt-in busy-wait | Prerequisites, nested, events, pipes | None (pipes avoid locks by convention) | Pipes; render graph elsewhere in engine | Named threads, 5 bands, fg/bg |
| **Unity Jobs** | Work-stealing | Main-thread `Complete` sync | `JobHandle` chains + container conflicts | Runtime race detector (editor-only) | `EntityCommandBuffer` | Limited |
| **Bevy** | WS task pools over ECS executor | Async tasks; systems never hold-and-wait | Derived from declared access + explicit order | All-or-nothing admission; ambiguity detection | `Commands` buffers, `ParamSet` | No |
| **oneTBB** | WS, steal-oldest | Continuations; `task_group::wait` runs queued work | `task_group`, flow graph | Concurrent containers only | Flow-graph buffering nodes | No |
| **Taskflow** | Work-stealing | Continuations, subflow, `corun` | Static DAG + conditional | None | Pipeline scaffolding | No |
| **enkiTS** | WS, lean | Braiding | Sets, simple deps, completion actions | None | None (by design) | Pinned tasks, 5 priorities |
| **Marl** (archived) | WS + spin | Fibers | Blocking waits, `WaitGroup`, `Ticket` | shared_ptr-by-value state | `Ticket::Queue` ordering lane | Bound TLS |
| **GCD** | Lazy global pool | Blocks; thread explosion risk | Queue ordering, groups | None | Serial queues | Main queue, QoS classes |
| **Go** | Per-P runqueues + steal half; global injection | Goroutines park; syscalls hand off the P | Channels, `context`, `errgroup` | Opt-in race detector; `lockrank` off by default | Channels, `sync.Pool` | No |
| **.NET TPL / async** | WS thread pool | Stackless `await`; sync-over-async deadlocks | `WhenAll`/`WhenAny`, dataflow | None (analyzers only) | Channels, `IAsyncEnumerable` | SynchronizationContext |
| **Rayon** | WS | Structured `join`/`scope` (sync only) | `join`/`scope` | Borrow checker (compile-time) | `par_iter` algebra | No |
| **Tokio** | WS | Stackless futures | `.await`, `JoinSet`, `select!` | Borrow checker (compile-time) | Channels, streams | Limited |
| **`std::execution`** | Pluggable schedulers | Lazy senders; coroutine interop | Algebra (`then`/`when_all`) | Structured lifetimes | None | Scheduler-defined |
| **This library** | Per-worker Chase-Lev deques + per-priority lock-free MPMC overflow | `co_await` only; in-task blocking is fatal (checked) | Derived from declared access + explicit `after`/`before` | Runtime harness, grant epochs, waits-for cycle + deadlock net, rank checks | `Deferred`, `Versioned`, `Event_bus`, pipes, `parallel_for` | 3 priorities; no pinning yet |

WS = work-stealing.

---

## 4. Performance analysis

- Topology dominates scaling. A single mutex-guarded ready queue serializes every submit and pop; under many producers and consumers it becomes the bottleneck long before the work does. Every system that targets scale uses per-worker deques with work stealing. The steal-oldest/run-newest discipline (TBB, Rayon, Taskflow, Go) is the proven sweet spot: depth-first local execution keeps caches hot, and stealing the oldest task grabs the biggest available subtree.
- Per-task overhead is an allocation story, with a spectrum rather than a binary. Anything built on `std::function` pays a general-heap allocation and an indirect call per task. Dedicated allocators (UE) turn that into pooled small-block traffic without removing it. Zero-allocation designs (enkiTS, caller-owned operation state, single-allocation intrusive blocks) reach single-digit to low-tens-of-nanoseconds per task, which is the difference between "a task per object" being viable or not.
- The waiting model sets the floor on latency under contention. Fibers make a context switch cost tens of nanoseconds, so waiting is cheap and code stays linear, at the price of stack memory and cache pressure from many live stacks. Stackless coroutines get the same linear code with one-allocation frames, and eager tasks avoid any suspension when awaiting already-settled work. Pure continuation styles have zero switch cost but push the restructuring onto the programmer. Oversubscription is the simplest and risks thread explosion and scheduling jitter.
- Unity's lesson is that most of DOTS's speed comes from data layout and Burst codegen, not the scheduler. A scheduler cannot rescue cache-hostile data.
- Wake latency versus throughput is a real tradeoff, directly visible in this library's own idle-policy benchmarks. Parked workers save power and avoid queue thrash but pay a wakeup syscall; spinning workers wake instantly but burn cores. Mature systems hybridize (spin briefly, then park), and Go's refinement, promoting a designated spinner and paying the wake syscall only on the fully-parked transition, moves the wake cost off the producer (this library ships both shapes as `spin_then_block` and `handoff`).

---

## 5. Ease of use analysis

Roughly ordered from easiest to hardest to use correctly:

1. GCD, Rayon, Go. Submit a block, call `par_iter`, or say `go`; the model fits in your head because there is almost no model to hold.
2. C# async/await. Natural to read and write until the sync/async boundary, where the deadlocks and `ConfigureAwait` boilerplate live.
3. Unity Jobs. The safety system catches races at runtime; the constraint is learning HPC# and structuring around sync points.
4. Taskflow. Declarative DAGs read well, and the profiler is good.
5. Bevy. Inside the ECS, parallelism is free and safe; the learning curve is the ECS itself, and ambiguity warnings need attention.
6. enkiTS. A tiny API, but you assemble higher-level patterns yourself.
7. UE Tasks. Capable, but genuinely hard to learn. Concurrency spans several coexisting abstraction levels (named threads, the legacy TaskGraph, the modern Tasks API, pipes), so the functionality is scattered and overlapping, and pipes in particular take real study before they read naturally. Discoverability suffers more than any single feature.
8. oneTBB. Capable but ceremonious; the flow graph has a steep learning curve and is easy to hold wrong.
9. Marl and other fiber systems. The code is linear, but fiber hazards (TLS, debuggers, stack sizing) bite.
10. `std::execution`. The most composable and the most to learn, with cryptic compile errors today.

A general principle: ease of use comes from one of two places. Either a decision is removed (GCD has no scheduler to configure, Go has no scheduler to see) or a failure mode is removed (Rust races do not compile, Unity races trip the editor's detector, this library's undeclared access faults at the harness). Expressiveness (TBB, senders) trades against approachability. The systems that aged best on this axis picked one of the two removals and committed to it.

---

## 6. Lessons and where they landed

The survey originally produced a lesson list against the 2026-06 baseline (a single mutex-guarded `priority_queue`, single-class workers, no blocking story). Most of it has since been implemented, so the list is kept with its outcomes; see [design.md](design.md) for the full rationale trail.

1. The shared mutex queue is the number one scaling limit. Implemented: per-priority lock-free MPMC queues plus per-worker Chase-Lev deques, the UE/TBB/Rayon topology.
2. Keep the zero-allocation instinct and broaden it. Largely implemented: monomorphic single-allocation task blocks, caller-owned `Access_op` state, frame/block fusion for coroutines. A tunable small-buffer callable for the residual heap cases is still open.
3. Decide a waiting story early. Settled beyond the original options: the coroutine-first transformation made `co_await` the one composition mechanism, deleted `then`/`when_all` in favour of coroutine composition, and made an in-task blocking `sync()` fatal in checked builds. The survey's continuation-vs-fiber dilemma dissolved once stackless coroutines provided linear code without fiber machinery.
4. First-class affinity will be needed. Still open, and the comparison keeps it honest: pinned tasks are the clearest remaining functional gap against enkiTS, UE, and Marl. Sequenced after the platform-integration layer.
5. Hybridize idle behavior. Implemented: `spin_then_block` (UE-style EventCount) and `handoff` (Go-style spinner promotion) as per-instance policies.
6. Stay `std::execution`-aware without building on it. Unchanged posture, reinforced by the adoption evidence in 2.13.
7. Borrow the best ease-of-use ideas: UE pipes and Unity's race detection. Implemented and extended. The per-object pipe became the core access serializer, and the harness went beyond Unity's detector: access checking in all checked builds (not editor-only), grant-window epochs, waits-for cycle detection, and a quiescence-based deadlock net.
8. (Added after the wider sweep.) Independent reinvention is validation. HPX's `async_rw_mutex`, Folly's `coro::SharedMutexFair`, PPL's `overwrite_buffer`, and Kotlin's `StateFlow` are the pipe, the pipe again, `Versioned`, and `Versioned` respectively, each grown independently under the same pressure. The pattern layer is not idiosyncratic.
9. (Added after the deadlock field survey.) Most peers forbid what this library checks. Bevy, Unity, StarPU, OpenMP, and Kokkos all prohibit dynamic mid-task acquisition outright; Bevy's tracker shows no access-model deadlocks as a result, and StarPU returns `-EDEADLK` for exactly the "acquire from inside a task" shape. This library permits awaited dynamic access as a residual and polices it with declared ranks and runtime cycle detection, a deliberate middle ground whose cost is carrying the checks.

The gap this library targets is unchanged by the wider survey: UE-Tasks-level functionality with enkiTS-level leanness as a standalone library, plus a combination none of the surveyed systems has in one place: access declared per object, a schedule derived from the declarations, a runtime harness that faults on undeclared access, and the recurring state-sharing patterns shipped as first-class types.

---

## Sources

- [Tasks Systems in Unreal Engine — Epic Developer Documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-systems-in-unreal-engine)
- [Tasks Systems in Unreal Engine 5.3 — docs.unrealengine.com](https://docs.unrealengine.com/5.3/en-US/tasks-systems-in-unreal-engine/)
- [Task Graph Insights in Unreal Engine 5](https://dev.epicgames.com/documentation/en-us/unreal-engine/task-graph-insights-in-unreal-engine-5)
- [Unity — DOTS](https://unity.com/dots)
- [Unity Manual — Job system overview](https://docs.unity3d.com/6000.3/Documentation/Manual/job-system-overview.html)
- [Unity Burst and the kernel theory of game performance — Sebastian Schöner](https://blog.s-schoener.com/2024-12-12-burst-kernel-theory-game-performance/)
- [Unity Discussions — ECS Stack Review, January 2025](https://discussions.unity.com/t/january-2025-full-ecs-stack-review/1579739)
- [Unity Discussions — ECS Stack Review, November 2025](https://discussions.unity.com/t/november-2025-ecs-stack-review/1694077)
- [Bevy — Ambiguous system ordering, Discussion #1312](https://github.com/bevyengine/bevy/discussions/1312)
- [Bevy — scope() deadlock fixed by caller participation, PR #892](https://github.com/bevyengine/bevy/pull/892)
- [Bevy — nested schedule hang, Issue #10032](https://github.com/bevyengine/bevy/issues/10032)
- [Bevy — Consolidate task pools, PR #18163](https://github.com/bevyengine/bevy/pull/18163)
- [Bevy in production — curated list](https://github.com/Vrixyz/bevy_awesome_prod/)
- [Threading Building Blocks — Wikipedia](https://en.wikipedia.org/wiki/Threading_Building_Blocks)
- [Parallelizing Data Flow and Dependency Graphs — Intel oneTBB docs](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/parallelizing-data-flow-and-dependency-graphs.html)
- [How Task Scheduler Works — Intel oneTBB docs](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/how-task-scheduler-works.html)
- [oneTBB — UXL Foundation repository](https://github.com/uxlfoundation/oneTBB)
- [Work stealing — Wikipedia](https://en.wikipedia.org/wiki/Work_stealing)
- [Taskflow — GitHub](https://github.com/taskflow/taskflow)
- [Taskflow — project site](https://taskflow.github.io/)
- [enkiTS — GitHub](https://github.com/dougbinks/enkiTS)
- [Google Marl — GitHub (archived 2026-04)](https://github.com/google/marl)
- [Marl scheduler design — docs/scheduler.md](https://github.com/google/marl/blob/main/docs/scheduler.md)
- [Parallelizing the Naughty Dog Engine Using Fibers (GDC 2015), Christian Gyrling — slides](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf)
- [FiberTaskingLib — GitHub](https://github.com/RichieSams/FiberTaskingLib)
- [Grand Central Dispatch internals — newosxbook.com](https://newosxbook.com/articles/GCD.html)
- [swift-corelibs-libdispatch — GitHub](https://github.com/apple/swift-corelibs-libdispatch/blob/master/dispatch/queue.h)
- [Go Scheduler — deep-dive writeup](https://nghiant3223.github.io/2025/04/15/go-scheduler.html)
- [Go — structured concurrency proposal, closed, Issue #29011](https://github.com/golang/go/issues/29011)
- [C# Async Antipatterns — Mark Heath](https://markheath.net/post/async-antipatterns)
- [Synchronous wrappers for asynchronous methods — Microsoft Learn](https://learn.microsoft.com/en-us/dotnet/standard/asynchronous-programming-patterns/synchronous-wrappers-for-asynchronous-methods)
- [Mixing rayon and tokio — Lobsters discussion](https://lobste.rs/s/mebxps/mixing_rayon_tokio_for_fun_hair_loss)
- [NVIDIA stdexec (`std::execution` reference impl) — GitHub](https://github.com/nvidia/stdexec)
- [Senders/Receivers: An Introduction — isocpp.org](https://isocpp.org/blog/2025/01/senders-receivers-an-introduction-lucian-radu-teodorescu)
- [Execution control library (since C++26) — cppreference](https://en.cppreference.com/cpp/execution)
- [C++26 is done! Trip report, March 2026 ISO C++ meeting — Herb Sutter](https://herbsutter.com/2026/03/29/c26-is-done-trip-report-march-2026-iso-c-standards-meeting-london-croydon-uk/)
- [Concerns about the design of std::execution::task — P3801R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3801r0.html)
- [Trying out C++26 executors — Mathieu Ropert](https://mropert.github.io/2025/11/21/trying_out_stdexec/)
- [libunifex — GitHub](https://github.com/facebookexperimental/libunifex)
- [cppcoro — GitHub](https://github.com/lewissbaker/cppcoro)
- [cppcoro development seems pretty dead — Issue #170](https://github.com/lewissbaker/cppcoro/issues/170)
- [folly::coro — GitHub](https://github.com/facebook/folly/tree/main/folly/coro)
- [concurrencpp — GitHub](https://github.com/David-Haim/concurrencpp)
- [libcoro — GitHub](https://github.com/jbaldwin/libcoro)
- [Boost.Cobalt — documentation](https://www.boost.org/doc/libs/release/libs/cobalt/doc/html/index.html)
- [Seastar — project site](https://seastar.io/)
- [HPX — STE||AR Group](https://hpx.stellar-group.org/)
- [The Big Game Engines Report 2025 — Sensor Tower](https://sensortower.com/blog/the-big-game-engines-report-of-2025)
- [Video Game Insights: Game Engines on Steam in 2025](https://gamedevreports.substack.com/p/video-game-insights-game-engines)
