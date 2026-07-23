# Profiler-guided optimization: findings and design abstract

Working record of the 2026-07 dead-time investigation on the `game_frame`
fixture, and the tuner design distilled from it. Companion to
[TODO.md](TODO.md) 2.4 (profiler-guided optimization), 2.5 (rank-shaped
dispatch), 2.11 (yield points), 2.12 (frame-boundary overlap). The measurement
tooling referenced throughout is `tools/graph_trace.h` (aggregated average-run
trace: per-node medians/variance, per-edge binding gaps, measured critical-path
frequency, dispatch waits, critical dead time).

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

## Tuner design (three tiers, least machinery first)

1. **Analysis + proposal — purely algorithmic, no model.** For each critical
   edge by descending mean binding gap: occupiers = nodes whose median bars
   overlap the gap window; movable iff slack > needed shift and not an
   ancestor of the successor; proposal = a cycle-safe delaying edge (edge
   *additions* never change semantics); predicted recovery =
   `min(gap, overlap)` minus the chain-extension guard above. Deliverable
   shape: `Graph_trace::suggest()` — a ranked report.
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
