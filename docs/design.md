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
library keeps what that system proved — work-stealing scheduling, ordered
prerequisites, nesting, events, pipes, priorities with a small fixed set of
levels — as concepts, though they take different form here (prerequisites are
`co_await` at the top of a body; nesting is internal completion-gating, not a
user-facing `nested` verb) and rebuilds around a different central idea:

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

Two positioning facts sharpen the bet
([research-deepdive.md](research-deepdive.md) §15, §2). First, the industry
already ships static frame skeletons: Unreal's tick system (`ETickingGroup`
phases, per-function tick prerequisites, opt-in `bRunOnAnyThread`) is a
coarse declared frame graph that has sat in the engine for a decade —
underused for parallelism because nothing declares what a tick touches, so
the engine cannot safely move work off the game thread by default. The
missing piece is not the graph shape or the dynamic runtime; it is the
access layer that makes the static structure safely parallelizable — which
is what this library supplies. Second, the boundary is deliberate: workloads
whose top-level shape is data-dependent per run (renderers are the canonical
case — the pass set is a function of frame content, which is why every
render graph redeclares per frame) are out of scope for the static graph by
design; it targets the stable frame skeleton, and data-dependent work lives
in the dynamic layer under the same access model.

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
`parallel_for` running *under the system's grant*, and split
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

Grant *inheritance* makes the model compose, but only for **structurally
gated** sub-work — `parallel_for` chunks and coroutine segments after
suspension — which carries the parent's grant set by value. Gating is the
soundness condition: the parent's completion waits for the child, so the
grant provably outlives it. That one rule lets a graph node fan out dynamic
work over its declared data without new declarations, and it is what
coroutine support reuses wholesale (§5). A detached `ts::launch` is *not*
gated (its handle may be dropped), so it inherits nothing and runs under an
empty context — a touch of the parent's data faults deterministically as
undeclared access, rather than racing the next acquirer on a timing window.
(An earlier design also inherited into a *concurrent* child, `ts::nested` /
`Task_scope`; that was removed — a concurrent grant-inheriting child can race
its parent on shared mutable state and the harness cannot see it, since both
sides declared the access. `parallel_for`'s synchronous join is the gated
form that survives. See docs/coroutine-first.md §4.3.)

Where inheritance does apply, it is bounded by **grant-window validity**:
each pipe carries a write-epoch with seqlock parity (bumped at write-grant
acquire and release; +2 on a graph write handoff, which elides both pipe
operations), and every context entry declared under a pipe grant captures
the epoch at declaration. A snapshot that outlives its window — a detached
coroutine created under a node's grant, resuming after the node released its
objects — fails the comparison at the next instrumented access and faults
with a stale-grant diagnostic rather than silently racing the next
acquirer. The parity choice makes one
rule serve both modes: a write entry is valid while its holder's window is
open; a read entry is valid until a *writer* acquires (other readers coming
and going don't bump), which is the actual safety condition, not the
origin reader's own lifetime. Nested-gated sub-work is structurally never
stale — the parent's completion, and so its release, waits for it.

### 2.3 Introspection: the graph knows its own structure

A derived schedule the user never wrote down needs a way to be read back —
tuning a graph means seeing which edges exist and why. `compile(DOT_path)`
dumps the DAG as Graphviz DOT with per-edge provenance: explicit orderings
solid, conflict-derived edges dashed with the object and modes on hover. The
distinction is actionable: a `W->R` edge is dataflow, while `R->W`/`W->W`
edges are ordering artifacts that versioning or deferral can delete — the
same lever that moved the sample's audio off the post-flip tail.

That provenance distinction has a consequence worth stating as a rule, because
it is easy to get backwards. A derived edge exists for *safety*: two conflicting
nodes must not overlap. Both directions satisfy that, so the one `compile()`
picks — declaration index — is an artifact of how the building code is written,
not a claim about the frame. **Declaration order resolves the direction of a
derived edge; it is not a specification.** Logical precedence is intent and
belongs in an explicit `after`/`before` edge, which costs nothing when it agrees
with the derived one (they dedup into a single edge that keeps the conflict in
its tooltip).

Two things depend on holding that line. The first is refactoring: in the
sample's optimised variant the draw producers stage through `Deferred` and stop
touching the queue, so the conflict — and the derived edge with it — vanishes;
only the intent edge keeps the frame correct across that change. The second is
headroom. Because direction carries no meaning, a future `compile()` may reorder
independent conflicting nodes to shorten the critical path. That optimisation is
only available while no program has quietly made declaration order load-bearing,
which is why the sample was audited and its two intent-bearing sets — the render
producers into `submit`, and every last-frame transforms reader ahead of the
flip — were made explicit even where a derived edge already pointed that way.

Naming follows the UE convention (a debug name at creation, kept in all builds)
with one modernization and one unification. The modernization: a defaulted
`std::source_location` argument means an entity can identify itself by *where
it is written* — `ts::Named{}` — with no macro. The unification: one type,
`ts::Named`, for all three kinds of entity. Nodes and objects must carry one
(there are no unnamed constructors, and the old `node<N>`/`objN` ordinal
fallbacks are gone with them); tasks carry one optionally, because their verb
captures the call site by default. `Named` keeps `{literal, file, line}` rather
than a whole `source_location` — `function_name()` is never used here.

It is a distinct wrapper type rather than a bare `const char*` because the
`Guarded` constructor forwards to `T` in place: a bare leading string would be
indistinguishable from `T`'s own first constructor argument. For the same
reason the constructor is constrained to an actual `Named`, so
`Guarded<std::string> g{"hello"}` is a compile error rather than a silently
default-constructed string named "hello". A trailing defaulted parameter after
an object pack is not expressible in C++, hence the leading position (also UE's
argument order) — and hence the one gap: the multi-object `ts::access`/`async`
have no site to capture and take only an explicit literal.

The load-bearing implementation rule is that a defaulted `source_location`
captures the *caller* of the function that declares it. So it is declared only
on the outermost verb the user calls, and the resulting `Named` is threaded down
every internal layer explicitly; an inner helper with its own default would
capture a library header and the whole feature would quietly become useless.
A test asserts that captured sites land in the test file, which is the only
thing that actually guarantees it.

An object's name lives on the pipe — the shared per-object structure every layer
already reaches — and a task's lives on its control block, `TS_SAFETY_CHECKS`-
gated so shipping pays nothing for it. A graph node's block carries the node's
name, and a coroutine frame inherits the identity of the task it was created
inside, so a diagnostic about a suspended frame names the node the user
declared rather than a block pointer.

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

### 2.4 What the graph is worth, measured

The graph is optional — the access verbs and `co_await` compose the same work —
so the library owes an answer to "what does declaring it buy". `sample/game_frame.cpp`
carries both spellings of one ~34-system frame over the same `World` and the
same system bodies (`build_frame_graph` vs `run_frame_graph_free`), which makes
the answer measurable rather than rhetorical. Three results, none of them the
expected one:

**Safety is not what it buys.** Every claim the harness enforces comes from the
pipe turn a declaration takes, and a hand-composed `ts::async(fn, objs…)` takes
the same turn. Dropping the graph weakens exactly one guarantee: nothing derives
or checks the *order*.

**Pipe FIFO does not stand in for the conflict edges.** The tempting shortcut —
launch the systems in declaration order and let each object's FIFO reproduce the
derived edges — does not hold, because the multi-object cascade enters links one
at a time in canonical (pipe-address) order. A system blocked on its first object
has not taken its slot on the later ones, so a system launched after it walks
straight past. In the sample this mis-orders `frustum_cull` before `camera` and
lets `submit` clear the draw queue before `cmd_record` reaches it — a frame of
draw commands lost, silently, with every declaration correct and the frame 7.6%
*faster* for the missing edges. This is the completeness hazard's twin: the
harness is an oracle for undeclared access, and there is no oracle for
mis-ordered *declared* access. `compile()` derives 69 edges for those 34 nodes;
by hand the same frame is 17 chain coroutines and 42 `co_await`s.

**The performance advantage is resume locality, not allocation amortization.**
The standing assumption was that near-zero-alloc re-runs were the graph's
measurable edge. They are not: graph-free costs +95 allocations per frame (134
vs 38) which at ~17 ns each is under 2 µs, against a measured +56–64 µs/frame on a
4.1 ms frame and +89–131 µs on a 0.46 ms one (+1.4–1.6% / +19–29%). The gap is ~50
coroutine suspend/resume round trips at ~1.8 µs each. The graph dispatches a
successor directly on the thread that settled its last predecessor; an awaited
handle suspends a frame and resumes it through the trampoline. So the lever for
closing it is resume locality, and the practical reading is that hand
composition is free on millisecond-grained systems and starts to cost at
fine grain.

### 2.5 The cost of the coroutine-first node dispatch

The coroutine-first graph runs each node's dispatch through the same
ownership-carrying block path as every other task, and that is not free. The
current per-node machinery costs roughly **9% more per node** than the
pre-transformation block did (in frame terms, under 1% of wall time — framework
machinery is only ~4–10% of a real `game_frame`, §2.4). This section records the
*measured* cause, because the intuitive one is wrong.

Deterministic instruction counting (callgrind, two isolated Shipping-like builds,
a 1000-empty-node worker-less driver so machinery dominates) puts it at **522 →
648 instructions/node** and attributes it precisely
([graph-regression-callgrind.md](graph-regression-callgrind.md)). The tempting
explanation — the coroutine-capable completion path, the `execution_flag`
self-lock in `run_graph_node`, the `add_nested` gating — is **disproven**: that
code is byte-identical between the two versions and predates coroutine-first, and
the settle/completion path is in fact *cheaper* now. The actual cost is the
**dispatch trampoline**. Coroutine-first unified node dispatch onto the generic
refcounted block path — `submit_ready(Task_ptr` by value, an atomic increment`)`
→ `run_block_dispatch` (adopt + destructor, an atomic decrement) → `execute` —
where the baseline queued a **borrowed raw `Node*`** with no reference counting.
That per-node atomic inc/dec pair plus the `Task_ptr` churn is the bulk; the
remainder is `global_scheduler()` re-resolved per dispatch (the baseline cached
it) and an unconditional `advance_pipe_links` on object-free nodes.

What the unified path buys is real — one dispatch/cascade code path shared by
dynamic multi-object `async`, coroutine node frames, and nested `execute()`
lends, and correct block ownership when a node body genuinely outlives its
synchronous return (a `co_await`). But most of the cost is **recoverable** on the
common case that does *not* need block ownership: a plain functor node whose block
is owned by `Run_state` for the whole run can dispatch as a borrowed pointer, the
scheduler pointer can be cached per run, and `advance_pipe_links` can be skipped
when `pipe_count == 0`. Those are tracked as a low-risk optimisation (TODO §2.16);
the current cost is documented and tolerable, not a blocker. (Keep this distinct
from §2.4's resume-locality finding, which compares graph against graph-free; this
one compares the current node dispatch against the block that preceded the
transformation.)

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

**Worker-less (single-threaded) mode** (`Scheduler_config::single_threaded`)
is UE's no-multithreading shape, verified against its source before adoption:
UE creates zero workers when the platform lacks threads and its
`LaunchInternal` then executes each task inline at the launch point, looping
the continuation chain. Ours mirrors that: no workers, `submit` runs the
task now on the submitting thread through a bounded FIFO trampoline (the
same iterative-drain shape as inline dispatch), a blocking wait drains the
thread's pending trampoline entries before parking (a body that admits work
and then `sync()`s it would otherwise deadlock — the same rule UE's wait
paths follow), and work triggered from an external thread runs on that
thread. One structural consequence landed with it: pipe job submission was
moved *outside* the pipe mutex (admission state still updates under the
lock; the admitted batch is submitted after unlock) — under inline-at-submit
a job body releasing its own pipe would otherwise deadlock on the held
mutex, and shorter critical sections are better in every mode. Worker-less
graph runs are excluded from the trace's parallel aggregates (their timings
describe the trampoline, not scheduling); their clean per-node durations are
the planned serial-baseline lane for the profiler-guided-optimization cost
model. Priorities and idle policies are inert in this mode by design.

**Scheduler as one implementation behind the `Task` API, not a plugin
zoo.** The intent is at most two or three interchangeable scheduler
implementations sharing the `Task`/`Guarded`/graph surface — the current
work-stealing pool, and (roadmap) a foreground/background two-pool variant
whose background band runs on OS-low-priority oversubscribed threads so the
OS preempts it out the instant foreground work appears (TODO 3.7). The
design constraint that keeps that tractable: the scheduler's parts —
eventcount, Chase-Lev deques, per-priority MPMC queues, the low-starvation
valve, the submit/steal loop — are composable building blocks, not a
monolith, so a new implementation reuses most of them and adds only its
distinguishing logic (a different idle/wake policy, an extra pool, an OS-QoS
band). Anything fancier than a wake-policy choice is out of scope at this
stage; the goal is a small, legible menu, not a general framework.

A corollary that is a standing rule: **the scheduler's public API stays
minimal, because it is the seam an alternative scheduler must reimplement.**
The contract the task layer consumes is `submit(func, data, priority)` plus
two queries (`worker_count()`, `single_threaded()`) and construction-time
config; everything else — queues, deques, eventcount, idle policies, the
profiling accumulators — is internal. Every public member added to
`Scheduler` is a member every future implementation must honor; additions
need that justification, and features should land as config fields or
internal machinery before they land as API.

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
makes re-arm (`reset()` for `Signal` and the graph's per-run nodes) and
pooling tractable. `shared_ptr` was systematically replaced with intrusive
refcounting (one pointer per handle, one allocation, a `destroy` thunk for
the aliasing wrappers). A coroutine task fuses further: the promise *embeds*
the block, so frame + block + result are one allocation.

The allocation story was then attacked empirically: a deterministic
allocation profiler (`--memprofile`) counts allocations per operation, which
found — among other things — that every queued task paid a second heap
allocation just to box a closure for the scheduler's raw function-pointer
API. The dispatch path was restructured so *the block itself is the queue
payload* (16-byte queue entries, per-dispatch state riding on the block).
Current counts: `launch` and `async` allocate exactly one block per
operation; a coroutine chain allocates one frame per coroutine. The measured
conclusion worth recording: allocation is a *secondary* cost here —
cross-thread scheduling latency dominates rich operations — so the remaining
pooling/arena work is scheduled as WIP, not emergency.

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

Cancellation makes that pair insufficient rather than wrong. `sync()`/`take()`
abort on a cancelled task, which is the correct assertion for the common case
(no token in play) but punishes the caller for a state the callee chose — and
there is no check-then-read that is not a race, since the check and the read
are separate observations of a state another thread is setting. Changing the
return type to an optional was rejected outright: every call site that cannot
be cancelled would pay an unwrap forever, for a case it does not have. So the
answer is two more verbs rather than a different one — `try_take()`
(non-blocking, empty when unsettled *or* cancelled, therefore also legal
inside a task) and `co_await t.as_optional()` (the same wait, branching
instead of fatalling). Both consume, so they compose with the "at most one
mover" rule rather than adding a third kind of read. Neither exists for
`void`: a void task has no result to be missing, so `is_done()` and
`is_cancelled()` already answer both questions, and defining a
`Maybe<void>`-shaped type to make generic code uniform was judged not to earn
its name.

### 4.3 One composition mechanism (historical: builders, `then`, `when_all`)

Earlier revisions carried a full callback-composition surface: `then`
continuations (with apply-style tuple unpacking), `when_all` joins,
`ts::task(fn).after(...)` builders doubling as reusable-task handles, deep
retraction, and inline dispatch for dynamic tasks. It worked, and each piece
had a defensible rationale — the builder survey (UE, TBB, Taskflow, .NET,
Rust all fix prerequisites at launch), the `when_all`-carries-results
argument, retraction as the oversubscription answer.

All of it is deleted. The coroutine expresses every one of those shapes as
ordinary control flow — sequencing is an await, a join is several awaits
(awaiting a settled task is free, so order does not serialize), a transform
is code after the await, a prerequisite is an await at the top of the body —
with results carried typed, in scope, no tuples and no callback plumbing.
Two vocabularies for the same graph meant every user chose per call site and
every reviewer read both; one vocabulary was worth more than the sum of the
deleted features. The functor forms that remain (`launch`, `access`/`async`
bodies, graph nodes, `parallel_for`) are *leaves* — work, not composition.

What replaced "reusable tasks" is nothing: a coroutine allocates one frame
per run, and the measured allocation story (§4.1) says that is not the cost
worth complicating the model for. `Signal::reset()` remains the one
sanctioned re-arm (a phase gate has no result and a trivially quiescent
window), and the graph re-arms its persistent node blocks per run — both on
the same `reset()` scalar re-arm the builder once used.

### 4.4 Blocking: the blue boundary (historical: retraction)

The old answer to "what if `sync()` is called with all workers busy?" was
**retraction**: a blocking `sync()` on a not-yet-started task ran it — and
its un-started prerequisite subtree — inline on the waiting thread, gated by
an atomic one-runner claim. It genuinely worked, and it was genuinely
subtle: the reuse×retraction corner produced the two hardest bugs in the
project's history (§4.5).

Coroutine-first dissolves the question instead of answering it. Threads
split into task threads and **blue threads** (main, dedicated engine
threads). Inside a task, waiting is `co_await` — the frame suspends, the
worker is freed, no deadlock is possible by construction. On a blue thread,
`sync()` parks — and parking a non-worker thread is harmless. The residual
in-task `sync()` is a bug by definition and is **fatal** under safety checks
(sharp message when the target is queued behind the caller's own grant). The
one deliberate exception is `parallel_for`'s join: the caller drains chunks
itself and then waits only on helpers that are provably running on workers —
bounded, deadlock-free, no retraction needed. With no retraction there is
also exactly one dispatch per run, which collapsed the claim/generation
machinery to a plain store plus a safety-check assert.

**Check the rule, not the incident.** Both of the checks guarding this
boundary — the in-task `sync()` fatal and the no-await-under-a-guard fatal —
were originally written at the point where the hazard would *materialize*: the
first fired only when the wait was about to park, the second only when the
`co_await` actually suspended. That is the wrong trigger, and the reason is
general enough to be a rule for any safety check in this library: **a check
whose trigger condition is the hazard's timing inherits the hazard's
nondeterminism.** In development a `sync()` target is usually already settled
and a contended pipe is usually free, so both checks stayed quiet through
every test run and then fired — or worse, did not fire — in production, on the
one frame where a prerequisite ran long. Moving them to the *call* (unconditional
in `sync_wait`; `await_ready` rather than `await_suspend`) makes the first
execution of a bad path fail, every time, on any machine.

**Ranks close the one structural hole.** Batch acquisition — a node's declared
set, a multi-object `ts::access` — is conservative two-phase locking in canonical
pipe-address order, deadlock-free by construction and independent of how many
objects it names. A field survey of Bevy, Unity DOTS, Legion, Orleans, oneTBB and
the Rust ecosystem found no deadlock attributable to declared-access *fatness*
anywhere; every reported one is a wait inside work. So the residual hole is
exactly one edge: nothing relates a grant a task already holds to an object it
awaits later. A declared `ts::Rank` supplies that edge, and Havender's argument
does the rest.

Two decisions carry the design. Ranks are **not defaulted** — not to address
order, which would make rejection ABI-dependent and non-reproducible across
builds, and not to declaration order, which is not a specification (§2.x). And
unranked is the **strict** state: holding an unranked object forbids dynamic
awaits, because an order that does not exist cannot be climbed. Only objects
that participate in a dynamic await need one, which is also the population that
can appear in a cycle.

The choice of rank over the alternatives was made on when it fires, not on what
it costs. It is O(1) — one scan of an eight-entry access context against one
field, on the cold await path — but so is a wait-edge insertion. The
difference is that a rank violation is a property of *one* await, so it fires the
first time the path executes, on any machine; a circular wait needs both halves
concurrently suspended, which is a scheduling coin-flip. Driver Verifier makes
the same argument explicitly: it bugchecks on the hierarchy violation, not when
an actual deadlock is occurring. One refinement fell out of implementing it: a
grant whose window has closed constrains nothing, so stale entries are skipped —
otherwise a detached coroutine, which carries its launcher's grant snapshot for
its whole life, would be treated as a holder forever.

**The net behind the detectors.** Both the in-task `sync()` fatal and the
circular-wait detector are *models*: they catch the shapes they were written
for. The waits-for graph in particular is blind to a cycle that passes through
a plain task-await edge — N holds an object and awaits foreign task T, T awaits
that object; neither suspension records an edge, and both frames sleep forever.
Rather than grow the model, the answer is Go's `all goroutines are asleep`
check, which models nothing: every worker idle, every queue empty, and nothing
completable from outside the pool ⇒ progress is impossible, whatever the shape.
It is O(1), needs no graph, and misses no deadlock class — including lost
wakeups, which are not cycles at all.

Three properties made it worth building rather than borrowing wholesale.
*Quiescence stays scheduler-local*, read through a plain function seam
(`detail::scheduler_quiescent`), so the task layer still does not know the
scheduler exists — and worker idleness is a busy↔idle transition count rather
than "parked", because under a spin idle policy a worker never parks. *The
interpretation costs nothing*: there is no per-task counter and nothing on the
hot path, because the observer is a boundary waiter that is already blocked.
And *the third clause is what Go lacks*: Go's check has a documented blind spot
where any live background thread masks a partial deadlock, precisely because a
goroutine cannot declare "this wait is legitimate". `ts::External_wait` is that
declaration. Its failure mode is the honest one — a forgotten registration
reports a correct program as deadlocked — so the fatal names the escape in its
own message, and the window is measured *continuously* over seconds rather than
sampled once, so a slow-but-legitimate handoff between two of the program's own
threads does not trip it.

**What the report says is the feature.** A net that only announces "deadlock"
leaves the user where they started, so the report is layered by collection cost:
the blocked waiter always; the live wait edges wherever that registry
exists, which is free and covers everyone suspended while holding a grant; and,
behind `TS_SUSPENSION_REGISTRY`, every live suspension including those holding
nothing — the two-hop shape the waits-for graph is blind to by construction.
When the last tier is compiled out the message names the rebuild flag, so the
escalation path is in the diagnostic rather than in a document.

The second tier is worth a note on why it is sound. Learned-order deadlock
*predictors* have a poor record — Linux reverted cross-release and never merged
DEPT, both on false positives — and this reads the same edges. The difference is
direction of inference: these edges are printed only after an independent
mechanism has established that nothing can progress, so they explain a
conclusion rather than reaching one, and a post-mortem cannot false-positive.

The third tier's default was measured rather than assumed, and the assumption
was wrong. Per-suspension bookkeeping was expected to be too expensive for
non-debug builds; it costs ~30 ns per suspension, is inside noise on every real
frame workload, and — because records shard by their own address — gets
*cheaper* per suspension as worker count rises rather than collapsing on a
shared lock. The one place it shows (~8%) is a microbenchmark where a single
suspension ping-pongs between two threads and nothing else happens. So it is on
wherever the harness is on. The sharding key is the load-bearing detail: a
coroutine resumes wherever its awaited work settled, so a record is linked by one
thread and unlinked by another, and keying shards on the *suspending thread*
would have put both halves of every suspension on one mutex — which is precisely
what the first measurement showed.

Hoisting the guard check had a second effect worth recording: it forced the
reentrancy exemption to be *stated*. A reentrant same-object access never
suspends, so it never reached the old check — the exemption existed only as an
accident of where the check sat. Now it is a predicate (every pipe of the
awaited task is write-owned by this task, so the access ran inline under the
held grant) with the narrowness spelled out: a *read* access under a *read*
guard is not exempt, because it does reach the pipe and can queue behind a
waiting writer that our own read hold is blocking.

### 4.5 Two real races, and the method that caught them

The (since-deleted) reuse×retraction corner produced the two hardest bugs in
the project's history, both instructive enough to keep on record:

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
it** (reading after is a TOCTOU; the generation capture in `release()` still
follows this rule today); **"only reproduces under TSan" does not mean TSan
artifact** — pin to two cores to amplify preemption windows; and
**capture-and-continue forensics beat theorem-proving** — the bug fell to an
event-ring instrumented build in one session. The racing machinery and the
forensic ring were both deleted with retraction; the lessons were not.

---

## 5. Coroutines: the composition model

Coroutine support began as an additive layer and was then made the
foundation (the coroutine-first transformation, `docs/coroutine-first.md`):
composition *is* coroutines, and the callback vocabulary is gone (§4.3). The
design points that carry the weight:

- **Fused frame and block.** The promise embeds the `Task_control_block`
  (first-member aliasing, the `Executable` pattern), so a coroutine task is
  one allocation and is an ordinary `Task<R>` to everyone else. The frame
  holds a running self-reference; awaiters, handles, and any gated completion
  it awaits (a nested graph run) hold refs, so a fire-and-forget frame lives
  exactly until settled.
- **Eager start, blue boundary.** A coroutine task runs to its first genuine
  suspension on the calling thread (no cold tasks — matching the launch
  model everywhere else), suspends without holding a worker, and is consumed
  by `co_await` from tasks or `sync()` from blue threads only (§4.4).
- **Access grants across suspension.** The harness's grant set is
  thread-local, and a coroutine migrates threads at every suspension. The
  model: a coroutine is a chain of task-*segments*, and each resumed segment
  re-installs the coroutine's grant snapshot and task identity — the
  grant-inheritance mechanism reused verbatim, no new concept. A
  coroutine *graph node* holds its declared grants until the frame
  completes — suspension does not release grants; body-return is not
  completion.
- **The pipe is already an async reader/writer lock**, so
  `co_await ts::read_write(obj)` yields an RAII guard with direct object access —
  `folly::coro::Mutex::co_scoped_lock`'s shape on top of machinery that
  existed anyway. The canonical coroutine footgun — suspending while holding
  a lock — is *detected*: a `co_await` under a live guard is fatal.
- **The suspended deadlock is detected too.** Two frames that each hold a
  grant and await the other's object deadlock with *no thread parked* — the
  failure mode a blocked-thread diagnostic can never see. Under safety
  checks every suspension-on-a-pipe records wait edges (held grants →
  awaited pipe) in a global registry, cleared at resume, cycle-checked on
  insert; the closing edge faults, naming both tasks and both objects. This
  is what makes awaited dynamic cross-object access a *blessable* residual
  pattern rather than a documented hazard.
- **Cancellation is value-based** because exceptions are off project-wide: a
  cancelled await resumes with cancelled state to inspect (fatal for a value
  task — check first), never a throw. Forced by the no-exceptions
  constraint; turned out cleaner — control flow stays visible.

Resumes run through a bounded trampoline (deep cascades resume iteratively,
proven to 50k depth), destruction through another (a deep chain of fused
frames releases iteratively), and both retain capacity — no steady-state
allocation.

### 5.1 Nested graph runs: lend, don't re-acquire

Coroutine nodes make `co_await inner.execute()` expressible, and composing a
frame out of pre-compiled sub-graphs is the obvious use. The obvious
implementation deadlocks: the inner node takes its own turn on an object its
caller is currently holding, and the caller cannot release it because it is
suspended waiting for the inner run.

The fix is to notice that an *awaited* inner run is strictly contained in the
caller's grant window, so the caller's exclusion is already exactly what the
inner run needs. At the nested `execute()` we intersect the inner graph's
compiled access set with the caller's `Access_context`; every overlap whose
held mode covers the inner mode is **lent** for that run, and the inner nodes
simply do not take turns on it. Recursion needs no extra rule — a grand-inner
graph intersects against its own caller's context, which already carries the
lent entries.

Three properties make this cheap rather than clever:

- **Binding is the mechanism.** Node pipe links live in a `compile()`-time
  slab; lending re-binds the surviving links compactly and shortens each
  node's link count. An unbound link is a turn that is never taken — no
  bypass flag threaded through the pipe, no second code path in admission.
- **Ordering is untouched.** The lend removes the *outer world's* exclusion,
  never the compiled conflict edges, so inner nodes still sequence against
  each other on a lent object exactly as they would standalone.
- **The pipe never learns about it.** External `async`s queue behind the
  caller's hold as before, so nothing outside the nesting can tell the
  difference.

The rest is refusing the cases where the containment argument fails. A caller
holding *read* where the inner graph *writes* cannot lend (upgrading would
re-acquire behind its own hold) — fatal, with the two restructurings named.
An earlier un-awaited nested run of the caller runs under the same grant and
could race the lent-to graph, so lending requires a quiet scope — fatal,
`co_await` the previous nested run first; already-settled runs are filtered
out, since only live ones are a hazard. And a detached run (`{.detach = true}`)
is *not* contained in the
caller's window, so it structurally receives no lend and queues like any
external work. Un-awaited runs otherwise join the caller's scope, which makes
"fire an inner run and forget it" safe by construction rather than by
discipline.

One limitation falls out and is worth stating rather than hiding: a graph runs
one execution at a time, so a sub-graph shared between two concurrently
running parents collides. That was previously an unguarded corruption; it is
now a fatal naming the fix (an instance per caller), and the general
relaxation — queued or pipelined runs — is roadmap work, not new machinery.

### 5.2 Rule policy: every check needs an escape and a shipping answer

The waiting rules are enforced by checks that abort. A user can uphold a rule
by means we cannot see, so a check can be a false positive; and a shipping
build should be able to decide what it keeps paying for. Both are properties
of a *check*, so they are declared per rule rather than left to whoever writes
one: a bit in `TS_ENABLED_RULES` (compile-out, state included) and, for the
rules that permit it, `ts::Relaxed_scope` (runtime opt-out, scoped).

The interesting result is that the runtime opt-out cannot be uniform. Relaxing
the guard-across-suspension rule does not merely accept a program we disapprove
of: a `Pipe_guard` installs its own `Access_context` as the thread's current
grant, so a frame that suspended under one would resume with the promise's
snapshot installed over it and then restore a `current_access` pointer captured
on a *different* thread. The rule protects an implementation invariant, and no
claim a user could make would make relaxing it sound. That splits the rules
into **advisory** (a hazard the caller may know is absent — scoped opt-out plus
compile-out), **structural** (an implementation invariant — compile-out only,
escape is the sanctioned form), and the **net** (the global quiescence check,
which has no call site to scope and whose escape is registering the legitimate
external wait instead).

A relaxation propagates with the ambient task state, not the thread — carried
across a coroutine's segments by its promise and captured into launched
sub-work alongside the grant snapshot. Wider than lexical scope, and
deliberately so: the child inherits the grant, hence the hazard the claim is
about. Full taxonomy, defaults, and the obligations a new check inherits:
[waiting-rule-policy.md](waiting-rule-policy.md).

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

`commit()` is one auto-dispatching verb. The pipe carries an always-on
`writer_owner` — the task currently holding its write grant — so `commit()`
called *by the holder* (a graph node's declared write, an `async` write body)
applies inline under the grant it already has, while any other caller's
commit becomes one enqueued write. The alternative — two spellings, the
under-grant `commit()` and an acquiring `commit_async()` — pushed a
scheduling decision onto the user that the pipe can answer itself, and made
the common "call it from wherever the frame logic sits" case a foot-gun
(the wrong spelling either double-acquires or deadlocks). Ownership is
behavior-bearing state, so it lives outside `TS_SAFETY_CHECKS`; the
diagnostic-only grant machinery stays gated.

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

**7.9a `TS_ENSURE` (the UE-`ensure`-shaped recoverable assert).** `ts::fatal`
is the right tool for corruption; some hazards deserve a loud diagnostic
without killing the process. The macro evaluates its expression once in
every configuration and yields the bool (`if (!TS_ENSURE(x, "..."))`
recovers naturally, UE semantics); on failure it counts every occurrence
but reports once per call site (a captureless-lambda function-local static
is the site claim), so a per-frame recurrence is one stack trace in the log
and an exact number in the counter. Presentation goes through a swappable
handler (`ts::set_ensure_handler`, `std::set_terminate` shape); the default
prints `ENSURE FAILED:` + stack and breaks into the debugger when one is
attached (the C++26 `std::is_debugger_present`/`std::breakpoint` pair,
polyfilled — `IsDebuggerPresent`/`__debugbreak` on Windows, `TracerPid` +
`int3`/`brk` on Linux, sysctl `P_TRACED` on macOS — slated for the platform
layer; `ts::fatal` uses the same break before `abort`, stopping at the
failure site). Counting and once-filtering live outside the handler, so a
host's custom presentation (its own attach dialog, say — the library ships
none deliberately) can never hide a failure: the test harness fails on
failures no test explicitly consumed, and the bench/stress drivers fail
their exit code on any — a tripped ensure cannot pass CI by virtue of the
program having survived it. (The mechanism's first user — the blocking-sync
warning — was later *promoted to fatal* when coroutine-first made an in-task
`sync()` a bug by definition rather than a hazard: the check lives at the
one chokepoint every blocking wait passes through (`sync_wait`) and fires
unconditionally when a task calls it — whether or not the target has already
settled, since the rule, not the wait's timing, is the hazard (§4.4) — and
distinguishes the certain-deadlock shape — the target is an access to an object the
waiting task holds, matched by comparing the target pipe's epoch address
against the context's captured epoch sources — from the general never-block
violation. `parallel_for` joins through its own counter and is structurally
exempt.)

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
- **Forensics on demand**: the event-ring harness that cracked the TOCTOU
  (§4.5) was purpose-built, kept until the machinery it instrumented was
  deleted, then deleted with it; the deterministic allocation profiler
  remains in-tree.
- **Platform reality of the oracles.** ThreadSanitizer has no Windows
  runtime, and most game development happens on Windows: TSan therefore
  verifies *this library's* concurrency machinery (portable sources, Linux
  CI), while a Windows-hosted game gets ASan, stress loops, and — chiefly —
  the access harness, which is the only race-adjacent oracle available on
  the dominant platform. That asymmetry is an argument for keeping the
  harness cheap enough to leave enabled: at ~1 ns per check, shipping with
  `TS_SAFETY_CHECKS` on may be viable for many titles, unlike the
  editor-only/development-only enforcement every surveyed system defaults
  to (Unity, RDG, Chromium). This is not offered as a blanket
  recommendation: the per-*call* cost is fixed but the per-*frame* cost is
  workload-shaped — a check on a hot trivial getter called millions of
  times a frame is a real tax, and no benchmark can characterize every
  program's method mix, so "leave it on" stays a per-title measured
  decision rather than a default. The design point is only that the option
  exists at all, which the editor-only competitors cannot offer.
- **Why both oracles, framed precisely.** The two are not the same check
  from two angles; they have inverted profiles and verify adjacent layers.
  TSan is *sound but incomplete*: given the synchronization it observes, a
  reported race is a real happens-before violation (order-independent
  verdict — it catches a race on runs where the timing happened to work,
  needing only the unordered pair to execute, not the corrupting order),
  but it sees only interleavings actually sampled, so coverage is
  schedule-dependent and it verifies the *implementation* under the contract
  (queue, pipe, refcounts, lock-counter, coroutine handshakes). The access
  harness is the dual — *complete but narrow*: any undeclared touch of a
  `Guarded` is caught on the first run of that path, deterministically and
  with no concurrency required (it catches the *latent* race the completeness
  hazard warns of, before parallelization realizes it), but the property is
  only declared-access completeness and only over `Guarded` objects, and it
  validates *possession* of a grant, not *ordering* against the writer that
  produced a value. Neither subsumes the other: a sound-but-incomplete
  dynamic detector plus a complete-but-narrow always-on invariant is the
  verification strategy, not a redundancy to collapse. Extending the harness
  from possession-checking toward ordering-checking is the grant-generation
  direction (TODO 1.11/1.13, the Unity version-stamped-handle idea).

---

## Appendices / references

Project documents:

- [task-internals.md](task-internals.md) — the dynamic-task design of
  record: control block, lifecycle, lock-counter, coroutine frames,
  completion gating.
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
