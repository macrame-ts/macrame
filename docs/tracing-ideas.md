# Tracing ideas borrowed from other tools

A curated pass over the profilers and telemetry systems surveyed in the 2026-07
prior-art search (game tools, HPC profilers, production observability), asking one
question per tool: what could `Graph_trace` and the surrounding tracing layer borrow?
Context for every verdict: our design is aggregated-only (no per-sample storage —
per-node Welford mean/var + P² P50/P95, criticality frequency, the B/M/I overhead
split, time-bucketed utilization; O(nodes+edges) state), rendered as an average-run
SVG, gated by `TS_PROFILING`. Verdicts: **adopt** (clear win, cheap), **consider**
(valuable, needs design), **reject** (conflicts with the design; the reason is the
useful part). Ideas are numbered 1-18 for referencing; numbers are stable IDs, don't renumber.

---

## Adopt

### 1. A/B statistical comparison with significance (PIX)

PIX's timing-capture comparison computes per-event mean deltas between a baseline
and an experiment range and colour-codes them by p-value, so a regression reads as
"real" or "noise" at a glance ([PIX blog](https://devblogs.microsoft.com/pix/timing-capture-statistical-comparison-features/)).
We already hold everything a Welch's t-test needs — per-node Welford mean, variance,
and run count — and the sample already traces `baseline` and `optimised` as two live
`Graph_trace` objects in one process. A `Graph_trace::compare(const Graph_trace&)`
producing a per-node table (mean delta, % change, significance) plus an optional
delta-annotated SVG would turn the worked optimisation exercise from "eyeball two
SVGs" into ranked, significance-filtered deltas. Render/fold-side only; no new
runtime cost.

**Verdict: adopt.** The data is already collected; the test is a few lines of
arithmetic at render time.

### 2. What-if sensitivity from the CPM pass (TaskProf)

TaskProf answers "what if this spawn site were k% faster?" by recomputing span over
its work tree ([TaskProf](https://github.com/rutgers-apl/TaskProf)). Our `write_SVG`
already runs a CPM pass over median durations; re-running it with node X scaled by
0.8 gives the projected makespan delta — per node, at render time, no runtime cost.
Surfacing "−20% body → −N µs makespan" in each node's tooltip (or a ranked
sensitivity list in the stats panel) directly answers the question the trace exists
for: which node is worth optimising. Off-path nodes report zero, which is itself
the lesson.

**Verdict: adopt.** Pure render-time reuse of existing CPM machinery.

**Decision (author, 2026-07): shelved** pending a clearer case. Reliability notes
from the discussion, for the revisit: CPM assumes infinite cores + zero dispatch
latency, so a projected delta is a structural *upper bound* — accurate in a
dependency-bound frame, optimistic in a core-bound one; medians are not additive;
CPM picks one path through median durations while the measured chain is a
frequency distribution (report share × delta, never CPM alone); no scheduling
feedback. Proposed mitigations: publish the model-fidelity ratio (CPM makespan /
measured makespan) as a confidence qualifier, restrict display to nodes with
measured chain share above a threshold, label deltas as structural upper bounds.

### 3. Dead-time cause classification (Scalasca wait states)

Scalasca classifies waiting into named states (late sender, imbalance) instead of
leaving "wait" undifferentiated ([Scalasca critical path](https://www.osti.gov/servlets/purl/1107288)).
Our analog is joining two datasets we already collect: for each critical dead-time
band, read the time-bucketed utilization under the gap. Cores saturated during the
gap = the chain waited for a *core* (core-bound — add workers or shed off-path
work); cores idle = the chain waited for a *dependency or dispatch* (dependency-
bound — cut edges, check wake latency). Label each band in its tooltip and split
the dead-time headline into the two causes. This automates the baseline-vs-
optimised diagnosis the game_frame exercise did by hand.

**Verdict: adopt.** A fold/render-side join of two existing aggregates.

### 4. Criticality ranking export (CRISP)

CRISP aggregates critical-path membership across many traces and presents ranked
"who sits on the path how often" lists ([CRISP](https://www.usenix.org/system/files/atc22-zhang-zhizhou.pdf)).
Our SVG carries the same data as colour; a ranked table (chain share × mean
duration × mean incoming dead time, sorted) in the stats panel or as text output
gives the same at-a-glance ordering without hunting the picture — and is the
natural thing to paste into a commit message or issue. Trivial: sort data we
already fold.

**Verdict: adopt.** A sort and a table over existing aggregates.

**Decision (author, 2026-07): implement after 3.** Rendered in the SVG, below the
graph and the legend; rank key = chain share × mean duration; as part of the same
change, compact the legend to at least two columns (height-optimised).

---

## Consider

### 5. Per-node compact duration histogram (Tracy)

Tracy's find-zone view draws a full per-zone duration histogram — the single
feature that exposes multimodality (a node that is fast except when a cache miss /
contention / branch makes it 10× slower). Our Welford+P² state cannot distinguish
a bimodal node from a noisy one; CV hints but does not show. A fixed 16–32-bin
log2 histogram per node (`graph_trace.h` fold; bins keyed off min/max or a fixed
µs ladder) costs ~64–128 bytes/node, stays O(nodes), and renders as a tooltip
sparkline. Tracy computes its histogram from stored events; ours would be streaming
and approximate, which is fine — the shape is the signal, not the exact counts.

**Verdict: consider.** Real diagnostic value; needs a bin-ladder design that
survives unknown duration ranges without per-node adaptivity complexity.

**Decision (author, 2026-07): implement after 4.** Tooltip rendering is feasible:
the tooltips are our own overlay script drawing SVG elements, so a sparkline
histogram is just more drawn rects — no reliance on native `<title>` (text-only).

### 6. Exemplar-run capture (Unity Profile Analyzer)

Profile Analyzer's answer to "what does the median frame look like?" is *Select
Median Frame* — jump to a real captured frame ([Unity how-to](https://unity.com/how-to/optimize-your-game-unity-profile-analyzer)).
Our average-run SVG deliberately synthesizes; the complement is retaining ONE real
run's raw stamps (O(nodes), a second `Trace_stamps`-sized buffer) chosen by policy —
e.g. the run closest to the median makespan, or the worst run — and rendering it as
an optional second timeline (or overlay) for outlier diagnosis. This does not
violate the no-samples philosophy: memory stays O(nodes), independent of run count;
it is one exemplar, not a trace. The design questions are the retention policy
(closest-to-median needs a two-pass or an accept-if-closer heuristic; worst-run is
trivial) and whether the SVG shows it as a toggle layer or a separate file.

**Verdict: consider.** Cheap memory, real debugging value for tail runs; policy
and presentation need a decision.

### 7. Trace serialization + diffing (pprof, CoD CI)

pprof profiles are additive, mergeable files, which makes diffing (`-diff_base`)
and CI trend-tracking natural; Call of Duty's CI profiles hundreds of machines per
build and trends the metrics. Serializing `Graph_trace` aggregates (keyed by node
name — stable across builds, unlike indices) to a small file enables: diff two
builds' traces, store per-commit baselines, flag regressions in CI (ties into TODO
10.1's benchmark-regression item and the A/B compare (1) — same comparison, one
side loaded from disk). Welford states merge and diff cleanly; P² markers do not
(see 8).

**Verdict: consider.** The format (versioning, node-name keying, tolerance for
graph shape drift between builds) is the design work; the payoff is CI regression
gating on the real sample.

**Decision (author, 2026-07): rescoped.** Serialize the mean frame (the
aggregates) to e.g. JSON as a general post-processing export — useful beyond any
one consumer. Diffing belongs to 1; CI integration is out of scope.

### 8. Mergeable quantile sketch — DDSketch/HDR as a P² alternative

P² is 5 markers per quantile, the memory floor — but two P² states cannot be
merged, which blocks cross-process aggregation (fleet telemetry, 9) and clean
serialization+merge workflows. DDSketch ([VLDB'19](https://www.vldb.org/pvldb/vol12/p2195-masson.pdf))
and HdrHistogram ([repo](https://github.com/HdrHistogram/HdrHistogram)) are
fixed-memory, relative-error, and mergeable, at a few hundred bytes per node
instead of ~80. Not worth switching for the single-process SVG; becomes the right
move the moment serialization/merge (7) or telemetry (9) is
real. A compact log2 histogram (5) is in fact a crude mergeable sketch
— one structure could serve both purposes.

**Verdict: consider.** Keep P² today; switch (or add the histogram as the
mergeable carrier) when merge becomes a requirement. Decide once, with the
serialization format.

**Decision (author, 2026-07): rejected.**

### 9. Fleet telemetry from shipped builds (Riot)

Riot ships a tiny end-of-game performance summary from every client and builds
distributions server-side, stratified by hardware band ([Riot](https://technology.riotgames.com/news/down-rabbit-hole-performance-monitoring))
— their stated rationale (full traces don't scale to a fleet; single numbers
mislead) is our rationale. Our aggregate is exactly the right shape for this: a
few KB per session, already statistical. What's missing is the pipeline above the
library (serialization, upload, server-side merge — the merge again requiring a
mergeable sketch). The library's part is small: a stable serialized form and a
documented "telemetry-grade" subset (means and counts merge trivially even without
sketches).

**Verdict: consider.** The library should enable it (serialization + mergeable
stats); the pipeline itself is the consumer's.

**Decision (author, 2026-07): rejected.**

### 10. Live HUD / runtime query API (microprofile, HPX counters)

microprofile's aggregate mode and HPX's performance counters expose running
accumulators live rather than at report time. Our accumulators are live too — the
fold completes at each run's settle, so between runs the aggregates are coherent
and a game could render an in-game HUD (per-node means, utilization, overhead
share) from them every second. Needs only a read API with a documented
happens-before (read between `execute()` calls, or snapshot under the run's
completion), not new collection. The SVG writer becomes one consumer of a queryable
surface instead of the only exit.

**Verdict: consider.** Cheap; the work is API surface and read-coherence rules,
worth doing when a real consumer appears.

**Decision (author, 2026-07): implement, after the preceding accepted items
(3, 4, 5).**

### 11. Per-node worker-imbalance metric (mpiP)

mpiP reports per-callsite min/mean/max across ranks — imbalance as a first-class
number. Our analog: for a `parallel_for`-heavy node, the spread of its busy time
across workers (via the owner-attribution sink, split per worker instead of one
sum: `owner_busy_[node]` → `[node][worker]`, memory nodes × workers) yields an
imbalance factor max/mean — the long-tail / bad-chunking diagnostic. Today the
per-worker histogram shows where a *node* ran, not how evenly its *fan-out* spread.

**Verdict: consider.** Memory grows to nodes × workers (still small); worth it
when tuning `parallel_for` granularity becomes a real workflow.

### 12. Multi-metric time strips (Vampir performance radar)

Vampir's radar draws metric × time heatmaps. Our utilization strip is one such row;
the same bucketed machinery generalizes to more rows: machinery share over time
(bucketed M vs B — *when* in the frame the scheduler overhead concentrates),
task volume over time (submit-rate bursts). Each row = one more per-worker bucket
array armed per run, rendered as a thin strip under the utilization one. Adds
collection cost (another distribute-across-buckets per task) — measure before
stacking many.

**Verdict: consider.** The utilization strip proved the pattern; add rows only
when a concrete question needs them (machinery-over-time is the likely first).

### 13. Long-window bucket merging (Arm MAP)

MAP keeps a bounded timeline of arbitrarily long runs by merging adjacent samples
as time grows. Our buckets are fixed-count per *run* and the fold is per-run, so
we don't have the unbounded-window problem — unless we ever trace a long session
as one window (a soak test, a level fly-through). Then MAP's trick applies
directly: when elapsed exceeds `buckets × width`, halve resolution by merging
neighbour pairs and double the width.

**Verdict: consider (dormant).** File the technique; irrelevant while tracing is
run-scoped.

---

## Rejected (and why)

### 14. Causal profiling / virtual speedup (Coz)

Coz answers "would speeding X up matter?" by *perturbing* execution — inserting
delays everywhere else to simulate a speedup, plus an experiment framework to make
the statistics come out ([Coz](https://arxiv.org/pdf/1608.03676)). For general
unstructured code that is the only way; for a declared DAG it is not — the CPM
what-if (2) answers the same question analytically, with zero runtime
perturbation. Coz's edge over CPM is capturing effects the DAG doesn't model
(contention, cache interference between co-running nodes); if that gap ever
matters, the serial-baseline lane (TODO 10.9) measures it more directly than
perturbation would.

**Verdict: reject.** Perturbs the execution we are trying to observe; CPM + the
serial-vs-parallel delta cover the question for DAG-structured work.

### 15. Full event tracing (RAD Telemetry, Tracy's storage model, Chrome tracing)

Store every zone, aggregate at display time. This is the model the trace is
defined against: memory proportional to duration, a capture/transport/storage
pipeline, and a viewer obligation. Score-P's docs state the trade cleanly
(profile = constant memory, trace = memory ∝ duration). Our position: aggregate
in-process, render the average; the exemplar-run capture (6) is
the bounded concession to "sometimes you need to see one real run".

**Verdict: reject.** By philosophy; the exemplar (6) covers the residual need at
O(nodes).

### 16. Intra-node folding (BSC folding)

Folding reconstructs time-resolved detail inside a repeated region from sparse
samples across many repetitions ([BSC](https://tools.bsc.es/folding)) — but it
requires a sampling mechanism (timer interrupts or progress markers inside
node bodies) that we deliberately don't have; nodes are opaque between `start` and
`end` stamps. Adopting it means either OS-level sampling machinery or a user-facing
intra-node marker API — both large surfaces for a diagnostic we haven't needed.

**Verdict: reject** until intra-node visibility is a demonstrated need; if it
becomes one, folding (one cheap sample per run, accumulated across runs) is the
design to return to — it is the only known way to get intra-node time resolution
without per-event storage.

### 17. Flame-graph export of critical-work attribution

Flame graphs aggregate *stack hierarchies*; our structure is a DAG whose only
hierarchy is node → {body, slices, async} — two levels. Flattening the DAG into
stacks loses the edges, which carry the entire scheduling story, and the SVG
already shows the two-level split in tooltips. An export would be a lossy
re-rendering of a picture we already draw better.

**Verdict: reject.** Shape mismatch; the criticality ranking table (4)
serves the "give me a sorted list" need instead.

### 18. Sub-run phase profiles (Score-P phase profiles)

Score-P can aggregate per user-declared phase within a run. Our graph nodes *are*
the phase structure — a finer sub-phasing would mean an API for declaring phases
inside nodes, which is the intra-node visibility problem again (see 16).
The per-run fold plus per-node aggregates already deliver what phase profiles
deliver in the flat-callpath world.

**Verdict: reject.** Redundant with the node structure we already have.

---

## Sources

- PIX statistical comparison: https://devblogs.microsoft.com/pix/timing-capture-statistical-comparison-features/
- PIX consolidated events: https://devblogs.microsoft.com/pix/graphing-consolidated-events-in-the-timing-capture-metrics-view/
- Unity Profile Analyzer: https://docs.unity3d.com/Packages/com.unity.performance.profile-analyzer@1.2/manual/profile-analyzer-window.html
- Unity Profile Analyzer how-to: https://unity.com/how-to/optimize-your-game-unity-profile-analyzer
- Tracy: https://github.com/wolfpld/tracy
- Unreal Insights timers: https://dev.epicgames.com/documentation/unreal-engine/using-the-timers-and-counters-tabs-in-unreal-insights-for-unreal-engine
- Superluminal: https://superluminal.eu/features/
- RAD Telemetry: https://www.radgametools.com/telemetry.htm
- microprofile: https://github.com/jonasmr/microprofile
- Riot performance telemetry: https://technology.riotgames.com/news/down-rabbit-hole-performance-monitoring
- CoD CI profiling (GDC 2018): https://www.gdcvault.com/play/1025324/Automated-Testing-and-Profiling-for
- mpiP: https://github.com/LLNL/mpiP
- ompP: https://link.springer.com/chapter/10.1007/978-3-540-68555-5_2
- Score-P memory model: https://docs.olcf.ornl.gov/software/profiling/Scorep.html
- Scalasca critical path: https://www.osti.gov/servlets/purl/1107288
- TAU: https://hpc.llnl.gov/software/development-environment-software/tau-tuning-and-analysis-utilities
- HPCToolkit: https://hpctoolkit.org/
- HPX performance counters: https://hpx-docs.stellar-group.org/branches/master/html/manual/optimizing_hpx_applications.html
- Intel Flow Graph Analyzer: https://www.intel.com/content/www/us/en/docs/advisor/user-guide/2024-1/flow-graph-analyzer-gui-overview.html
- Arm MAP: https://www.alcf.anl.gov/files/Hulguin_Arm_Forge_DDT_MAP.pdf
- CRISP (Uber, ATC'22): https://www.usenix.org/system/files/atc22-zhang-zhizhou.pdf
- TaskProf: https://github.com/rutgers-apl/TaskProf
- Coz: https://arxiv.org/pdf/1608.03676
- BSC folding: https://tools.bsc.es/folding
- DDSketch (VLDB'19): https://www.vldb.org/pvldb/vol12/p2195-masson.pdf
- HdrHistogram: https://github.com/HdrHistogram/HdrHistogram
- P² (Jain–Chlamtac): https://www.cse.wustl.edu/~jain/papers/ftp/psqr.pdf
- Akinshin on P²: https://aakinshin.net/posts/p2-quantile-estimator-intro/
- pprof: https://github.com/google/pprof
- Vampir: https://vampir.eu/tutorial/manual/introduction
- Aggregate trace visualization (2024): https://arxiv.org/abs/2412.07036
- Process-visualization patent (US10692030): https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10692030
