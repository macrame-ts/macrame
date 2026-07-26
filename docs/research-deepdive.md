# Static vs. Dynamic, Deep Dive: Critique Mapping, Access-Control Comparison, and Course Check

*Deep-research pass, 2026-07. Companion to
[research-static-vs-dynamic.md](research-static-vs-dynamic.md) (the original
evidence review, referenced as "the original doc"),
[task-systems-comparison.md](task-systems-comparison.md), and
[design.md](design.md). Sources: primary docs, papers, GitHub
issues/discussions, and forum threads, gathered 2026-07; the full source
index is in §20. Abbreviations are expanded at first use.*

*Every paragraph is numbered §N.M for reference. §1 is the cast-of-systems
glossary. Each later section opens with the question(s) from the review
brief it addresses.*

---

# Static vs. Dynamic — Deep Dive, Critique Mapping, and Course Check

*Deep-research pass, 2026-07. Companion to
[research-static-vs-dynamic.md](research-static-vs-dynamic.md) (the original
evidence review), [task-systems-comparison.md](task-systems-comparison.md), and
[design.md](design.md). Every paragraph is numbered (§N.M) for reference in
discussion. Abbreviations are expanded at first use.*

---

## 1. Cast of systems

One short description per system mentioned in the original document, framed by
what it contributes to *this* library's discussion.

**§1.1 — CUDA Graphs (NVIDIA).** A GPU-side build-once/run-many task graph:
you record ("capture") or hand-build a graph of kernel launches + memory ops,
"instantiate" it (validation + resource assignment, the expensive step), then
"launch" the whole graph in ~10 µs instead of paying per-kernel dispatch. The
closest GPU analogue of our `compile()`/`execute()` split, and the richest
documented source of both the amortization win and the static-graph pain
(no dynamic shapes, debugging opacity, stale captured pointers).

**§1.2 — OpenMP `taskgraph` (BSC / LLVM).** OpenMP = Open Multi-Processing,
the compiler-directive parallelism standard for C/C++/Fortran. Its tasking
model (`#pragma omp task` with `depend(in:x) depend(out:y)` clauses) is
dynamic: tasks are created at run time and the runtime infers edges from the
declared in/out sets. `taskgraph` is a recent extension that *records* one
execution of a task region and *replays* the frozen graph on subsequent
executions — dynamic front-end, static replay. Their motivation text is the
best independent statement of "per-task overhead confines tasking to coarse
grains."

**§1.3 — PaRSEC, PTG and DTD (U. Tennessee ICL).** An HPC (high-performance
computing) distributed task runtime powering dense linear algebra (DPLASMA).
Two front-ends: **PTG** — Parameterized Task Graph — the DAG is written as an
*algebraic formula* (in a DSL called JDF): "task (i,j) depends on task
(i-1,j)", evaluated on demand, so a trillion-task DAG is never materialized in
memory. **DTD** — Dynamic Task Discovery — a conventional front-end where a
sequential-looking loop submits tasks with declared data access and the
runtime builds the DAG on the fly. PTG out-scales DTD (no sequential
submission bottleneck, O(1) memory); DTD exists because writing JDF was too
hard for users. The pair is the cleanest controlled experiment on
"static performance vs. dynamic programmability" in the literature.

**§1.4 — StarPU (INRIA).** HPC runtime for heterogeneous machines (CPU+GPU).
The canonical **STF** — Sequential Task Flow — system: you submit tasks in
program order, each declaring which data handles it reads/writes; the runtime
derives the DAG from those declarations (write-after-read, read-after-write
etc.), exactly like superscalar CPUs derive instruction dependencies. Notable
for *performance-model-driven scheduling*: it measures task durations per
device and uses the model (HEFT-family heuristics) to place work. Our
`Guarded::async` layer is STF-shaped; our TODO 2.4/2.5 is its
performance-model idea.

**§1.5 — Legion / Realm (Stanford/NVIDIA).** HPC runtime where tasks declare
*logical regions* (data partitions) with privileges (read/write/reduce) and
the runtime computes dependencies dynamically — the most thorough
declared-access model in HPC. Its own measurements showed dynamic dependence
analysis was the dominant runtime cost, which led to "tracing": memoize the
analysis of a repeated task sequence and replay it — i.e. they *reconstructed
a static graph from a dynamic execution* because analysis-per-run was too
expensive. Strong independent validation of build-once/run-many.

**§1.6 — Taskflow (U. Wisconsin / formerly Cpp-Taskflow).** The popular
MIT-licensed C++ DAG library (~12k stars): explicit graph building
(`taskflow.emplace(...)`, `a.precede(b)`), work-stealing executor, graph
reuse, **condition tasks** (in-graph control flow — §8), **subflows** (a task
spawns a nested graph at run time), composition of graphs. No data/access
model at all — edges mean only order — hence no safety story, but the best
ergonomics-per-feature ratio in the C++ DAG space and the main open-source
reference for "what users ask a DAG library for" (their issue tracker is a
goldmine).

**§1.7 — Intel oneTBB flow graph.** TBB = Threading Building Blocks, the
long-standing Intel C++ parallelism library. Its `flow::graph` is the most
general node-based model (data-flow messages, buffering nodes, join policies,
cycles) and the cautionary tale for API ceremony: explicit node types and
`make_edge` wiring for everything. Mentioned here mostly as the negative
ergonomics baseline.

**§1.8 — Frostbite FrameGraph (EA/DICE).** The GDC 2017 render-graph design
(O'Donnell) that the AAA industry converged on: render passes declare which
GPU resources they read/write/create; the graph is *redeclared every frame*,
compiled (cull unused passes, place barriers, alias transient memory), then
executed. "Access-declared scheduling" for GPU work — the closest published
relative of our thesis, from a different domain. Details in §4 and §13.

**§1.9 — Unreal RDG (Render Dependency Graph).** Unreal Engine's FrameGraph
equivalent: passes declare resource access via parameter structs; the graph
compiles per frame; a validation layer catches undeclared/misdeclared access
during setup. Runs *on top of* (and independently of) UE's generic Tasks
system. Referenced for its validation design (§7) and per-frame-rebuild
rationale (§9).

**§1.10 — Granite (Themaister / Hans-Kristian Arntzen).** An open-source
Vulkan engine whose render-graph write-up is the most technically explicit
public description of render-graph compilation (barrier derivation, aliasing,
queue assignment). Rebakes the graph on demand rather than every frame —
the "parameterized static" point on the spectrum.

**§1.11 — Bevy (Rust ECS).** ECS = Entity Component System. Bevy's scheduler
derives system-level parallelism from typed component access
(`Query<&mut T>` = write on component type T) — compile-time-declared,
runtime-scheduled, the closest CPU-side cousin of our derived edges. Source
of the two most instructive cautionary tales: the parallel executor *losing*
to single-threaded on real games (granularity, §12), and ordering-ambiguity
tooling (§6).

**§1.12 — Unity C# Job System.** Jobs declare `NativeContainer` access;
a runtime **safety system** (editor-only) detects races between jobs and
enforces declared dependencies. The best-regarded ease-of-use feature in the
whole space and the nearest production analogue of our harness (§13).

**§1.13 — Unreal Engine Tasks System / TaskGraph.** The author's own prior
system: fully dynamic task launch with prerequisites, nested tasks, events,
pipes; no graph artifact. The dynamic-first baseline this library deliberately
extends with a static layer.

**§1.14 — Cilk / OpenCilk.** The academic origin of work-stealing fork-join
(MIT, 1990s): `spawn f(); ... sync;` with the provable steal-bound and
**serial elision** — deleting the keywords yields a valid sequential program,
so the parallel program *is* its own spec. The purest dynamic end of the
spectrum; explained further in §15.

**§1.15 — Naughty Dog / Our Machinery fiber job systems.** GDC-documented
game engines running everything as small jobs with fiber-based waits and
atomic counters as the only synchronization — no graph object anywhere.
The shipping proof that dynamic-only can carry AAA frames; the counterpoint
to our static layer.

**§1.16 — HPX, Rayon, Tokio, GCD.** Brief: HPX = C++ std-conforming
futures/continuations runtime scaled to distributed memory (heavyweight);
Rayon = Rust's ergonomic fork-join/data-parallel library (`par_iter`);
Tokio = Rust's async I/O runtime; GCD = Apple's Grand Central Dispatch,
OS-managed queues. They appear only as API-ergonomics reference points.

**§1.17 — Halide.** A DSL (domain-specific language) for image pipelines
whose core idea — separate the *algorithm* (what is computed) from the
*schedule* (when/where) so an optimizer can search schedules without changing
meaning — is the theoretical justification for whole-graph optimization
(§5). Not a task system; an argument.

**§1.18 — Task Bench / METG.** A cross-runtime benchmark (Stanford/SLAC,
SC 2020) defining **METG** — Minimum Effective Task Granularity — the
smallest task size at which a runtime still achieves (by convention) 50% of
peak efficiency. Headline: ~100 µs is the practical floor across 15 runtimes
*at scale (distributed, 16+ nodes)*; single-node floors are lower (§3).

**§1.19 — OpenTimer.** An open-source VLSI (Very Large Scale Integration —
chip design) static timing analyzer, Taskflow's flagship application:
incremental timing propagation over circuit graphs — millions of gates, hence
millions of tiny tasks with structure-given dependencies (§16).

**§1.20 — Galois / TREES.** Research systems for *irregular* parallelism
(graph analytics, adaptive mesh refinement) where dependencies are functions
of runtime data — the workloads for which any static graph is structurally
impossible. Quoted as the outer boundary of our applicability.

**§1.21 — PyGraph / vLLM / PyTorch `reduce-overhead`.** LLM-inference
consumers of CUDA Graphs; cited for the economics: capture cost, VRAM per
graph variant, and "graph everything" sometimes losing to eager — the
evidence that static replay is a bet, not a free lunch.

---

## 2. The spectrum: what points 2–4 are, and whether not covering them is a problem

*(Addresses: "the doc states we don't cover (2), (3) and (4). is this a
problem? why? practical examples. why can't this be solved by dynamic tasks
used along the static graph.")*

**§2.1 — Restating the three uncovered points concretely.** Point (2)
*parameterized/updatable static*: same topology, different knobs per run.
Point (3) *per-iteration redeclared*: a fresh graph is declared and compiled
every iteration because the *set of nodes* is data-dependent, but each
iteration still gets static analysis. Point (4) *in-graph control flow*: the
topology is fixed but which branch runs, or how many times a sub-graph loops,
is decided by data produced inside the run.

**§2.2 — Point (2) in practice, and why we mostly get it for free.** CUDA
needs `cudaGraphExecUpdate` because kernel parameters (pointers, sizes, batch
dims) are *baked into the instantiated graph* at capture. Our nodes are
closures over `Guarded` state; per-run data does not live in the graph — it
flows through the objects. A node body that reads `settings.shadow_quality`
from a guarded config object *is* a parameterized node; no API needed.
What we genuinely lack from (2) is *structural* parameterization: enabling or
disabling a node per run (TODO 2.6, the enable predicate, covers the
dominant case), and swapping a node's body or its access set (rare; the
honest answer is "rebuild or keep N compiled variants"). Verdict: (2) is a
small, mostly-covered gap; the predicate closes most of the rest.

**§2.3 — Point (3) in practice.** The render-graph case: this frame has fog,
a planar reflection, and 3 shadow-casting lights; next frame the reflection is
culled and there are 5 lights. The *pass set itself* — the node list — is a
function of frame content. A build-once graph cannot express "one
shadow-depth node per visible light" without worst-case unrolling (declare
MAX_LIGHTS nodes, disable unused ones — which the enable predicate would at
least make cheap). Game-side equivalents: one node per active split-screen
player; per-level system sets (water sim only in water levels); server tick graphs
whose per-zone nodes follow player population.

**§2.4 — Why dynamic tasks alongside the graph only partially substitute for
(3).** Functionally they do substitute: a static "shadows" node can
`ts::nested`-spawn one task per visible light under its grant, and this is the
designed answer. What is lost, precisely:

1. **Successor granularity.** The nested tasks gate the *node's* completion;
   every successor of "shadows" waits for *all* lights, even a successor that
   consumes only the first shadow map. Inside the static graph the unit of
   dependency is the node; dynamic sub-work cannot export finer edges to the
   graph's successors. A redeclared-per-frame graph (RDG-style) would give
   each light its own node with its own downstream edges.
2. **No lookahead across the boundary.** The scheduler sees nested tasks only
   when the node body runs. Critical-path shaping (TODO 2.5) cannot rank work
   it has not seen; a per-frame-declared graph ranks everything.
3. **No derived safety edges for the *shape-varying* part.** Nested work
   inherits the node's declared grant — safe, but capped at the node's
   declaration. A light-culling result that says "this frame also touches the
   volumetrics buffer" cannot widen the node's access set per run; in RDG it
   simply declares differently this frame.
4. **Observability.** The DOT dump, ambiguity detection (TODO 2.2), and any
   profiler-guided reordering see a "shadows" black box, not the real
   structure.

**§2.5 — Is that a problem?** For the library's stated positioning — the
graph is the *stable frame skeleton*; data-dependent work lives in the
dynamic layer — no, it is a boundary, not a defect, and §15 shows shipping
engines draw the same boundary (UE's tick groups are exactly a static
skeleton with dynamic content). It becomes a problem only if the library
claims to cover workloads whose *top-level* structure is data-dependent
(renderers being the loudest example). The honest positioning: we sit at
points 1+5+6 of the spectrum and deliberately skip 3; users whose shape
varies per run should either use the dynamic layer for the varying region or
wait for (cheap-compile + enable-predicate), which together cover most of
(3)'s practical demand. What we should *measure and publish* is `compile()`
cost — if it is cheap enough, per-frame rebuild becomes a supported pattern
instead of an anti-pattern, and point (3) collapses into "we cover it too"
(§9.5).

**§2.6 — Point (4), and how much of it the enable predicate buys.** In-graph
control flow divides into: *branch/skip* (run A or B, or skip A, per run) —
covered acceptably by TODO 2.6's per-run predicate, which is exactly render
graphs' conditional-pass pattern; *iteration* (run this subgraph until a
condition computed inside the run) — not covered, and deliberately so: the
loop moves to the caller (`while (!converged) graph.execute().sync();`),
which is legitimate *because* re-runs are allocation-free — the very
objection Taskflow's authors raise against client-side loops ("global
synchronization barrier per iteration") is an objection to *expensive*
re-runs and to draining the whole pool between iterations. Where the loop
body is a small subgraph of a larger frame, looping the whole graph
over-synchronizes; that is the real residual gap, and it is the case Taskflow
condition tasks (§8.3) exist for. Verdict: accept the gap, document the
caller-side loop, keep 2.6.

---

## 3. Granularity: the ~100 µs number, unavoidably-small tasks, and whether our answers suffice

*(Addresses Q2.1 and the standalone "Granularity" question.)*

**§3.1 — What METG actually measured (and what it didn't).** Task Bench's
~100 µs floor is for *distributed* runtimes at scale (MPI-class systems,
16+ nodes, communication on the critical path). The single-node,
shared-memory floor is far lower: a Chase-Lev pop is ~10–30 ns, a
steal ~100–500 ns, a task-block alloc ~50–100 ns, an eventcount wake
~1–20 µs. Practical single-node guidance across TBB/Rayon/Taskflow
documentation and issues converges on ~1–10 µs of work per task to keep
overhead under ~10%; the 100 µs figure is the *cross-machine* number. Your
instinct ("aligns with my experience") is right for the regime where wake
latency and queue round-trips sit on the critical path — i.e. sparse
parallelism, which is a game frame's shape — and §3.6 below is the sharper
statement of it.

**§3.2 — Legitimate workloads where many small tasks are unavoidable.** The
pattern behind every real case: **the dependency structure is given by the
problem, not chosen by the programmer, and it is fine-grained.**

1. *Tiled dense linear algebra* (PLASMA/DPLASMA/StarPU's home turf): a
   Cholesky on 10k×10k with 256×256 tiles is ~60k tasks of ~1–5 ms — fine;
   shrink the matrix or grow the core count and per-task work falls toward
   µs. Tiles cannot be merged freely: merging collapses the lookahead that
   lets the runtime overlap the panel factorization (critical path) with
   trailing updates. Coarsening trades away exactly the parallel slack you
   parallelized for.
2. *Circuit/graph propagation* (OpenTimer, §16): the task graph mirrors the
   netlist; a gate's arrival-time update is nanoseconds of arithmetic. You
   *can* batch by topological level — OpenTimer does — but incremental
   updates (change one gate, propagate the cone) make level-batches ragged
   and tiny again. The structure is the data; you don't get to redraw it.
3. *Irregular algorithms* (Galois): per-node relaxations in SSSP or mesh
   refinement are sub-µs, and which neighbors become ready depends on
   runtime values — neither coarsenable (no static clustering respects the
   dynamic frontier) nor statically graphable at all.
4. *Latency-bound pipelines*: audio DSP at 48 kHz (a 128-sample block is
   2.6 ms of budget for a whole effect chain), robotics control at 1 kHz
   (§9.1) — the *deadline* caps how much work can be batched into one task.
5. *LLM inference decode* (the CUDA Graphs case): one token step is a long
   chain of small kernels; kernel count is fixed by the model architecture,
   sizes shrink with batch=1. This is why graph replay wins there — the
   structure is maximally stable AND maximally fine.

**§3.3 — Why those systems don't "just use coarse tasks."** Summarizing the
mechanism in each case: (a) *dependency-frontier loss* — merging tasks merges
their dependency sets, and a merged task starts only when the union of inputs
is ready and releases only when everything inside finishes: coarsening
inflates the critical path (this is the formal reason, and it's why HPC went
to tiles-as-tasks in the first place); (b) *load imbalance* — coarse chunks
of variable cost create stragglers, and work stealing can't split what you
fused (Rayon/TBB solve this with *adaptive* splitting — which is precisely
"small tasks, created lazily"); (c) *deadlines* — batching adds latency;
(d) *incrementality* — small updates touch small task subsets only if tasks
are small.

**§3.4 — Now the important part: is your "parallelism at a higher
abstraction level" position wrong?** No — for the workload you target, and
the evidence in the original doc actually supports it twice over: the METG
line says fine grain is *lost* to overhead in general-purpose runtimes, and
Bevy's regression (§12) shows what happens when a frame is scheduled at
too-fine system granularity. The game-frame decomposition — O(100) coarse
system nodes, each internally `parallel_for`-flat — is the shape UE, Unity,
and every shipped engine converged on. The cases in §3.2 are *different
workloads*: their structure-given fine grain is exactly what your coarse
static graph should *not* try to serve, and it's fine to say so. The one
watch-out: "coarse node + internal parallel_for" assumes intra-node work is
*flat* (data-parallel). Where intra-node work has its own dependency
structure (a physics solver's islands, constraint graph coloring), you need
the D5 primitives (mailboxes, coloring) — that's the part of the roadmap
this question actually stresses, more than the graph.

**§3.5 — Does the static graph compose well with dynamic tasks today?
Mostly yes; the four real seams.** (1) Nested tasks extend the node's
completion — a long nested tail delays all successors (the §2.4 point);
(2) a `Guarded::async` against an object a node holds waits for the *whole
node*, not for the node's last touch of that object — object hold windows
are node-granular (per-accessor acquire already shrank this; it can't shrink
below a node); (3) blocking `sync()` inside a node body remains forbidden
(documented, not enforced — a runtime check "no blocking sync under an
active node grant" would be cheap and is worth adding, same family as the
coroutine suspend-under-guard fatal); (4) the `ts::launch` grant-inheritance
hole (§13.6) — the one *soundness* seam, distinct from the three
*performance* seams.

**§3.6 — Wake cost, precisely.** Yes: "wake cost" = the latency and syscall
cost of unparking a worker that went to sleep on the eventcount because the
queues were empty. Measured park/unpark round-trips are ~1–20 µs on
Windows/Linux (plus scheduler-placement jitter — the woken thread must also
get a core); Bevy's mobile traces showed 10–70 µs wake segments. It is the
dominant per-task cost precisely in the regime you name: *sparse* parallelism
— tasks launched on the critical path while most workers are parked, each
launch paying a wake. It is much discussed: Go's `nmspinning` protocol
(at most one spinner; producers wake only on the 0→1 transition), Tokio's
LIFO slot (a submitted task is grabbed by the submitting worker's next pop
without queue traffic), TBB's "mailbox"/local bypass (a spawned task runs on
the spawner without touching shared queues), Folly's LifoSem, and UE's
spin-then-park cycles all attack it. Our three idle policies are exactly this
menu (`spin_then_block` = UE, `handoff` = Go), and inline dispatch
(`set_inline`, `then(run_inline)`) is the bypass-the-queue-entirely answer.
(Correction after author review: a `normal`-priority task submitted from a
worker already lands in the submitter's own deque and is popped LIFO — which
is the essential behavior of Tokio's slot; the original text overstated the
gap. The residual differences are producer-side wake signaling —
`spin_then_block` signals the eventcount per submit even when the submitter
will consume the task itself, which `handoff` already suppresses to the
0→1 transition — and a few per-op deque atomics a dedicated one-entry slot
avoids. An M2-stage-5 micro-refinement at most, not a missing capability.)

**§3.7 — Are inline tasks "enough"?** For the chain case (A then B then C,
each small) — yes: inline dispatch runs the successor on the settling thread
at function-call cost minus allocation. Three honest limits: (1) an inline
task still pays block allocation + atomics (~100–200 ns; TODO 4.x free-lists
shrink, don't eliminate); (2) inlining *serializes* — it is the right tool
only when the successor would not have run concurrently with anything
anyway; inlining a wide fan-out defeats the parallelism; (3) inline bodies
must not block and inherit the settling thread's timing (documented). So the
guidance to publish: chains and low-fan-out continuations below ~5 µs →
inline; parallel fan-outs below ~5 µs → merge into the parent or use
`parallel_for` chunking, never one-task-per-item; above that → queue freely.
That guidance plus a measured per-node dispatch table (original doc §7.2
"publish the granularity numbers") is the complete answer.

---

## 4. Rendering: what render multithreading actually consists of, and what we cover

*(Addresses Q2.2-Frostbite "can this library be used for inter-rendering
parallelism; what MT techniques popular in rendering don't we help with",
Q6.2 "how access control saves VRAM", and 7.3-2.2 "convince me".)*

**§4.1 — The catalog, classified.** Renderer multithreading decomposes into
three kinds of work, and the classification answers the question:

| Technique | Kind | Generic task system covers it? |
|---|---|---|
| Parallel visibility/culling, draw-command generation | CPU data/task parallelism | **Yes, fully** — UE runs these as ordinary TaskGraph tasks (`FParallelMeshDrawCommandPass`: one setup task per pass + N generation tasks) |
| Parallel command-list recording | CPU fork-join + *ordered* join | **Yes, with an idiom** — command lists must be *replayed in submission order* per queue (GPU execution order = submission order; draw order is semantics). Record into an indexed slot array in parallel (chunk i → slot i), then one sequential tail task submits slots 0..N−1; `when_all`'s argument-ordered tuple is the fixed-fan-out form. Expressible today, not packaged |
| PSO (Pipeline State Object — baked GPU pipeline config) compilation | CPU background tasks | **Yes** — the best fit in the catalog; needs background priority + *priority boost of an already-queued task* when a PSO becomes needed this frame (we lack dynamic re-prioritization — a real, small gap) |
| Render/RHI thread split | Pipeline of dedicated threads | **Partially** — this is an SPSC (single-producer single-consumer) command stream consumed one frame behind, with frame fences; our named-thread affinity item (TODO 3.2) is the prerequisite; the ordered-stream idiom itself is `Pipe`-shaped |
| Async compute, queue assignment | GPU scheduling | **No, and shouldn't** — a second hardware timeline; CPU-side task systems have no representation of it |
| Barrier derivation, resource state machines | GPU resource-state compilation | **No** — this is what render-graph compile *is*; declared access producing *state transitions*, not just edges |
| Transient VRAM aliasing | GPU memory compilation | **No** (see §4.4 — but the *idea* transfers) |
| N frames in flight | GPU-timeline lifetime management | **No** as-is — but note `Versioned<T>` is the same double-buffer-and-publish shape on the CPU side, minus the fence |
| Texture/asset streaming | CPU I/O tasks + GPU fence completion | **CPU side yes**; the "resource now resident" handoff is fence-gated |

**§4.2 — The layering every shipping engine converged on.** Generic job
system at the bottom; the render graph as a *GPU-resource compiler* on top of
it; the RHI (Render Hardware Interface) thread doing ordered translation
below. UE is explicit: RDG's setup tasks, compile, and parallel pass
execution all run *as UE::Tasks tasks* (`FRDGBuilder::AddSetupTask` takes
`UE::Tasks::FPipe*` and `ETaskPriority`; parallel execute batches passes into
spans dispatched as tasks — `r.RDG.ParallelExecute`, span min/max cvars), and
UE 5.5 deprecated the old standalone `FParallelCommandListSet` in favor of
RDG-native dispatch passes ("Use GraphBuilder.AddDispatchPass instead") —
parallel recording is being folded INTO the graph. Destiny (GDC 2015,
Tatarchuk) and id Tech 7 (fully job-based, "one worker thread per core
handling the job queue") tell the same story: the renderer's CPU work runs on
the *same generic job system as everything else*. So the direct answer to
"can this library be used for inter-rendering parallelism": **yes, for
everything CPU-side — that is most of the thread count — and that is exactly
how UE uses its generic system**; what it cannot and should not try to be is
the GPU-facing compiler (barriers, aliasing, fences, ordered submit) that
engines build as a domain layer on top.

**§4.3 — What we're missing, concretely, if a renderer adopted us today.**
(1) Ordered-collection join: a `parallel_for`-like fork whose results are
consumed *in index order* by a sequential tail — expressible today (a
successor node reading a slot array), but worth a documented idiom.
(2) Priority boost on a queued task (PSO case) — we can cancel-and-relaunch;
a real boost needs queue surgery (M2 stage 5 territory).
(3) Named-thread affinity (TODO 3.2) for the render/main-thread pattern.
(4) External-completion integration: a `Signal` triggered by a GPU fence
callback covers "task gated on GPU progress" today — worth a guide example.
None of these are the render-graph compile; all are generic-scheduler
features with non-rendering uses too.

**§4.4 — Q6.2 answered: how "access control" saves VRAM in Frostbite.** You
did not get it wrong — the mechanism is: declared per-pass resource access →
the compiler knows each transient resource's *lifetime interval* [first
writing pass, last reading pass] over the frame's pass order → two resources
whose intervals don't overlap are *placed* at the same physical memory
(D3D12 placed resources / Vulkan memory aliasing on a heap; interval-graph
coloring, same algorithm family as linear-scan register allocation) →
measured on Battlefield-1-era Frostbite: 720p transient memory 147 MB
unaliased → 76–80 MB aliased; 4K DX12 1042 MB → 472 MB (~50–55% saved). The
declarations are what make lifetimes *knowable*; without them the allocator
must assume every resource lives the whole frame. The caveats they document
are instructive for us: aliasing is only sound if declarations are complete
(an undeclared read past the declared last-read reads garbage — their
completeness hazard, GPU edition) — the same invariant our harness enforces
on the CPU side. CPU analogue for us: scope-bounded arenas (TODO 4.6) are
lifetime-derived memory reuse in the same spirit; a full analogue (per-node
transient buffers with graph-derived lifetimes, alias slots across
non-overlapping nodes) is a plausible future differentiator but needs the
declared-transient-resource concept first — parked as a research note.

**§4.5 — 7.3-2.2 re-argued (the "I still don't get it" item).** The original
doc's §7.3 claim was: ambiguity detection (TODO 2.2) deserves its raised
priority because Bevy shipped exactly this diagnostic and its absence is the
top user-facing failure of derived schedules. The evidence, now concrete:
Bevy's own engine schedules accumulated **370+ ambiguities** (296 in
ExtractSchedule alone) before they added a CI gate — the diagnostic being
optional meant even the engine's own maintainers let conflicts rot; and
their detector's precision limits (it ignores query filters, flagging
provably-disjoint systems — issue #11796, closed as not-planned) push users
toward suppression attributes, eroding trust. The lesson for 2.2 is
three-part: ship it **on by default at Warn** (opt-out, not opt-in — the rot
is the default outcome otherwise), make it *precise* (we operate on object
instances, not type-level access sets — we structurally avoid Bevy's
false-positive class), and pair every report with an *actionable fix menu*
(add edge / mark commutative / split object). §6 gives the full argument
that our declaration-order tiebreak makes this a determinism-preservation
tool rather than a race-prevention tool.

---

## 5. What whole-graph optimization concretely enables

*(Addresses Q2.2 "what exact optimisations static graphs make possible,
especially things we haven't discussed before.")*

**§5.1 — The complete menu.** Everything below requires seeing the whole DAG
before running it; a spawn-as-you-go runtime structurally cannot do any of
them (it learns each edge too late to plan around it).

1. **Transient memory aliasing** (render graphs): resource lifetimes =
   [first-writer, last-reader] intervals over the pass order; non-overlapping
   intervals share physical memory. §4.4 explains the mechanics; ~50%
   transient VRAM on Battlefield-1-era Frostbite.
2. **Barrier/sync placement and batching** (render graphs, CUDA): knowing
   every consumer lets you emit one transition at the optimal point instead
   of conservative per-use sync; split barriers start the transition early.
   CPU analogue: our object *handoff* (skip release+re-acquire when the sole
   ready successor takes the object in the same mode) — already shipped.
3. **Dead-node elimination / pass culling**: unreferenced passes dropped
   before execution (render graphs cull passes whose outputs nothing reads).
   Our analogue would be: nodes whose written objects no enabled node reads
   this run — only meaningful once the enable predicate (TODO 2.6) exists.
4. **Node fusion**: merge nodes below the dispatch-cost floor when they
   share an edge and their combined access set doesn't lose parallelism
   (mobile GPU subpass merging is the render version). Nobody hand-fuses
   correctly under churn; a compiler can.
5. **List scheduling / critical-path ranking** (HEFT = Heterogeneous
   Earliest Finish Time — the classic list-scheduling heuristic): compute
   each node's *upward rank* (longest path to the sink) and dispatch ready
   nodes in rank order, so the critical path never waits behind filler.
   Requires the whole graph + duration estimates. This is TODO 2.5, and
   StarPU's model-driven schedulers are the production precedent.
6. **Exact resource budgeting**: total transient allocation, peak
   concurrency, per-priority queue capacity are all computable at compile;
   arenas can be sized exactly (TODO 4.6's graph-run arena).
7. **Speculative prefetch/placement**: knowing a node's successors lets the
   runtime prefetch their data or place them on the worker that will have
   the inputs hot (NUMA/cache affinity). StarPU does this with data
   prefetch to GPUs.
8. **Static validation** (§7): cycle checks, ambiguity reports, unused-node
   warnings, access-conflict provenance — all "free" byproducts of a
   materialized graph.
9. **Offline schedule search** (Halide's argument): with a stable DAG and a
   cost model you can *simulate* schedules offline and ratify the best —
   TODO 2.4's endgame. Not possible when the graph exists only as a
   run-time trace.
10. **Lock elision**: two accesses provably ordered by graph edges need no
    runtime arbitration at all; the compiled schedule can skip pipe
    acquisition where the DAG already serializes (our handoff is the
    two-node version; the general version — precomputed acquisition plans —
    is unexplored and probably worth a TODO note).

**§5.2 — Which are real for us near-term.** Shipped: (2) handoff, partial
(10). Roadmapped: (5) = TODO 2.5, (9) = TODO 2.4, (6) = TODO 4.6, (8) = TODO
2.2. Newly suggested by this pass: (4) node fusion guided by measured
dispatch cost, and the general form of (10). (1), (3), (7) need
prerequisites we lack (transient CPU allocations as declared resources;
enable predicate; NUMA awareness) — long-term at best.

---

## 6. Ordering ambiguity: why it's a problem *and* an optimization

*(Superseded in part: the author challenged this section's framing; the
dedicated resolution document is
[ordering-ambiguity.md](ordering-ambiguity.md), which concedes the
Bevy-nondeterminism argument does not transfer to our deterministic
orientation and rescopes TODO 2.2 as optimizer infrastructure. This section
kept for the record.)*

*(Addresses Q2.3 and "Ordering ambiguity as user-facing friction".)*

**§6.1 — The precise definition.** Two nodes conflict on an object (at least
one writes) but no edge — explicit or derived — orders them. The pipe still
serializes the *accesses* (no data race), but *which order* they run in is
decided by scheduler timing, per run. The result is not corruption; it is
**nondeterministic observed values**: A-then-B computes a different frame
than B-then-A.

**§6.2 — Wait — our derived edges order conflicts by declaration index, so
where does ambiguity come from?** Correct and important: in our graph as
shipped, every conflicting pair gets a derived edge (declaration order is
the tiebreak), so we have no *unordered* conflicts — we have **arbitrarily
ordered** ones. That is the same user-facing problem wearing a seatbelt:
the order is deterministic (good — Bevy's nondeterminism complaints don't
apply verbatim) but *load-bearing on an accident* — the order of `add_node`
calls. Reorder two `add_node` lines in a refactor and observed values change
silently. Bevy's ambiguity detector reports "conflicting access + no
explicit constraint"; our version of the same diagnostic is "conflicting
access whose only ordering is the declaration-index tiebreak" — i.e. *the
edge exists but nobody asked for it*. That is precisely TODO 2.2's edge
provenance.

**§6.3 — Why users need to know when systems run.** Three concrete reasons,
in increasing order of pain: (1) *game correctness is order-sensitive at the
frame scale* — input→movement→camera in one order is responsive, in the
other adds a frame of latency; damage-then-heal vs heal-then-damage decides
whether a character dies. These aren't low-level determinism concerns; they
are gameplay semantics. (2) *Reproducibility infrastructure* — replays,
lockstep networking, server rewind, and — mundane but constant — bug
reports ("happens 1 in 20 runs") and CI flakiness all require the same
inputs to produce the same frame. (3) *Refactoring safety* — when order is
implicit, any change to the schedule (adding a node, changing an access
mode, upgrading the library) can flip an order someone accidentally depends
on. Unity's third-party-package `[UpdateBefore]`/`[UpdateAfter]` web and
Bevy's stageless saga are both this complaint at ecosystem scale.

**§6.4 — And yes, ambiguity is also the optimization space.** Your instinct
is right: a conflicting pair whose order genuinely doesn't matter
(commutative writes — two systems appending to independent keys, two
+=-accumulators) is exactly the freedom a scheduler wants — run whichever is
ready first, or whichever the critical path prefers. The resolution of the
apparent tension: **the system cannot distinguish "order doesn't matter"
from "order matters and nobody said so"** — both look identical in the
access sets. The fix is to make the user classify each flagged pair once:
add an explicit edge (order matters) or mark the pair commutative (order
free — now a legal reordering lever for TODO 2.4c). Ambiguity detection is
not "forbid ambiguity"; it is "convert accidental order into either declared
order or declared freedom." Bevy ships exactly this triage (resolve or
`ambiguous_with`); we'd be following a proven pattern with better
provenance (we know *which object and modes* created each edge).

---

## 7. Cycles and build-time validation

*(Addresses Q2.4: why you never saw a cycle in UE; who is vulnerable; what
RDG validates.)*

**§7.1 — Why you never saw a cycle in UE: the API makes them
unconstructible.** In UE Tasks (and futures, and continuations generally), a
task's prerequisites must *already exist* as handles at launch time. You can
only point backward in creation order — the graph is built in topological
order by construction, and a cycle would require naming a task that doesn't
exist yet. This isn't your usage being unusual, and it isn't under-use: it is
a structural property of every "dependencies at creation" API. What UE *can*
produce is the adjacent failure: a *wait cycle* through `FTaskEvent`s or
nested `Wait()` calls (A waits an event B will trigger, B waits on A) — a
deadlock, not a graph cycle, and diagnosed as a hang instead of a nice error.

**§7.2 — Who actually gets cycles.** Vulnerability requires *two-phase
construction*: create nodes first, add edges between existing nodes second.
Taskflow (`a.precede(b)` on any two tasks), TBB flow graph (`make_edge`),
and our `Graph_node::after/before` all qualify — a refactor or a loop
building edges from data can close a cycle. Systems deriving edges from
declarations are safe-by-construction *within* the derivation: STF systems
(StarPU/OpenMP) infer edges from submission order — always forward, acyclic;
our conflict-derived edges follow declaration index — same property. So in
our graph, **only explicit `after`/`before` can create a cycle** (alone or
by closing a loop through derived edges) — the Kahn check exists for exactly
that residue, plus the data-driven-edges case (a user wiring `before` calls
from config/asset data, where a content author can create the cycle — this
is where cycle *reporting quality* matters: name the nodes on the cycle,
which we can do since we keep node names in all builds).

**§7.3 — How often it bites elsewhere.** Rarely as a graph-build error, and
that's the point of checking: it converts a class of would-be deadlocks into
a build-time message. The high-frequency real-world version is in *derived*
ordering systems with constraint webs: Unity DOTS `[UpdateBefore/After]`
across third-party packages produces unsatisfiable constraint sets
(cycle in the *constraint* graph) that users report regularly; Bevy's
`.before()/.after()` likewise. The lesson for us: cycles will come from the
explicit-ordering web as node counts and team size grow, and the error
message (full cycle path, provenance per edge) is the feature, not the
detection itself.

**§7.4 — What RDG's "rich validation during pass setup" actually is.** A
dedicated validation layer (`FRDGUserValidation`), on by default in
Debug/Development builds and compiled out of Test/Shipping, that "emits
fatal checks with clearly marked resource / pass names as early as possible
if RDG is used incorrectly." Concretely it catches: a pass touching an RHI
resource it did not declare in its parameter struct (resource-access
methods are "restricted to pass lambdas and the validation layer will
assert if the method is improperly called"); `ValidateShaderParameters` —
declared inputs must cover every parameter the bound shader expects
(completeness in the other direction); extraction of a resource "not
produced by any pass" (`ValidateExtractResource` — the check that caught
Epic's own TSR bug, UE-178829); declared-but-unreferenced passes and
resources (warned, then culled); lifetime misuse — the docs single out "a
common mistake is to pass memory into RDG lambda that is not guaranteed to
exist later," the setup/execute timeline split being the graph's signature
hazard. The transferable idea is
**declaration-vs-behavior cross-checking**: the declaration is machine-read,
the actual use is instrumented, and the two are reconciled continuously.
Our harness does exactly this at the object level (undeclared touch →
fatal); what we *don't* yet check is the reverse direction — **over-declaration**
(a node declares write on an object it never touches → false serialization,
silently costing parallelism forever). RDG warns about it; we could too, at
TS_SAFETY_CHECKS level: track per-node "declared but never accessed this
run" and report after N runs. Cheap, high-value, new TODO candidate.

---

## 8. In-graph control flow, precisely

*(Addresses Q3.1: "unroll harder"?, Taskflow condition tasks, CUDA IF/WHILE.)*

**§8.1 — "The fix was never 'unroll harder'" decoded.** "Unrolling" = compiling
a data-dependent loop into a static DAG by replicating the body for the
worst-case trip count: `while (!converged)` becomes body₁→check₁→body₂→check₂→…
→bodyₖ for a guessed K, with later copies skipped at run time. It fails
threefold: K is unknowable (guess low = wrong answer, guess high = huge
graph), the graph bloats (CUDA users literally capture per-batch-size graph
*variants* at 50–200 MB VRAM each — unrolling across a different axis), and
skipped copies still pay dispatch. The sentence means: every system that hit
data-dependent control flow solved it with a *runtime decision primitive*,
never by telling users to pre-expand all outcomes.

**§8.2 — Taskflow condition tasks, mechanically.** A condition task is a
node whose body returns an `int`; the returned value *selects which outgoing
edge fires* (0 = first successor, 1 = second, …). Non-selected successors
don't run — unless reached some other way. Edges out of a condition task are
**weak** dependencies; ordinary edges are **strong**. A node fires when its
strong-dependency counter hits zero OR any weak edge into it fires — that
asymmetry is what lets a back-edge (loop) work without deadlocking the
counter, and it is also the footgun: a node with both strong and weak
in-edges from concurrently active paths can fire twice, or race its own
second invocation; Taskflow's docs carry explicit warnings. Cycles are
allowed (a condition task returning 0 to loop back re-fires the loop body).
Powerful, and semantically sharp-edged — the reason TODO 2.6 deliberately
stops at predicates.

**§8.3 — CUDA conditional nodes, mechanically.** CUDA 12.3+ adds IF and
WHILE *container* nodes: the node owns a *body graph* and a **condition
handle** — a device-side value any kernel can set
(`cudaGraphSetConditional(handle, pred)`). IF: on reaching the node, if the
handle is nonzero, launch the body graph once. WHILE: re-launch the body
graph as long as the handle is nonzero (the body itself updates the handle).
The control decision is made *on the GPU during execution* — no CPU
round-trip — which is the entire point: before this, "check convergence"
meant sync-to-host per iteration. Note both vendors converged on the same
shape: a *structured* control node wrapping a subgraph (not arbitrary
edges), preserving analyzability. If we ever go past predicates, that —
a `repeat(subgraph, predicate)` structured node — is the design to copy,
not Taskflow's free-form weak edges.

---

## 9. Rebuild-per-iteration, the robotics case, and why render graphs redeclare

*(Addresses Q3.2, including "high chance I won't be satisfied with a simple
explanation" and the PaRSEC point.)*

**§9.1 — Why a robotics graph "rebuilds at 1 kHz": it didn't want to.** The
Taskflow #49 user had a *stable* control pipeline (sense→estimate→plan→act)
running one iteration per millisecond. Early Taskflow coupled graph objects
to executions, so each iteration re-created the graph — allocation spikes =
latency jitter = unacceptable in a control loop. The user was *asking for*
build-once/run-many; Taskflow added graph reuse in response. This friction
is evidence *for* our design (allocation-free re-runs), not against it. The
transferable requirement it does impose: re-run must be jitter-free, not
just amortized-cheap — worth stating as an explicit guarantee and testing
(no allocation, no lock convoy on `execute()`).

**§9.2 — Why RDG/FrameGraph rebuild every frame — the deep version.** Four
compounding reasons, in decreasing order of fundamentality:

1. **The variant space is exponential, so memoization keys are worthless.**
   The pass set is a function of: visible light count and types, shadow
   cache states, active post-effects, quality settings, resolution,
   HDR/SDR, split-screen count, debug views… Each is a dimension; a
   compiled-graph cache would need a key over all of them and would miss
   constantly. When shape variance is *combinatorial*, rebuilding is
   cheaper than caching. (Contrast the game-side frame skeleton: one
   variant, maybe a handful — which is why build-once fits there.)
2. **Transient resources are sized by frame content.** Aliasing (§4.4)
   needs exact lifetimes and sizes *this frame*; a persistent graph would
   recompute allocation anyway — most of "compile" would run per frame
   regardless, so keeping the topology static saves little.
3. **Authorship locality beats structure reuse.** Declaration code *is* the
   renderer's feature code: a feature's passes are declared inline where the
   feature lives, by the engineer who owns it, conditioned on that feature's
   flags. A persistent central graph would concentrate all features'
   structure in one place with cross-team merge conflicts — the
   immediate-mode style is a *social* architecture win, repeatedly cited by
   its designers. ("Immediate-mode-looking code, retained-mode execution.")
4. **They made compile cheap enough that reuse buys ~nothing.** Linear
   passes over ~100–400 pass declarations, arena-allocated, no general
   subgraph isomorphism — per-frame compile is tens of µs. Once compile ≪
   frame budget, build-once is *all* downside (staleness risk, invalidation
   logic) and no upside. Granite is the halfway house that proves the
   trade: it bakes (compiles once) because its pass set is stable per
   scene-configuration, and rebakes on change — i.e. *the choice tracks
   shape volatility, exactly as §3.2's lesson states.*
5. And the point that unifies them (also answering "why not a static graph
   with dynamic-task deviations"): in a renderer the "deviations" *are*
   first-class passes needing barriers, aliased memory, and culling — the
   whole-graph analyses. Dynamic escapes bypass analysis; a renderer wants
   the analysis *on the varying part most of all*. Where analysis-on-
   the-varying-part is not needed — our CPU case — the static-skeleton+
   dynamic-escape split is sound. That's the honest asymmetry between our
   graph and render graphs, and it's also §2.5's boundary restated.

**§9.3 — The PaRSEC point, decompressed.** PTG (§1.3) shows there is a
*third* answer besides build-once and rebuild-per-run: the graph as a
**formula** — dependencies expressed algebraically ("task(i,j) ← task(i-1,j)"),
evaluated lazily per task, never materialized. Zero build cost, zero rebuild
cost, O(1) graph memory even for 10¹² tasks. Its price: you must express
your dependence structure as closed-form index algebra (the JDF language) —
which real users found so hard that PaRSEC added DTD (sequential submission,
runtime discovery) *knowing* it scaled worse. Relevance to us: (a) it is the
strongest evidence that build-time ergonomics dominate adoption — a
formally superior representation lost to a friendlier worse one; (b) our
access-derived edges are our JDF-avoidance — declarations are the friendly
front-end from which structure is derived; (c) no CPU-game-frame workload
needs formula-graphs — our graphs are O(100) nodes, materialization is
free. So: an instructive extreme, not a direction.

**§9.4 — What our answer to occasional shape change should be** (from the
original doc §7.3, now sharpened): (i) measure and publish `compile()` cost —
if it's tens of µs for ~100 nodes, per-frame or per-mode-switch rebuild is
simply *supported* and most of point (3) evaporates; (ii) pre-compiled
variants for small discrete mode sets (LOD tiers, in/outdoors); (iii) the
enable predicate for per-run node subsetting; (iv) explicitly do NOT build
topology-diffing exec-update (CUDA's) — its complexity exists because GPU
graph instantiation is expensive; ours isn't.

---

## 10. Lifetime hazards: what more can be done than we do

*(Addresses Q3.3: "what else can be done; why is destroying a taskflow while
the executor runs so hard to check?")*

**§10.1 — Why Taskflow couldn't "easily assert" it.** The #82 crash is
instructive because it wasn't even the documented UB — it was a
*static/thread_local destruction-order* bug inside Taskflow's own per-thread
allocator (a `thread_local` handle outliving the memory-pool manager it
writes into at thread exit, corrupting a freed vector). The reporter
explicitly asked for "asserts... to prevent this kind of dangling reference"
(citing Kakoune's SafePtr — a debug-checked ownership pointer). The general
reasons a "graph destroyed while running" assert is harder than it looks:
(1) the check itself races — destruction is concurrent with the completion
transition it's checking, so a naive flag test can pass and then the state
changes; a correct check needs the dtor to synchronize with executor state
(an atomic run-count with acquire semantics), which is cheap but must be
designed in, not bolted on; (2) the crash site is far from the cause
(teardown of an unrelated static), so even detecting corruption doesn't
attribute it; (3) the hazard extends past the graph object to everything the
closures capture — the library can guard its own structures, never the
captured world. Taskflow's actual mitigations: documentation + move-ownership
overloads (`executor.run(std::move(tf))` — transfer the graph into the
executor so it can't dangle).

**§10.2 — What we do already, and the gaps.** We track sequential runs (one
`execute()` in flight), so unlike Taskflow the graph knows whether a run is
live — a `TS_SAFETY_CHECKS` fatal in `~Static_task_graph` when the current
run's `done` isn't settled is nearly free and closes the exact #82-class
misuse. Gaps beyond that: (a) a `Guarded` object destroyed while a compiled
graph still references its pipe — the graph holds raw pointers; proposal: a
pipe-side registration count (compile increments, graph dtor decrements;
`~Guarded` fatals if nonzero, naming the graph) — cheap, TS_SAFETY_CHECKS
only, and symmetric with the recorder/journal dtor rule the deferred layer
already enforces; (b) node closures capturing stack locals by reference —
undetectable in general (same boundary every C++ system hits; Rust is the
language answer); the guide should state the rule the way Taskflow's
cookbook does, plus our `Named` diagnostics make the fatal message
actionable. Recommendation: add (a) and the dtor-while-running fatal as a
small P1 item; document (b).

---

## 11. Debugging and observability; "eager mode"

*(Addresses Q3.4.)*

**§11.1 — What "eager mode" means.** PyTorch terminology: *eager* = each
operation executes immediately when the line runs (normal Python semantics —
you can print, step, breakpoint anywhere); *graphed* = operations are
captured/compiled and replayed later as a unit, so the code you wrote is no
longer the code that runs. It is not single-threaded per se — it is
*execute-at-declaration-site*. The debugging recommendation everywhere
(PyTorch, RDG) is: develop in eager, switch to graphed when correct. RDG has
exactly this: `r.RDG.ImmediateMode` "bypasses graph compilation in favor of
executing the pass directly" — a validation failure then has the offending
pass on the call stack instead of firing later in a compiled walk.

**§11.2 — Our equivalent, and a cheap proposal.** Our graph is less exposed
than CUDA (bodies are ordinary closures; a debugger breaks in them fine;
nothing captures stale pointers by design since data flows through `Guarded`
state). What we lack is the *localization* tool: a serial debug mode.
Proposal (small): `execute({.serial = true})` — run nodes one at a time in
the compiled topological order on the calling thread, full harness active.
Gives: deterministic single-step debugging, a bisection baseline ("breaks
parallel, works serial → ordering/declaration bug; breaks both → logic
bug"), and a Heisenbug-free repro environment. Bevy ships exactly this
(single-threaded executor as a supported option) and its users use it
constantly. Near-zero implementation cost (the Kahn order exists; dispatch
loop is a for-loop).

---

## 12. The Bevy regression: pathological, or the fate of fine-grained schedules?

*(Addresses Q3.5.)*

**§12.1 — Two separate cases, one pathological and one not.** Issue #11378
(the "80+% of compute time in the executor" one) IS the pathological case:
6000 systems (2000 replicated groups), executor called ~17,000 times per 129
schedule runs at ~470 µs per call, 12 FPS at 7% CPU on 32 cores — a stress
construction, though note the user built it *expecting* group-level
parallelism, i.e. the granularity trap invited real usage. Discussion #5958
is NOT pathological: a shipped iOS game with an ordinary system count, where
the parallel executor measured **~1113 µs median schedule vs ~617 µs
single-threaded** (p90 1499 vs 742), and used *more* CPU (~50% vs ~40%). The
mechanism (discussion #8304, maintainer profiling): per-system completion
signaled over an async channel between lock-free queues — "multiple atomic
fences and potentially the delay of waking up an OS thread" per system —
plus "multiple 10–70+ µs segments where we're just waiting on the OS to wake
up" (mobile cores park aggressively). Their mitigation direction is
literally our architecture: "moving the multithreaded executor... to running
synchronously in the system task itself after the system has completed"
(= our completion-driven dispatch: `node_complete` runs on the settling
worker; no executor thread, no channel), plus work-stealing queues inside
the executor (= our Chase-Lev deques).

**§12.2 — What it means for us.** The lesson isn't "Bevy did it wrong";
it's the granularity floor made visible: when median system cost is a few
µs, *any* per-system dispatch cost dominates, and OS wake latency (§3.6)
turns idle-worker parallelism into a net loss. We already have the two
structural mitigations Bevy is retrofitting (completion-driven dispatch on
the settling thread; stealing deques) and the two escape valves they added
(single-threaded fallback — our §11.2 proposal doubles as it; inline
dispatch). What we should copy is the *honesty*: publish the per-node
dispatch cost table and the merge-below-X-µs guideline (original doc §7.2),
because our 19-node sample can't reveal this class of problem — a
1000-tiny-node graph would.

---

## 13. Access-control systems compared: who does what better, and where our harness fails

*(Addresses Q3.6 in depth, plus "focus on systems that provide any kind of
access control, compare, point out what they do better.")*

**§13.1 — The comparison table.**

| System | Declaration unit | Check time | Enforced in ship builds | Catches escaped refs | Derives schedule from declarations |
|---|---|---|---|---|---|
| **Unity Job Safety** | NativeContainer (whole container) | Runtime, editor/play-mode only | No | Partially (handle travels inside the view struct) | No — validates a hand-wired JobHandle DAG against declared access |
| **Bevy** | Component/resource *type* | Compile (types) + schedule build | Yes (it's the type system) | Yes within safe Rust; not through `UnsafeWorldCell` | Yes (parallelism, not order) |
| **Rust generally** | Type ownership/borrows | Compile | Yes | Yes in safe code | n/a |
| **UE thread guards** | Whole thread class (`IsInRenderingThread()`) | Runtime, dev builds | Usually no (`checkSlow`) | No | No |
| **Chromium SEQUENCE_CHECKER** | Per-object sequence affinity | Runtime, DCHECK builds | No | Only if the callee's methods are instrumented | No |
| **RDG/FrameGraph** | Per-resource per-pass | Graph build + runtime validation layer (Debug/Dev) | No | n/a (GPU resources) | Yes (barriers, aliasing, order) |
| **Destiny engine** | Per-resource, frame-phase | Runtime | Unknown (GDC talk) | — | Partially |
| **Go race detector / TSan** | None — tracks actual memory accesses | Runtime, instrumented builds | No (2–20× cost) | **Yes, unconditionally** | No |
| **This library** | Per-object *instance* | Runtime, `TS_SAFETY_CHECKS` builds | Optional (compile-out) | **No — the main gap set, §13.6** | **Yes (order + parallelism + harness)** |

**§13.2 — Unity, mechanically (what they do better #1: views carry their
safety state).** Every `NativeContainer` view struct embeds an
`AtomicSafetyHandle` referencing central per-container state; every access
site calls `CheckReadAndThrow`/`CheckWriteAndThrow`; the job system stamps
each job's handles with the granted mode at schedule time, and — the
definitive framing, from Joachim Ante: "The safety system essentially cross
checks read/write access of containers against the actual provided job
dependencies." Two ideas are genuinely better than ours: (1) **versioned
handles catch stale aliased views** — a cached `NativeSlice` over an array
that a newly scheduled write-job owns is *invalidated by a version bump*, so
the escaped view's next use throws even though the escape happened earlier;
our escaped references keep working (§13.6); (2) **the check state travels
with the reference**, so access from any context is checkable without a
thread-local. Their weaknesses vs ours: whole-container granularity rejects
provably-disjoint writers (pushing users to
`[NativeDisableContainerSafetyRestriction]`, which turns tracking off
entirely); editor-only enforcement ("in runtime builds there aren't any
guarantees these cases won't cause crashes"); statics bypass everything
("Accessing static data from a job circumvents all safety systems"); and
scheduling is main-thread-only ("You can only call Schedule and Complete
from the main thread") — no nested/worker-spawned jobs at all, which our
grant-inheritance model handles as a first-class feature.

**§13.3 — Bevy/Rust, and your Arc<Mutex> intuition — confirmed with data.**
Your model ("borrow checker prevents races in simple cases; anything complex
moves to runtime constructs") matches the empirical studies precisely. Qin
et al. (PLDI 2020; 850 unsafe usages, 170 bugs across Servo, TiKV, two OSes,
Firefox components): **all 70 memory-safety bugs involved unsafe code** (the
static guarantee is real) — but **25 of 41 non-blocking concurrency bugs
were in safe code**, and **all 59 blocking bugs (deadlocks) were in safe
code**, 30 of them *double-locks caused by Rust's own implicit
scope-end unlock* (the guard lives longer than the developer thinks — the
ownership model created a new deadlock idiom). A companion study of Servo/
TiKV/Rand: races in safe code all came from atomics misuse; and Servo
carries 6.6 `unsafe` tags per KLOC — the escape hatch is routine, not
exceptional. At the ecosystem level, RustSec lists **63 thread-safety
advisories**, dominated by one bug shape: a hand-written `unsafe impl
Send/Sync` that doesn't propagate the inner type's constraint — including
`std` itself (CVE-2017-20004: `MutexGuard<Cell<i32>>` wrongly `Sync`) and
tokio (RUSTSEC-2021-0124: oneshot close/send race, memory corruption in the
flagship runtime). And the interior-mutability finding is directly ours to
heed: 13 bugs came from *mutation hiding under an immutable borrow* — the
exact hazard our A3 rule (read positions invoke with `const T&`, mutation
under a read classification is a compile error) exists to prevent. So:
compile-time checking eliminates one class completely, moves the failures
into the escape hatches and the semantic layer (deadlocks, atomicity, cross-
lock TOCTOU) — which no access-control system, ours included, can see.

**§13.4 — What the borrow checker still does better than our harness.**
Within safe code it is *sound and total*: every alias, every escape, every
path — checked at compile time, zero runtime cost, enforced in shipping.
Our harness is a sampling oracle: it fires only when an instrumented method
actually executes on a violating path in a checked build. Rust's guarantee
degrades at `unsafe`; ours degrades at every uninstrumented access. The
honest ranking for the guide: borrow checking > our harness > thread guards
> convention; our compensations are per-instance granularity (finer than
Bevy's types), schedule *derivation* (Unity validates, doesn't derive), and
zero model constraint on ordinary C++ (design.md §7.9's deliberate trade).

**§13.5 — What the others do better — the transferable list.** (1) Unity's
version-stamped views (→ inspiration for closing our §13.6 gap); (2) RDG's
*reverse* check — unused/over-declaration warnings ("declared but never
accessed"), which costs parallelism silently in our graph today (§7.4
proposal); (3) RDG's immediate mode (→ §11.2 serial mode); (4) Chromium's
`DETACH_FROM_SEQUENCE` — an explicit re-binding ritual for ownership
transfer, cleaner than implicit adoption (relevant to future `Guarded`
adoption, TODO 1.5); (5) Go/TSan's unconditional coverage of escaped
references — unmatched by any declaration system; our TSan-on-Linux CI loop
is the correct complement and should be advertised as *part of the safety
story*, exactly as Uber runs periodic race-detector sweeps rather than
production enforcement; (6) Destiny's frame-phase dimension: their tracker
validates accesses against *when in the frame* they occur, not just who —
our graph has this information (node identity) and could check "object
accessed outside its declared frame window" for external threads someday.

**§13.6 — Where our harness will not help — the honest catalog.** In
increasing order of subtlety:

1. **Uninstrumented methods** — a public method missing `TS_CHECK_ACCESS()`
   is a silent hole (TODO 1.4's clang-tidy check is the answer; until then,
   convention).
2. **POD / methodless types** — nothing to instrument (TODO 1.4's
   `Guarded_value`, or the shelved `Granted<T>` wrapper).
3. **Escaped raw references** — the user's own example: a body passes `T&`
   to a helper that *stores* it; later access through the stored pointer via
   free functions or field access never enters an instrumented method. The
   harness cannot see it; TSan can. Unity partially catches the analogous
   case only because their views carry safety state. Mitigation tiers:
   convention ("never store the ref"), the `Granted<T>` wrapper (deref
   re-checks; still launderable via `T&`), TSan sweeps.
4. **Sub-object escapes** — a method legitimately returns a span/pointer
   into the object; subsequent access bypasses the object's methods entirely
   (TODO 1.6's range harness is the designed answer).
5. **The grant-snapshot validity hole (found while writing this report).**
   `ts::launch` (unlike `ts::nested`) inherits the launcher's grant *by
   value with no validity window*: a graph-node body that `ts::launch`es
   work touching the node's objects produces a task that may run *after the
   node completed and released those objects* — the harness sees a valid
   inherited grant and stays silent while the access races the next
   acquirer. `access.h`'s own comment documents the mechanism ("possibly
   after the launcher's body unwinds — still holding the grant") — designed
   for legitimate fire-and-forget from plain tasks, but unsound from graph
   nodes and pipe accessors, whose grants have an end. This is exactly the
   RustSec bug shape (§13.3): an inheritance edge that grants capability
   without propagating its constraint. Proposed fix (new TODO): stamp
   `Access_context` snapshots with a per-grant generation; `access_check`
   compares against the pipe's current generation and fatals on a stale
   grant ("task outlived the access scope it inherited from") — turns a
   silent race into a diagnostic. Alternative (blunter): inherit into
   `ts::nested` only; plain `launch` from a granted scope gets an empty
   context.
6. **Semantic races the declarations can't express** — check-then-act
   across two accesses (read version, then write assuming it), lost-update
   patterns, deadlock via `sync()` misuse: correctly ordered, correctly
   declared, still wrong. Rust's studies (§13.3) show these *survive even
   compile-time soundness*; no roadmap item fixes them; the docs should say
   so plainly (the pipe's FIFO at least makes cross-access reasoning
   deterministic).
7. **Shipping builds** — `TS_SAFETY_CHECKS=0` strips the oracle; every
   surveyed system shares this (Unity editor-only, RDG Dev-only, Chromium
   DCHECK, `checkSlow`) and the documented industry consequence is "works
   in editor, corrupts in shipping." Our differentiator candidate: the
   harness is ~1 ns/check — cheap enough that *leaving it on in shipping*
   is actually viable for many titles; worth benchmarking and offering as a
   recommended default rather than assuming the industry pattern.

**§13.7 — Bottom line.** Our harness occupies a useful middle: stronger
than thread guards/convention, weaker than borrow checking, uniquely paired
with schedule derivation. Its failure modes concentrate exactly where every
surveyed system's do — escape hatches and uninstrumented surface — and the
5 findings above (esp. the grant-generation fix, the over-declaration
report, and TSan-as-advertised-complement) are the concrete course
corrections this comparison yields.

---

## 14. API surface and ergonomics

*(Addresses Q3.7.)*

**§14.1 — The measured ergonomics evidence.** Taskflow vs TBB, same
7-task/8-edge graph: 17 LOC vs 37 LOC, and the TCAD paper's OpenTimer
rewrite: 9123 → 4482 lines of parallel code, max cyclomatic complexity
58 → 20, attributed to replacing exhaustive OpenMP depend clauses with graph
code. The general adoption pattern: Taskflow grew in TBB's own niche almost
purely on ergonomics; Rayon/GCD win mindshare on "the model fits in your
head"; `std::execution` is the cautionary opposite. The survey's principle
stands: ease comes from *removing a decision* or *removing a failure mode*.

**§14.2 — Where we sit, honestly.** Our core spelling is genuinely small —
one rule (const-ness = mode) carries `access`/`async`/multi-object/graph
nodes; `launch/task/then/when_all` match industry idiom. The width is real
though: three option aggregates, seven-ish verbs, plus
`Deferred/Versioned/Signal/parallel_for/coroutines` — and it will grow
(D5's four primitives). Three defenses, all cheap: (1) **layered
disclosure** — the guide already orders layers; enforce a "you can be
productive with §2's six functions" front page, and keep the umbrella
header honest; (2) **name discipline** — the `access/async` verb split and
the one spelling rule are our GCD-grade decisions; protect them in review
(the flagged `Access_options`/`Task_options` unification belongs here);
(3) **counted comparisons** — an honest "same program in ours/Taskflow/TBB"
appendix in the docs; we win the wiring count by construction (derived
edges = zero `precede` calls), which is the measurable form of our
ergonomics claim. Risk to watch (original doc §3.7): *invisible* edges
surprising users — the mirror cost of no wiring; ambiguity provenance (2.2)
and the DOT dump are the counterweights and should be presented as core
workflow, not tooling extras.

---

## 15. The hybrid consensus, Cilk, UE's phases, and the steelman leftovers

*(Addresses Q4.1, the Cilk reference, Q4.2, Q4.4/4.5, Q4.6.)*

**§15.1 — Q4.1: the consensus on static-skeleton + dynamic-fill.** No
system ships a *bidirectional* integration like ours (dynamic work that
composes with the graph's arbitration via shared pipes); but the
architectural pattern itself — stable declared skeleton, dynamic content —
is the de-facto shipping consensus: Destiny's frame is "job strings"
structuring the main loop with data-parallel jobs inside (their GDC talk
also describes a runtime access tracker — the closest shipped cousin of our
whole thesis); UE's tick groups + TaskGraph fill; Bevy's schedule +
one-shot systems; Taskflow's explicit selling point is "the same API for
static and dynamic tasking" (subflows); OpenTimer's stated reason for
adopting Taskflow was "model both static and dynamic task dependencies."
Where the consensus is thinner: HPC's STF world found that *pure* dynamic
discovery hits a submission-analysis wall at scale and grew static escapes
(taskgraph record/replay, Legion tracing, graph trimming — the last one
notable because trimming STF "essentially expresses the same information as
PTG": the dynamic model's fix degenerated into the static model). Read
together: the field converges on our exact split from both directions.
What nobody has shipped — and our §2.4 seams define — is fine-grained
*edge* exchange between the two halves; that is the genuinely open part of
our bet, and the reason to keep the dynamic layer first-class rather than
an escape hatch.

**§15.2 — The Cilk reference, decoded.** The steelman cites Cilk for two
properties. *Serial elision*: a Cilk program with `spawn`/`sync` deleted is
a valid sequential program computing the same result — the parallel program
is its own sequential specification; there is no graph artifact to
construct, keep alive, invalidate, or debug — the "graph" is the call tree,
reconstructed fresh by every execution. *The steal bound*: T_P ≤ T_1/P +
O(T_∞) — greedy work stealing is provably within a constant of optimal for
whatever DAG the execution unfolds, no scheduler tuning. Together they are
the strongest form of "you don't need a graph object": correctness from
sequential semantics, performance from a theorem. The limits (why this
doesn't refute our layer): the implicit DAG is *fork-join only* (series-
parallel); it cannot express our cross-system frame DAG (propagation feeding
three consumers is not a nested fork-join), offers no cross-run
amortization, no whole-graph analysis, no access safety — and its
determinism claim holds only for programs already race-free, which is the
part Cilk doesn't check (Cilkscreen/Cilksan exist because it can't).
`parallel_for` + retraction is our Cilk-shaped subset, with the same
serial-elision flavor (caller participation ≈ the serial schedule).

**§15.3 — Q4.2: UE's fourteen-odd phases — agreed, and it sharpens our
pitch.** Your reading matches the evidence: `ETickingGroup` (TG_PrePhysics,
TG_DuringPhysics, TG_PostPhysics, TG_PostUpdateWork…) + per-function
`AddTickPrerequisite` + `bRunOnAnyThread` is *literally a coarse static
frame graph* — declared groups, declared edges, opt-in parallel dispatch —
sitting in the engine for a decade, and underused for parallelism. Why: no
access model. Nothing declares what a Tick touches, so the engine cannot
safely default anything off the game thread; `bRunOnAnyThread` is opt-in
precisely because safety is by unchecked convention, and adoption follows
the completeness hazard (one undeclared touch = a shipping race). This is
the cleanest possible statement of our thesis: **UE has the graph shape and
the dynamic runtime; what it lacks is the declared-access layer that would
make the static structure safely parallelizable** — this library exists to
supply exactly the missing piece. Consider this framing for design.md's
positioning section.

**§15.4 — Q4.4/4.5: solvable by dynamic tasks?** 4.4 (closed-world
composition): substantially yes — `ts::nested` + inherited grants is the
sanctioned "spawn from a library callback" path, and `Guarded::async`
composes with the graph through the pipes (most systems' seam is unsound;
ours is the differentiator). Residual: dynamically spawned work is invisible
to graph-level planning (§2.4) — accept and document. 4.5 (load
adaptivity): mostly moot for us — our nodes dispatch through the same
work-stealing deques as dynamic tasks; the static *shape* doesn't pin
placement. The steelman's real warning is for *schedule shaping* (TODO
2.4/2.5): a rank computed from yesterday's profile can encode stale load —
which the 2.4 dry-run's ambient-drift finding already demonstrated. The
mitigation is the item's own design: ranks as dispatch *hints* (order among
ready nodes), never reservations; re-derive from rolling measurements.

**§15.5 — Q4.6: the "copying" point — you read it right, it's not about
user data.** PyGraph's "parameter copies" are CUDA-graph-internal: kernel
launch parameters are baked into the executable graph, so updating them
means copying into the graph exec structure per launch — an artifact of
GPU graph instantiation, with no analogue here (our node bodies read live
`Guarded` state; nothing is baked). The transferable residue is only the
general cost-benefit framing: static machinery has fixed costs (compile
time, N-variant memory), and "graph everything" can lose to eager below
some threshold — our version is just "don't put 2-µs work in graph nodes"
(§3) plus publishing `compile()` cost (§9.4). No course correction needed.

---

## 16. Who needs millions of tasks (Q6.3)

**§16.1 — The concrete case.** OpenTimer: static timing analysis over
million-gate netlists (netcard: 1.4M gates). The task graph mirrors the
circuit: per-gate/per-pin arrival-time propagation, "hundreds of millions of
tasks and dependencies" across incremental iterations. Why not coarsen: the
dependency structure IS the netlist (not chosen); incremental updates
re-propagate only affected cones (small task subsets — only meaningful if
tasks are small); and levelized batching (their OpenMP v1) couldn't express
dynamic dependencies, which is *the stated reason* they moved to Taskflow.
Notably even there the library's job is graph bookkeeping at scale, not
magic: v2 is 3–4% slower than OpenMP on one core and both saturate at ~10
cores — "the structure of a timing graph dominates the maximum parallelism."
Similar shapes: sparse linear solvers (elimination trees), build systems,
dataflow analytics. **None of this is our market**, and the design docs
should say so once, plainly: we target O(100)-node frame skeletons over
coarse subsystems; workloads whose graphs are problem-given and huge are
Taskflow/HPC territory. (Our dynamic layer would carry a few million
*dynamic* tasks per second fine; it's the *graph* representation that's
deliberately not sized for millions of nodes.)

---

## 17. Run-many result plumbing (Q7.2.2)

**§17.1 — Why it matters more than "nice to have."** The evidence chain:
Taskflow's own maintainer, explaining reusable graphs, warns "each framework
is *stateful* — subsequent runs may produce different results based on
user's algorithms" — the run-many model forces results into side state,
and the library then can't help you sequence reads of that state against
runs. In systems with *no* access model (Taskflow), that's an inconvenience;
users wire futures/promises by hand around the graph. For *us* it is
subtler and more principled: results routed through `Guarded` state are
declared and safe (this is the designed data plane, and why the pressure is
lower for us than for Taskflow) — but *per-run scalar outputs* (frame
stats, a convergence residual, "did the query hit") don't deserve a
`Guarded` each; today they become ad-hoc members captured by reference,
which is precisely the undeclared mutable state our thesis frowns on. So
typed chaining (TODO 2.1: a node minting a per-run `Task<R>`) is not
convenience — it closes the last sanctioned-but-undeclared data path in the
graph model. Your suspicion is half right though: it's a P1 for hygiene,
not because users are blocked (dynamic `when_all`/`then` already cover
result-bearing composition outside the graph).

---

## 18. Profiler-guided scheduling, explained properly (7.3-2.4/2.5)

**§18.1 — What the original paragraphs meant, in plain terms.** TODO 2.4:
record real per-node durations from runs; use them to *reorder dispatch* —
when several nodes are ready at once, start the one on the longest
remaining path (the critical path) first, so the frame's spine never waits
behind filler work. TODO 2.5: compute that "longest remaining path" rank at
`compile()` (upward rank = my duration + max over successors' ranks — one
O(V+E) pass over the Kahn order) and use it to bias *which ready node the
settling worker runs next* and *submission order into the deques* — shaping
dispatch without touching the scheduler's priority classes (whose two
structural problems the dry-run exposed: `high` detours through the global
queue; `low` is valve-gated). This is HEFT-family list scheduling, the
textbook approach; StarPU is the production precedent (measured per-task
performance models driving schedule decisions — their `dmda` scheduler).

**§18.2 — How others tackled it (2.4/2.5's "collect how others did").**
StarPU: auto-calibrated per-(task,device) duration histories; model-based
placement; the caution from their literature — models mispredict under
contention and input variance, so schedulers treat them as hints. Legion:
mapper callbacks expose the decision to the application rather than
auto-optimizing. Halide: offline autoscheduling over a cost model — valid
because the algorithm/schedule split makes candidate schedules
semantics-preserving; our analogue is the annotated-movable-edges search
(staleness-OK `Versioned` readers as the movable DOF), which is exactly the
"user ratifies semantic moves" design. TFProf/Taskflow: visualization
first, optimization second. The consistent field lesson, matching our
dry-run: measurement infrastructure precedes levers — interleaved A/B,
medians, per-lane timelines; single-run deltas are noise (~25% ambient
drift vs 5–10% lever effects in our own experiment).

---

## 19. Course corrections: what this pass changes

**§19.1 — Confirmations (no change).** The layering thesis (coarse static
skeleton + dynamic fill + one access model) matches where every surveyed
ecosystem converged from both directions (§15.1); the granularity position
(§3.4) is defensible and evidence-backed; declaration-derived edges with a
runtime oracle remain unoccupied territory in general-purpose C++ (§13.1's
table has no other row with the last two columns both "yes").

**§19.2 — New TODO items this report proposes.**
1. **Grant-generation validity check** (from §13.6.5) — close the
   `ts::launch`-inherited stale-grant hole: per-grant generation stamped
   into snapshots, checked in `access_check`. P1-adjacent: it's a
   soundness gap in the safety story we advertise.
2. **Over-declaration report** (from §7.4, RDG precedent) — "node declared
   X but never accessed it in N runs" diagnostic; recovers silently lost
   parallelism; rides the harness.
3. **Serial execution mode** (from §11.2, RDG immediate mode / Bevy
   single-threaded executor precedent) — `execute({.serial=true})`; cheap;
   doubles as the small-workload fallback.
4. **Graph/Guarded lifetime fatals** (from §10.2) — dtor-while-running
   fatal + pipe registration count so `~Guarded` catches a live compiled
   graph referencing it.
5. **Publish `compile()` cost + per-node dispatch cost + merge-below-X
   guideline** (from §9.4, §12.2) — the two numbers every sophisticated
   evaluator asks first; converts "rebuild per frame" from anti-pattern to
   measured option.
6. **Blocking-sync-under-node-grant runtime check** (from §3.5) — the
   documented-but-unenforced rule; same family as the coroutine
   suspend-under-guard fatal.
7. (Note, not new) Items the user asked to add — in-graph control flow,
   whole-object serialization, sequential-runs-only, run-many results —
   already exist as TODO 2.6, D5/docs guidance, 2.3, 2.1 respectively;
   §19.3 adjusts two of their priorities.

**§19.3 — Priority adjustments the evidence argues.** 2.2 ambiguity
detection: confirm the raise, and ship **on-by-default Warn** (Bevy's rot
lesson, §4.5). 2.6 enable predicate: promote within P2 — it is the answer
to spectrum points (2)+(4) and render-graph-style conditional passes
(§2.2, §2.6), the single highest-leverage graph feature after 2.1.
Blocking-check and lifetime fatals: fold into the pre-public hardening
sweep (cheap, and they close documented-but-unenforced contracts, the
category users trip first).

**§19.4 — Positioning adjustments for design.md.** (a) Add the UE-tick-
groups framing (§15.3): "the industry already ships static frame skeletons;
what's missing everywhere is the access layer that makes them safely
parallel — that's the product." (b) State the §2.5 boundary explicitly:
top-level data-dependent shape (renderers) is out of scope by design; cite
the render-graph rebuild rationale (§9.2) as the reason that domain builds
its own compiler. (c) Advertise TSan CI as part of the safety story (the
Uber pattern, §13.5), not an internal detail. (d) Consider the
shipping-builds harness stance (§13.6.7): ~1 ns/check may allow "leave it
on" as a recommended default — a genuine differentiator vs every
editor-only competitor, pending a benchmark.

---

## 20. Source index

**§20.0 — Verification status.** The load-bearing external claims went
through a 3-vote adversarial verification pass (each claim attacked by three
independent verifiers against the primary source): 25 claims verified, 0
refuted; of the 12 synthesized findings, 11 passed 3-0 and one 2-1 — the
"work stealing is Pareto-optimal for µs-scale tasks" result (NSDI'22,
McClure et al.), where the dissent notes the conclusion is simulation-based
(validated on Caladan) and scoped to commodity hardware; the out-of-scope
counter-regimes (hardware-assisted centralized steering, preemptive
single-queue designs) are immaterial for a CPU game-engine scheduler. Treat
that one number set as directional rather than exact.

Beyond the original doc's appendix (all of which was re-used), the sources
this pass added or leaned on:

- **Task Bench / METG**: Slaughter et al., SC 2020
  ([taskbench2020.pdf](https://legion.stanford.edu/pdfs/taskbench2020.pdf)) —
  METG(50%) definition; 390 ns best-case (trivial dependencies, small CPU
  clusters); 0→3 dependencies raises METG mean 21×; ~100 µs at-scale bound;
  overheads spanning five orders of magnitude across 15 systems.
- **µs-scale scheduling costs**: McClure et al. (Caladan/Shenango lineage) —
  work-steal check ~120 ns; core wake/reallocation ~1 µs IPI floor, 2.2–7.4 µs
  (Shenango), 29 µs (Arachne); work stealing on the latency/efficiency Pareto
  frontier across all tested regimes.
- **Bevy executor evidence**:
  [discussion #5958](https://github.com/bevyengine/bevy/discussions/5958)
  (iOS game: parallel 1113 µs vs single-threaded 617 µs median, CPU 50% vs
  40%), [discussion #8304](https://github.com/bevyengine/bevy/discussions/8304)
  (async-channel completion signaling; 10–70 µs OS wake segments; inline-
  executor mitigation), [issue #11378](https://github.com/bevyengine/bevy/issues/11378)
  (6000 systems, executor 80+% of compute, 470 µs/invocation),
  [PR #13950](https://github.com/bevyengine/bevy/pull/13950) (370+ ambiguities
  in Bevy's own schedules; CI gate),
  [issue #11796](https://github.com/bevyengine/bevy/issues/11796) (ambiguity
  detector ignores query filters; closed not-planned),
  [RFC 45](https://github.com/bevyengine/rfcs/blob/main/rfcs/45-stageless.md).
- **Unity safety system**:
  [AtomicSafetyHandle](https://docs.unity3d.com/ScriptReference/Unity.Collections.LowLevel.Unsafe.AtomicSafetyHandle.html),
  [secondary-version bumping](https://docs.unity3d.com/ScriptReference/Unity.Collections.LowLevel.Unsafe.AtomicSafetyHandle.SetBumpSecondaryVersionOnScheduleWrite.html),
  [job system troubleshooting](https://docs.unity3d.com/2020.1/Documentation/Manual/JobSystemTroubleshooting.html)
  ("Accessing static data from a job circumvents all safety systems"; main-
  thread-only Schedule/Complete),
  [Entities safety](https://docs.unity3d.com/Packages/com.unity.entities@1.0/manual/concepts-safety.html)
  ("in runtime builds there aren't any guarantees"), Joachim Ante in
  [Jobs system safety](https://discussions.unity.com/t/jobs-system-safety/751210)
  ("cross checks read/write access of containers against the actual provided
  job dependencies"), Unity blog "Improving job system performance 2022.2
  part 2" (safety-check overhead campaign).
- **Rust empirical studies**: Qin et al., PLDI 2020, "Understanding Memory
  and Thread Safety Practices and Issues in Real-World Rust Programs"
  (850 unsafe usages, 170 bugs; all 70 memory-safety bugs involve unsafe;
  25/41 non-blocking and 59/59 blocking bugs in safe code; 30 double-locks
  from implicit scope-end unlock; 13 interior-mutability bugs); the earlier
  Servo/TiKV/Rand study (safe-code races all from atomics misuse; Servo 6.6
  unsafe tags/KLOC); [RustSec thread-safety category](https://rustsec.org/categories/thread-safety.html)
  (63 advisories), CVE-2017-20004 (std `MutexGuard` wrongly `Sync`),
  [RUSTSEC-2021-0124](https://rustsec.org/advisories/RUSTSEC-2021-0124.html)
  (tokio oneshot race), the actix-web unsafe controversy
  ([safety-dance #39](https://github.com/rust-secure-code/safety-dance/issues/39)).
- **Render graphs**: [FrameGraph GDC 2017 deck](https://www.slideshare.net/DICEStudio/framegraph-extensible-rendering-architecture-in-frostbite)
  (declared access → lifetimes; greedy acquire-before-first-use/release-
  after-last-use; 720p 147→76–80 MB, 4K 1042→472 MB; async compute opt-in
  per pass; "built from scratch every frame"),
  [UE RDG docs](https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine)
  (validation layer "fatal checks with clearly marked resource / pass
  names", Debug/Development only; culling, split barriers, transient
  aliasing, async-compute fences; immediate mode), UE issue
  [UE-178829](https://issues.unrealengine.com/issue/UE-178829)
  (`ValidateExtractResource`: "not produced by any pass" — validator
  catching Epic's own TSR bug), UE source (RDG parallel-execute cvars;
  `FParallelCommandListSet` deprecated 5.5 for `AddDispatchPass`;
  `FRDGBuilder::AddSetupTask` taking `UE::Tasks` types),
  [Granite deep dive](https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/),
  [render-graph survey (Loggini)](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html),
  [frame-graph theory (Stolecki)](https://stoleckipawel.dev/posts/frame-graph-theory/).
- **Renderer/job-system integration**:
  [UE parallel rendering overview](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine),
  [UE mesh drawing pipeline](https://dev.epicgames.com/documentation/unreal-engine/mesh-drawing-pipeline-in-unreal-engine),
  [UE actor ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine),
  [PSO precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine),
  [Destiny GDC 2015 (Tatarchuk)](https://www.gdcvault.com/play/1021926/Destiny-s-Multithreaded-Rendering)
  (runtime tracker verifying "all data accesses... are valid and occur at
  safe times within the frame"),
  [id Tech 7 interview](https://slayersclub.bethesda.net/en-US/article/id-tech-7-interview),
  [vkguide multithreading chapter](https://vkguide.dev/docs/extra-chapter/multithreading/),
  [D3D12 command recording](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles),
  [GPUOpen async compute](https://gpuopen.com/learn/concurrent-execution-asynchronous-queues/).
- **Taskflow**: [TCAD 2021 paper](https://tsung-wei-huang.github.io/papers/tcad21-taskflow.pdf)
  (17 vs 22 vs 37 LOC; OpenTimer 9123→4482 LOC, complexity 58→20, up to
  9.8× speedup; DREAMPlace: condition tasks beating TBB by up to 47.8%
  runtime and 3.8–4.4× memory by expressing the loop as a cyclic graph;
  "the same API used for static tasking all applies to dynamic tasking"),
  [conditional tasking docs](https://taskflow.github.io/taskflow/ConditionalTasking.html)
  (strong/weak dependencies; double-scheduling UB; strong-edge-loop
  deadlock), [issue #49](https://github.com/taskflow/taskflow/issues/49)
  (1 kHz loop; "latencies due to the dynamic memory allocations";
  `tf::Framework` reuse shipped v2.1.0; "each framework is *stateful*"),
  [issue #82](https://github.com/taskflow/taskflow/issues/82)
  (thread_local/static destruction-order heap corruption; user requesting
  runtime dangling-reference asserts).
- **CUDA conditional nodes**:
  [NVIDIA blog](https://developer.nvidia.com/blog/dynamic-control-flow-in-cuda-graphs-with-conditional-nodes/)
  (device-set condition handles; IF/WHILE; 12.8 added ELSE and SWITCH;
  motivation: split graphs "compromise CUDA's ability to make
  optimizations").
- **HPC**: PaRSEC PTG/DTD papers (PTG "instantiate any part of the DAG from
  any task... without materializing"; DTD as prototyping interface; tiny-
  task overhead in LU panels), the STF-scalability study (dependency-
  analysis overhead grows with problem size; graph trimming "essentially
  expressing the same information as PTG"; tile-size coarsening as the
  other lever), Legion tracing (SC 2018), OpenMP taskgraph (arXiv
  2212.04771).
- **Go/TSan deployment**: [Go race detector blog](https://go.dev/blog/race-detector),
  [Uber's dynamic race detection at scale](https://www.uber.com/en-IN/blog/dynamic-data-race-detection-in-go-code/)
  (~2,000 races found / 1,011 fixed across 50M LoC; periodic sweeps, not
  PR-gating or production), [Clang TSan docs](https://clang.llvm.org/docs/ThreadSanitizer.html)
  (5–15× slowdown), Chromium
  [threading docs](https://github.com/chromium/chromium/blob/main/docs/threading_and_tasks.md)
  (`SEQUENCE_CHECKER`, `DETACH_FROM_SEQUENCE`).
