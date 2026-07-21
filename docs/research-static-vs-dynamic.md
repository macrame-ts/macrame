# Static Task Graphs vs. Dynamic Task Spawning: Evidence Review

*Research compiled 2026-07. Companion to [task-systems-comparison.md](task-systems-comparison.md)
(system-by-system survey) and [design.md](design.md). Purpose: ground the
`Static_task_graph` layer's positioning in what other ecosystems actually measured,
complained about, and converged on — user testimonials, issue trackers, papers with
numbers, and conference talks, not marketing claims. Findings are mapped onto this
library's design and onto concrete roadmap items (docs/TODO.md 2.x) in §7.*

---

## 1. Definitions and the design spectrum

"Static" and "dynamic" are endpoints of a spectrum, and almost no production system
sits at either end. The useful axes:

- **When is the graph's *shape* known?** Before any execution (static), at
  submission time (dynamic discovery), or only as data arrives (data-dependent).
- **When is dependence *analysis* paid?** Once at build/compile (amortized across
  runs), or per task at spawn time.
- **How are edges *expressed*?** Explicit wiring (`a.precede(b)`), derived from
  declared data access (this library, Bevy, StarPU, OpenMP `depend`), or implicit in
  control flow (fork-join spawn/sync).

The observed spectrum, roughly in order of increasing dynamism:

1. **Fully static, build-once/run-many** — CUDA Graphs (instantiated executable
   graph, replayed), OpenMP `taskgraph` record-and-replay, PaRSEC's Parameterized
   Task Graph (the DAG exists as an *algebraic formula*, never fully materialized),
   this library's `Static_task_graph` after `compile()`.
2. **Parameterized / updatable static** — CUDA Graph exec-update APIs (change node
   parameters, not topology: [constraints](https://docs.nvidia.com/dl-cuda-graph/cuda-graph-basics/constraints.html));
   render graphs that rebake on resolution/config change (Granite).
3. **Per-iteration redeclared, compiled within the iteration** — Frostbite
   FrameGraph / Unreal RDG: the graph is *declared fresh every frame* from
   immediate-mode-looking code, then compiled and executed as a static plan for
   that frame. Static analysis, dynamic shape.
4. **Static graph with in-graph control flow** — Taskflow condition tasks
   (successor selected at run time, cycles allowed), CUDA 12.3+ conditional
   IF/WHILE nodes.
5. **Sequential task flow (STF)** — StarPU, OpenMP task `depend`, PaRSEC DTD: the
   program *submits* tasks in sequential order with declared data access; the
   runtime infers the DAG on the fly. Dynamic discovery, declared access.
6. **Fully dynamic spawn** — Cilk (`spawn`/`sync`, graph implicit in control flow),
   Naughty Dog / Our Machinery fiber job systems (jobs + atomic counters, no graph
   object at all), UE Tasks, Rayon, TBB `task_group`.

This library spans 1 (the graph), 5-ish (`Guarded::access/async` — per-object
declared access with runtime pipe ordering), and 6 (`ts::task`/`launch`/`nested`),
with the access harness as the runtime oracle tying them together.

---

## 2. Pros of static graphs — the evidence

### 2.1 Amortized scheduling and dependence-analysis cost

The single best-documented advantage. Per-task overhead is what caps usable
granularity: the [Task Bench paper](https://arxiv.org/abs/1908.05790) (Slaughter
et al., SC 2020) measured "minimum effective task granularity" (METG) across 15
runtimes and concluded that **~100 µs is the smallest task granularity even the
most efficient systems reliably support at scale** — everything below that is
runtime overhead. A prebuilt graph attacks exactly this constant:

- **CUDA Graphs** is the flagship result. NVIDIA/PyTorch measured eager kernel
  dispatch overhead at **20–30% of total step time at small batch sizes**, replaced
  by a single ~10 µs whole-graph launch
  ([PyTorch blog](https://pytorch.org/blog/accelerating-pytorch-with-cuda-graphs/),
  [NVIDIA best-practice guide](https://docs.nvidia.com/dl-cuda-graph/cuda-graph-basics/cuda-graph.html)).
  In [vLLM](https://docs.vllm.ai/en/stable/design/cuda_graphs/), graph replay
  removes ~28% per-iteration decode overhead from launch/sync calls; every serious
  LLM inference engine (vLLM, TensorRT, PyTorch `reduce-overhead` mode) now
  captures decode into graphs.
- **OpenMP `taskgraph`** (BSC, [arXiv 2212.04771](https://arxiv.org/abs/2212.04771);
  [record-and-replay landed in LLVM](https://reviews.llvm.org/D146642); presented as
  an [OpenMP spec proposal](https://www.openmp.org/wp-content/uploads/Enabling-Graph-Execution-with-OpenMP-Taskgraph.pdf)).
  Motivation, in their words: task-management overheads in GCC/LLVM OpenMP are
  "very high … which defeats the potential benefits of the tasking model and makes
  it suitable for coarse-grained tasks only." Recording the task dependency graph
  once and replaying it removes task creation and dependency-resolution cost from
  every subsequent execution.
- **Legion "dynamic tracing"** ([Lee et al., SC 2018](https://legion.stanford.edu/pdfs/trace2018.pdf)):
  in a *dynamic* runtime, "the most expensive component … is dynamic dependence
  analysis," and that analysis is "redundant for common cases where dependencies
  are (mostly) unchanging." Their fix — memoize the analysis of a trace and replay
  it — is a static graph reconstructed from a dynamic execution. Follow-up work
  ([Automatic Tracing, arXiv 2406.18111](https://arxiv.org/abs/2406.18111)) automates
  the trace identification because the win was too large to leave manual.
- **PaRSEC PTG vs. DTD** ([Hoque et al., SLAS 2017](https://dl.acm.org/doi/10.1145/3148226.3148233)):
  the compiled parameterized task graph avoids materializing the DAG at all and
  out-scales dynamic task discovery, whose sequential submission loop becomes the
  bottleneck at scale.
- **This library**: graph node blocks + `Run_state` built at `compile()` and
  re-armed per run made re-runs essentially allocation-free and **~19% faster**
  than per-run block construction (CLAUDE.md, `Static_task_graph`). Same mechanism,
  same reason.

### 2.2 Whole-graph optimization: seeing the future enables global decisions

A scheduler that knows the whole DAG before running it can make decisions a
spawn-as-you-go runtime structurally cannot:

- **Frostbite FrameGraph** ([O'Donnell, GDC 2017](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)):
  declared render passes + declared resource access → derived barriers, queue
  sync, and **transient memory aliasing worth roughly half the transient VRAM**
  on Battlefield 1 (a GBuffer dead after pass 3 and a bloom buffer born at pass 4
  share memory — only provable with the whole frame visible). This is the direct
  graphics analogue of access-derived edges, and the industry converged on it:
  [UE RDG](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine),
  Ubisoft Anvil, [Granite](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/),
  and essentially every post-2017 AAA renderer ([survey](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html)).
- **Halide's core argument** ([Ragan-Kelley et al.](https://people.csail.mit.edu/jrk/halide12/))
  generalizes this: separating *what is computed* (the algorithm / the declared
  graph) from *when and where* (the schedule) creates a search space an optimizer
  can explore without changing program meaning. A static task graph is the same
  separation for CPU tasks — the DAG is the algorithm, dispatch order/placement is
  the schedule. HEFT-style list scheduling, criticality ranks, and offline
  simulation are only well-posed when the graph is a stable artifact.
- **CUDA Graphs** again: instantiation performs "the expensive work … once during
  instantiation" (upfront validation, resource assignment, launch optimization),
  reused every launch.

### 2.3 Derived edges from declared access: correctness by construction

Manually wired dependencies rot; derived ones don't. The strongest ecosystem
evidence is ECS schedulers:

- **Bevy** derives system-level parallelism from declared component access
  (`Query<&mut T>` vs `Query<&T>`) — the schedule is correct against data races *by
  construction*, and [Schedule v3 (Bevy 0.10)](https://bevy.org/news/bevy-0-10/)
  invested further in **ambiguity detection**: reporting pairs of systems with
  conflicting access and no explicit ordering, "which can be configured in the new
  `ScheduleBuildSettings`." That a detector for under-constrained order was worth
  building is itself evidence: derived conflict edges catch what hand-wiring
  misses.
- **StarPU / OpenMP `depend` / PaRSEC DTD**: the whole STF family exists because
  inferring the DAG from sequential submission order + declared access modes is
  dramatically less error-prone than explicit wiring
  ([StarPU](https://www.researchgate.net/publication/29605829_STARPU_A_Unified_Platform_for_Task_Scheduling_on_Heterogeneous_Multicore_Architectures)).
- **Unity's container safety system** (survey §2.2) runtime-enforces declared job
  access — the best-regarded ease-of-use feature in the whole space.
- **This library**: `compile()` derives edges from access conflicts on shared
  `Guarded` instances; the access harness is the runtime oracle for completeness.
  Same family; the distinguishing move is that the *dynamic* layer
  (`Guarded::async`, multi-object `ts::access`) shares the pipe-acquisition
  primitive, so derived-graph and dynamic work compose instead of racing.

### 2.4 Determinism, validation, and structural observability

- A compiled graph can be cycle-checked (Kahn), visualized (`dump()` in Taskflow,
  RDG's validation layers, this library's graph-viz work), diffed, and profiled
  as a stable object across runs. RDG "capitalizes on the dependency-graph to
  provide rich validation during pass setup" — whole classes of misuse are caught
  at build, not mid-run.
- Reproducible structure means reproducible measurement: profiler-guided
  reordering (TODO 2.4/2.5) is meaningful because run N and run N+1 execute the
  same DAG. Taskflow ships [TFProf](https://taskflow.github.io/) for exactly this.

### 2.5 Production success stories (detail in §6)

CUDA Graphs in every major inference stack; FrameGraph-style renderers across the
AAA industry; [OpenTimer v2](https://tsung-wei-huang.github.io/papers/tcad21-ot2.pdf)
running incremental VLSI timing with "hundreds of millions of tasks and
dependencies" on Taskflow; PaRSEC PTG powering DPLASMA dense linear algebra at
scale; OpenMP taskgraph headed for the spec.

---

## 3. Cons and frictions — the evidence

### 3.1 Data-dependent control flow is the wall

Every static-graph system hit it, and the fix was never "unroll harder":

- **Taskflow's conditional tasking exists because static DAGs couldn't express
  it.** The [TPDS 2022 paper](https://tsung-wei-huang.github.io/papers/tpds21-taskflow.pdf)
  is explicit that users "ought not to partition the control flow or unroll it to
  a flat DAG"; the [docs](https://taskflow.github.io/taskflow/ConditionalTasking.html)
  motivate condition tasks as avoiding the run-stop-check-rerun client-side loop
  and its "global synchronization barriers." Branches, iteration, and early-out
  are first-class *because the pure DAG model failed users* — their timing-analysis
  workload literally loops until convergence.
- **CUDA Graphs prohibits dynamic control flow and dynamic shapes** outright
  ([constraints](https://docs.nvidia.com/dl-cuda-graph/cuda-graph-basics/constraints.html)).
  The workarounds are institutionalized pain: pad inputs to fixed sizes, or
  capture **one graph per batch-size bucket** — vLLM captures a graph per size in
  {1, 2, 4, 8, 16, …} at **50–200 MB VRAM each** and 30–60 s of startup capture
  ([vLLM design doc](https://docs.vllm.ai/en/stable/design/cuda_graphs/),
  [handling dynamic patterns](https://docs.nvidia.com/dl-cuda-graph/latest/torch-cuda-graph/handling-dynamic-patterns.html)).
  NVIDIA eventually added [conditional IF/WHILE nodes](https://developer.nvidia.com/blog/dynamic-control-flow-in-cuda-graphs-with-conditional-nodes/)
  (CUDA 12.3/12.4) — the hybrid convergence again.
- **Research framing**: for irregular algorithms, "dependences generally are
  complex functions of runtime values" — not knowable statically (see the
  discussion in [TREES](https://arxiv.org/pdf/1608.00571) and the Galois line of
  work). A static graph is a bet that your dependences are *not* such functions.

### 3.2 Rebuild cost when the shape changes per iteration

- [Taskflow issue #49](https://github.com/taskflow/taskflow/issues/49) (2018, a
  robotics user running a 1 kHz control loop): "having to reconstruct the graph at
  each iteration is not very elegant and may also bring some latencies due to the
  dynamic memory allocations that happen behind the scene." Graph reuse /
  `executor.run(taskflow)` retaining the graph came out of exactly this class of
  request.
- **The render-graph world made the opposite call and it's instructive**: RDG and
  FrameGraph *redeclare the graph every frame* (visibility, culling, and quality
  settings change the pass set), and therefore engineered the declaration+compile
  path to be cheap enough to pay per frame ([RDG docs](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine),
  [Granite deep dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/) —
  Granite bakes once but supports rebaking "however often you need").
  Lesson: build-once/run-many is only the right default when the shape is actually
  stable across runs; if it isn't, the system must either make compile cheap or
  offer parameter update without topology rebuild (CUDA's `cudaGraphExecUpdate`).
- **PaRSEC DTD's raison d'être**: writing the PTG's algebraic dependence
  expressions (JDF) was hard enough that a dynamic-discovery front-end was added
  and accepted *despite* known scalability costs
  ([DTD paper](https://dl.acm.org/doi/10.1145/3148226.3148233)) — build-time
  ergonomics are a real cost of the static end.

### 3.3 Capture-by-reference lifetime hazards

A build-once graph stores closures that outlive the build scope by construction:

- Taskflow documents this as a standing footgun: destroying a taskflow while an
  executor still runs it is UB, and "it is your responsibility to keep relevant
  taskflows alive during their execution"
  ([executor cookbook](https://taskflow.github.io/taskflow/ExecuteTaskflow.html));
  [issue #82](https://github.com/taskflow/taskflow/issues/82) is a user crash from
  exactly this. Their mitigation: move-ownership overloads (`run(std::move(tf))`).
- CUDA Graphs has the same hazard one level down: captured kernel parameters
  embed *addresses*; replay reads whatever lives there now. The
  [PyGraph paper](https://arxiv.org/abs/2503.19779) documents parameter-copy
  overheads and static-address constraints as first-order frictions in PyTorch's
  CUDA-graph integration.
- For this library: `add_node` closures and the `Guarded` objects they name must
  outlive the graph; the graph holds raw object references by design. Worth an
  explicit line in the guide (the deferred/versioned layer already treats recorder
  lifetime as fatal-checked — the graph should state its rule as clearly).

### 3.4 Debugging and observability of prebuilt/replayed graphs

- CUDA Graphs again supplies the sharpest documented pain: "issues that would be
  obvious in eager mode can manifest as silent failures, memory errors, or
  unexpected behavior in graphed code," and **silent numerical errors** (stale
  captured addresses, non-deterministic ops baked in) are called out as the
  hardest class ([NVIDIA troubleshooting guide](https://docs.nvidia.com/dl-cuda-graph/latest/troubleshooting/debugging-strategies.html)).
  An illegal access inside a replayed graph can't even be attributed to a kernel
  with `CUDA_LAUNCH_BLOCKING=1`.
- In-graph control flow adds its own hazards: Taskflow's condition tasks introduce
  weak/strong dependency semantics with documented race and deadlock pitfalls
  ("a task that has both strong and weak incoming edges from active paths is a
  strong signal that a race or deadlock may be present" —
  [docs](https://taskflow.github.io/taskflow/ConditionalTasking.html)). Control
  flow encoded as graph edges is harder to read than control flow written as code.
- Mitigations that worked elsewhere: graph visualization (`dump()`, TFProf, RDG's
  immediate-mode debug views), build-time validation, and — notably — keeping an
  *eager/dynamic fallback mode* for debugging (PyTorch's recommendation).

### 3.5 Per-node scheduling overhead still gates granularity

A static graph amortizes *analysis*, not *dispatch*. If nodes are tiny, the
per-node dispatch cost dominates and parallel execution loses to single-threaded:

- **Bevy** is the best-documented case because its schedule is access-derived —
  the closest cousin to this library.
  [Discussion #5958](https://github.com/bevyengine/bevy/discussions/5958): a
  shipped iOS game measured **~600 µs median frame (single-threaded executor) vs
  ~1100 µs (parallel executor)** — the parallel schedule *doubled* median frame
  cost and worsened variance, traced to task-scope spin loops, waker traffic, and
  10–70 µs OS wakeup segments ([discussion #8304](https://github.com/bevyengine/bevy/discussions/8304)).
  [Issue #11378](https://github.com/bevyengine/bevy/issues/11378): the
  multithreaded executor bottlenecking at 1000+ systems, with executor bookkeeping
  at "80+% of compute time" in the pathological case. Bevy's responses: cheaper
  conflict checks ([PR #16885](https://github.com/bevyengine/bevy/pull/16885)),
  automatic single-threaded fallback for small workloads, and a supported
  single-threaded executor option ([issue #6689](https://github.com/bevyengine/bevy/issues/6689)).
- This matches Task Bench's METG framing (§2.1): the graph removes analysis
  overhead but each node still pays queue/wake costs; nodes below ~queue-round-trip
  granularity should be merged or inlined. This library's `set_inline` node
  dispatch and the object-handoff path (skip release/re-acquire) are precisely
  attacks on this constant — worth benchmarking and documenting as such.

### 3.6 Declared-access completeness and granularity

The safety of derived edges holds only if declarations are complete and
well-grained — both are real user frictions, not hypotheticals:

- **Completeness**: an undeclared touch races, and the graph can *promote* a
  latent bug by manufacturing parallelism around it (this library's own
  "completeness hazard," CLAUDE.md). Unity mitigates with runtime enforcement;
  Bevy with type-system-derived access; this library with the runtime harness.
  Systems with *no* oracle (Taskflow, TBB flow graph — edges are whatever you
  wired) silently allow both under- and over-constraint.
- **Granularity**: whole-object declarations serialize readers against any writer
  and collapse the DAG toward sequential (the central tension named in CLAUDE.md).
  Bevy's answer evolved to finer units (per-component access, `Without<>` filters,
  disjoint queries); Unity's to per-`NativeContainer`; render graphs to
  per-resource-subresource. Ambiguity detection (Bevy) and conflict provenance
  (TODO 2.2) are the tooling this tension demands.
- **Ordering ambiguity as user-facing friction**: Bevy's whole
  [stageless rework (RFC 45)](https://github.com/bevyengine/rfcs/blob/main/rfcs/45-stageless.md)
  was driven by users unable to reason about when systems run — "core building
  blocks — stages, run criteria, and states — are presented as independent but
  actually have tons of hidden internal coupling." Access-derived order that is
  *correct* can still be *ambiguous* (two writers, no explicit edge → nondeterministic
  observable order); Unity's `[UpdateBefore]/[UpdateAfter]` attribute web across
  third-party packages is the same complaint in another engine
  ([forum thread](https://discussions.unity.com/t/dealing-with-system-update-orders-and-the-eventual-third-party-dots-packages/764107)).

### 3.7 Ceremony and API surface (the TBB flow graph lesson)

TBB flow graph is the cautionary tale for front-end weight: explicit node types
(`function_node`, `join_node` with queueing vs reserving policies, buffer
policies where the default queueing join can grow RAM without bound —
[Intel docs](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/flow-graph-basics-nodes.html),
[reservation semantics](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/flow-graph-basics-reservation.html)),
push/pull edge protocol, `make_edge` wiring. It is the most general model in
mainstream C++ and widely regarded as verbose and ceremony-heavy for plain
dependency graphs (survey §2.3); Intel's own guidance frames it as one resolution
level among several, with `task_group` + manual reference counting as the
lower-level escape ([migration guide](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/migrating-from-low-level-task-api.html)).
Taskflow's growth at TBB's expense in the DAG niche is substantially an ergonomics
story. Access-derived edges (no wiring for the common case) are this library's
answer; the risk to watch is the mirror image — *invisible* edges surprising users,
which is what ambiguity/provenance tooling (TODO 2.2) exists to counter.

---

## 4. The steelman for dynamic tasking

The strongest opposing position, assembled honestly:

1. **The graph often isn't knowable.** Irregular parallelism (graph algorithms,
   adaptive refinement, game AI reacting to world state) has dependences that are
   functions of runtime values. Cilk's model — dynamic `spawn`/`sync` with the DAG
   implicit in control flow — comes with the provable work-stealing bound
   (T_P ≈ T_1/P + O(T_∞)) and *serial elision*: delete the keywords and the program
   is its own sequential spec ([OpenCilk](https://www.opencilk.org/doc/tutorials/introduction-to-cilk-programming/)).
   No graph object to build, keep alive, or invalidate.
2. **Shipping game engines are dynamic-first.** Naughty Dog runs everything as
   jobs with fiber-based mid-job waits and atomic counters — no graph artifact
   ([Gyrling, GDC 2015](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf));
   Our Machinery's job system was ~300 lines around the same counter idea
   ([blog archive](https://ruby0x1.github.io/machinery_blog_archive/post/fiber-based-job-system/index.html));
   UE's Tasks System (which this library's author built) is dynamic launch +
   prerequisites + pipes, and ships the largest AAA catalogue in existence. A
   frame's *work content* varies wildly (visibility, combat, streaming); these
   teams chose per-frame dynamic spawn and made the runtime fast instead of making
   the shape static.
3. **Programmability is a measured, not soft, concern.** PaRSEC added DTD because
   users couldn't or wouldn't write PTGs ([SLAS 2017](https://dl.acm.org/doi/10.1145/3148226.3148233));
   the entire STF family (StarPU, OpenMP `depend`) trades runtime overhead for
   write-it-like-sequential-code. Sequential submission with inferred edges is the
   most successful *usability* model in HPC tasking.
4. **Composition across module boundaries.** A dynamic task can be spawned from
   anywhere — a library callback, a physics solver's guts — without registering in
   a central graph. A static graph is a closed world; everything inside must be
   known to the builder. (Taskflow's composition and subflows, and this library's
   nested tasks, are patches on exactly this.)
5. **Load adaptivity.** Work stealing rebalances a dynamic task soup by
   construction. A static schedule that pins or orders work can encode yesterday's
   load profile (this library's TODO 2.4 dry-run found exactly that measurement
   problem — ambient drift dwarfed lever effects).
6. **When the amortization premise fails, static is pure cost**: PyGraph found
   CUDA-graphifying *everything* can hurt — it deploys graphs "guided by a
   cost-benefit analysis" because parameter copies and memory overheads sometimes
   exceed the launch savings ([arXiv 2503.19779](https://arxiv.org/abs/2503.19779)).
   Cold-start is real: vLLM pays 30–60 s of capture at startup for its graph
   library.

The honest synthesis: **dynamic-first wins when shape volatility is high and task
granularity is large enough to hide per-spawn overhead; static-first wins when a
stable shape re-runs many times at fine granularity, or when whole-graph analysis
(memory aliasing, barriers, schedule search) pays for itself.** A game frame
contains both kinds of work, which is why every mature system became a hybrid.

---

## 5. The hybrid convergence

The single clearest pattern in this research: **every system that started at one
end of the spectrum shipped an escape hatch toward the other.**

| System | Started as | Added |
|---|---|---|
| Taskflow | static DAG | condition tasks (in-graph branches/loops), subflows (spawn a graph *during* a task), composition, runtime tasking |
| CUDA Graphs | static replay | stream *capture* (build static from dynamic execution), exec-update APIs, [conditional IF/WHILE nodes](https://developer.nvidia.com/blog/dynamic-control-flow-in-cuda-graphs-with-conditional-nodes/), device graph launch |
| OpenMP tasks | dynamic (depend clauses) | `taskgraph` record-and-replay — static extracted from dynamic |
| Legion/Realm | dynamic dependence analysis | tracing/memoization → compiled subgraph replay; then *automatic* tracing |
| PaRSEC | static PTG | DTD dynamic discovery front-end |
| StarPU | dynamic STF | schedule persistence/reuse work; explicit-communication STF refinements |
| Frostbite/RDG | immediate-mode renderers | per-frame *declared then compiled* graphs — dynamic shape, static analysis, with conditional pass execution kept as callbacks |
| Bevy | stage-based static order | Schedule v3: access-derived parallelism + dynamic run conditions, one-shot systems, exclusive-system escapes |
| Naughty Dog / UE | fully dynamic jobs | frame-template structure by convention (the "graph" lives in code layout, counters as barriers) |

Convergence point: **declared structure where the shape is stable; dynamic escape
where data decides; and a bridge that keeps the two composable.**

This library's equivalents, mapped:

- **Subflow / dynamic-in-node** → `ts::nested` inside a graph node: node
  completion (successors + object release) gates on nested tasks, and nested work
  inherits the node's `Access_context`. This is Taskflow's subflow with an access
  story attached.
- **Dynamic work outside the graph** → `Guarded::async` / multi-object
  `ts::access` share `pipe_acquire`/`pipe_release` with graph nodes in canonical
  address order — concurrent dynamic access queues behind a node holding the
  object instead of racing it. Most surveyed systems have *no* sanctioned
  composition between their graph and their ad-hoc tasks; this is a genuine
  differentiator.
- **Cross-run state / data-dependent content** → `Deferred`/`Versioned`: the
  graph shape stays static while per-frame *data* flows through journals and
  version flips; `Versioned` placement (early vs late readers) is exactly the
  "what stays dynamic" seam render graphs keep as callbacks.
- **Control flow** → today: cancellation propagation (skip-not-run), body-level
  early-out via token polling, and the dynamic layer for genuinely data-dependent
  work. There is **no in-graph branch/skip primitive** — see §7.

---

## 6. Success stories worth citing publicly

Vetted, citable, with numbers — candidates for design.md:

1. **CUDA Graphs in LLM inference** — eager launch overhead 20–30% of step time at
   small batches; ~10 µs whole-graph launch; vLLM decode replay removing ~28%
   per-iteration overhead; adopted by vLLM, TensorRT, PyTorch inductor
   ([PyTorch blog](https://pytorch.org/blog/accelerating-pytorch-with-cuda-graphs/),
   [vLLM](https://docs.vllm.ai/en/stable/design/cuda_graphs/)). The strongest
   "amortize the launch path" result in the industry.
2. **Frostbite FrameGraph / the render-graph consensus** — declared access →
   derived barriers + ~50% transient VRAM savings on Battlefield 1; became the
   AAA-standard architecture within five years
   ([GDC 2017](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)).
   The best precedent for *access-derived* graphs specifically.
3. **OpenTimer v2 on Taskflow** — incremental VLSI static timing with hundreds of
   millions of tasks/dependencies; conditional tasking born from its convergence
   loops ([TCAD 2021](https://tsung-wei-huang.github.io/papers/tcad21-ot2.pdf),
   [TPDS 2022](https://tsung-wei-huang.github.io/papers/tpds21-taskflow.pdf)).
4. **OpenMP `taskgraph`** — record-and-replay merged into LLVM and proposed for
   the spec because per-task overhead confined OpenMP tasking to coarse grains
   ([arXiv 2212.04771](https://arxiv.org/abs/2212.04771)).
5. **Legion dynamic tracing** — dynamic dependence analysis identified as the
   dominant runtime cost; memoized trace replay recovered it, later automated
   ([SC 2018](https://legion.stanford.edu/pdfs/trace2018.pdf)).
6. **Bevy Schedule v3** — access-derived parallel scheduling plus ambiguity
   reporting as the ECS state of the art; also the source of the granularity
   cautionary tale (§3.5) — cite both halves
   ([Bevy 0.10](https://bevy.org/news/bevy-0-10/)).
7. **Task Bench METG** — the cross-runtime granularity floor (~100 µs at scale)
   framing why amortizing per-task overhead matters at all
   ([arXiv 1908.05790](https://arxiv.org/abs/1908.05790)).

---

## 7. Implications for this library

### 7.1 Where the access-derived static graph is strong (defensible claims)

- **Build-once/run-many amortization is the industry-validated bet** for
  stable-shape frame work: CUDA Graphs, OpenMP taskgraph, and Legion tracing all
  independently landed on "analyze once, replay cheap." The compile-time node
  blocks + re-arm design (allocation-free re-runs, ~19% measured) is the same
  mechanism and can cite the same lineage.
- **Access-derived edges are the render-graph/ECS lesson applied to CPU tasks**:
  no manual wiring for the common case, conflicts can't be silently forgotten
  (the harness is the oracle Unity has and Taskflow lacks), and declaration
  enables the Halide-style separation that makes 2.4/2.5 (profiler-guided
  optimization, compile-time rank shaping) well-posed at all.
- **Graph/dynamic composability via shared pipe acquisition** is rarer than it
  looks: Taskflow tasks vs. ad-hoc threads, CUDA graphs vs. streams, and Bevy
  systems vs. manual tasks all leave the seam unsound or unsanctioned. The
  canonical-order acquire shared between nodes, `ts::async`, and multi-object
  access is a positioning point worth stating loudly in design.md.
- **What stays dynamic has a designed home**: nested tasks (subflow-equivalent
  with inherited access), `Guarded::async` (escape hatch that queues rather than
  races), `Deferred`/`Versioned` (data varies, shape doesn't). This matches the
  convergence pattern in §5 rather than fighting it.

### 7.2 Honest weaknesses (state them in design.md before someone else does)

- **No in-graph control flow.** No conditional/skip nodes, no in-graph iteration;
  a per-frame branch (e.g. "skip weather sim indoors") today means either running
  the node body with an early-return, cancelling, or restructuring around the
  dynamic layer. Every surveyed static system eventually added something here
  (§5); an early-return body is fine for cheap skips but still pays acquisition +
  dispatch.
- **Run-many voids results.** Nodes are void; data flows only through `Guarded`
  state. Taskflow has the same property and users still ask for typed chaining —
  hence TODO 2.1.
- **Whole-object granularity serializes** (the CLAUDE.md tension): a
  `Guarded<Big_system>` writer blocks all readers. Sharding/double-buffering
  (`Versioned`, the sample's transform swap) is the documented answer, but the
  friction is real and Bevy's evolution shows users push on it hard.
- **Sequential runs only** (one `execute()` in flight) — frame overlap is a known
  gap (TODO 2.3); CUDA graph relaunch-while-in-flight and render-graph frame
  pipelining set user expectations here.
- **Node granularity floor**: Bevy's regression case (§3.5) is the cautionary
  benchmark — publish a measured per-node dispatch cost and a "merge nodes below
  X µs" guideline, or the first mobile adopter will rediscover it.
- **Lifetime rules of build-once closures** (§3.3): document that node bodies and
  their `Guarded` targets must outlive the graph, in guide.md, with the Taskflow
  precedent in mind.

### 7.3 Roadmap items the evidence supports, challenges, or suggests

Supported:

- **2.2 Ambiguity detection (P2)** — *strongly supported.* Bevy shipped exactly
  this (conflicting access + no explicit order → report), and its stageless saga
  shows ordering ambiguity is the top user-facing failure mode of derived
  schedules. Edge provenance is also the prerequisite for 2.4(c)'s commutative
  reordering. Evidence suggests promoting its priority within P2.
- **2.4/2.5 Profiler-guided optimization + rank shaping (P2)** — supported by the
  Halide separation argument and HEFT literature: a declared, stable DAG is what
  makes offline schedule search legitimate. The dry-run's own finding (ambient
  drift dwarfs lever effects) matches the field's experience — interleaved A/B
  measurement infrastructure first.
- **2.3 Pipelined execution (P3)** — supported; CUDA graphs and render graphs
  both normalized overlap of successive runs. Fine at P3: nothing in the evidence
  says it's urgent before typed chaining or ambiguity tooling.
- **2.1 Typed graph chaining (P1)** — supported as the ergonomics gap users hit
  first (Taskflow's per-run futures and `when_all`-style joins exist for a
  reason); per-run `Task<R>` minting preserves the run-many model.

Challenged / suggested additions:

- **Consider a minimal in-graph skip primitive.** The strongest recurring theme in
  §3.1/§5 is that pure DAGs grow control flow eventually. Full Taskflow-style
  condition tasks bring documented race/deadlock semantics (§3.4) — arguably not
  worth importing. The cheap 80% is a **per-run node enable/predicate** (render
  graphs' conditional pass execution: the node dispatches, evaluates a predicate
  *before* acquisition, and completes-as-skipped without running the body or
  taking the objects), preserving DAG acyclicity and the safety story. Worth a
  TODO 2.x entry as a designed-but-unscheduled item; loops can stay outside the
  graph (re-`execute()` is the loop, as Taskflow's own critics of client-side
  looping concede is fine when per-run cost is low — which re-arm makes true here).
- **Graph rebuild/update cost story.** If a user's shape changes occasionally
  (LOD tiers, mode switches), today's answer is "keep N compiled graphs" or
  recompile. The CUDA exec-update and render-graph cheap-rebuild precedents
  suggest at least documenting the intended pattern (pre-compile variants; measure
  `compile()` cost so users can judge per-frame rebuild) before inventing
  machinery.
- **Publish the granularity numbers.** Task Bench's METG and Bevy's regressions
  make per-node overhead the first question a sophisticated evaluator asks. A
  benchmark table (per-node dispatch cost queued vs `set_inline` vs handoff) is
  cheap ammunition and an honest guardrail. Ties to 10.1 (benchmark regression
  infra).

### 7.4 Positioning sentence candidates (for design.md)

- "Static where the shape is stable, dynamic where the data decides, and one
  access model under both — the graph, ad-hoc async, and staged writes all
  acquire the same per-object pipes, so composition is sound rather than
  coincidental."
- The library should *not* claim static graphs are generally faster than dynamic
  tasking — the evidence says they're faster **per re-run at fine granularity
  under stable shape**, and honest framing (with the CUDA/OpenMP/Legion citations)
  is more persuasive to the target audience than a blanket claim.

---

## Appendix: source index

- Taskflow: [TPDS 2022 paper](https://tsung-wei-huang.github.io/papers/tpds21-taskflow.pdf) ·
  [conditional tasking docs](https://taskflow.github.io/taskflow/ConditionalTasking.html) ·
  [issue #49 (reuse)](https://github.com/taskflow/taskflow/issues/49) ·
  [issue #82 (lifetime crash)](https://github.com/taskflow/taskflow/issues/82) ·
  [executor lifetime docs](https://taskflow.github.io/taskflow/ExecuteTaskflow.html) ·
  [OpenTimer v2](https://tsung-wei-huang.github.io/papers/tcad21-ot2.pdf)
- TBB: [flow graph nodes](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/flow-graph-basics-nodes.html) ·
  [reservation](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/flow-graph-basics-reservation.html) ·
  [low-level task migration](https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2021-6/migrating-from-low-level-task-api.html)
- CUDA Graphs: [PyTorch blog](https://pytorch.org/blog/accelerating-pytorch-with-cuda-graphs/) ·
  [constraints](https://docs.nvidia.com/dl-cuda-graph/cuda-graph-basics/constraints.html) ·
  [debugging](https://docs.nvidia.com/dl-cuda-graph/latest/troubleshooting/debugging-strategies.html) ·
  [conditional nodes](https://developer.nvidia.com/blog/dynamic-control-flow-in-cuda-graphs-with-conditional-nodes/) ·
  [vLLM design](https://docs.vllm.ai/en/stable/design/cuda_graphs/) ·
  [PyGraph](https://arxiv.org/abs/2503.19779)
- Render graphs: [FrameGraph GDC 2017](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in) ·
  [Granite deep dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/) ·
  [UE RDG](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine) ·
  [render-graph survey](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html)
- ECS: [Bevy stageless RFC 45](https://github.com/bevyengine/rfcs/blob/main/rfcs/45-stageless.md) ·
  [Bevy 0.10](https://bevy.org/news/bevy-0-10/) ·
  [discussion #5958](https://github.com/bevyengine/bevy/discussions/5958) ·
  [discussion #8304](https://github.com/bevyengine/bevy/discussions/8304) ·
  [issue #11378](https://github.com/bevyengine/bevy/issues/11378) ·
  [Unity DOTS update order](https://docs.unity3d.com/Packages/com.unity.entities@1.0/manual/systems-update-order.html) ·
  [Flecs pipelines](https://www.flecs.dev/flecs/md_docs_2Systems.html)
- HPC: [Task Bench](https://arxiv.org/abs/1908.05790) ·
  [PaRSEC DTD](https://dl.acm.org/doi/10.1145/3148226.3148233) ·
  [StarPU](https://www.researchgate.net/publication/29605829_STARPU_A_Unified_Platform_for_Task_Scheduling_on_Heterogeneous_Multicore_Architectures) ·
  [OpenMP taskgraph](https://arxiv.org/abs/2212.04771) ·
  [LLVM record/replay](https://reviews.llvm.org/D146642) ·
  [Legion tracing SC18](https://legion.stanford.edu/pdfs/trace2018.pdf) ·
  [automatic tracing](https://arxiv.org/abs/2406.18111)
- Dynamic-first: [Gyrling GDC 2015](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf) ·
  [Our Machinery job system](https://ruby0x1.github.io/machinery_blog_archive/post/fiber-based-job-system/index.html) ·
  [OpenCilk](https://www.opencilk.org/doc/tutorials/introduction-to-cilk-programming/) ·
  [Halide](https://people.csail.mit.edu/jrk/halide12/)
