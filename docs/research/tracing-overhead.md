# Tracing overhead: this library vs. public profiler figures

Status: research note (untracked, not committed). Measured on this repo's `master`,
Release x64 (MSVC), on a 22-hardware-thread Windows 11 host. Wall-clock benchmarks on a
non-isolated desktop, so run-to-run spread is real — the numbers below are reported with
their spread and the conclusions are drawn only where the signal clears the noise.

Throughout, claims are tagged **[MEASURED]** (numbers produced here), **[CITED]** (published
third-party figures), or **[INFERENCE]** (my interpretation of the two).

---

## 1. What "our tracing" is, and the two costs to isolate

The trace/profiling instrumentation is gated by `TS_PROFILING` (default 1; `TS_PROFILING=0`
compiles the DOT dump, the runtime trace, and every busy/body/machinery counter out). Two
distinct costs exist:

- **(a) Compiled-in, disarmed** — `TS_PROFILING=1` but no `Graph_trace` attached. Documented
  cost: "one relaxed load + branch per task". This is what a normal (non-tracing) run of a
  `TS_PROFILING=1` binary pays just for the instrumentation being present.
- **(b) Armed** — the same binary with a `Graph_trace` attached during an explicit `--trace`
  run. Documented cost: ~2 `steady_clock` reads/task plus a per-run fold.

Our tracing is **per-task instrumentation** (cost scales with task count, not body size),
**armed only during explicit `--trace` runs** (not always-on), and **aggregating** — it folds
each run into Welford mean/variance + P² quantiles + per-worker histograms + per-edge counters
(O(nodes+edges) state), rather than streaming a per-event record to a UI. That regime
difference is the crux of section 6.

Methodology note: measurement (a) uses the `--bench` game-frame path, which runs the graph
**untraced** (`game_frame_stats` attaches no trace), so a `TS_PROFILING=1` vs `=0` delta there
is purely the compiled-in disarmed cost. Measurement (b) uses a throwaway `--armed` path
(added, measured, and reverted) that runs the *same* baseline graph traced vs untraced on the
same default scheduler and scale, so its delta is purely the armed cost. The tree is clean:
the throwaway source and the temporary `TS_PROFILING=0` vcxproj edit were both reverted and the
default Release rebuilt at `TS_PROFILING=1`.

---

## 2. Measurement (a): compiled-in disarmed cost — [MEASURED]

Default Release (`TS_PROFILING=1`) vs a Release built with `TS_PROFILING=0` added to the
Release|x64 `<PreprocessorDefinitions>` (nothing else changed — this cleanly isolates tracing,
unlike Shipping, which also flips `TS_SAFETY_CHECKS`). `--bench`, three runs each. Medians in
bold; the game-frame paths are untraced.

| Metric | `TS_PROFILING=1` (runs) | `=0` (runs) | Median 1 → 0 | Verdict |
|---|---|---|---|---|
| spin throughput, ns/op | 358, 349, 446 → **358** | 384, 367, 468 → **384** | 358 → 384 | within noise |
| game graph 1.0, µs/frame | 4104, 4094, 4158 → **4104** | 4108, 4103, 4096 → **4103** | 4104 → 4103 (−0.02%) | within noise |
| game graph .05, µs/frame | 419, 434, 580 → **434** | 419, 437, 437 → **436** | 434 → 436 | within noise |
| graph (8 empty nodes), ns/op | 5163, 5985, 5914 | 5145, 5933, 5699 | ~identical | within noise |

**Finding [MEASURED].** The compiled-in disarmed cost is **below the wall-clock noise floor**.
The game-frame graph at scale 1.0 differs by 0.02% between the two builds; the empty-task spin
series' median moved the *wrong* direction (the `=0` build measured nominally slower), which is
only possible if the true difference is dwarfed by the ±13% run-to-run spread. **[INFERENCE]**
This confirms the "one relaxed load + branch per task" claim: on a modern out-of-order core a
predictable-branch relaxed load per task is unmeasurable against real work. For a non-tracing
run there is effectively no reason to prefer the `TS_PROFILING=0` build for speed.

---

## 3. Measurement (b): armed cost — [MEASURED]

Default Release binary. Same baseline game-frame graph, run traced (a `Graph_trace` attached)
vs untraced, on the default global scheduler (22 hw threads), at three body scales. 166
tasks/run (the graph's true task volume, from `trace.tasks_per_run()` — well above the 20 node
count, since every access/pipe/coroutine task counts). Per-frame medians of 5 reps; three
independent measurement runs.

| Scale | Untraced µs/frame | Traced µs/frame | Δ µs/frame | Δ % | **ns/task** |
|---|---|---|---|---|---|
| 1.0 (fat bodies) | 4124 / 4088 / 4080 | 4108 / 4096 / 4119 | −16 / +8 / +40 | −0.4 / +0.2 / +1.0 | noise (−94…+238) |
| 0.05 | 412 / 424 / 412 | 427 / 439 / 422 | +15 / +15 / +10 | +3.7 / +3.6 / +2.5 | **92 / 92 / 63** |
| 0.0 (near-empty) | 168 / 168 / 171 | 178 / 176 / 182 | +10 / +8 / +11 | +6.0 / +4.7 / +6.5 | **61 / 47 / 67** |

**Finding [MEASURED].** Armed per-task overhead is **~50–90 ns/task** (central estimate
~75 ns/task). At the realistic scale-1.0 frame (fat ~38 ms bodies) it is **unmeasurable** —
166 tasks × ~75 ns ≈ 12 µs on a ~4100 µs frame is 0.3%, inside the ±40 µs frame noise, so the
per-task figure only resolves once bodies shrink toward empty.

**Cross-check [INFERENCE].** The documented "~2 `steady_clock` reads/task" predicts ~30–60 ns
(MSVC `steady_clock` is `QueryPerformanceCounter`, ~20–30 ns/read on this class of hardware).
Measured ~50–90 ns sits just above that band; the excess is the per-run fold (Welford + P² +
histogram + owner-attribution bridge + time-bucket distribution) amortized across 166 tasks,
plus the armed booking arithmetic. Same order of magnitude — the model holds.

**Caveat.** Armed cost scales with task **count**, not body size, so the *percentage* is
entirely a function of granularity: 0.3% at a real frame, ~3–4% at 20×-shrunk bodies, ~5–6% at
empty bodies. Quoting a single "% overhead" for instrumentation is meaningless without the
event rate — the ns/event figure is the portable one.

---

## 4. Public figures for other profilers — [CITED]

Classification: **instrumentation** = per-event cost that scales with event count;
**sampling** = fixed statistical rate (~1–5%), independent of event count.

| # | Profiler | Type | Per-event cost | % overhead | Source |
|---|---|---|---|---|---|
| 1 | **Tracy** | instrumentation | **2.25 ns/zone** (start+end; ~1.1 ns/event) | — | Tracy manual (`tracy.tex`), primary [1] |
| 2 | **Unreal Insights** | instrumentation | "some ns" (unquantified) | named events "several %" → ~20%; callstack channel "double-digit %" | Epic docs (qualitative) + practitioner [3][4] |
| 3 | **Optick** | instrumentation | ~30–60 ns/event (third-party est.) | — | repo "super-lightweight" only; third-party [5][6] |
| 4 | **Superluminal** | sampling | — (8 kHz Win / 10 kHz Xbox sampler) | none published | vendor [7] |
| 5 | **Intel VTune** | sampling | — | **~2%** EBS @1 ms; **~5%** user-mode @10 ms | Intel docs, primary [8][9] |
| 6 | **Linux perf** | sampling | — | **~1–5%** @1000 Hz default | Gregg + practitioner [10][11] |
| 7 | **RAD Telemetry** | instrumentation | no ns published (">1M zones/s") | "extremely low" | RAD changelog (qualitative) [12] |
| 8 | **Perfetto / Chrome** | instrumentation | **~285–630 ns/event** enabled; **1–2 ns** disabled | — | Perfetto docs, primary [13] |

Where a hard number does **not** exist, say so rather than guess: Unreal Insights has no
official per-event ns (only % frame-time bands, and the sharp ones are third-party); Optick's
only number is a third-party estimate; Superluminal publishes no overhead %; RAD Telemetry
publishes no per-zone ns. The cleanest primary per-event anchors are **Tracy (2.25 ns/zone)**
and **Perfetto (285–630 ns/event)** — they bracket the instrumentation spectrum by ~2 orders of
magnitude (RDTSC+assembly vs protobuf track-events).

---

## 5. Head-to-head — [MEASURED] vs [CITED] + [INFERENCE]

Our armed figure normalized: **~75 ns/task ≈ ~75 ns/"event"** (a task is our event unit; there
is no separate start/end record — we fold both timestamps into the aggregate in place).

| Profiler | Type | Per-event | Ours (~75 ns/task) is… | Why [INFERENCE] |
|---|---|---|---|---|
| Tracy | instr. | ~1.1 ns/event | **~65× more expensive** | Tracy: RDTSC (~5–10 ns) + assembly + push to a lock-free queue, serialize later. Ours: 2× QPC reads (~20–30 ns each) + inline statistical fold, no queue/serialize. |
| Perfetto/Chrome | instr. | 285–630 ns/event | **~4–8× cheaper** | Perfetto builds a protobuf track-event per call; ours does no record construction. |
| Optick | instr. | ~30–60 ns/event | **comparable** (~1–2×) | Same regime (game-frame instrumentation), similar clock-read-bound cost. |
| Unreal Insights | instr. | % bands: several %→~20% | **comparable-to-cheaper** in % terms | Ours adds 0.3% at a real frame, ~3–6% only at empty bodies; UI named events "several %"→~20% always-on. |
| VTune / perf | sampling | ~1–5% flat | **different regime** | Their % is fixed by sample rate; ours scales with task count. At real granularity ours (0.3%) is *under* their band; at empty bodies (~6%) it's above. |

**Disabled/compiled-out comparison [MEASURED]+[CITED].** Our disarmed compiled-in cost
(unmeasurable, "load+branch") matches the "off" cost these instrumentation profilers advertise:
Tracy/Perfetto disabled `TRACE_EVENT` is 1–2 ns. Parity there.

---

## 6. Honest context: why the per-event comparison is not apples-to-apples

**[INFERENCE], the important caveat.** Our tracing is a *different product* from Tracy/Insights,
so the ~65×-vs-Tracy figure should not be read as "65× worse profiler":

1. **Armed only, offline.** Ours arms exclusively during an explicit `--trace` run and produces
   an average-frame SVG. Tracy/Insights are built to run **always-on in a live game**, streaming
   a per-frame timeline you can scrub. We cannot show a single anomalous frame; they can. We
   pay nothing the 99.9% of the time we are not tracing (section 2); an always-on profiler pays
   its per-event cost every frame it is enabled.
2. **Aggregate, not transport.** We fold each run into O(nodes+edges) statistics in-process —
   no wire format, no serialization, no UI stream, no per-event record storage (O(events)
   memory). Tracy/Insights/Perfetto pay a wire-format + transport + storage + UI-streaming cost
   that dominates their per-event budget and that we simply do not incur. That is *why* Perfetto
   (protobuf) is 4–8× our cost and Tracy (hand-tuned lock-free queue + deferred serialize) is
   still ~65× cheaper per event: Tracy spent enormous effort making the capture path a handful
   of RDTSC-clocked instructions precisely because it must survive being always-on.
3. **Clock choice.** Our 2× `QueryPerformanceCounter` reads are the bulk of the per-task cost.
   Tracy uses RDTSC directly (a fraction of QPC). Adopting RDTSC would close much of the gap but
   trades portability/monotonicity guarantees — not worth it for an armed-only offline tool.
4. **Event granularity.** Our "event" is a whole task (a scheduled unit of real work), not a
   fine-grained scope a user sprinkles by the thousand inside a function. 166 events/frame is
   two orders of magnitude below a Tracy-instrumented frame's zone count, so our *frame-level*
   overhead stays small even at a higher per-event cost.

**Net [INFERENCE].** Where it matters (a real frame), our armed overhead is 0.3% and our
disarmed overhead is zero. Per event we are far dearer than Tracy and moderately dearer than
Optick, but cheaper than Perfetto — and the gap to Tracy is bought back by aggregating instead
of transporting: we deliver mean/variance/quantiles/critical-path directly, with no capture
buffer, serializer, or viewer in the loop. The honest summary is "cheap because rarely armed
and never transported", not "cheapest per event".

---

## 7. Methodology caveats

- Wall-clock on a shared 22-thread desktop: run-to-run spread is ±10–13% on the throughput
  microbenchmarks and ±1% on the fat game-frame. Every conclusion above is drawn only where the
  signal clears that spread; where it doesn't (measurement a, and armed at scale 1.0) the honest
  reading is "below noise", not a point estimate.
- Fat vs cheap bodies: the armed % is dominated by body size. The ns/task figure is the
  size-independent one; the % is quoted only paired with a scale.
- Armed cost scales with task **count**, not body size or wall time — a frame with 10× the tasks
  pays 10× the tracing, regardless of how long each task runs.
- Public figures are cited as published; hardware, workload, and event granularity differ across
  every source, so cross-profiler ns comparisons are order-of-magnitude, not exact.

---

## 8. Sources

- [1] Tracy manual, `tracy.tex`: "The impact of profiling is only 2.25 nanosecond per zone
  (this includes two events: start and end of a zone)." —
  https://raw.githubusercontent.com/wolfpld/tracy/master/manual/tracy.tex
- [3] Epic, Developer Guide to Tracing (qualitative only) —
  https://dev.epicgames.com/documentation/en-us/unreal-engine/developer-guide-to-tracing-in-unreal-engine
- [4] PerfGuard Unreal Insights tutorial ("several percent", "double-digit-percent") —
  https://getperfguard.com/tutorials/unreal-insights
- [5] Optick repo — https://github.com/bombomby/optick
- [6] Third-party profiler comparison (Optick ~30–60 ns; Tracy 5–50 ns range) —
  https://www.pistack.xyz/posts/2026-06-21-cpp-performance-profiling-tracy-optick-remotery-microprofile/
- [7] Superluminal features (8/10 kHz sampler) — https://superluminal.eu/features/
- [8] Intel VTune, HW event-based sampling (~2% @1 ms) —
  https://www.intel.com/content/www/us/en/docs/vtune-profiler/user-guide/2023-0/hw-event-based-sampling-collection.html
- [9] Intel VTune, user-mode sampling/tracing (~5% @10 ms) —
  https://www.intel.com/content/www/us/en/docs/vtune-profiler/user-guide/2023-0/user-mode-sampling-and-tracing-collection.html
- [10] Brendan Gregg, perf CPU sampling (1000 Hz default) —
  https://www.brendangregg.com/blog/2014-06-22/perf-cpu-sample.html
- [11] perf overhead practitioner note (~1–5%) —
  https://oneuptime.com/blog/post/2026-01-15-analyze-performance-perf-ubuntu/view
- [12] RAD Telemetry history/changelog (">1M zones/s", qualitative) —
  https://www.radgametools.com/telemetry/history.html
- [13] Perfetto track-event performance (285–630 ns enabled, 1–2 ns disabled) —
  https://perfetto.dev/docs/instrumentation/track-events
