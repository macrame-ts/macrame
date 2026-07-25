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
`parallel_for` had hardcoded `default_scheduler()`, so a node on the sample's
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

Remaining (needs the same seam plus a per-task kind tag — a gated change to the
task core):
- **Per-kind aggregates + scheduling counters.** Per task kind {node, slice,
  async, continuation, nested}: count + Welford duration + total busy. Plus
  cheap `find_work` counters: steals, local-deque hits, global-queue hits,
  low-valve firings. This is what would surface a scheduling inversion (like
  the `networking` local-deque/steal case) *directly* — a normal task showing
  high steal-wait — rather than leaving it to be inferred from a node's
  dispatch wait. Render as a compact global line and/or a thin per-bucket
  kind-mix ribbon (a few bands, never per-task bars).
