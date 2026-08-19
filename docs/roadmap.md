<!-- Public-facing roadmap, synthesised from docs/TODO.md (the internal working list) and docs/pattern-farming.md (the in-progress pattern review). Those two files remain the detailed working records; this document is the curated, thematic view. -->

# Roadmap

Where Macrame is going - a high-level, thematic view for users and contributors
deciding whether the library fits their plans and where they might help. The
internal detail, cross-references and rejected alternatives live in
[docs/TODO.md](TODO.md) and the pattern review [docs/pattern-farming.md](pattern-farming.md).

**What already ships** is in [CHANGELOG.md](../CHANGELOG.md) and the
[user guide](guide.md), not repeated here. The work-stealing scheduler,
coroutine-first composition, the per-object reader/writer pipe, `Guarded` access
control (with the zero-allocation attended `Access_op` path) and its runtime
harness, static task graphs, `Deferred`/`Versioned`,
`Event_bus`, `parallel_for` (including colouring), cooperative cancellation (with
non-blocking and optional-on-cancel result accessors), and the deadlock/waits-for
detectors are **done** and are not roadmap items.

**Stability.** The library goes public as **v0.1.0**: the API is stable in shape
but not frozen, and the areas below are where it will still move before 1.0. The
core safety model - declared access, derived schedules, the harness - is settled;
most remaining work is performance, breadth, new composition vocabulary and
diagnostics rather than redesign.

**Horizons** are words, not dates: *near-term* (targeted around 1.0), *mid-term*
(wanted, not scheduled), *exploratory* (research or demand-gated - may never ship
in the form sketched). Items drawn from the pattern review are candidate designs,
not commitments; that review is itself unfinished.

---

## 1. Performance and allocation

The largest active campaign. The safety model is paid for with allocations and
dispatch overhead that are understood and being driven down; the goal is that
using Macrame at the coarse level it targets costs close to hand-written code.

- **Near-term** - driving an uncontended `access` towards the cost of a direct
  call. The caller-owned zero-allocation attended path (`Access_op`) has shipped;
  the residual is shaving the fixed overhead off the inline fast path.
- **Near-term** - tail-chaining a pipe turn onto the releasing worker instead of
  waking a parked one, closing a measured gap where a hot single-object chain pays
  a wake per hand-off.
- **Mid-term** - per-type recycling free-lists for control blocks, a tunable
  small-buffer callable to replace `std::move_only_function` on hot paths, and an
  opt-in scoped bump arena (automatic for a `parallel_for` or a graph run).
- **Mid-term** - shrinking the task control block further (flag packing, a futex
  wait primitive to drop the per-block mutex/condvar), folding the last edge
  vector into intrusive dependent-side storage, and a measured cache-line /
  false-sharing audit across every hot shared structure.
- Multi-object `access` still schedules like `async`; its inline fast path is a
  near-term follow-up so single- and multi-object attended access match.

## 2. Scheduler and threading model

One process-wide, reconfigurable work-stealing pool is the model, and it stays.
Remaining work is filling gaps around it rather than adding pool varieties.

- **Near-term** - diagnose the throughput collapse observed when workers are
  configured well above core count (every producer submit pays a wake); relevant
  to anyone oversubscribing deliberately.
- **Mid-term** - a platform abstraction layer (roughly six functions: park/unpark
  with timeout, thread spawn/name/affinity, CPU count and topology, a relax
  primitive). It unblocks several items below and is the gate for real portability.
- **Mid-term** - run-a-functor-on-every-worker (per-thread init/flush/warm-up),
  timed/delayed tasks, promoting the high/low priority bands onto per-worker
  deques, and a scheduler-aware offload verb for calling a genuinely blocking
  third-party API from a task without occupying a worker (shunt to a scratch
  thread, suspend, resume via a `Signal`).
- **Mid-term** - a live, stackable global concurrency throttle (clamp active
  worker count for a scoped region without tearing the pool down), and per-frame
  time-budget scheduling for background categories (streaming, GC, navmesh rebuild)
  that spreads a large job across frames to hold a stable frame time.
- **Mid-term** - a minimal task-to-worker affinity escape hatch for *irreducible*
  pinning only (single-thread graphics APIs, OS main-thread requirements), inside
  the one pool. Named threads remain the anti-pattern the access-derived graph
  exists to replace, so this stays an escape hatch, off by default.
- **Exploratory** - a small number (2-3) of interchangeable scheduler
  implementations behind the unchanged task API, the motivating one being a
  background pool on OS-low-priority threads for genuinely non-interfering
  gap-filling; plus standby/oversubscription workers for opaque blocking a task
  system cannot suspend through.

## 3. Concurrency safety and diagnostics

Strengthening the runtime harness is a standing priority - it is the library's
signature property, and its coverage gaps are where comparable systems'
real-world failures concentrated. Much has landed (waits-for cycle detection, the
quiescence deadlock net, declared ranks, the waiting-rule policy); what remains
is closing coverage holes.

- **Near-term** - a check for the caller that awaits a detached graph run while
  holding one of its objects, and for the caller that touches an object lent to an
  unsettled nested run - both currently rely on documented discipline.
- **Mid-term** - access-check *completeness*: a clang-tidy check that every public
  method of a guarded type opens with the access macro, plus a value wrapper so
  POD/method-less types are covered rather than silently invisible.
- **Mid-term** - a sub-object / range harness so a grant can cover part of an
  object or a declared region, and an investigation (then sample or hazard doc) of
  nested `Guarded` - a guarded object whose member is itself guarded.
- **Fixing** - a known lifetime gap where a coroutine frame's by-value parameters
  or cross-suspension resources outlive the task's settle (the functor-task version
  of this is already fixed).
- **Exploratory** - the mitigation menu for whole-object granularity (sharding,
  `Versioned` placement, sub-object grants); thin macros mirroring the declared
  ranks onto Clang's thread-safety attributes for a compile-time signal on your own
  named state; and a "no detached launch alive at shutdown" leak assert. A checked
  accessor wrapper and page-protection/ASan-poisoning schemes were evaluated and
  shelved; escaped-reference coverage stays ThreadSanitizer's job.

## 4. Graph scheduling and profiler-guided optimisation

The static graph already derives schedules, profiles itself, and measures its
critical path. The next steps turn that measurement into automatic optimisation
and add the compile-phase analyses render graphs run - the strongest concrete
answer to "what does a materialised graph buy that online scheduling cannot".

- **Mid-term** - profiler-guided optimisation: reorder and re-bucket from measured
  durations. A conflict edge is a disjunctive arc the graph sees up front, so it
  can compute upward ranks and orient/dispatch to shorten the makespan, or *report*
  an edge's critical-path cost and suggest `Versioned`/deferral. Includes deriving
  a HEFT-style node priority from the criticality already measured in `--trace` and
  feeding it back into dispatch (with a runtime priority escalation on await for
  the anti-inversion case).
- **Mid-term** - change-version skip nodes: a node caches the write-versions of its
  read-set and skips its whole body next run if none advanced (whole systems no-op
  when their declared inputs are unchanged) - the version source already exists.
  Alongside it, automatic pass culling (drop nodes that reach no declared output)
  and an over-declaration / clobber-poison diagnostic.
- **Mid-term** - typed graph chaining so a node can consume a prerequisite node's
  result (nodes are void-only today); cooperative yield points inside long-running
  nodes; measured `compile()` and per-node dispatch costs with a node-fusion
  guideline; and automatic fusion of serial single-in/single-out chains.
- **Exploratory** - pipelined / overlapping runs and frame-fence straddling;
  conditional/branch nodes the tracer can still see; transient graph-scoped scratch
  resources with lifetime aliasing (and a transient-vs-imported classification);
  finer SSA / N-version resource handles; and a `derive = off` experiment trading
  derived-edge determinism for arrival-ordered conflicts. Most of these are gated on
  real usage data rather than toy-sample numbers.

## 5. Composition, cancellation and time

The best value-to-effort cluster in the pattern review: these ride the existing
awaiter handshake, cancellation callbacks and resume trampoline, and cover what a
frame loop most predictably wants around `co_await`. Two things are simply absent
today - a *first-of-N* and a notion of *time*.

- **Near-term** - a timer / delayed-dispatch primitive (a min-heap on one thread
  firing a `Signal`, zero-cost when unarmed). Nothing schedules against time today;
  it is foundational for everything below.
- **Near-term** - `when_any` / race with a loser policy (cancel / detach / keep),
  the universal spelling of speculative execution, hedging and first-responder; the
  samples already open-code it. The eager + cooperative-cancellation model makes it
  safe by construction, without the drop-mid-await data-loss footgun of poll-based
  systems.
- **Mid-term** - timeout / deadline combinators (both the all-or-nothing and the
  anytime-algorithm "return best-so-far" shapes), linked and child cancellation
  tokens carrying a cancel *reason*, `retry`/`repeat`/backoff, a non-cancellable
  `shield` section for commits that must finish, and a cooperative `yield`.
- **Mid-term** - a structured task-group scope: launch a heterogeneous set of
  *detached* children (no grant inheritance - the safe half of the removed
  `Task_scope`) and join them under a completion policy. Gated on a recoverable
  "failure" channel decision, since exceptions are disabled.
- **Mid-term** - an OS-completion-to-`Signal` bridge documented as the sanctioned
  async-I/O idiom, a value-carrying `Signal<T>`, and a countdown latch and
  auto-reset event as near-free `Signal` variants.
- **Exploratory** - the resume-locality problem: measurement shows most of a
  chained-stage cost is the awaited work *leaving the awaiting thread*. Keeping a
  soon-to-be-awaited task on its submitter's thread resembles the retraction
  coroutine-first deleted, so it needs a fresh, suspension-based design pass, not a
  patch.

## 6. Streaming and message flow

The single biggest *category* the survey found missing. Macrame models a single
value (`Task`), a latest value (`Versioned`) and a one-shot event (`Signal`), but
nothing models a value *sequence over time* with flow control - a strategic axis
rather than a 1.0 gap, built bottom-up.

- **Mid-term** - a `co_await`-native counting semaphore / in-flight limiter (cap N
  concurrent async operations independent of data dependencies), the smallest and
  most self-contained streaming piece and the backpressure knob under the rest.
- **Mid-term** - a bounded async channel with backpressure (both ends awaitable,
  capacity as the knob, drop policies), then async generators / streams over it,
  and a completion-order fan-in set (`as_completed` / `JoinSet`) to consume a
  growable task set as results land.
- **Mid-term** - a sequence-gated reorder buffer (parallel work emitted downstream
  in order with a no-gap gate) and a bounded-in-flight pipeline built from it - the
  assembly-line shape, and the principled fix for the "pipe FIFO is not a conflict
  edge" ordering trap.
- **Mid-term** - a reactive `Versioned::changed()` subscription (park a reader
  until the next publish rather than polling per frame) and a broadcast event bus
  with per-subscriber cursors and lag-as-data - the streaming upgrade of the shipped
  `Event_bus` and blackboard patterns.
- **Exploratory** - keyed / content-addressed joins across streams, and an
  actor/mailbox recipe (a task draining a live bounded inbox under a grant) that
  falls out once channels exist.

## 7. Data-parallel, staged-write and pattern primitives

`parallel_for` and `Deferred`/`Versioned` cover the common cases; the roadmap adds
the reduce/scan/sort family, richer fan-out control, a faster staged-write layer,
and the cross-entity primitives colouring alone cannot serve.

- **Mid-term** - `parallel_reduce` (deterministic by default via index-ordered
  combine), `parallel_scan`, `parallel_sort`, and a partitioner / grain-size knob
  for `parallel_for` (static range vs guided chunking, a range-body signature).
- **Mid-term** - the intra-system entity-interaction menu for item *i* reads/writes
  item *j*: a gather/apply mailbox, a commutative accumulator, a union-find island
  helper. Interaction colouring (the other half) already ships; its island-aware
  and chunk-DAG escalations remain open.
- **Mid-term** - the `Deferred`/`Versioned` performance chain: a per-journal bump
  arena, a lock-free `stage()`, a typed command tier (`Deferred<T, Cmd>`) past
  today's closure-per-command cost, and sort-key deterministic playback for the
  parallel recorder.
- **Mid-term** - reconsidering whether `parallel_for` should inherit the caller's
  (especially write) grant, and whether a diagnostic for overlapping writes is
  feasible - today data-race freedom rests entirely on the disjoint-partition
  contract.
- **Exploratory** - a research pass over the broad parallel-pattern catalogue to
  find the next `Versioned`/`Deferred`-shaped generalisation; a user-facing
  per-worker sharded object pool; and general task-local / context propagation
  across `co_await` boundaries.

## 8. Platform and portability

Macrame is developed on Windows (MSVC/clang-cl) and tested on Linux under
Clang/TSan. Broader reach is wanted but unclaimed until it is real.

- **Mid-term** - the platform abstraction layer (see theme 2) is the enabler; a
  portable ThreadSanitizer build story and a proper AddressSanitizer configuration
  ride with it.
- **Exploratory** - macOS, console (a fibre backend behind the same task API) and
  mobile (QoS over a fixed pool) are noted, not committed. No support claim will be
  made ahead of a tested implementation.

## 9. Tooling, benchmarks and CI

Verification is a first-class concern; the gaps are automation and depth of the
built-in profiling.

- **Near-term** - gate the Shipping configuration (`TS_SAFETY_CHECKS=0`) on every
  push, so a mis-gated safety field fails CI rather than the next release; and a
  benchmark regression baseline (store medians, flag regressions, assert
  allocation-free graph re-runs).
- **Mid-term** - deeper internal profiling: decompose framework overhead into named
  phases (acquire, completion, queue-lock wait) to price the next scheduler
  redesign before doing it; a serial-baseline trace lane giving the cleanest per-node
  cost model and a serial-vs-parallel delta that flags contention.
- **Mid-term** - documentation and samples: a layered-disclosure front page and
  scope statement, a known-limits section (what the harness does and does not
  catch), renderer-pattern samples (parallel command-list recording,
  RHI-stream-as-pipe), a `Signal` examples set, a nested/multiple-graphs sample, and
  a pass to lead users towards coroutines throughout.

## 10. Validation and adoption

- **Near-term / ongoing** - finding an open-source, frame-structured C++ workload
  to integrate against and validate the access-declared-parallelism approach on
  real code. This is the strongest available test of the design and the best source
  of API pressure, so it is prioritised over synthetic samples.

## 11. API stability

Small frictions surfaced during development are being resolved before they become
costly to change post-public: whether the `Access_options` and task-launch option
aggregates share a base or stay split, and a final terminology and naming sweep.
These are near-term and land with the API-stability review.

---

## Contributing and influence

Contributions of every size are welcome - see [CONTRIBUTING.md](../CONTRIBUTING.md).
The best levers for shaping any theme are a concrete workload that exercises it
(theme 10) and evidence: several exploratory items are gated on real usage data
rather than toy benchmarks, and the pattern-review items graduate only when a real
use case pulls them. After the public launch, specific items may be seeded as
GitHub issues for discussion and pick-up; until then, [docs/TODO.md](TODO.md) and
[docs/pattern-farming.md](pattern-farming.md) are the fullest picture.

### Theme-to-working-list map

For contributors who want the detail behind a theme. Area numbers refer to
[TODO.md](TODO.md); `PF` numbers refer to the pattern review in
[pattern-farming.md](pattern-farming.md) (which has its own numbering).

| Theme | TODO.md | pattern-farming.md |
|---|---|---|
| 1. Performance and allocation | 1.1, 1.2, 1.18, 1.19; 4 (all) | PF 4.3 |
| 2. Scheduler and threading model | 3.2-3.9 | PF 2.41, 2.43, 2.44 |
| 3. Concurrency safety and diagnostics | 1.4-1.7, 1.10, 1.16, 1.17, 2.19, 6.20 | PF 5.2 |
| 4. Graph scheduling and PGO | 2.1-2.6, 2.9-2.13, 2.18 | PF 2.23-2.34, 2.42 |
| 5. Composition, cancellation and time | 6.3, 6.6, 6.16, 6.17 | PF 2.1-2.9, 2.20-2.22, 2.45, 2.55 |
| 6. Streaming and message flow | - | PF 2.10-2.19 |
| 7. Data-parallel and staged-write patterns | 5.1, 5.4, 7.1, 7.2, 9.4 | PF 2.35-2.40, 2.46-2.53 |
| 8. Platform and portability | 3.6, 10.2 | PF 2.49 |
| 9. Tooling, benchmarks and CI | 10.1, 10.3-10.12, 10.15, 10.16 | - |
| 10. Validation and adoption | 9.5 | - |
| 11. API stability | 1.8; inconsistency sweep | - |
