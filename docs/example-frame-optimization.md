# Worked example: reading a frame trace and optimising it

> **Absolute figures corrected (2026-08).** The old µs figures in this doc
> (baseline 4793 µs → optimised 3120 µs, "on 6 workers") were a **profiler
> artifact, not a bigger workload**. Before the
> `parallel_for`-fans-out-on-the-current-scheduler fix, the sample's
> `parallel_for` work leaked to the global default pool, so a "6-worker" trace
> secretly ran on the dedicated 6 workers *plus* a full default pool (~12 cores):
> the frame times read ~2× too fast **and** utilization undercounted the leaked
> work. The serial workload never grew — it has been a constant **~36.6 ms**
> since the 30-system frame landed (2026-07-24). The fixes that made the trace
> honest: `parallel_for`-on-the-current-(now single-global-)scheduler routing,
> owner attribution, time-bucketed utilization, and the body / framework-overhead
> split. The sample now runs **8 workers** (`variant_workers = 8`); the honest
> current picture is **baseline ≈ 85 % util / ≈ 12 % dead / ≈ 6.5 ms → optimised
> ≈ 95 % util / ≈ 10 % dead / ≈ 5.6 ms** (util and dead time are the portable
> numbers; frame time is machine-dependent). The headline figures in the body
> below are updated to these; the lever-by-lever µs deltas in §4 were measured on
> the pre-fix config and are kept as **relative magnitudes only (re-measure
> pending)** — which levers help, and why, is unchanged. See
> [profiler-guided-optimization.md](profiler-guided-optimization.md).

This walks through the `game_frame` sample as an optimisation exercise. It has
two variants of the same ~30-system frame, built from the *same* system bodies:

- **baseline** — a straightforward composition. Every system declares what it
  reads and writes; the graph parallelises it with no hand-tuning.
- **optimised** — the same frame after reading its own trace, with the levers
  the visualization makes obvious applied in an `optimise()` section.

Generate both traces (on an 8-worker scheduler) plus the structure dump with:

```
task_system --trace 200
show_graph.bat            # renders the DOT and opens both average-run SVGs
```

producing `sample_game_frame_avg_baseline.svg`,
`sample_game_frame_avg_optimised.svg`, and `sample_game_frame.dot`.

The point of the exercise is not the exact percentage it ends up saving; it is
*which* optimisations the trace tells you are worth trying, and — just as valuable —
which ones it tells you not to bother with. It ends by showing what a *finished*
optimisation looks like: the frame stops being limited by its dependency chain
and starts being limited by its core count, and the picture says so at a glance.

## 1. The frame

The frame models a heavy 30 fps-class scene, ~36.6 ms of work single-threaded:

- a **frame head** (input → camera / networking / scripting VM);
- a **gameplay trio** (combat, economy, quests) reading the head + last frame's
  transforms;
- **AI** (speculative nav queries) feeding **animation** (graph eval → IK →
  skinning);
- a **split physics pipeline** (broadphase → narrowphase → solver → finalize);
- **propagation** of this frame's transforms, then the **flip**;
- a **render pipeline** (frustum → occlusion → shadow → command recording →
  submit) that reads *last* frame's transforms, so it overlaps this frame's
  simulation — the render thread with one frame of latency, modelled with
  `Versioned` and nothing else;
- off-path leaves: cloth, audio, vfx, streaming, replication, stats, GC, debug.

Every store has a single writer, so `compile()` derives the whole DAG from the
access declarations. Nothing below knows about threads.

The optimised variant also publishes a **gameplay snapshot** (`Versioned`): the
trio's results are packed and flipped at frame end, so next frame's AI reads
them as a stable previous version — the lever in §4 that does the most work.

## 2. Reading the baseline trace

The baseline on 8 workers:

> **core utilization: ≈ 85 %** · **critical path dead time: ≈ 12 %** · frame time **≈ 6.5 ms**

Two things jump out of the picture:

1. **It is bound by the length of its dependency chain.** The critical chain is
   the sim spine: `input → networking → scripting → combat → AI → anim_graph →
   ik_post → propagation → flip → cloth`. Utilization is high but not saturated,
   and the frame finishes no sooner than that longest path however many cores are
   free — so every gain has to come from *shortening the chain*, not from filling
   cores. (On the pre-fix leaked-pool trace this read as a starker 60 % util /
   ~40 % idle contrast; the secret ~12 cores exaggerated the idle share. The
   structural reading is the same on honest 8 cores.)

2. **The fat bars on that chain are serial nodes.** `combat` (~0.86 ms),
   `ik_post` (~0.82 ms) and `UI` (~1.5 ms) are the widest bars, and they are
   modelled single-threaded — a single core carries each while the rest of the
   chain waits on it. `cloth` sits alone on the **post-flip tail**: it reads the
   fresh transforms, so it is the last thing in the frame, adding its whole cost
   to the makespan.

That reading dictates the strategy. On a chain-bound frame, **only shortening the
critical chain reduces the makespan.** Anything that merely fills idle cores
raises utilization without making the frame finish sooner.

## 3. The levers the trace tells you *not* to bother with

Two "optimisations" look attractive and do nothing for the makespan here:

- **Staging the draw producers (`Deferred`).** The baseline's draw producers
  (command recording, particles, UI) write the draw queue directly, so they
  serialise. Converting them to `Deferred` staging lets them run in parallel —
  and it does: utilization rises. But makespan is unchanged, because the draw
  producers were never on the critical path; freeing them just fills cores the
  critical chain wasn't using.
- **Moving cloth off the tail (version choice), on its own.** Declaring cloth on
  *last* frame's transforms takes it off the post-flip tail. On the un-optimised
  spine this measured **neutral**: cloth's 2 ms of parallel work simply moved
  into the middle of the frame and competed with the spine, delaying it by about
  what the tail saved. Nothing evicts a running node, so filler added next to a
  busy critical chain just slows it down.

Both are real, both are in the optimised variant — but if you were chasing the
makespan and reached for these first, the trace would have saved you the effort.

## 4. The levers that move the makespan

Two kinds of lever shorten the critical chain: **split** its fattest serial bars,
and **cut** its length by breaking a dependency.

*Split the fat serial bars.* `combat`, `ik_post` and `UI` are the widest bars on
the chain and are modelled single-threaded. Splitting each into internally-parallel
work (`parallel_for` across entities / characters / widgets) both shortens the
chain and consumes the idle cores the frame had. (The `Deferred` and cloth
version-choice levers from §3 ride along here: they don't move the makespan, but
once the chain is shorter there is idle capacity for their work to fill.)

*Cut a dependency.* The chain runs `… → trio → AI → anim → …`: AI waits for this
frame's gameplay trio. But AI does not *need* this frame's gameplay — one frame of
AI latency is invisible and standard. Publish the trio's results as a `Versioned`
**gameplay snapshot** at frame end and have AI read the *previous* version. The
`trio → AI` edges vanish; AI now starts as soon as its nav paths are ready, and the
whole animation sub-chain slides forward. This single lever is the largest cut in
the exercise.

| lever | what | makespan |
|---|---|---|
| baseline | — | 4793 µs |
| + split `combat`, `ik_post`, `UI` | per-entity / character / widget `parallel_for` on the fattest critical bars | 3941 µs (−18 %) |
| + AI reads last frame's gameplay (`Versioned`) | deletes the `trio → AI` edges; AI starts on its paths, not the trio | **3120 µs (−35 %)** |

> *These absolute µs and their per-lever percentages were measured on the pre-fix
> (leaked-pool) config and are kept as **relative magnitudes only — re-measure
> pending**. Read them for ordering, not as absolute times: the split of the
> fattest critical bars helps, and the `Versioned` cut helps more. On the honest
> 8-worker config the whole exercise saves roughly 6.5 → 5.6 ms (see the note at
> the top and §5).*

The `Versioned` cut is worth more than all the splits combined, and it costs
almost nothing to model — a snapshot node that stages the trio's results and a
flip node, both off the critical path. It is the same mechanism the render pipeline
already uses to read last frame's transforms; here it is turned on the gameplay
dependency instead.

## 5. The result: chain-bound → core-bound

| | baseline | optimised |
|---|---|---|
| frame time | ≈ 6.5 ms | **≈ 5.6 ms** |
| core utilization | ≈ 85 % | **≈ 95 % (green)** |
| critical path dead time | ≈ 12 % | ≈ 10 % |

(Utilization and dead time are the portable numbers; frame time is
machine-dependent, and the per-lever µs deltas in §4 were measured on the pre-fix
config — see the note at the top.)

The baseline is already busy but **chain-bound**: utilization is high, yet the
frame is limited by the *length* of its critical chain, so the headroom that is
left can only be recovered by shortening that chain — not by filling cores. The
levers do exactly that, and utilization climbs toward saturation: the optimised
frame is **core-bound**, cores nearly full (≈ 95 %), and what waiting remains is
the critical path waiting for a *free core* rather than a missing dependency. In
the picture the single long critical spine has fragmented — no node is critical
in a large majority of runs, because the binding path now bounces between
whichever ready node is waiting on a core.

(On the pre-fix leaked-pool trace this flip looked far more dramatic — baseline
60 % util / 5 % dead → optimised 85 % util / 19 % dead — because the secret ~12
cores made the baseline read half-idle and pushed the optimised chain into heavy
core contention. The honest 8-core picture is a milder version of the same
transition: utilization up, the frame moving from chain-bound toward core-bound.)

That transition is the signal that the optimisation is *done*. On a core-bound
frame, rearranging has nothing left to exploit — every core is busy. From here,
faster means **cutting work** (a smaller scene, cheaper systems) or **adding
cores**, not restructuring the graph. The trace tells you when you have reached
that point instead of guessing.

## 6. What carries over to a real frame

- **Read utilization and dead time together.** Low dead time + low utilization =
  critical-path bound: shorten the chain. High dead time = scheduling bound:
  worker count, ordering, or contention. High utilization + rising dead time is
  the *finished* state — core-bound, nothing left to rearrange.
- **Optimise the critical path, not the utilization number.** The trace makes
  the critical chain and its fattest bars obvious; that is where makespan lives.
- **Cutting a dependency beats splitting a bar.** The largest lever here was not a
  faster node — it was deleting an edge. `Versioned` lets a consumer read last
  frame's data where one frame of latency is acceptable (AI, audio, render), which
  removes the producer from *this* frame's critical chain entirely. Look for those
  edges first: they are cheap and they shorten the chain rather than just widening
  a bar on it.
- **Levers are frame-state-dependent.** A version choice or an ordering edge that
  is neutral (or harmful) on a long spine can pay once the spine is short.
  Measure each in place — the exercise did, keeping only what moved the number.

The mechanics behind the metrics (streaming aggregation, the measured critical
path, the dead-time and utilization definitions) are in
[guide.md §6.2](guide.md); the ranked tuner design this exercise motivates is in
[profiler-guided-optimization.md](profiler-guided-optimization.md).
