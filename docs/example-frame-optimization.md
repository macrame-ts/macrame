# Worked example: reading a frame trace and optimising it

This walks through the `game_frame` sample as an optimisation exercise. It has
two variants of the same ~30-system frame, built from the *same* system bodies:

- **baseline** — a straightforward composition. Every system declares what it
  reads and writes; the graph parallelises it with no hand-tuning.
- **optimised** — the same frame after reading its own trace, with the levers
  the visualization makes obvious applied in an `optimise()` section.

Generate both traces (on a 6-worker scheduler) plus the structure dump with:

```
task_system --trace 200
show_graph.bat            # renders the DOT and opens both average-run SVGs
```

producing `sample_game_frame_avg_baseline.svg`,
`sample_game_frame_avg_optimised.svg`, and `sample_game_frame.dot`.

The point of the exercise is not the 17 % it ends up saving; it is *which*
optimisations the trace tells you are worth trying, and — just as valuable —
which ones it tells you not to bother with.

## 1. The frame

The frame models a heavy 30 fps-class scene, ~35 ms of work single-threaded:

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

## 2. Reading the baseline trace

The baseline on 6 workers:

> **core utilization: 60 %** · **critical dead time: ~6 %** · makespan **~4.87 ms**

Two things jump out of the picture:

1. **It is critical-path bound, not utilization bound.** Dead time is low (~6 %,
   green) — the critical chain rarely waits for a worker — but utilization is
   only 60 %. The frame is limited by the *length of its dependency chain*, not
   by running out of cores. The chain is the sim spine: `input → networking →
   scripting → combat → AI → anim_graph → ik_post → propagation → flip → cloth`.

2. **The fat bars on that chain are serial nodes.** `combat` (~0.86 ms),
   `ik_post` (~0.82 ms) and `UI` (~1.5 ms) are the widest bars, and they are
   modelled single-threaded — they don't use the idle 40 % of core time. `cloth`
   sits alone on the **post-flip tail**: it reads the fresh transforms, so it is
   the last thing in the frame, adding its whole cost to the makespan.

That reading dictates the strategy. On a critical-path-bound frame, **only
shortening the critical chain reduces the makespan.** Anything that merely fills
idle cores raises utilization without making the frame finish sooner.

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

Shorten the critical chain by splitting its fattest serial bars — exactly the
nodes the picture flags:

| lever | what | makespan |
|---|---|---|
| baseline | — | 4849 µs |
| + parallelise `combat`, `ik_post` | per-entity / per-character split of the two fattest critical bars | 4430 µs (−9 %) |
| + parallelise `UI` | per-widget layout (it was 1.5 ms serial and gating submit) | 4086 µs (−16 %) |
| + cloth on last frame | now the spine is short there is idle capacity for cloth to fill early | 4023 µs (−17 %) |

Each split turns a serial bar into internally-parallel work (`parallel_for`
across entities), which both shortens the critical chain *and* consumes the idle
cores the frame already had. Note the last row: the cloth version-choice that was
neutral in §3 now pays a little — once the spine is short enough to leave idle
capacity, moving cloth's work earlier fills it instead of fighting the chain. The
same lever, opposite verdict, depending on the state of the frame.

## 5. The result

| | baseline | optimised |
|---|---|---|
| makespan | 4866 µs | **4023 µs** (−17 %) |
| core utilization | 60 % | **64 %** |
| critical tail | cloth (post-flip) | none (ends at propagation/flip) |

The optimised frame is close to its critical-path floor: the remaining fat bars
(`AI`, `anim_graph`, `propagation`) are already internally parallel, and the head
(`networking`, `scripting`) is genuinely serial work. From here, faster means
*cutting work* — a smaller scene, cheaper systems — not rearranging, because the
cores are as full as a chain of this length allows.

## 6. What carries over to a real frame

- **Read utilization and dead time together.** Low dead time + low utilization =
  critical-path bound: shorten the chain. High dead time = scheduling bound:
  worker count, ordering, or contention. They point at different fixes.
- **Optimise the critical path, not the utilization number.** The trace makes
  the critical chain and its fattest bars obvious; that is where makespan lives.
- **Levers are frame-state-dependent.** A version choice or an ordering edge that
  is neutral (or harmful) on a long spine can pay once the spine is short.
  Measure each in place — the exercise did, keeping only what moved the number.

The mechanics behind the metrics (streaming aggregation, the measured critical
path, the dead-time and utilization definitions) are in
[guide.md §6.2](guide.md); the ranked tuner design this exercise motivates is in
[profiler-guided-optimization.md](profiler-guided-optimization.md).
