# Design rationale

This document explains why the library is shaped the way it is. It covers the
central idea, the decisions behind each layer, and the alternatives that were
tried or analyzed and rejected. It assumes you know concurrent
programming in depth. Some examples draw on game-engine architecture (frame
graphs, job systems, the render/game thread split), but that background is not
required. The user-facing companion is
[guide.md](guide.md). Four deeper documents are linked throughout and listed
at the [end](#appendices--references).

---

## 1. Central idea and lineage

This library draws on Unreal Engine's Tasks System and keeps the concepts
that system proved: work-stealing scheduling,
ordered prerequisites, nesting, events, pipes, and priorities with a small
fixed set of levels. They take a different form here. Prerequisites are
`co_await` at the top of a body, and nesting is internal completion gating
rather than a user-facing `nested` verb. The library rebuilds these concepts
around a different central idea, access-declared concurrency.

Every unit of work declares which shared objects it reads and which it
writes. From those declarations the library derives scheduling constraints,
in the form of a reader/writer pipe per object and conflict edges in a
static graph. A runtime harness then verifies at execution time that no code
touches a guarded object without a matching grant.

The nearest relatives each hold two of the three pieces:

- ECS schedulers (Bevy, Unreal Mass, Unity DOTS) derive schedules from
  declared component access, but at per-component-type granularity, inside
  an ECS data model.
- Unity's Job Safety System validates read/write handles at runtime, but it
  validates a hand-built dependency graph rather than deriving one.
- `folly::Synchronized` binds data to its lock with const-selected access,
  but it blocks, and it has no schedule derivation and no harness.

No production system we surveyed combines per-object-instance granularity,
derived scheduling, and a runtime oracle in a general-purpose C++
library. That intersection is what this library provides. The full survey is
in [task-systems-comparison.md](task-systems-comparison.md).

Two facts from the research sharpen this position
([research-deepdive.md](internals/research-deepdive.md) §15, §2). First, the industry
already ships static frame skeletons. Unreal's tick system (`ETickingGroup`
phases, per-function tick prerequisites, opt-in `bRunOnAnyThread`) is a
coarse declared frame graph that has sat in the engine for a decade. It is
underused for parallelism because nothing declares what a tick touches, so
the engine cannot safely move work off the game thread by default. The
missing piece is not the graph shape or the dynamic runtime; it is the
access layer that makes the static structure safely parallelizable, which
is what this library supplies. Second, the boundary is deliberate. Workloads
whose top-level shape is data-dependent per run are out of scope for the
static graph by design. Renderers are the canonical case: the pass set is a
function of frame content, which is why every render graph redeclares per
frame. The static graph targets the stable frame skeleton, and
data-dependent work lives in the dynamic layer under the same access model.

---

## 2. The access model

### 2.1 Granularity: coarse objects, not component columns

The unit of guarding is an object instance, in practice a whole subsystem
(`Guarded<Physics_world>`, a transform store, a nav mesh) rather than a
fine-grained entity. This is a deliberate position in a real tension:

- Fine granularity maximizes theoretical parallelism, since two tasks
  touching different rigid bodies never conflict. But it explodes the
  conflict graph to O(entities), scatters memory, and makes per-access
  overhead dominate.
- Coarse granularity keeps the graph at O(subsystems), around a hundred
  nodes for a real frame, but one writer serializes all readers of that
  object.

The resolution is to wrap coarsely, get intra-system parallelism from
`parallel_for` running under the system's grant, and split state along the
seams that need to be concurrent. The sample double-buffers its transform
store so the writer and the frame's readers touch different objects. Once
enough subsystems process concurrently to fill the cores, finer
decomposition buys nothing.

The ECS alternative, where access is declared per component type and batched
over contiguous columns (Bevy's `FilteredAccessSet`, Mass processor fragment
access, Unity's per-`NativeContainer` handles), was analyzed and declined.
Its cache-layout advantage is real, but it comes from the ECS's SoA data
model, not from the scheduler, and a coarse wrapped subsystem already owns
its arrays contiguously inside. A `Guarded_pool<T>` per-type primitive was
sketched and rejected as buying nothing the coarse model lacks (§7.2).

### 2.2 The harness as the correctness oracle

Deriving safety from declarations has a known failure mode, named early and
designed around: the completeness hazard. The derived schedule is only as
safe as the declarations are complete. A stray reference or an undeclared
task can race an object the graph believes it owns, and parallelization can
promote such a latent bug into a live one.

The harness is the answer. `TS_CHECK_ACCESS()` in a guarded type's methods
checks `this` against a thread-local grant set installed around every task
body, and faults on any undeclared access with a message, a stack trace, and
an abort. It costs about 1 ns per call (an inline array scan, no locks) and
compiles out under `TS_SAFETY_CHECKS=0`. It is per-method instrumentation,
so it is opt-in and not airtight. The design position is that a cheap
runtime oracle that stays on throughout development beats both unverifiable
convention and a compile-time scheme that would constrain the programming
model (§7.9 on Rust/Pony).

Grant inheritance makes the model compose, but only for structurally gated
sub-work, meaning `parallel_for` chunks and coroutine segments after
suspension, which carry the parent's grant set by value. Gating is the
soundness condition. The parent's completion waits for the child, so the
grant provably outlives it. That one rule lets a graph node fan out dynamic
work over its declared data without new declarations, and it is what
coroutine support reuses wholesale (§5). A detached `ts::launch` is not
gated, since its handle may be dropped, so it inherits nothing and runs
under an empty context. If it touches the parent's data, it faults
deterministically as undeclared access rather than racing the next acquirer
on a timing window. (An earlier design also inherited into a concurrent
child, `ts::nested` / `Task_scope`. That was removed. A concurrent
grant-inheriting child can race its parent on shared mutable state, and the
harness cannot see it because both sides declared the access.
`parallel_for`'s synchronous join is the gated form that survives. See
docs/internals/coroutine-first.md §4.3.)

Where inheritance does apply, it is bounded by grant-window validity. Each
pipe carries a write epoch with seqlock parity, bumped at write-grant
acquire and release, and bumped by 2 on a graph write handoff, which elides
both pipe operations. Every context entry declared under a pipe grant
captures the epoch at declaration. A snapshot that outlives its window,
such as a detached coroutine created under a node's grant that resumes after
the node released its objects, fails the comparison at the next instrumented
access and faults with a stale-grant diagnostic rather than silently racing
the next acquirer. The parity choice makes one rule serve both modes. A
write entry is valid while its holder's window is open. A read entry is
valid until a writer acquires, since other readers coming and going do not
bump the epoch; that is the actual safety condition, not the origin reader's
own lifetime. Nested-gated sub-work is structurally never stale, because the
parent's completion, and so its release, waits for it.

### 2.3 Introspection: reading back the derived structure

A derived schedule the user never wrote down needs a way to be read back,
because tuning a graph means seeing which edges exist and why.
`compile(DOT_path)` dumps the DAG as Graphviz DOT with per-edge provenance.
Explicit orderings are solid, and conflict-derived edges are dashed, with
the object and modes shown on hover. The distinction is actionable. A
`W->R` edge is dataflow, while `R->W` and `W->W` edges are ordering
artifacts that versioning or deferral can delete. That is the same lever
that moved the sample's audio off the post-flip tail.

That provenance distinction has a consequence worth stating as a rule, because
it is easy to get backwards. A derived edge exists for safety: two conflicting
nodes must not overlap. Both directions satisfy that, so the direction
`compile()` picks, by declaration index, is an artifact of how the building
code is written, not a claim about the frame. Declaration order resolves the
direction of a derived edge; it is not a specification. Logical precedence is
intent and belongs in an explicit `after`/`before` edge. Declaring one costs
nothing when it agrees with the derived edge, since the two dedup into a
single edge that keeps the conflict in its tooltip.

Two things depend on holding that line. The first is refactoring. In the
sample's optimised variant the draw producers stage through `Deferred` and stop
touching the queue, so the conflict vanishes and the derived edge with it.
Only the intent edge keeps the frame correct across that change. The second is
headroom. Because direction carries no meaning, a future `compile()` may reorder
independent conflicting nodes to shorten the critical path. That optimisation is
only available while no program has quietly made declaration order load-bearing.
This is why the sample was audited and its two intent-bearing sets, the render
producers into `submit` and every last-frame transforms reader ahead of the
flip, were made explicit even where a derived edge already pointed that way.

Naming follows the UE convention of a debug name given at creation and kept
in all builds, with one modernization and one unification. The modernization
is that a defaulted `std::source_location` argument lets an entity identify
itself by where it is written, spelled `ts::Named{}`, with no macro. The
unification is one type, `ts::Named`, for all three kinds of entity. Nodes
and objects must carry one; there are no unnamed constructors, and the old
`node<N>`/`objN` ordinal fallbacks are gone with them. Tasks carry one
optionally, because their verb captures the call site by default. `Named`
keeps `{literal, file, line}` rather than a whole `source_location`, since
`function_name()` is never used here.

`Named` is a distinct wrapper type rather than a bare `const char*` because
the `Guarded` constructor forwards to `T` in place, so a bare leading string
would be indistinguishable from `T`'s own first constructor argument. For the
same reason the constructor is constrained to an actual `Named`, so
`Guarded<std::string> g{"hello"}` is a compile error rather than a silently
default-constructed string named "hello". A trailing defaulted parameter after
an object pack is not expressible in C++, hence the leading position, which is
also UE's argument order. The leading position leaves one gap: the
multi-object `ts::access`/`async` end in an object pack and have no site to
capture. That is why the option field is a `Named` rather than a literal.
Writing `{.name = ts::Named{} }` lets the caller capture the site the verb
cannot.

The load-bearing implementation rule is that a defaulted `source_location`
captures the caller of the function that declares it. It is therefore declared
only on the outermost verb the user calls, and the resulting `Named` is
threaded down every internal layer explicitly. An inner helper with its own
default would capture a library header, and the whole feature would quietly
become useless. A test asserts that captured sites land in the test file,
which is the only thing that actually guarantees it.

An object's name lives on the pipe, the shared per-object structure every
layer already reaches. A task's name lives on its control block, gated by
`TS_SAFETY_CHECKS` so shipping pays nothing for it. A graph node's block
carries the node's name, and a coroutine frame inherits the identity of the
task it was created inside, so a diagnostic about a suspended frame names the
node the user declared rather than a block pointer.

Everything downstream of the name, meaning the structure dump and the runtime
trace, is gated by `TS_PROFILING` (default on; define it to 0 for shipping).
The formatters live outside the library proper, in `tools/`.

The runtime trace aggregates rather than records. Per node it keeps a
Welford mean and variance, P^2 quantile markers (Jain–Chlamtac) for P50/P95,
min/max, and a per-worker histogram, which is O(nodes + edges) state however
many runs fold in. The graph's entire involvement is three stamps into
pre-sized `Run_state` arrays and one call at run settle. Every statistic,
the timeline reconstruction, and the SVG live in `tools/graph_trace.h`, so
the scheduler logic stays legible.

The reconstructed "average run" has one subtlety. With mean-based bars an
edge's endpoints can never cross: `start_v >= end_u` holds pointwise in every
real run, expectation preserves pointwise order, and `E[start] + E[dur] =
E[end]` by linearity. Medians make a better "typical run" because they ignore
outlier frames, but they are not linear, so median bars of an edge-connected
pair can overlap even though no real run overlapped them. The resolution is
the edge's streamed meet point, the mean of predecessor-end and
successor-start. Crossing bars are clamped to it, predecessors first in
topological order. Unrelated nodes sharing a lane are deliberately not
clamped. Their aggregate overlap is real, since the lane is only the node's
modal worker, so they stack into sub-rows instead of being hidden.

Critical-path detection is measured, not inferred. Each run the trace walks
backward from the latest-finishing node through the binding predecessor, the
incoming-edge node that finished last and whose completion therefore released
this one, and counts chain membership per node and edge. A variable frame has
no single critical path, so the aggregate is a frequency ("critical in 82% of
runs"), rendered as highlight intensity rather than a boolean. The structural
CPM pass, using median durations and zero latency, is computed alongside for
per-node slack. It is the dependency lower bound, blind to queue latency and
pipe contention, and its divergence from the measured chain is itself the
signal that a frame is scheduling-bound rather than dependency-bound. A third
stamp at the dependency counter's zero transition splits that overhead out
exactly. Ready-to-start is acquire plus queue latency, attributed per node as
"dispatch wait".

### 2.4 What the graph is worth, measured

The graph is optional, since the access verbs and `co_await` compose the same
work, so the library owes an answer to what declaring it buys.
`sample/game_frame.cpp` carries both spellings of one ~30-system frame over
the same `World` and the same system bodies (`build_frame_graph` vs
`run_frame_graph_free`), so the answer can be measured. There are three
results, and none of them is the expected one.

Safety is not what it buys. Every claim the harness enforces comes from the
pipe turn a declaration takes, and a hand-composed `ts::async(fn, objs…)`
takes the same turn. Dropping the graph weakens exactly one guarantee, which
is that nothing derives or checks the order.

Pipe FIFO does not stand in for the conflict edges. The tempting shortcut of
launching the systems in declaration order and letting each object's FIFO
reproduce the derived edges does not hold, because the multi-object cascade
enters links one at a time in canonical (pipe-address) order. A system blocked
on its first object has not taken its slot on the later ones, so a system
launched after it walks straight past. In the sample this mis-orders
`frustum_cull` before `camera` and lets `submit` clear the draw queue before
`cmd_record` reaches it. A frame of draw commands is lost silently, with every
declaration correct, and the frame runs 7.6% faster for the missing edges.
This is the completeness hazard's twin. The harness is an oracle for
undeclared access, and there is no oracle for mis-ordered declared access.
Every edge `compile()` derives from those declarations must be written by hand
as an explicit `co_await` in one of the chain coroutines the frame is cut
into, plus the joins that fold them back together.

The performance advantage is resume locality, not allocation amortization.
The standing assumption was that near-zero-alloc re-runs were the graph's
measurable edge. They are not. Graph-free costs ~95 more allocations per
frame, which at ~17 ns each is under 2 µs, against a measured +56–64 µs/frame
on a 4.1 ms frame and +89–131 µs on a 0.46 ms one (+1.4–1.6% / +19–29%). The
gap is about 50 coroutine suspend/resume round trips at ~1.8 µs each. The
graph dispatches a successor directly on the thread that settled its last
predecessor, while an awaited handle suspends a frame and resumes it through
the trampoline. The lever for closing the gap is therefore resume locality,
and the practical reading is that hand composition is free on
millisecond-grained systems and starts to cost at fine grain.

### 2.5 The cost of the coroutine-first node dispatch

The coroutine-first graph unified each node's dispatch onto the same
ownership-carrying block path as every other task, and that was not free. It
cost roughly 9% more per node than the pre-transformation block. In frame
terms that is under 1% of wall time, since framework overhead is only ~4–10%
of a real `game_frame`
(see [profiler-guided-optimization.md](internals/profiler-guided-optimization.md)).
This section records the measured cause, because the intuitive one is wrong.
It also records the recovery, which not only erased the regression but took
the per-node machinery below the old block by slimming a completion cost both
versions shared.

Deterministic instruction counting puts it at 522 to 648 instructions per
node and attributes it precisely
([graph-regression-callgrind.md](internals/graph-regression-callgrind.md)). The
measurement used callgrind on two isolated Shipping-like builds with a
1000-empty-node worker-less driver, so machinery dominates. The tempting
explanation, covering the coroutine-capable completion path, the
`execution_flag` self-lock in `run_graph_node`, and the `add_nested` gating,
is disproven. That code is byte-identical between the two versions and
predates coroutine-first, and the settle/completion path is in fact cheaper
now. The actual cost is the dispatch trampoline. Coroutine-first unified node
dispatch onto the generic refcounted block path: `submit_ready(Task_ptr` by
value, an atomic increment`)`, then `run_block_dispatch` (adopt plus
destructor, an atomic decrement), then `execute`. The baseline instead queued
a borrowed raw `Node*` with no reference counting. That per-node atomic
increment/decrement pair plus the `Task_ptr` churn is the bulk of the cost.
The remainder is `global_scheduler()` being re-resolved per dispatch, where
the baseline cached it, and an unconditional `advance_pipe_links` on
object-free nodes.

What the unified path buys is real: one dispatch/cascade code path shared by
dynamic multi-object `async`, coroutine node frames, and nested `execute()`
lends, plus correct block ownership when a node body outlives its synchronous
return through a `co_await`. Most of the cost has now been recovered on the
common case that does not need block ownership, all measured on the same
harness against a re-measured baseline of 662 ins/node on the current
toolchain:

- Borrowed-pointer dispatch. A plain functor node's block is owned by
  `Run_state` for the whole run. Runs are sequential and joined, so the block
  is never freed mid-run, and its queued dispatch can carry a borrowed raw
  pointer with no reference count. A `flags.borrowed` bit selects the path,
  keeping the ownership-carrying path for the async, coroutine, and
  multi-object blocks that can outlive their launcher. This saved 24
  ins/node.
- Cached scheduler. The run resolves `global_scheduler()` once into
  `Run_state` and dispatches its nodes through that rather than re-resolving
  per node. This saved 23 ins/node.
- Skipping `advance_pipe_links` when `pipe_count == 0`, meaning an
  object-free node entered no pipes. This saved about 2 ins/node.
- Slim node completion, the biggest lever. A graph node block never has an
  external `sync()`/`co_await` waiter, continuations, or a nested parent, yet
  it paid the full generic `Task_control_block` completion primitive on every
  node: a `std::mutex`, a `condition_variable::notify_all` that wakes nobody,
  and an empty continuations vector. The node-specific completion fires
  `on_complete` directly under the atomic flags. This saved 153 ins/node.

Together these bring the per-node machinery from 662 to 459 ins/node
(−30.7%), below the 522-ins/node pre-transformation block, because slimming
the completion primitive helps a cost that predated the transformation and
was present in both versions. Wall-clock on the worker-less machinery
microbench moved from ~53 to ~45 ns/node, about 15%. That is smaller than the
instruction delta because the removed mutex and condition variable were
uncontended there. A per-run re-arm batching lever was attempted and
reverted, since its gain sat below the measurement noise floor. Keep this
whole finding distinct from §2.4's resume-locality result, which compares
graph against graph-free; this one compares node dispatch against the block
that preceded the transformation.

---

## 3. Scheduler

The scheduler evolved in measured stages from a deliberately naive baseline
(one mutex-guarded `std::priority_queue`) to the standard production shape:

1. Per-priority lock-free MPMC queues (Vyukov array-with-sequences), scanned
   from high to low. The baseline's mutex plateaued at ~6.4 M submits/s
   beyond four producers, and this stage removed the
   comparator-under-one-mutex bottleneck.
2. A Vyukov eventcount for parking idle workers (prepare/re-check/commit,
   epoch-based), replacing a counting semaphore. It needs no permit
   accounting, and it extends to asking whether there is work anywhere once
   work lives in per-worker deques.
3. Chase-Lev work-stealing deques per worker for `normal` work (owner LIFO,
   thieves FIFO steal, bounded with overflow to the global queue), measured
   at +17–22% on fork-join workloads.
4. A low-starvation valve that serves one `low` task after N consecutive
   high/normal ones, the Go/Tokio "check the global queue every ~61" idea,
   so `low` means low rather than never.

Priorities are three fixed levels applied at dispatch. High is strict, and
low is valve-protected. Task priority is deliberately not mapped to OS
thread priority (§7.5).

#### Idle policies

There are three per-instance runtime modes:

- `spin` never parks.
- `spin_then_block`, the default, is the UE style. It spins N scans, then
  parks.
- `handoff` is the Go style. At most one spinner keeps watch, a spinner that
  takes work promotes a successor, and producers pay a wake syscall only on
  the fully-parked 0→1 transition. This moves wake latency off the submit
  path, with measured ~40% lower wake latency.

Two variants were built and then removed or rejected. A bare park-immediately
`block` mode was redundant, since `spin_then_block` with zero spin cycles is
the same thing. The "pure" handoff, where producers never touch wake state at
all, is correct only with seq_cst Dekker fences, and `atomic_thread_fence` is
invisible to ThreadSanitizer, which this project treats as disqualifying
(§7.6). The shipped handoff keeps correctness on an always-advanced epoch,
and the spinner count is advisory and relaxed.

#### Worker-less (single-threaded) mode

Worker-less mode (`Scheduler_config::single_threaded`) is UE's
no-multithreading shape, verified against its source before adoption. UE
creates zero workers when the platform lacks threads, and its
`LaunchInternal` then executes each task inline at the launch point, looping
the continuation chain. Ours mirrors that. There are no workers, and
`submit` runs the task immediately on the submitting thread through a
bounded FIFO trampoline, the same iterative-drain shape as inline dispatch.
A blocking wait drains the thread's pending trampoline entries before
parking, because a body that admits work and then `sync()`s it would
otherwise deadlock; UE's wait paths follow the same rule. Work triggered
from an external thread runs on that thread. One structural consequence
landed with it: pipe job submission was moved outside the pipe mutex.
Admission state still updates under the lock, and the admitted batch is
submitted after unlock. Under inline-at-submit a job body releasing its own
pipe would otherwise deadlock on the held mutex, and shorter critical
sections are better in every mode. Worker-less graph runs are excluded from
the trace's parallel aggregates, since their timings describe the trampoline
rather than scheduling. Their clean per-node durations are the planned
serial-baseline lane for the profiler-guided-optimization cost model.
Priorities and idle policies are inert in this mode by design.

#### A single worker pool

There is a single process-wide scheduler, brought up explicitly with
`create_scheduler` and reconfigured by teardown and recreation. It is never
created lazily, because a scheduler is
heavy, and there is never a set of coexisting pools. This is an invariant,
not a default. `Scheduler`'s constructor is private, and the sole way to
build one is a `detail::make_scheduler` factory reserved for the global
holder (it returns a `unique_ptr`, the type being non-movable), which
enforces the single-instance rule. Ad-hoc `Scheduler s{cfg}` does not
compile, and a second `create_scheduler` while one is running is fatal. Two
things fall out. First, a whole degree of freedom disappears. With one pool
there is no "which scheduler" question, so the former `thread_local
current_scheduler` selector is gone and `current_worker_index >= 0` is the
only worker-vs-external test. This removed a recurring bug class where a
null selector on a non-worker thread silently mis-attributed work; a
profiling metric once read 0.0% for exactly this reason. Second, benchmark
integrity. A second pool with never-parking (`spin`) workers would burn
cores alongside the pool under test and add noise; one pool means only one
pool ever spins. Tests and benchmarks that want a specific worker count or
idle policy reconfigure the global for their scope rather than standing up a
rival pool.

#### One implementation behind the task API

The intent is at most two or three interchangeable scheduler implementations
sharing the `Task`/`Guarded`/graph surface, not a plugin framework. Today
that means the current work-stealing pool, and on the roadmap a
foreground/background two-pool variant whose background band runs on
OS-low-priority oversubscribed threads, so the OS preempts it out the
instant foreground work appears (TODO 3.7). The design constraint that keeps
that tractable is that the scheduler's parts (the eventcount, the Chase-Lev
deques, the per-priority MPMC queues, the low-starvation valve, the
submit/steal loop) are composable building blocks rather than a monolith. A
new implementation reuses most of them and adds only its distinguishing
logic, such as a different idle/wake policy, an extra pool, or an OS QoS
band. Anything fancier than a wake-policy choice is out of scope at this
stage; the goal is a small, legible menu, not a general framework.

A corollary is a standing rule: the scheduler's public API stays minimal,
because it is the seam an alternative scheduler must reimplement. The
contract the task layer consumes is `submit(func, data, priority)`, two
queries (`worker_count()`, `single_threaded()`), and construction-time
config. Everything else, including the queues, deques, eventcount, idle
policies, and the profiling accumulators, is internal. Every public member
added to `Scheduler` is a member every future implementation must honor.
Additions need that justification, and features should land as config fields
or internal machinery before they land as API.

#### The pipe's FIFO contract

A `Guarded` object's accessors run in submission order with reader
coalescing. This is a semantic contract, not a scheduling choice. Analysis
of reader/writer prioritization knobs (writer-priority, batch-readers)
concluded that they are value-changing reorderings in a submission-ordered
queue: a read moved past an earlier write observes different state. That is
unlike a lock, where waiters race with no defined order and preference
merely biases the race. So FIFO plus coalescing is the only default,
relaxations would be explicit staleness opt-ins, and the real
reader-throughput answer is structural (`Versioned<T>`, §6) rather than
queue policy.

---

## 4. The task core

### 4.1 A monomorphic control block

Every task is one heap allocation, a fully monomorphic `Task_control_block`
that is parameterized on nothing and uses no virtual dispatch. The result is
type-erased behind a `void*` into a wrapper the handle refcounts
intrusively, and the body is reached through one function pointer.
Templating the block, the obvious alternative, was rejected for two reasons.
The scheduler is runtime-configured, and thread-locals like the current
worker index would fracture across instantiations. And a single block type
is what makes re-arm (`reset()` for `Signal` and the graph's per-run nodes)
and pooling tractable. `shared_ptr` was systematically replaced with
intrusive refcounting: one pointer per handle, one allocation, and a
`destroy` thunk for the aliasing wrappers. A coroutine task fuses further.
The promise embeds the block, so frame, block, and result are one
allocation.

The allocation story was then attacked empirically. A deterministic
allocation profiler (`--memprofile`) counts allocations per operation.
Among other things, it found that every queued task paid a second heap
allocation just to box a closure for the scheduler's raw function-pointer
API. The dispatch path was restructured so the block itself is the queue
payload, with 16-byte queue entries and per-dispatch state riding on the
block. The current counts are that `launch` and `async` allocate exactly one
block per operation, and a coroutine chain allocates one frame per
coroutine. The measured conclusion worth recording is that allocation is a
secondary cost here. Cross-thread scheduling latency dominates rich
operations, so the remaining pooling and arena work is scheduled as WIP
rather than urgent.

### 4.2 Results: `const&` by default, `take()` to move

`sync()` returns the result by `const&` and does not consume it, while
`take()` is the explicit destructive move. `std::future::get()`'s move-out
semantics were copied at first and then deliberately reverted. `future`
moves because its shared state must be droppable after one `get`, hence the
`shared_future` split. Here the handle owns the refcounted block, so a
`const&` into it is valid for the handle's lifetime. The contract becomes
"any number of readers; at most one mover, and it must be last". That gives
the multi-consumer ergonomics of `shared_future` without a second type, and
move-only results still work.

Cancellation makes that pair insufficient rather than wrong. `sync()` and
`take()` abort on a cancelled task, which is the correct assertion for the
common case with no token in play, but it punishes the caller for a state
the callee chose. There is also no check-then-read that is not a race, since
the check and the read are separate observations of a state another thread
is setting. Changing the return type to an optional was rejected outright,
because every call site that cannot be cancelled would pay an unwrap
forever, for a case it does not have. The answer is two more verbs rather
than a different one. `try_take()` never blocks and comes back empty when
the task is unsettled or cancelled, which also makes it legal inside a task.
`co_await t.as_optional()` is the same wait, branching instead of fatalling.
Both consume, so they compose with the "at most one mover" rule rather than
adding a third kind of read. Neither exists for `void`. A void task has no
result to be missing, so `is_done()` and `is_cancelled()` already answer
both questions, and defining a `Maybe<void>`-shaped type to make generic
code uniform was judged not to earn its name.

This whole vocabulary applies unchanged to the caller-owned `Access_op` that
single-object `Guarded::access` returns. `access` is the attended verb,
meaning the caller stays for the result, so the operation's entire state
(completion core, result storage, body, pipe entry) lives in the returned
object rather than a heap block, and an access allocates nothing. `async`
keeps the detached, heap-backed `Task<R>`. The consume verbs are the same,
refined by the handle owning its storage: `co_await`, `take()`, and
`try_take()` as above, plus a ref-qualified `sync()`. The `&` overload is
the non-consuming `const&` peek, and the `&&` overload returns by value so
the temporary form `obj.access(fn).sync()` stays dangle-free. The handle is
non-movable because it is pinned; the pipe's FIFO holds its embedded entry's
address. Destroying it unsettled is the caller-owned analog of dropping an
in-flight refcount, a checked bug that then blocks. The full rationale is
`docs/internals/access-op-design.md` (TODO 1.19).

### 4.3 One composition mechanism (historical: builders, `then`, `when_all`)

Earlier revisions carried a full callback-composition surface: `then`
continuations with apply-style tuple unpacking, `when_all` joins,
`ts::task(fn).after(...)` builders doubling as reusable-task handles, deep
retraction, and inline dispatch for dynamic tasks. It worked, and each piece
had a defensible rationale. The builder survey showed UE, TBB, Taskflow,
.NET, and Rust all fixing prerequisites at launch, `when_all` carried
results, and retraction answered oversubscription.

All of it is deleted. The coroutine expresses every one of those shapes as
ordinary control flow. Sequencing is an await. A join is several awaits, and
since awaiting a settled task is free, the order does not serialize. A
transform is code after the await, and a prerequisite is an await at the top
of the body. Results are carried typed and in scope, with no tuples and no
callback plumbing. Two vocabularies for the same graph meant every user
chose per call site and every reviewer read both. One vocabulary was worth
more than the sum of the deleted features. The functor forms that remain
(`launch`, `access`/`async` bodies, graph nodes, `parallel_for`) are leaves.
They express work, not composition.

Nothing replaced "reusable tasks". A coroutine allocates one frame per run,
and the measured allocation story (§4.1) says that is not a cost worth
complicating the model for. `Signal::reset()` remains the one sanctioned
re-arm, since a phase gate has no result and a trivially quiescent window,
and the graph re-arms its persistent node blocks per run. Both use the same
scalar `reset()` re-arm the builder once used.

### 4.4 Blocking: the blue boundary (historical: retraction)

The old answer to the question of what happens when `sync()` is called with
all workers busy was retraction. A blocking `sync()` on a not-yet-started
task ran it inline on the waiting thread, along with its un-started
prerequisite subtree, gated by an atomic one-runner claim. It worked, and it
was subtle. The reuse and retraction corner produced the two hardest bugs in
the project's history (§4.5).

Coroutine-first dissolves the question instead of answering it. Threads
split into task threads and blue threads, the latter being main and
dedicated engine threads. Inside a task, waiting is `co_await`. The frame
suspends, the worker is freed, and no deadlock is possible by construction.
On a blue thread `sync()` parks, and parking a non-worker thread is
harmless. The residual in-task `sync()` is a bug by definition and is fatal
under safety checks, with a sharp message when the target is queued behind
the caller's own grant. The one deliberate exception is `parallel_for`'s
join. The caller drains chunks itself and then waits only on helpers that
are provably running on workers, which is bounded and deadlock-free and
needs no retraction. With no retraction there is also exactly one dispatch
per run, which collapsed the claim/generation machinery to a plain store
plus a safety-check assert.

A check should trigger at the call, not when the hazard materializes. Both
of the checks guarding this boundary, the in-task `sync()` fatal and the
no-await-under-a-guard fatal, were originally written at the point where the
hazard would materialize. The first fired only when the wait was about to
park, and the second only when the `co_await` actually suspended. That is
the wrong trigger, and the reason is general enough to be a rule for any
safety check in this library: a check whose trigger condition is the
hazard's timing inherits the hazard's nondeterminism. In development a
`sync()` target is usually already settled and a contended pipe is usually
free, so both checks stayed quiet through every test run and then fired in
production, or worse did not fire, on the one frame where a prerequisite ran
long. Moving them to the call, unconditional in `sync_wait` and in
`await_ready` rather than `await_suspend`, makes the first execution of a
bad path fail every time, on any machine.

Ranks close the one structural hole. Batch acquisition, whether a node's
declared set or a multi-object `ts::access`, is conservative two-phase
locking in canonical pipe-address order, deadlock-free by construction and
independent of how many objects it names. A field survey of Bevy, Unity
DOTS, Legion, Orleans, oneTBB and the Rust ecosystem found no deadlock
attributable to the fatness of declared access anywhere; every reported one
is a wait inside work. The residual hole is exactly one edge: nothing
relates a grant a task already holds to an object it awaits later. A
declared `ts::Rank` supplies that edge, and Havender's argument does the
rest.

Two decisions carry the design. Ranks are not defaulted, neither to address
order, which would make rejection ABI-dependent and non-reproducible across
builds, nor to declaration order, which is not a specification (§2.3). And
unranked is the strict state. Holding an unranked object forbids dynamic
awaits, because an order that does not exist cannot be climbed. Only objects
that participate in a dynamic await need a rank, which is also the
population that can appear in a cycle.

The choice of rank over the alternatives was made on when it fires, not on
what it costs. It is O(1), one scan of an eight-entry access context against
one field on the cold await path, but so is a wait-edge insertion. The
difference is that a rank violation is a property of one await, so it fires
the first time the path executes, on any machine. A circular wait needs both
halves concurrently suspended, which is a scheduling coin-flip. Driver
Verifier makes the same argument explicitly: it bugchecks on the hierarchy
violation, not when an actual deadlock is occurring. One refinement fell out
of implementing it. A grant whose window has closed constrains nothing, so
stale entries are skipped. Otherwise a detached coroutine, which carries its
launcher's grant snapshot for its whole life, would be treated as a holder
forever.

Behind the detectors sits a net. Both the in-task `sync()` fatal and the
circular-wait detector are models; they catch the shapes they were written
for. The waits-for graph in particular is blind to a cycle that passes
through a plain task-await edge. Suppose N holds an object and awaits
foreign task T, and T awaits that object. Neither suspension records an
edge, and both frames sleep forever. Rather than grow the model, the answer
is Go's `all goroutines are asleep` check, which models nothing. If every
worker is idle, every queue is empty, and nothing is completable from
outside the pool, then progress is impossible, whatever the shape. It is
O(1), needs no graph, and misses no deadlock class, including lost wakeups,
which are not cycles at all.

Three properties made it worth building rather than borrowing wholesale.
Quiescence stays scheduler-local, read through a plain function seam
(`detail::scheduler_quiescent`), so the task layer still does not know the
scheduler exists. Worker idleness is a busy-to-idle transition count rather
than "parked", because under a spin idle policy a worker never parks. The
interpretation costs nothing: there is no per-task counter and nothing on
the hot path, because the observer is a boundary waiter that is already
blocked. And the third clause is what Go lacks. Go's check has a documented
blind spot where any live background thread masks a partial deadlock,
precisely because a goroutine cannot declare that a wait is legitimate.
`ts::External_wait` is that declaration. Its failure mode is a false
positive, where a forgotten registration reports a correct program as
deadlocked, so the fatal names the escape in its own message. The window is
also measured continuously over seconds rather than sampled once, so a slow
but legitimate handoff between two of the program's own threads does not
trip it.

What the report says is the feature. A net that only announces "deadlock"
leaves the user where they started, so the report is layered by collection
cost. The blocked waiter is always reported. The live wait edges are
reported wherever that registry exists, which is free and covers everyone
suspended while holding a grant. Behind `TS_SUSPENSION_REGISTRY`, every live
suspension is reported, including those holding nothing, which is the
two-hop shape the waits-for graph is blind to by construction. When the last
tier is compiled out, the message names the rebuild flag, so the escalation
path is in the diagnostic rather than in a document.

The second tier is worth a note on why it is sound. Learned-order deadlock
predictors have a poor record. Linux reverted cross-release and never merged
DEPT, both on false positives, and this tier reads the same edges. The
difference is the direction of inference. These edges are printed only after
an independent mechanism has established that nothing can progress, so they
explain a conclusion rather than reaching one, and a post-mortem cannot
false-positive.

The third tier's default was measured rather than assumed, and the
assumption was wrong. Per-suspension bookkeeping was expected to be too
expensive for non-debug builds. It costs ~30 ns per suspension, is inside
noise on every real frame workload, and gets cheaper per suspension as
worker count rises rather than collapsing on a shared lock, because records
shard by their own address. The one place it shows, at ~8%, is a
microbenchmark where a single suspension ping-pongs between two threads and
nothing else happens. So it is on wherever the harness is on. The sharding
key is the load-bearing detail. A coroutine resumes wherever its awaited
work settled, so a record is linked by one thread and unlinked by another.
Keying shards on the suspending thread would have put both halves of every
suspension on one mutex, which is precisely what the first measurement
showed.

Hoisting the guard check had a second effect worth recording: it forced the
reentrancy exemption to be stated. A reentrant same-object access never
suspends, so it never reached the old check, and the exemption existed only
as an accident of where the check sat. Now it is a predicate. The awaited
access was lent every object it declares, so it took no pipe turn and ran
inline under the caller's grants. The narrowness is spelled out too: a read
access under a read guard is not exempt, because it does reach the pipe and
can queue behind a waiting writer that our own read hold is blocking.

### 4.5 Two real races, and the method that caught them

The since-deleted reuse and retraction corner produced the two hardest bugs
in the project's history, both instructive enough to keep on record:

- The token rewrite race. Relaunching a reused task rewrote its cancellation
  token while a prior round's worker could still read it. It was fixed by
  making the token immutable per task, set once before first launch, which
  removes the racing write rather than synchronizing an edge nobody could
  pin.
- The premature-dispatch generation TOCTOU. The dispatch path read the
  task's reuse generation after the atomic decrement that granted dispatch
  rights. A releaser preempted between those two instructions could wake
  after a full retract/reset/relaunch cycle and stamp its dispatch with the
  next generation, which then ran a round whose prerequisites were not met
  and returned the previous round's result. It was fixed by capturing the
  generation before the granting RMW and publishing it monotonically.

The second bug had survived four independent happens-before analyses, all of
which "proved" the code correct, and reproduced at 28% per iteration on a
plain optimized build pinned to two cores, despite having been seen only
under TSan and initially dismissed as tooling noise. Three transferable
lessons are now house rules. Capture state before the RMW that grants you
rights over it, since reading after is a TOCTOU; the generation capture in
`release()` still follows this rule today. A bug that only reproduces under
TSan is not necessarily a TSan artifact; pin to two cores to amplify
preemption windows. And capture-and-continue beats theorem-proving; the bug
fell to an event-ring instrumented build in one session. The racing
machinery and the event ring were both deleted with retraction. The lessons
were not.

### 4.6 Exceptions: agnostic library, fatal boundary

The library uses no exceptions. Failures are `ts::fatal`, cancellation is a
value, and results come back through `sync()` or `co_await`. That is a
property of the source, not of a compiler flag, so the question left over is
what the build should require, and the answer that matters to a consumer is
nothing. Macrame compiles with or without exception support, and a program
embedding it picks for itself.

Getting there costs one rule, because a user body is library-adjacent code
running inside library frames. A worker's dispatch loop, a pipe release, and
a coroutine resume all hold state (grants, lock counts, refcounts, a pipe's
FIFO position) that unwinding would leave half updated, and in an
exceptions-off build those frames have no unwind paths at all. There are
three ways to answer that.

1. Propagate, in the `std::future` style: capture into an `exception_ptr`
   and rethrow at the consume point. Every grant release, link advance, and
   counter transition becomes an exception-safety obligation, and a throwing
   graph node needs a defined meaning that interacts with cancellation. It
   buys a feature the library's own error model does not use.
2. Do nothing, which is what an exceptions-off library linked against an
   exceptions-on consumer did: the throw unwound into `macrame.lib` frames
   compiled with no landing pads. The measured result on MSVC was a process
   fail-fast (`0xC0000409`) with no message and no stack trace.
3. Report and abort at the seam. Every path that invokes a user body does
   so through one place (`detail::invoke_user_body`, plus the coroutine
   promise's `unhandled_exception` for the frame arm), which catches,
   reports, and aborts.

The library takes (3), for the same reason the rest of it is fatal by
design: the condition is a program error, and the diagnostic is worth more
than a recovery path nobody could use correctly. The seams live in headers,
so they compile with the consumer's exception setting. The handler exists
exactly where there is something to catch, and compiles to the bare
invocation where there is not.

What the report has to carry follows from where it runs. A handler runs
after unwinding, so `ts::fatal`'s stack trace starts at the seam and the
throw site is already gone. The two things that locate the fault are the
running task's identity, which the library already tracks for every other
diagnostic (`Named`, or the call site that created the task), and the
exception's own `what()` where it derives from `std::exception`. Both go in
the message, which is why the seam's handler delegates to a function in the
library (`escaped_exception_diagnose`) rather than formatting inline in a
header.

The seam brackets the body's whole contribution, not just the call. A
result-bearing body returns a value that has to be moved into the task's
storage, and that move is the body's type's code; a move that is really an
allocating copy can throw from it. So the emplace happens inside the seam
alongside the call. Constructing the result outside would have reopened the
hole for exactly the types whose moves are not free. The one thing left
outside is the body's destructor, which is `noexcept` unless a type opts
out, and a type that opts out has already left the standard library's own
contract.

What remains genuinely whole-program is MSVC's `_HAS_EXCEPTIONS=0`. It
rewrites standard-library declarations, so it cannot be a private choice of
the library build. It is therefore tied to one option
(`MACRAME_NO_EXCEPTIONS`), exported as a usage requirement when that option
is on, and covered by the same `detect_mismatch` link tripwire that guards
`TS_SAFETY_CHECKS`. Elsewhere no tripwire is warranted. libstdc++ and libc++
support linking `-fno-exceptions` and `-fexceptions` objects provided
nothing propagates across the boundary, which is exactly what the seams
guarantee.

---

## 5. Coroutines: the composition model

Coroutine support began as an additive layer and was then made the
foundation (the coroutine-first transformation, `docs/internals/coroutine-first.md`).
Composition is coroutines, and the callback vocabulary is gone (§4.3). The
main design points follow.

- Fused frame and block. The promise derives from the `Task_control_block`
  (the `Executable` pattern, where recovery is a standard derived cast), so
  a coroutine task is one allocation and is an ordinary `Task<R>` to
  everyone else. The frame holds a running self-reference. Awaiters,
  handles, and any gated completion it awaits, such as a nested graph run,
  hold refs, so a fire-and-forget frame lives exactly until settled.
- Eager start, blue boundary. A coroutine task runs to its first genuine
  suspension on the calling thread, matching the launch model everywhere
  else; there are no cold tasks. It suspends without holding a worker, and
  it is consumed by `co_await` from tasks or `sync()` from blue threads only
  (§4.4).
- Access grants across suspension. The harness's grant set is thread-local,
  and a coroutine migrates threads at every suspension. The model is that a
  coroutine is a chain of task segments, and each resumed segment
  re-installs the coroutine's grant snapshot and task identity. This reuses
  the grant-inheritance mechanism verbatim, with no new concept. A coroutine
  graph node holds its declared grants until the frame completes. Suspension
  does not release grants, and body return is not completion.
- The pipe is already an async reader/writer lock, so
  `co_await ts::read_write(obj)` yields an RAII guard with direct object
  access. This is the shape of `folly::coro::Mutex::co_scoped_lock` on top
  of machinery that existed anyway. The canonical coroutine hazard,
  suspending while holding a lock, is detected: a `co_await` under a live
  guard is fatal.
- The suspended deadlock is detected too. Two frames that each hold a grant
  and await the other's object deadlock with no thread parked, which is the
  failure mode a blocked-thread diagnostic can never see. Under safety
  checks every suspension on a pipe records wait edges, from the held grants
  to the awaited pipe, in a global registry, cleared at resume and
  cycle-checked on insert. The closing edge faults, naming both tasks and
  both objects. This is what makes awaited dynamic cross-object access a
  sanctioned residual pattern rather than a documented hazard.
- Cancellation is value-based because the library does not use exceptions.
  A cancelled await resumes with cancelled state to inspect, never a throw;
  for a value task that state is fatal, so check first. This was forced by
  the no-exceptions constraint and turned out cleaner, since control flow
  stays visible.

Resumes run through a bounded trampoline; deep cascades resume iteratively,
proven to 50k depth. Destruction runs through another, so a deep chain of
fused frames releases iteratively. Both retain capacity, so there is no
steady-state allocation.

### 5.1 Nested graph runs: the lend protocol

Coroutine nodes make `co_await inner.execute()` expressible, and composing a
frame out of pre-compiled sub-graphs is the obvious use. The obvious
implementation deadlocks. The inner node takes its own turn on an object its
caller is currently holding, and the caller cannot release it because it is
suspended waiting for the inner run.

The fix is to notice that an awaited inner run is strictly contained in the
caller's grant window, so the caller's exclusion is already exactly what the
inner run needs. At the nested `execute()` the library intersects the inner
graph's compiled access set with the caller's `Access_context`. Every
overlap whose held mode covers the inner mode is lent for that run, and the
inner nodes do not take turns on it. Recursion needs no extra rule. A
grand-inner graph intersects against its own caller's context, which already
carries the lent entries.

Three properties make this cheap:

- Binding is the mechanism. Node pipe links live in a slab built at
  `compile()` time. Lending re-binds the surviving links compactly and
  shortens each node's link count. An unbound link is a turn that is never
  taken, so there is no bypass flag threaded through the pipe and no second
  code path in admission.
- Ordering is untouched. The lend removes the outer world's exclusion, never
  the compiled conflict edges, so inner nodes still sequence against each
  other on a lent object exactly as they would standalone.
- The pipe never learns about it. External `async`s queue behind the
  caller's hold as before, so nothing outside the nesting can tell the
  difference.

The rest is refusing the cases where the containment argument fails. A
caller holding read where the inner graph writes cannot lend, since
upgrading would re-acquire behind its own hold. That case is fatal, with the
two restructurings named in the message. An earlier un-awaited nested run of
the caller runs under the same grant and could race the lent-to graph, so
lending requires a quiet scope. That case is also fatal; `co_await` the
previous nested run first. Already-settled runs are filtered out, since only
live ones are a hazard. And a detached run (`{.detach = true}`) is not
contained in the caller's window, so it structurally receives no lend and
queues like any external work. Un-awaited runs otherwise join the caller's
scope, which makes firing an inner run and forgetting it safe by
construction rather than by discipline.

One limitation falls out. A graph runs one execution at a time, so a
sub-graph shared between two concurrently running parents collides. That was
previously an unguarded corruption. It is now a fatal naming the fix, an
instance per caller, and the general relaxation to queued or pipelined runs
is roadmap work rather than new machinery.

### 5.2 Rule policy: per-rule opt-out and compile-out

The waiting rules are enforced by checks that abort. A user can uphold a
rule by means the library cannot see, so a check can be a false positive,
and a shipping build should be able to decide what it keeps paying for. Both
are properties of a check, so they are declared per rule rather than left to
whoever writes one. A bit in `TS_ENABLED_RULES` compiles a check out, state
included, and for the rules that permit it, `ts::Relaxed_scope` is a scoped
runtime opt-out.

The interesting result is that the runtime opt-out cannot be uniform.
Relaxing the guard-across-suspension rule does not merely accept a program
the library disapproves of. An `Access_guard` installs its own
`Access_context` as the thread's current grant, so a frame that suspended
under one would resume with the promise's snapshot installed over it and
then restore a `current_access` pointer captured on a different thread. The
rule protects an implementation invariant, and no claim a user could make
would make relaxing it sound. That splits the rules into three classes:

- Advisory rules guard a hazard the caller may know is absent. They allow
  the scoped opt-out as well as compile-out.
- Structural rules guard an implementation invariant. They are compile-out
  only, and the escape is the sanctioned form.
- The net is the global quiescence check. It has no call site to scope, and
  its escape is registering the legitimate external wait instead.

A relaxation propagates with the ambient task state, not the thread. It is
carried across a coroutine's segments by its promise and captured into
launched sub-work alongside the grant snapshot. That is wider than lexical
scope, and deliberately so, since the child inherits the grant and with it
the hazard the claim is about. The full taxonomy, the defaults, and the
obligations a new check inherits are in
[waiting-rule-policy.md](internals/waiting-rule-policy.md).

### 5.3 Thread-local access through out-of-line accessors

Every thread-local the library owns is a private member of a class whose
only interface is out-of-line value accessors
(`include/ts/detail/thread_local.h`). The failure mode this defends against
is one where the value is installed correctly and read from the wrong place.
A coroutine frame that holds state across a suspension invites the compiler
to resolve a thread-local's block address once and reuse that address after
the frame has resumed on another worker; the loop-invariant hoist above a
suspension point is the shape that reproduces it. The carriage machinery is
not at fault in that picture. The segment swap put the right value in the
resuming thread's slot, and the generated code read a slot belonging to the
thread that suspended.

Two of the library's own safety thread-locals were live instances, each an
asymmetry between one side inlined into the frame and one side resolving
freshly. `current_access` produced a false access violation against a
program that held its grant correctly, and the live `Access_guard` count
produced a false `await_under_guard` fatal. (That count has since stopped
being thread-local. It is a property of the task, so it lives on the
coroutine promise.) The second is the sharper argument for a structural
defence, since `await_under_guard` is the one rule a Shipping build keeps
and a misread counter breaks it in both directions.

The defence is structural rather than conventional because a convention has
to be re-argued at every new toucher and lapses silently when it is not.
Making the slot private and the accessors its only interface means a future
toucher inherits the property without knowing the story, because naming the
variable does not compile. That earned itself immediately. The sweep that
introduced the barrier found raw touches still bypassing the informal
convention in the access installer that runs for every task and node body.
Two constraints keep it honest. Accessors pass by value, since a `T&`
accessor hands the caching hazard back to the caller one level up. State
that cannot be passed by value (the resume, inline-dispatch, and destroy
queues) exposes whole operations instead, so the address never leaves the
accessor's frame. Rust gives thread-locals this shape by construction, with
`thread_local!` plus a scoped `.with()` and no way to obtain a raw
reference.

The cost is a call where there was a load, on paths including the hottest
one. Measured 2026-08 by interleaved A/B on `current_task`, the game-frame
benchmark moved from 12569.5 to 12572.1 ns/op (0.02%) against 5-8%
run-to-run swings on the controls, which is below the noise floor of the
measurement. The one public consequence is that `ts::current_worker_index()`
is a function rather than a variable.

The bug-class writeup, covering the upstream issue trail, which compilers
were observed doing what, and why observing a compiler recompute is not
immunity, lives at the top of `include/ts/detail/thread_local.h`. The hazard
as users meet it, including a canary test for their own build, is covered in
[guide.md](guide.md) §8.4.

---

## 6. Deferred and versioned state

The pipe's FIFO answer degrades in the many-small-writers case. Every write
serializes against every reader, and readers observe a timing-dependent
prefix of the write stream. The staged-write layer answers this. Its design
study is [command-buffer-design.md](internals/command-buffer-design.md) and its
operational contracts are in
[deferred-versioned-state.md](internals/deferred-versioned-state.md).

The essence is that a command buffer is a recording front-end terminating in
an ordinary `Guarded` write. `Deferred<T>` mints per-producer recorders.
Staging a closure touches only the producer's private journal slot, taking
no grant and contending with no readers, and one `commit` applies the whole
batch under a single write access. Readers see none of a batch before the
commit and all of it after, which gives stable snapshots instead of racy
prefixes. The pipe remains the only arbitration mechanism, and the harness
is untouched.

`commit()` is one auto-dispatching verb. The pipe carries an always-on
`writer_owner`, the task currently holding its write grant, so a `commit()`
called by the holder (a graph node's declared write, an `async` write body)
applies inline under the grant it already has, while any other caller's
commit becomes one enqueued write. The alternative of two spellings, an
under-grant `commit()` and an acquiring `commit_async()`, pushed a
scheduling decision onto the user that the pipe can answer itself, and made
the common case of calling it from wherever the frame logic sits
error-prone, since the wrong spelling either double-acquires or deadlocks.
Ownership is behavior-bearing state, so it lives outside `TS_SAFETY_CHECKS`,
while the diagnostic-only grant machinery stays gated.

The rejected alternative here was a "lazy `Guarded`", a mode where `async`
writes queue but do not execute until a flush node runs. It fails on its own
FIFO. Parked writes at the queue's front either block every later reader
until the flush, which is semantically clean but gives zero reader
parallelism, or readers must overtake queued writes, which is the
value-changing relaxation of §3 reappearing as an instance mode. Per-command
cost is a full task block instead of a journal append, and recording is
undeclared, so no graph edges can be derived from it. UE's own architecture
agrees from the other side: immediate and deferred RHI command lists are two
types, not a mode flag on one.

`Versioned<T>` extends staging with two replicas behind one guarded front.
Readers hold a stable published version all frame while the next is staged,
and `publish()` flips atomically. The load-bearing choices:

- The swap exchanges the replicas' contents, so the front's address never
  changes. Graph declarations, the pipe, and the harness need zero changes.
- A publish is three phases, and only the middle one, a content swap taking
  nanoseconds, holds the write grant. Applying the batch and resyncing the
  second replica overlap readers. The resync runs as a read job on the
  front's pipe, which makes the pipe itself the shadow-ownership chain. FIFO
  holds the next writer behind the resync with no extra synchronization.
- Resync is by replay. Both replicas are identical at every publish start,
  so re-applying the same batch to the second replica reproduces the new
  version bit-exactly, at a cost proportional to the delta rather than to
  `sizeof(T)`, provided commands are deterministic. That requirement is
  explicit, and an opt-in divergence check, a bitwise replica compare after
  replay, turns a violation into an immediate fatal rather than a drifting
  heisenbug.
- There are no read-your-writes; a version's outputs arrive as the next
  version. This is the cost of stable snapshots, stated as a contract.
- Ordering is FIFO within a recorder, and that order is semantic. Across
  recorders it is arbitrary but reproducible, thanks to a fixed drain order,
  and building semantics on it is declared a bug. `Parallel_recorder`, which
  provides per-worker slots for staging from inside a `parallel_for`, is the
  explicit, localized surrender of cross-thread reproducibility.

The UE research grounded several choices. `ENQUEUE_RENDER_COMMAND`'s
linear-allocated coarse queue and its splice-in-submit-order parallel
recording, where determinism comes from splice position rather than thread
timing, shaped the journal's slot-order contract. `FRHICommandList`'s POD
commands from a per-list linear allocator is the planned typed-command/arena
tier. The Render Dependency Graph, with passes declaring resource access and
order derived, is production validation of this library's central premise,
applied to GPU resources.

---

## 7. Rejected alternatives

This is the record of things analyzed and not done, with the reasons. Each
was a real candidate, and several were prototyped.

#### 7.1 Lazy `Guarded` (deferred writes as an instance mode)

Rejected for four reasons. The FIFO forks badly: blocking readers gives no
parallelism, and letting readers pass writes changes observed values. Each
command costs a full task block. Recording is undeclared, so no edges can be
derived from it. And it makes one type carry mode-dependent semantics. The
idea survives as the recording front-end design (§6).

#### 7.2 `Guarded_pool<T>` / per-component-type access (the ECS shape)

Rejected. With coarse subsystem wrapping the graph is already small and the
subsystem already owns its arrays contiguously, so the two ECS benefits
arrive without a new primitive. Dynamic `parallel_for` chunking cannot use
per-slice grants anyway, since chunks are claimed at runtime.
Slices-as-grants returns only as a possible future harness refinement for
handed-out sub-object references.

#### 7.3 Reader/writer pipe policy knobs (`writer_priority`, `batch_readers`)

Rejected as defaults or "throughput options". In a submission-ordered queue
they reorder reads relative to writes and therefore change results. A lock
can bias its acquisition race; a queue cannot bias away its own order.
Staleness-tolerant reads may return someday as an explicit per-read opt-in.

#### 7.4 Priority inheritance by reordering

Priority inversion through a pipe, where a high-priority access is queued
behind low-priority work, is real, but fixing it by reordering would break
the FIFO contract. The designed fix, shelved until a real workload needs it,
is inheritance: dispatch the pipe head at the maximum priority queued behind
it, and in the static graph run a one-pass reverse-topological
effective-priority computation at compile time.

#### 7.5 Mapping task priority to OS thread priority

Rejected. A syscall costs ~1 µs against a ~200 ns dispatch, it is
permission-gated on Linux, and it is the wrong tool. OS priority arbitrates
against unrelated processes, while task priority orders work within the
pool. Every surveyed native system except Apple's GCD, whose kernel is built
for per-work-item QoS, keeps priorities in userspace queues. A fixed
low-priority pool attribute for background work is the legitimate future
use.

#### 7.6 `std::atomic_thread_fence` on visibility edges

A house rule. ThreadSanitizer does not model fences, so a fence-based edge
makes the project's primary race oracle blind. Every publish/consume edge
uses variable release/acquire (or RMWs) instead, including one case where
the Chase-Lev deque's textbook fences were converted to prove a suspected
false positive. The suspicion was wrong; the racy code was genuinely racy
(see §4.5). Verifiability outranks micro-optimization of fence placement.

#### 7.7 Wholesale `std::execution` (senders) adoption

Rejected. Senders' compile-time operation-state composition contradicts the
monomorphic-block decision (§4.1), it does not help fire-and-forget work,
and P2300 is still settling. Two ideas are kept on the shelf:
`Task`/`Scheduler` as sender/scheduler adapters for interop, and carrying
the access context through receiver environments instead of thread-locals,
which could open compile-time access checking.

#### 7.8 `Access::append` edge derivation for staging

Analyzed while building `Deferred`. Recording needs no grant at all, so
adding an access mode for it would grow the mode lattice solely to derive
producer-to-commit edges that are cheap to declare by hand with `.after`.
Deferred until the completeness hazard actually bites in this position.

#### 7.9 Compile-time access proof (the Rust/Pony direction)

A type-system guarantee through borrow checking or reference capabilities is
the strongest form of this library's property, and it requires a language,
or a template regime that rejects ordinary C++ patterns and explodes compile
times. The runtime harness at ~1 ns is the deliberate trade: a weaker
guarantee with zero model constraint. Senders' typed environments (§7.7) are
the one path back toward compile-time checking worth watching.

#### 7.9a `TS_ENSURE` (the UE-`ensure`-shaped recoverable assert)

`ts::fatal` is the right tool for corruption, but some hazards deserve a
loud diagnostic without killing the process. The macro evaluates its
expression once in every configuration and yields the bool, so
`if (!TS_ENSURE(x, "..."))` recovers naturally, matching UE semantics. On
failure it counts every occurrence but reports once per call site (a
captureless-lambda function-local static is the site claim), so a per-frame
recurrence is one stack trace in the log and an exact number in the counter.
Presentation goes through a swappable handler (`ts::set_ensure_handler`, in
the `std::set_terminate` shape). The default prints `ENSURE FAILED:` plus a
stack trace and breaks into the debugger when one is attached, using the
C++26 `std::is_debugger_present`/`std::breakpoint` pair, polyfilled:
`IsDebuggerPresent`/`__debugbreak` on Windows, `TracerPid` plus `int3`/`brk`
on Linux, and sysctl `P_TRACED` on macOS, slated for the platform layer.
`ts::fatal` uses the same break before `abort`, stopping at the failure
site. Counting and once-filtering live outside the handler, so a host's
custom presentation (its own attach dialog, say; the library deliberately
ships none) can never hide a failure. The test harness fails on failures no
test explicitly consumed, and the bench/stress drivers fail their exit code
on any, so a tripped ensure cannot pass CI by virtue of the program having
survived it.

The mechanism's first user was the blocking-sync warning. Coroutine-first
later promoted it to fatal, since an in-task `sync()` is a bug by
definition, not a hazard. The check sits at the one chokepoint every
blocking wait passes through, `sync_wait`, and fires whenever a task calls
it, settled target or not, because the rule is the hazard, not the wait's
timing (§4.4). It separates two shapes: the certain-deadlock case, where the
target is an access to an object the waiting task already holds (matched by
comparing the target pipe's epoch address against the context's captured
epoch sources), and the general never-block violation. `parallel_for` joins
through its own counter and is exempt.

#### 7.10 Requiring copyable `T` / `.copy()` accessors

`folly::Synchronized` offers lock-and-copy-out. Here the guarded objects are
whole subsystems, where copying is exactly what must never happen
implicitly. `Guarded<T>` imposes no copyability; snapshot semantics are
provided structurally by `Versioned<T>` instead.

---

## 8. Verification approach

The verification approach is worth documenting as design, because several
decisions above exist for it.

- TSan is the primary oracle. It runs on Linux/Clang against the same
  sources (the Windows-specific harness is isolated), and every concurrency
  change gates on it. This is why fences are banned (§7.6) and why the
  advisory-counter/authoritative-epoch split exists in the handoff policy.
- Death tests cover every fatal path via subprocess scenarios. Safety checks
  are tested, not assumed.
- Determinism serves as a test. Both engine samples run twice and compare
  results bitwise, which is what caught a replica-divergence bug on day one
  of `Versioned` and motivated the built-in divergence check.
- Instrumentation is built on demand. The event-ring harness that found the
  TOCTOU (§4.5) was purpose-built, kept until the machinery it instrumented
  was deleted, then deleted with it. The deterministic allocation profiler
  remains in-tree.
- The oracles have a platform reality. ThreadSanitizer has no Windows
  runtime, and most game development happens on Windows. TSan therefore
  verifies this library's concurrency machinery (portable sources, Linux
  CI), while a Windows-hosted game gets ASan, stress loops, and chiefly the
  access harness, which is the only race-adjacent oracle available on the
  dominant platform. That asymmetry is an argument for keeping the harness
  cheap enough to leave enabled. At ~1 ns per check, shipping with
  `TS_SAFETY_CHECKS` on may be viable for many titles, unlike the
  editor-only or development-only enforcement every surveyed system
  defaults to (Unity, RDG, Chromium). This is not offered as a blanket
  recommendation. The per-call cost is fixed but the per-frame cost is
  workload-shaped. A check on a hot trivial getter called millions of times
  a frame is a real tax, and no benchmark can characterize every program's
  method mix, so leaving it on stays a per-title measured decision rather
  than a default. The design point is only that the option exists at all,
  which the editor-only competitors cannot offer.
- Both oracles are needed, for precise reasons. The two are not the same
  check from two angles; they have inverted profiles and verify adjacent
  layers. TSan is sound but incomplete. Given the synchronization it
  observes, a reported race is a real happens-before violation, and the
  verdict is order-independent: it catches a race on runs where the timing
  happened to work, needing only the unordered pair to execute, not the
  corrupting order. But it sees only interleavings actually sampled, so
  coverage is schedule-dependent, and it verifies the implementation under
  the contract (queue, pipe, refcounts, lock-counter, coroutine
  handshakes). The access harness is the dual, complete but narrow. Any
  undeclared touch of a `Guarded` is caught on the first run of that path,
  deterministically and with no concurrency required; it catches the latent
  race the completeness hazard warns of, before parallelization realizes
  it. But the property is only declared-access completeness, only over
  `Guarded` objects, and it validates possession of a grant, not ordering
  against the writer that produced a value. Neither subsumes the other. A
  sound-but-incomplete dynamic detector plus a complete-but-narrow
  always-on invariant is the verification strategy, not a redundancy to
  collapse. Extending the harness from possession-checking toward
  ordering-checking is the grant-generation direction (TODO 1.11/1.13, the
  Unity version-stamped-handle idea).

---

## Appendices / references

Project documents:

- [task-internals.md](internals/task-internals.md): the dynamic-task design of
  record, covering the control block, lifecycle, lock-counter, coroutine
  frames, and completion gating.
- [task-systems-comparison.md](task-systems-comparison.md): the survey of
  UE Tasks, TBB, Taskflow, HPX, folly, Marl, Rayon/Tokio, Go, GCD, and the
  scheduler literature.
- [command-buffer-design.md](internals/command-buffer-design.md): the staged-write
  design study, including the UE rendering research and the full
  lazy-`Guarded` analysis.
- [deferred-versioned-state.md](internals/deferred-versioned-state.md):
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
