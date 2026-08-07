# Profiler-guided optimization: findings and design abstract

**The canonical collection point for profiler-guided design and automatic perf
analysis.** Ideas, findings, and the closed-loop design live here; the
[TODO.md](TODO.md) items (2.4 profiler-guided optimization, 2.5 rank-shaped
dispatch, 2.11 yield points, 2.12 frame-boundary overlap) are the work-item
index and point back to this document for the design detail — add new design
thinking here, not scattered across the TODO. The measurement tooling
referenced throughout is `tools/graph_trace.h` (aggregated average-run trace:
per-node medians/variance, per-edge binding gaps, measured critical-path
frequency, dispatch waits, critical path dead time).

## The fixture

`game_frame` traced on two budgets per `--trace` run: the default scheduler and
a starved 4-worker one. The 4-worker frame is scheduling-bound: critical dead
time ~22.5% of makespan (vs 2.4% on 12 workers), dominated by one gap —
`economy` (critical in ~82% of runs) sat ready ~888 µs behind `networking`
while `audio`'s 1.5 ms serial body occupied a lane from t=0.

## Experiments

1. **Priority (refuted).** `audio` at `Priority::low`: no change to its
   placement or the gap (makespan −2.5%, within noise). Mechanism: priority
   orders the *queue*; audio is a root dispatched while a worker is free at
   t=0, and nothing evicts a runner. The gap is occupancy, not arbitration.
2. **Ordering (half-confirmed, net negative).** `audio.after(AI)`: the gap
   collapsed (888 → 34 µs; dead time 22.5% → 4.2%) — occupancy confirmed as
   the mechanism — but makespan **worsened 10%** (4363 → 4802 µs). `after` is
   completion-to-start: audio's whole 1.5 ms body moved onto the chain tail
   and became 100% critical; the frame turned dependency-bound on a longer
   chain. Reverted.

## Lessons

- **Dead time is the diagnostic; makespan is the objective.** A tuner
  minimizing dead time would have shipped experiment 2 as a win. Every
  proposal must be scored on makespan.
- **The lever shape is missing.** The tuning wants "audio yields dispatch
  precedence to the trio", not "audio starts after AI completes". Candidate
  primitives: yield points inside long bodies (TODO 2.11 — the only
  eviction-shaped lever), a start-after-start ordering, rank-shaped dispatch
  (TODO 2.5). A completion edge to a *shorter* predecessor (e.g.
  `audio.after(quests)`) may land between the two outcomes — untested.
- **The chain-extension guard is computable analytically.** Delaying an
  occupier of duration D past a gap predicts a new chain of at least
  `chain_through_delay_point + D`; for audio (D = 1.5 ms ≫ the 888 µs gap)
  this flags the regression *before* running it.
- **Optimal edge sets are worker-count-dependent** (the dual-run picture shows
  a green 12-worker frame and a red 4-worker one from the same graph); any
  tuner output is parameterized by budget.
- **Rank cannot come from structure alone — it needs measured durations.** A
  node's importance (upward rank = weighted longest path to the sink) is
  meaningless with structural weights: a chain of ten 10 µs nodes and one 3 ms
  node have identical node *count* but 30× different critical weight, and two
  equal-length-by-count paths can differ by any factor in time. Node count and
  topology give no usable rank; only measured (or estimated) durations do. This
  is why rank-shaped dispatch (2.5) and every proposal here are gated on the
  trace capture — which now exists (`tools/graph_trace.h`). Node priority
  (high/normal/low) is a coarse stand-in that encodes *intent*, not *cost*; the
  real weights are the per-node medians the trace already streams. (This also
  answered a live confusion — no, "rank by node count" is not a usable fallback;
  it is useless, and the whole point of the profiler feedback loop below is to
  supply the durations rank actually needs.)

## Tuner design (three tiers, least machinery first)

1. **Analysis + proposal — purely algorithmic, no model.** For each critical
   edge by descending mean binding gap: occupiers = nodes whose median bars
   overlap the gap window; movable iff slack > needed shift and not an
   ancestor of the successor; proposal = a cycle-safe delaying edge (edge
   *additions* never change semantics); predicted recovery =
   `min(gap, overlap)` minus the chain-extension guard above. Deliverable
   shape: `Graph_trace::suggest()` — a ranked report of candidate
   optimizations. **TODO (not yet built):** a standalone automatic-analysis
   pass that emits this list from a trace, so the numbers stop needing a human
   to interpret. Signals it consumes, beyond the per-edge gaps: the
   **critical-work vs critical-path (structural CP) gap** — measured binding
   work minus the CPM dependency floor — classifies the whole frame and routes
   the search (≈0: dependency-shaped, target the structural chain; ≫0: critical
   nodes inflated by contention, protect their resources; ≪0: a low-work chain
   straggling on waits, a scheduling problem). Core utilization and the
   dead-time headline colour then say whether recoverable idle even exists.
2. **Evaluation — reality, not simulation.** A 200-frame trace run costs ~1 s,
   so the loop is greedy hill-climbing with measurement as the objective:
   apply top proposal, re-trace, accept iff makespan improved, iterate
   (`--tune` driver mode). Sidesteps model fidelity entirely; would have
   auto-rejected experiment 2 in one iteration.
3. **Offline evaluator — only if the search grows combinatorial** (staleness
   annotations × orderings): list scheduling over the DAG with measured
   medians and k workers — the CPM math plus a worker count, ~50 lines,
   deterministic. Not a runtime replay (no stealing/pipes/inline dispatch);
   tier 2 remains the arbiter. Must reproduce measured timelines on the
   fixture before its proposals are trusted (TODO 2.4's gate).

Semantic scope: the tuner proposes only semantics-safe moves autonomously
(ordering-edge additions, rank weights, slice granularity); semantic
relaxations (staleness-tolerant `Versioned` reader placement) are proposed but
user-ratified, per TODO 2.4's taxonomy.

## Perf capture: the on-disk feedback channel

The loop needs a durable artifact linking a measured run back to the next
`compile()`. Two consumers hang off one file.

- **`perf capture`** — a well-defined on-disk file holding the aggregated trace
  a compile needs: per-node medians, per-edge binding gaps, criticality
  frequency, worker count. It can be a literal copy of an artifact we already
  emit (the trace SVG is self-describing — the numbers are in it), or a
  dedicated compact format; the analysis script writes it to a known path.
- **Offline analysis script** — takes a perf capture (the SVG, or any format we
  produce) and emits two things: (1) a human-readable **report of recommended
  structural changes** — extra ordering edges, node reordering, priority and
  `set_inline` hints — ranked by predicted makespan recovery (this is tier-1
  `suggest()`, just run as a script over a saved capture rather than inline);
  and (2) the **perf capture itself** — the raw input copied, or processed —
  written to the well-defined path for the compile to read. The structural
  recommendations stay *user-ratified* (semantic scope above); the script
  proposes, the human applies.
- **`graph.compile(dot_path = nullptr, perf_capture_path = nullptr)`** — an
  optional path to a perf capture. When present, `compile()` reads the measured
  durations and uses them as the rank weights for native dispatch shaping
  (2.5) — *automatic*, no graph edit, no user action. Without a capture it
  falls back to structural/priority rank (coarse, per the durations note in
  Lessons). This closes the loop: **trace → perf capture on disk → next
  `compile()` reads it → rank-ordered dispatch**, with the analysis script as
  the optional side branch that turns the same capture into human-facing
  structural advice.

The split matters: the *scheduling* half (rank weights → dispatch order) is
consumed automatically by `compile()` and changes nothing about the graph; the
*structural* half (edges, node order, staleness relaxations) is advice a human
ratifies. Same capture, two paths — mirroring the two optimization layers
(scheduling vs structural) kept distinct throughout this document.

## Task tracing: done and remaining

Done (2026-07): **true time-bucketed core utilization** — the scheduler
accumulates each armed task's busy span into per-worker time buckets, folded
into a per-bucket utilization the trace draws as the background wash. Every task
kind (slices, async, continuations) counts at its real time, so the background
is no longer a node-concurrency proxy that undercounted `parallel_for`.

Done (2026-07): **per-node true busy (owner attribution).** A node body sets a
`current_trace_owner` TLS (`detail::Trace_owner_scope`); launched sub-work
inherits it the way `Access_context` already propagates; `detail::Trace_busy_scope`
measures each such body while armed and attributes it to the node via a
scheduler-free bridge (so `task.h` stays scheduler-independent). Node tooltip:
`True busy (body+slices+async)` — a `parallel_for` node reads ≫ its bar (core
time across the fan-out vs the wall-time bar); scalar nodes ≈ body.

**A prerequisite fix, and a correction it forced:** owner attribution and true
utilization both depended on the fan-out running on the *traced* scheduler.
`parallel_for` had hardcoded `global_scheduler()`, so a node on the sample's
dedicated N-worker trace scheduler leaked its slices to the global default pool
— which meant the earlier "true utilization" and every variant frame-time in
the exercise were measured with the frame secretly spread across two pools (the
dedicated N **plus** up to a full default pool). Fixing `parallel_for` to fan
out on `current_scheduler` corrected this: honest single-pool numbers are
~2× the pre-fix ones (e.g. optimised frame time 3.1 → 6.8 ms on 6 workers, its
critical path 2.65 → 6.1 ms), and the frame is correctly critical-path-bound
(~46–47% average utilization, peaking in the parallel stretches). The
optimised-vs-baseline contrast survives (6.8 vs 7.6 ms). **The
[example-frame-optimization.md](example-frame-optimization.md) numbers predate
this fix and need a refresh.**

Done (2026-07): **task volume.** A per-worker task counter in `run_task` (armed
only, mirrors the busy counter), folded per run into a total + mean-per-run,
shown in the stats panel (`tasks: N total (~M/run)`). M is every task the
scheduler ran — nodes + `parallel_for` slices + async + continuations — so it
far exceeds the node count and exposes the real fan-out (game_frame: ~135/run
baseline, ~153/run optimised, over 30 nodes).

Done (2026-07): **task-system cost (% of frame on machinery).** Each worker's
frame time decomposes into **B** (user functor, minus its internal submits) / **M**
(machinery) / **I** (idle, in neither bucket). Headline **`M / (B + M)`** = overhead
per unit of useful compute, a third classifier on the SVG (bands `overhead_ok_share`
0.05 / `overhead_bad_share` 0.15); the stats line shows `body … µs / machinery … µs
per run`. Mechanism, all gated on `TS_PROFILING` and armed-only (disarmed = one
relaxed load + branch, unmeasurable on the spin throughput series — bench held at
~194 ns/op):
  - `run_task` books its whole span to **M**; the functor's `Trace_busy_scope`
    nets its own span back **M→B** (`trace_body_add`). Net per task M = setup +
    completion + successor dispatch.
  - Only a **successful** `find_work` counts as M (`find_work_dispatch` in the
    worker loop times a hit; a failed scan before parking is idleness, not cost).
  - A submit issued **from inside** a functor (e.g. `parallel_for` slice fan-out)
    reclassifies B→M (`Submit_cost_scope` gated on an in-functor TLS) — the
    dispatch is our work, not the user's.
- Two subtleties the implementation forced: (a) an **inline / retracted** body runs
  the functor bracket but bypasses `run_task`, so it must credit B *only* — a
  `trace_body_under_run_task` TLS, captured-and-cleared per body scope (so a nested
  inline body reads false), routes it to `trace_body_only` instead of netting a
  non-existent `run_task` span out of M; without this, M went **negative**. (b)
  `machinery_ticks()` needs the same **in-flight** compensation as `busy_ticks()`:
  the fold runs *inside* the settling worker's terminal task, after that body netted
  its span out of M but before `run_task` books the span back in — a plain sum reads
  low by the whole settling body (drove M to −180 µs/run on game_frame before the
  fix). Adding each in-flight slot's elapsed span compensates it.
- Results (game_frame, 6-worker trace scheduler, 200 frames): **baseline body 39.0
  ms / machinery 4.85 ms → 11.1% overhead; optimised 38.8 ms / 2.48 ms → 6.0%.** The
  metric discriminates: the optimised variant halves machinery (coarser batching,
  fewer/larger tasks), a clean "your tasks were a bit too fine" signal. **Caveat:**
  measured with tracing on, so it is an *upper bound* — the coarse per-task clock
  brackets charge their own read latency to M, and real graph nodes pay pipe
  acquire/release + object handoff + successor dispatch that a bare submit/run
  benchmark (~200 ns/op) omits; the two are not directly comparable, the bench is
  the untraced-bare floor.

Done (2026-08): **the overhead metric, two blind spots closed + a ground-truth
oracle.** The summed-M metric above had a known under-count and no cross-check.
Both are now addressed; the summed-M number stays, extended, and gains a
side-by-side oracle.
  - **Per-run setup folded into M (blind spot 1).** The per-run graph setup
    (`bind_links_for_run` + the per-node re-arm loop + indegree init + the initial
    root dispatch) runs on the calling thread in `execute()`, inside *no* `run_task`
    span, and scales with node count — so it escaped M entirely. A `Trace_setup_scope`
    (in `detail/trace_owner.h`, armed-only, gated) now brackets it and routes the span
    to M via a new `trace_machinery_add` → `Scheduler::add_machinery_ticks` bridge
    (mirroring `trace_body_add`). Two mechanics make it exact: (a) an **overflow lane**
    — the scheduler's busy vector is sized `workers + 1`, and a non-worker caller
    (`current_worker_index < 0`, e.g. the frame loop) lands in the trailing slot instead
    of being dropped; the bridge falls back to `global_scheduler()` when
    `current_scheduler` is null (the main thread), which is the scheduler a graph run
    always targets. (b) The setup scope flags its span `run_task`-booked, so a body
    dispatched *inline* within it (a `set_inline` root, or — in worker-less mode — every
    node, since the whole frame drains serially inside the scope) nets its own span back
    out, leaving pure machinery. `begin_run` was moved ahead of the setup so the snapshot
    brackets it. Measured cost on game_frame: ~0.30 µs/node (baseline, 35 nodes → 10.6
    µs/frame) / ~0.40 µs/node (optimised, 37 nodes) — small next to fat bodies, but it is
    the term that grows with a large cheap-bodied graph.
  - **Worker-less ground truth (the oracle).** In a single-thread (`single_threaded =
    true`) traced run the whole frame is serial on one thread with no idle to confound
    it, so `framework = total_wall − B` is the *complete* framework cost by pure
    subtraction — setup, dispatch, completion, pipe, trampoline, everything, with zero
    blind spot. `total` is bracketed in the sample around the whole `execute().sync()`
    frame loop (not the trace's node-span makespan, which omits pre-first-node setup and
    the fold); B is the body accumulator, fed here by the overflow lane since a
    worker-less run has no workers. Reported as `overhead_true = (total − B)/total`. On
    game_frame this is ~2.2%. The summed-M accumulator read off the *same* worker-less
    run closes to it **exactly** (gap 0.00 pt) — the built-in validation that the
    accounting has no serial blind spot; it is therefore the per-op framework **floor**.
  - **Dual print + the gap.** The `--trace` console lines and the SVG headline
    (`graph_trace.h` `write_SVG`, via `set_ground_truth_overhead`) now show BOTH the
    multi-worker summed-M overhead (incl. setup) AND the worker-less serial floor, plus
    the gap. game_frame, 6-worker trace, 100 frames: **baseline summed-M 10.5% (B 38.3
    ms / M 4.51 ms) vs serial floor 2.3% → gap +8.3 pt; optimised summed-M 4.4% (B 38.0 /
    M 1.74) vs floor 2.2% → gap +2.2 pt.** The gap direction is the point and is *not*
    fudged to match: the multi-worker figure is *higher* than the serial floor, and the
    gap is the machinery only workers pay — cross-thread dispatch, pipe hand-off,
    park/wake, steal — i.e. the price of the parallelism, not a metric error. The
    serial-floor validation (gap 0 on the serial run) is what licenses reading the
    multi-worker gap as real parallel cost rather than an accumulator blind spot. All
    `TS_PROFILING`-gated and armed-only (disarmed = one relaxed load + branch; the
    throughput series is untouched — it never arms).

Done (2026-08): **Phase 1 (additive): four-way subtraction breakdown.** The summed-M
metric answers "how much of compute is machinery" by *accumulation*; this adds the
complementary *subtraction* view over core-time, and prints both side by side so the two
can be diffed. Additive and reversible — no existing machinery was removed; a parallel
computation runs alongside summed-M.
  - **The model.** In core-time `T = workers × makespan`, every worker-moment is body,
    framework-machinery, or idle: `T = B + M_core + P`, so `M_core = busy − B` and
    `P = T − busy` (`busy` = `busy_ticks()`, the `run_task` span sum; `B` = `body_ticks()`).
    Framework work done OFF-worker — the per-run `execute()` setup on the frame-loop thread —
    is absent from `busy`, so it is a separate fourth bucket, **Orchestration**. The headline
    is a four-way split **body / machinery(M_core) / idle / orchestration** as shares of `T`.
  - **Mechanism** (all `TS_PROFILING`-gated, armed-only, disarmed = one relaxed load+branch).
    A dedicated per-`Busy_slot` `orchestration` accumulator + `Scheduler::add_orchestration_ticks`
    (overflow-lane aware) + a `trace_orchestration_add` bridge (`trace_owner.h`/`scheduler.cpp`,
    mirroring `trace_machinery_add`). `Trace_setup_scope` now books its span to `trace_orchestration_add`
    IN ADDITION to `trace_machinery_add`, so summed-M is untouched. The fold (`trace_stamps.h` →
    `Graph_trace::on_run_complete`) snapshots an orchestration delta and computes the per-run four-way
    (`four_way_*_share()` getters); `--trace` console and the SVG headline print both metrics plus a
    reconciliation line.
  - **The reconciliation is the point.** Summed-M already includes the setup span (via the
    unchanged machinery bridge), so summed-M vs `(M_core + Orch)` leaves a residual =
    **successful-`find_work` dispatch + inline-body time** — both counted in summed-M but living
    OUTSIDE `busy`, so outside `M_core = busy − B`. It reconciles tightly: game_frame, 6-worker
    trace, 200 frames — **baseline** summed-M 4334 µs vs (M_core 4293 + Orch 18 = 4311) → residual
    **23 µs** (~0.5%); **optimised** summed-M 1825 µs vs (M_core 1788 + Orch 15 = 1804) → residual
    **21 µs**. Orchestration is ~16–18 µs/frame. The four-way shares: **baseline** body 80.8% /
    M_core 9.1% / idle 10.2% / orch ~0.0% (of a ~47 ms core-time `T`); **optimised** body 92.4% /
    M_core 4.3% / idle 3.3% / orch ~0.0% — the optimised frame converts idle and M_core into body
    share, matching the summed-M and dead-time stories. A large residual would be data, not a bug
    (Phase 2 — retiring the direct-M accumulation in favour of pure subtraction — is gated on human
    review of these numbers, not done here).

Remaining:
- **Per-kind aggregates + scheduling counters.** Per task kind {node, slice,
  async, continuation, nested}: count + Welford duration + total busy. Plus
  cheap `find_work` counters: steals, local-deque hits, global-queue hits,
  low-valve firings. This is what would surface a scheduling inversion (like
  the `networking` local-deque/steal case) *directly* — a normal task showing
  high steal-wait — rather than leaving it to be inferred from a node's
  dispatch wait. Render as a compact global line and/or a thin per-bucket
  kind-mix ribbon (a few bands, never per-task bars).

## Internal profiling: decomposing M to optimise the machinery itself

The overhead headline says *how much* frame time is machinery; optimising the
machinery needs *where* it goes, *which work* causes it, and *why* it is slow
(contention vs instructions). Ranked plan (2026-07), value/effort first:

1. **Decompose M into named phases.** Two bridges already separate their share
   (`add_dispatch_ticks` = successful `find_work`, `add_submit_ticks` =
   in-functor fan-out); the rest of M is one lump. Add per-worker **acquire**
   (`pipe_acquire` / handoff) and **completion** (successor release +
   dep-counter + object release) accumulators, bracketed the same armed/relaxed
   way. Turns `M 2.48 ms` into `dispatch / acquire / completion / submit`,
   i.e. into a decision about what to attack. ~4 clock reads/task armed, same
   gating discipline.
2. **Queue-lock contention.** M does not distinguish machinery *instructions*
   from machinery *waiting on the lock*, and the single mutex-guarded priority
   queue is the known scaling bottleneck. A per-worker queue-lock-wait
   accumulator (request→hold time) + a CAS-retry count on the MPMC queues
   quantifies exactly the thing a sharded/lock-free queue redesign would buy
   back — it prices the redesign before doing it.
3. **Cheap ratio counters (no timers).** `find_work` hit/miss, local-pop vs
   steal ratio, park/unpark syscall count — plain `fetch_add`s. These say *why*
   dispatch is expensive (e.g. a low hit ratio argues for better wake
   targeting, not faster scans). Overlaps with the per-kind/scheduling-counter
   item above; land them together.
4. **Attribute M per node and per kind.** Extend the owner-attribution sink
   (per-node true-busy) to machinery, so the trace shows whose completions /
   acquires dominate (a node with many conflict edges pays more successor
   release); split M by kind {node, slice, async, continuation} — "slices are
   40% of M" is a batch-coarser signal. Reuses the `trace_owner` seam.
5. **Microbench ladder.** Per-feature deltas over the ~194 ns bare-op floor:
   one prerequisite, one continuation, one acquire, one conflict edge, each in
   isolation. In-situ M says how much; the ladder prices each feature so a
   design change can be costed in advance.
6. **Wake-latency histogram.** Split idle into genuine no-work vs
   parked-while-work-available (submit→observe gap) — the metric that would
   justify tuning the `spin_then_block` / `handoff` parking policy.

Start with 1 + 2 (3 rides along): phase decomposition makes the single M
number actionable at near-zero cost, and lock-wait measures the one bottleneck
already slated for redesign. 4–6 follow once the phase split shows where to
dig.
