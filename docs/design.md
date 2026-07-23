# Design rationale

This document explains *why* the library is shaped the way it is: the thesis,
the decisions behind each layer, and — deliberately — the alternatives that
were tried or analyzed and rejected. It assumes you know game-engine
architecture (frame graphs, job systems, the render/game thread split) and
concurrent programming in depth. The user-facing companion is
[guide.md](guide.md); four deeper documents are linked throughout and listed
at the [end](#appendices--references).

---

## 1. Thesis and lineage

The author designed and implemented Unreal Engine's Tasks System. This
library keeps what that system proved — work-stealing scheduling,
prerequisites, nested tasks/events, pipes, priorities with a small fixed set
of levels — and rebuilds around a different central idea:

**Access-declared concurrency.** Every unit of work declares which shared
objects it reads and which it writes. From those declarations the library
*derives* scheduling constraints (a reader/writer pipe per object; conflict
edges in a static graph), and a runtime **harness** verifies at execution
time that no code touches a guarded object without a matching grant.

The nearest relatives each hold two of the three pieces:

- **ECS schedulers** (Bevy, Unreal Mass, Unity DOTS) derive schedules from
  declared component access — but at per-component-*type* granularity, inside
  an ECS data model.
- **Unity's Job Safety System** validates read/write handles at runtime — but
  validates a *hand-built* dependency graph rather than deriving one.
- **`folly::Synchronized`** binds data to its lock with const-selected access
  — but blocks, and has no schedule derivation and no harness.

No production system we surveyed combines *per-object-instance* granularity,
*derived* scheduling, and a *runtime oracle* in a general-purpose C++
library. That intersection is the bet this library makes. The full survey is
in [task-systems-comparison.md](task-systems-comparison.md).

---

## 2. The access model

### 2.1 Granularity: coarse objects, not component columns

The unit of guarding is an object instance — in practice a whole subsystem
(`Guarded<Physics_world>`, a transform store, a nav mesh), not a fine-grained
entity. This is a deliberate position in a real tension:

- **Fine granularity** maximizes theoretical parallelism (two tasks touching
  different rigid bodies never conflict) but explodes the conflict graph to
  O(entities), scatters memory, and makes per-access overhead dominate.
- **Coarse granularity** keeps the graph at O(subsystems) — around a hundred
  nodes for a real frame — but one writer serializes all readers of that
  object.

The resolution: wrap coarsely, then get intra-system parallelism from
`parallel_for` and nested tasks running *under the system's grant*, and split
state along the seams you need concurrent (the sample double-buffers its
transform store so the writer and the frame's readers touch different
objects). Once enough subsystems process concurrently to fill the cores,
finer decomposition buys nothing.

The ECS alternative — access declared per component *type*, batched over
contiguous columns (Bevy's `FilteredAccessSet`, Mass processor fragment
access, Unity's per-`NativeContainer` handles) — was analyzed and declined:
its cache-layout advantage is real but comes from the ECS's SoA data model,
not from the scheduler; a coarse wrapped subsystem already owns its arrays
contiguously inside. A `Guarded_pool<T>` per-type primitive was sketched and
rejected as buying nothing the coarse model lacks (§7.2).

### 2.2 The harness as the correctness oracle

Deriving safety from declarations has a known failure mode, named early and
designed around — the **completeness hazard**: the derived schedule is only
as safe as the declarations are complete. A stray reference or an undeclared
task can race an object the graph believes it owns, and parallelization can
*promote* such a latent bug into a live one.

The harness is the answer: `TS_CHECK_ACCESS()` in a guarded type's methods
checks `this` against a thread-local grant set installed around every task
body, and faults (message + stack trace + abort) on any undeclared access.
It costs ~1 ns per call — an inline array scan, no locks — and compiles out
under `TS_SAFETY_CHECKS=0`. It is per-method instrumentation, i.e. opt-in
and not airtight; the design position is that a cheap, always-on-in-dev
*runtime oracle* beats both unverifiable convention and a compile-time
scheme that would constrain the programming model (§7.9 on Rust/Pony).

Grant *inheritance* makes the model compose: sub-work launched from a task
body (nested tasks, `parallel_for` chunks, coroutine segments after
suspension) carries the parent's grant set by value. That one rule lets a
graph node fan out dynamic work over its declared data without new
declarations, and it is what coroutine support reuses wholesale (§5).

### 2.3 Introspection: the graph knows its own structure

A derived schedule the user never wrote down needs a way to be read back —
tuning a graph means seeing which edges exist and why. `compile(DOT_path)`
dumps the DAG as Graphviz DOT with per-edge provenance: explicit orderings
solid, conflict-derived edges dashed with the object and modes on hover. The
distinction is actionable: a `W->R` edge is dataflow, while `R->W`/`W->W`
edges are ordering artifacts that versioning or deferral can delete — the
same lever that moved the sample's audio off the post-flip tail.

Node names follow the UE convention (a debug name at creation, kept in all
builds), with one modernization: `std::source_location` in the `Node_name`
descriptor's defaulted constructor argument makes a placeholder `{}` label
the node with the user's `add_node` call site — no macro. A trailing
defaulted parameter after the object pack is not expressible in C++, hence
the leading-parameter position (also UE's argument order).

Objects are named the same way — `ts::Named` as an optional leading
constructor argument on `Guarded`/`Versioned`. It is a distinct wrapper type
rather than a bare `const char*` because the constructor forwards to `T`
in place: a bare leading string would be indistinguishable from `T`'s own
first constructor argument. The name is stored on the pipe — the shared
per-object structure every layer already reaches — so the dump (and future
tooling) resolves it without new plumbing.

Everything downstream of the name (the structure dump and the runtime trace)
is gated by `TS_PROFILING` (default on; define it to 0 for shipping); the
formatters live outside the library proper, in `tools/`.

The runtime trace aggregates rather than records: per node, a Welford
mean/variance, P^2 quantile markers (Jain–Chlamtac) for P50/P95, min/max, and
a per-worker histogram — O(nodes + edges) state however many runs fold in. The
graph's entire involvement is three stamps into pre-sized `Run_state` arrays
and one call at run settle; every statistic, the timeline reconstruction, and
the SVG live in `tools/graph_trace.h`, so the scheduler logic stays legible.

The reconstructed "average run" has one subtlety. With *mean*-based bars an
edge's endpoints can never cross: `start_v >= end_u` holds pointwise in every
real run, expectation preserves pointwise order, and `E[start] + E[dur] =
E[end]` by linearity. Medians make a better "typical run" (they ignore
outlier frames), but they are not linear — median bars of an edge-connected
pair can overlap even though no real run overlapped them. The resolution is
the edge's streamed **meet point** (the mean of predecessor-end and
successor-start): crossing bars are clamped to it, predecessors first in
topological order. Unrelated nodes sharing a lane are deliberately *not*
clamped — their aggregate overlap is real (the lane is only the node's modal
worker), so they stack into sub-rows instead of being hidden.

Critical-path detection is *measured*, not inferred: each run the trace walks
backward from the latest-finishing node through the **binding predecessor** —
the incoming-edge node that finished last, i.e. whose completion released this
one — and counts chain membership per node and edge. A variable frame has no
single critical path, so the aggregate is a frequency ("critical in 82% of
runs"), rendered as highlight intensity rather than a boolean. The structural
CPM pass (median durations, zero latency) is computed alongside for per-node
slack; it is the dependency lower bound, blind to queue latency and pipe
contention, and its divergence from the measured chain is itself the signal
that a frame is scheduling-bound rather than dependency-bound. A third stamp
at the dependency counter's zero transition splits that overhead out exactly:
ready-to-start is acquire + queue latency, attributed per node as "dispatch
wait".

---

## 3. Scheduler

The scheduler evolved in measured stages from a deliberately naive baseline
(one mutex-guarded `std::priority_queue`) to the standard production shape:

1. **Per-priority lock-free MPMC queues** (Vyukov array-with-sequences),
   scanned high→low. The baseline's mutex plateaued at ~6.4 M submits/s
   beyond four producers; this removed the comparator-under-one-mutex
   bottleneck.
2. **Vyukov eventcount** for parking idle workers (prepare/re-check/commit,
   epoch-based) replacing a counting semaphore — no permit accounting, and it
   extends to "is there work anywhere?" once work lives in per-worker deques.
3. **Chase-Lev work-stealing deques** per worker for `normal` work (owner
   LIFO, thieves FIFO steal, bounded with overflow to the global queue) —
   measured +17–22% on fork-join workloads.
4. **A low-starvation valve** (serve one `low` after N consecutive
   high/normal — the Go/Tokio "check the global queue every ~61" idea) so
   `low` means *low*, not *never*.

Priorities are three fixed levels applied at dispatch. High is strict; low is
valve-protected. Task priority is deliberately **not** mapped to OS thread
priority (§7.5).

**Idle policies.** Three per-instance runtime modes: `spin` (never park),
`spin_then_block` (UE-style: spin N scans, then park — the default), and
`handoff` (Go-style: at most one spinner keeps watch; a spinner that takes
work promotes a successor; producers pay a wake syscall only on the
fully-parked 0→1 transition — moving wake latency off the submit path,
measured ~40% lower wake latency). Two variants were built and then
*removed or rejected*: a bare park-immediately `block` mode (redundant —
`spin_then_block` with zero spin cycles) and the "pure" handoff where
producers never touch wake state at all — correct only with seq_cst Dekker
fences, and `atomic_thread_fence` is invisible to ThreadSanitizer, which
this project treats as disqualifying (§7.6). The shipped handoff keeps
correctness on an always-advanced epoch; the spinner count is advisory and
relaxed.

**The pipe's FIFO is a semantic contract, not a scheduling choice.** A
`Guarded` object's accessors run in submission order with reader coalescing.
Analysis of reader/writer prioritization "knobs" (writer-priority,
batch-readers) concluded they are **value-changing reorderings** in a
submission-ordered queue — a read moved past an earlier write observes
different state. That is unlike a lock, where waiters race with no defined
order and preference merely biases the race. So: FIFO + coalescing is the
only default; relaxations would be explicit staleness opt-ins; and the real
reader-throughput answer is structural — `Versioned<T>` (§6) — not queue
policy.

---

## 4. The task core

### 4.1 A monomorphic control block

Every task is one heap allocation: a **fully monomorphic**
`Task_control_block` — parameterized on nothing, no virtual dispatch. The
result is type-erased behind a `void*` into a wrapper the handle refcounts
intrusively; the body is reached through one function pointer. Templating
the block (the obvious alternative) was rejected because the scheduler is
runtime-configured and thread-locals like the current-scheduler pointer would
fracture across instantiations — and because a single block type is what
makes reuse (`reset()` re-arms scalars in place), pooling, and the forensic
tooling below tractable. `shared_ptr` was systematically replaced with
intrusive refcounting (one pointer per handle, one allocation, a `destroy`
thunk for the aliasing wrappers).

The allocation story was then attacked empirically: a deterministic
allocation profiler (`--memprofile`) counts allocations per operation, which
found — among other things — that every queued task paid a second heap
allocation just to box a closure for the scheduler's raw function-pointer
API. The dispatch path was restructured so *the block itself is the queue
payload* (16-byte queue entries, per-dispatch state riding on the block), and
`when_all`'s six-allocations-per-join was collapsed into one intrusive join
state. Current counts: `launch` and `async` allocate exactly one block per
operation. The measured conclusion worth recording: allocation is a
*secondary* cost here — cross-thread scheduling latency dominates rich
operations — so the remaining pooling/arena work is scheduled as WIP, not
emergency.

### 4.2 Results: `const&` by default, `take()` to move

`sync()` returns the result by `const&` and does not consume it; `take()` is
the explicit destructive move. `std::future::get()`'s move-out semantics were
copied at first and then deliberately reverted: `future` moves because its
shared state must be droppable after one `get` (hence the `shared_future`
split), but here the handle *owns* the refcounted block, so a `const&` into
it is valid for the handle's lifetime. The contract becomes "any number of
readers; at most one mover, and it must be last" — the multi-consumer
ergonomics of `shared_future` without a second type, and move-only results
still work.

### 4.3 Builders, not persistent task objects

`ts::task(fn).after(...).launch()` is launch-time configuration spelled
fluently, and it doubles as the **reusable task** handle (`reset()` +
relaunch re-uses one block/body/result slot). It is deliberately *not* a
persistent reconfigurable task object: a survey across UE, TBB, Taskflow,
.NET, Rust and others found the cold-task/builder pattern consistently
avoided where offered; systems fix prerequisites at launch. Result-passing
was likewise kept off `after` (ordering and cancellation only) because a
prerequisite's result feeding a dependent's body *is* `when_all` + `then` by
construction.

### 4.4 Blocking without deadlock: retraction

`sync()` on a not-yet-started, non-pipe task runs it — and its un-started
prerequisite subtree — *inline on the waiting thread*, gated by an atomic
one-runner claim. This "retraction" (plus caller participation in
`parallel_for`) is why nested fork-join cannot deadlock the pool even with
every worker blocked, without oversubscription machinery.

### 4.5 Two real races, and the method that caught them

The reuse×retraction corner produced the two hardest bugs in the project's
history, both instructive:

- **Token rewrite race**: relaunching a reused task rewrote its cancellation
  token while a prior round's worker could still read it. Fixed by making
  the token immutable per task (set once, before first launch) — removing
  the racing write rather than synchronizing an edge nobody could pin.
- **Premature-dispatch generation TOCTOU**: the dispatch path read the
  task's reuse generation *after* the atomic decrement that granted dispatch
  rights. A releaser preempted between those two instructions could wake
  after a full retract/reset/relaunch cycle and stamp its dispatch with the
  *next* generation — which then ran a round whose prerequisites weren't
  met, returning the previous round's result. Fixed by capturing the
  generation *before* the granting RMW and publishing it monotonically.

The second bug had survived four independent happens-before analyses (all
"proving" the code correct) and reproduced at 28% per iteration on a plain
optimized build pinned to two cores — despite having been seen only under
TSan and initially dismissed as tooling noise. Three transferable lessons,
now house rules: **capture state before the RMW that grants you rights over
it** (reading after is a TOCTOU); **"only reproduces under TSan" does not
mean TSan artifact** — pin to two cores to amplify preemption windows; and
**capture-and-continue forensics beat theorem-proving** — the bug fell to an
event-ring instrumented build (kept in-tree, compiled out by default) in one
session.

---

## 5. Coroutines

C++20 coroutine support is an additive layer (`coroutine_support.h`), not a
re-founding: `co_await task` registers the coroutine as a continuation on
the same control block `then` uses; a coroutine returning `Task<R>` is an
ordinary task to everyone else.

Three design points carry the weight:

- **Access grants across suspension.** The harness's grant set is
  thread-local, and a coroutine migrates threads at every suspension. The
  model: a coroutine is a chain of task-*segments*, and each resumed segment
  re-installs the coroutine's grant snapshot — the nested-task inheritance
  mechanism reused verbatim, no new concept.
- **The pipe is already an async reader/writer lock**, so
  `co_await ts::read_write(obj)` yields an RAII guard with direct object access —
  `folly::coro::Mutex::co_scoped_lock`'s shape on top of machinery that
  existed anyway. The canonical coroutine footgun — suspending while holding
  a lock — is *detected*: a `co_await` under a live guard is fatal. The
  project's signature safety mechanism catches the signature coroutine
  mistake.
- **Cancellation is value-based** because exceptions are off project-wide: a
  cancelled await resumes with cancelled state to inspect, never a throw.
  This was forced by the no-exceptions constraint and turned out cleaner
  than exception-driven cancellation — control flow stays visible.

Resumes run through a bounded trampoline (deep continuation cascades resume
iteratively, proven to 50k depth), and coroutine frames currently heap —
folding the control block into the frame is planned WIP alongside the
allocator work.

---

## 6. Deferred and versioned state

The pipe's FIFO answer degrades in the many-small-writers case: every write
serializes against every reader, and readers observe a timing-dependent
prefix of the write stream. The staged-write layer answers this; its design
study is [command-buffer-design.md](command-buffer-design.md) and its
operational contracts are
[deferred-versioned-state.md](deferred-versioned-state.md). The essence:

**A command buffer is a recording front-end terminating in an ordinary
`Guarded` write.** `Deferred<T>` mints per-producer **recorders**; staging a
closure touches only the producer's private journal slot — no grant, no
contention with readers — and one `commit` applies the whole batch under a
single write access. Readers see none of a batch before the commit and all
of it after (stable snapshots instead of racy prefixes). The pipe remains
the only arbitration mechanism; the harness is untouched.

The rejected alternative here was a **"lazy `Guarded`"** — a mode where
`async` writes queue but don't execute until a flush node runs. It fails on
its own FIFO: parked writes at the queue's front either block every later
reader until the flush (semantically clean, zero reader parallelism — clean
and pointless) or readers must overtake queued writes (the value-changing
relaxation of §3, now as an instance mode). Per-command cost is a full task
block instead of a journal append, and recording is undeclared so no graph
edges can be derived from it. UE's own architecture agrees from the other
side: immediate and deferred RHI command lists are *two types*, not a mode
flag on one.

**`Versioned<T>`** extends staging with two replicas behind one guarded
front: readers hold a stable published version all frame while the next is
staged; `publish()` flips atomically. The load-bearing choices:

- The swap exchanges the replicas' *contents*, so the front's address never
  changes — graph declarations, the pipe, and the harness need zero changes.
- A publish is three phases and only the middle one (a content swap,
  nanoseconds) holds the write grant; applying the batch and resyncing the
  second replica overlap readers. The resync runs as a *read job on the
  front's pipe*, which makes the pipe itself the shadow-ownership chain —
  FIFO holds the next writer behind the resync with no extra synchronization.
- **Resync by replay**: both replicas are identical at every publish start,
  so re-applying the same batch to the second replica reproduces the new
  version bit-exactly — cost proportional to the delta, not to `sizeof(T)` —
  *if commands are deterministic*. That requirement is explicit, and an
  opt-in divergence check (bitwise replica compare after replay) turns a
  violation into an immediate fatal rather than a drifting heisenbug.
- **No read-your-writes**: a version's outputs arrive as the next version.
  This is the honest cost of stable snapshots, stated as a contract rather
  than hidden.
- Ordering: FIFO within a recorder is semantic; across recorders it is
  arbitrary-but-reproducible (fixed drain order), and building semantics on
  it is declared a bug. `Parallel_recorder` (per-worker slots for staging
  from inside a `parallel_for`) is the explicit, localized surrender of
  cross-thread reproducibility.

The UE research grounded several choices: `ENQUEUE_RENDER_COMMAND`'s
linear-allocated coarse queue and splice-in-submit-order parallel recording
(determinism from splice position, not thread timing) shaped the journal's
slot-order contract; `FRHICommandList`'s POD commands from a per-list linear
allocator is the planned typed-command/arena tier; the Render Dependency
Graph — passes declaring resource access, order derived — is production
validation of this library's central thesis, applied to GPU resources.

---

## 7. Rejected alternatives

The record of things analyzed and *not* done, with the why. Each was a real
candidate; several were prototyped.

**7.1 Lazy `Guarded` (deferred writes as an instance mode).** Rejected: the
FIFO fork (block readers = no parallelism; readers pass writes = changed
observed values), per-command task-block cost, undeclared recording (no
derivable edges), and one type with mode-dependent semantics. Survives as
the *recording front-end* design (§6).

**7.2 `Guarded_pool<T>` / per-component-type access (the ECS shape).**
Rejected: with coarse subsystem wrapping, the graph is already small and the
subsystem already owns its arrays contiguously — the two ECS benefits arrive
without a new primitive. Dynamic `parallel_for` chunking cannot use
per-slice grants anyway (chunks are claimed at runtime); slices-as-grants
returns only as a possible future harness refinement for handed-out
sub-object references.

**7.3 Reader/writer pipe policy knobs (`writer_priority`, `batch_readers`).**
Rejected as defaults or "throughput options": in a submission-ordered queue
they reorder reads relative to writes and therefore change results. A lock
can bias its acquisition race; a queue cannot bias away its own order.
Staleness-tolerant reads may return someday as an explicit per-read opt-in.

**7.4 Priority inheritance by reordering.** Priority inversion through a
pipe (high-priority access queued behind low-priority work) is real; fixing
it by *reordering* would break the FIFO contract. The designed fix —
shelved, awaiting a real workload — is inheritance: dispatch the pipe head
at the maximum priority queued behind it; in the static graph, a one-pass
reverse-topological effective-priority computation at compile time.

**7.5 Mapping task priority to OS thread priority.** Rejected: a syscall
(~1 µs) against a ~200 ns dispatch; permission-gated on Linux; and the wrong
tool — OS priority arbitrates against unrelated processes, while task
priority orders work within the pool. Every surveyed native system except
Apple's GCD (whose kernel is built for per-work-item QoS) keeps priorities
in userspace queues. A fixed low-priority *pool* attribute for background
work is the legitimate future use.

**7.6 `std::atomic_thread_fence` on visibility edges.** House rule:
ThreadSanitizer does not model fences, so a fence-based edge makes the
project's primary race oracle blind. Every publish/consume edge uses
variable release/acquire (or RMWs) instead — including one case where the
Chase-Lev deque's textbook fences were converted to prove a suspected
false positive (the suspicion was wrong; the racy code was genuinely racy,
see §4.5). Verifiability outranks micro-optimization of fence placement.

**7.7 Wholesale `std::execution` (senders) adoption.** Rejected: senders'
compile-time operation-state composition contradicts the monomorphic-block
decision (§4.1), doesn't help fire-and-forget work, and P2300 is still
settling. Two ideas are kept on the shelf: `Task`/`Scheduler` as
sender/scheduler adapters for interop, and — the genuinely interesting one —
carrying the access context through receiver environments instead of
thread-locals, which could open compile-time access checking.

**7.8 `Access::append` edge derivation for staging.** Analyzed while
building `Deferred`: recording needs no grant at all, so adding an access
mode for it would grow the mode lattice solely to derive producer→commit
edges that are cheap to declare by hand (`.after`). Deferred until the
completeness hazard actually bites in this position.

**7.9 Compile-time access proof (the Rust/Pony direction).** A type-system
guarantee (borrow checking, reference capabilities) is the strongest form of
this library's property — and requires a language, or a template regime that
rejects ordinary C++ patterns and explodes compile times. The runtime
harness at ~1 ns is the deliberate trade: weaker guarantee, zero model
constraint. Senders' typed environments (§7.7) are the one path back toward
compile-time checking worth watching.

**7.10 Requiring copyable `T` / `.copy()` accessors.** `folly::Synchronized`
offers lock-and-copy-out; here the guarded objects are whole subsystems
where copying is exactly what must never happen implicitly. `Guarded<T>`
imposes no copyability; snapshot semantics are provided structurally
(`Versioned<T>`) instead.

---

## 8. Verification approach

Worth documenting as design, because several decisions above exist *for*
it:

- **TSan is the primary oracle**, run on Linux/Clang against the same
  sources (the Windows-specific harness is isolated); every concurrency
  change gates on it. This is why fences are banned (§7.6) and why the
  advisory-counter/authoritative-epoch split exists in the handoff policy.
- **Death tests** cover every fatal path via subprocess scenarios; safety
  checks are tested, not assumed.
- **Determinism as a test**: both engine samples run twice and compare
  results bitwise — which is what caught a replica-divergence bug on day
  one of `Versioned` and motivated the built-in divergence check.
- **Forensic instrumentation kept in-tree** (compiled out by default): the
  event-ring harness that cracked the TOCTOU (§4.5) remains as a regression
  tool, alongside the deterministic allocation profiler.

---

## Appendices / references

Project documents:

- [task-internals.md](task-internals.md) — the dynamic-task design of
  record: control block, lifecycle, lock-counter, retraction, nested tasks.
- [task-systems-comparison.md](task-systems-comparison.md) — the survey: UE
  Tasks, TBB, Taskflow, HPX, folly, Marl, Rayon/Tokio, Go, GCD, and the
  scheduler literature.
- [command-buffer-design.md](command-buffer-design.md) — the staged-write
  design study, including the UE rendering research and the full
  lazy-`Guarded` analysis.
- [deferred-versioned-state.md](deferred-versioned-state.md) —
  `Deferred`/`Versioned` contracts, mechanisms, and the forward plan.

External systems referenced above: Unreal Engine Tasks / RDG /
`ENQUEUE_RENDER_COMMAND` / `FRHICommandList`; Intel oneTBB; Taskflow;
Meta folly (`Synchronized`, `coro::Mutex`, eventcount lineage); Bevy ECS;
Unreal Mass; Unity DOTS (Job Safety System, `EntityCommandBuffer`); Flecs;
Go's runtime scheduler (`nmspinning`); Tokio; Rayon; Apple GCD; Dmitry
Vyukov's MPMC queue and eventcount; the Chase-Lev deque
(Lê–Pop–Cohen–Nardelli formulation); Box2D v3 and Jolt (solver graph
coloring / islands, from the entity-interaction research); PRISM (chromatic
scheduling); P2300 `std::execution`.
