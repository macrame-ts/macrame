# Third-party tracing integration

Design study: pairing the aggregated `Graph_trace` with an external per-event
tracer. Decision document — nothing here is implemented.

## 1. Why

`Graph_trace` deliberately stores no samples: O(nodes+edges) streaming
aggregates, rendered as the average run. That answers "what does this frame
cost, statistically" and it can stay attached for thousands of runs — but it
cannot answer "what happened in frame 1234": the outlier, the one-off stall,
the exact interleaving that produced a P95 tail. That job needs per-event
storage, and building event storage ourselves would abandon the design's core
economy. The alternative is a seam that forwards our existing instrumentation
points to a mature external tracer when one is compiled in: the aggregated
trace remains the always-on statistical view; the external tool provides the
concrete-run timeline, live inspection, and outlier drill-down. The two views
are complementary — the trace says *which* frames are interesting, the tracer
shows *one of them* in full.

## 2. The seam

A single hook header, `include/ts/detail/trace_backend.h`, following the
`graph_introspect.h` pattern: consumers include it unconditionally; all gating
lives inside. Placement in `detail/` rather than `tools/` because the hooks
land in core translation units (`scheduler.cpp`, `static_task_graph.cpp`,
`parallel_for.h`) — `tools/` headers are for the dev-only visualization stack
that only `static_task_graph.cpp` touches. Backend adapters (the Tracy or
Perfetto mapping) live in `tools/`, selected by a macro:

- `TS_TRACE_BACKEND` unset (default): every hook is an empty inline — zero
  cost, no dependency, identical to today.
- `TS_TRACE_BACKEND_TRACY` (or `_PERFETTO`): the hooks expand to the vendored
  backend's API. The backend sources sit under `third_party/`, added to the
  build only in this configuration. The library proper never requires them —
  the no-external-dependencies rule holds for every default build.

Hook points, all places where instrumentation already exists:

| hook | site | maps to |
|---|---|---|
| worker start | `Worker_thread::main` (`worker_thread.cpp`) | thread name ("ts worker N") |
| task run | `Scheduler::run_task` (`scheduler.h`) | per-task zone; generic name, or the owning node's via `current_trace_owner` |
| node body | `Trace_busy_scope` site in `run_graph_node` (`static_task_graph.cpp`) | named zone (`ts::Named`), priority → zone colour |
| graph run | `execute()` begin / `Trace_stamps::fold` settle (`static_task_graph.cpp`) | frame mark (named per graph) |
| `parallel_for` slice | helper body (`parallel_for.h`) | zone named from the inherited owner |
| pipe / queue locks | `pipe_acquire` (`guarded.cpp`) | lock instrumentation (Tracy only; requires wrapping the mutex type, so this hook is optional and intrusive) |
| per-run scalars | after the fold | plots: makespan, machinery share, utilization |

The per-task and per-slice hooks ride the same armed/disarmed discipline as
the busy counters where possible; zone emission itself is the backend's cost
(see per-candidate numbers). Estimated seam size: ~100–150 lines for the
header, ~50–80 per backend adapter.

## 3. Candidates

### Tracy (v0.13.1, 2025-12; BSD-3-Clause; active)

Integration: vendor `public/` and compile one translation unit
(`TracyClient.cpp`) when enabled; client requires C++11, TLS, and — verified
against the current client sources — contains no `throw`/`try`/`catch`, so it
builds under our `/EHs-c-` + `_HAS_EXCEPTIONS=0` configuration. The C API
(`TracyC.h`) takes runtime `const char*` zone names, which fits our stable
per-node `ts::Named` names without macro gymnastics; `TracyCZoneN` /
`___tracy_emit_zone_begin` carry source-loc + name + colour. Thread naming,
`FrameMarkNamed` (one mark per graph), `TracyPlot`, and `TracyLockable` cover
every seam row. `TRACY_ON_DEMAND` buffers nothing until a viewer connects —
the shipped-binary story: near-zero cost until someone attaches.

Cost: ~2.25 ns/zone amortized (manual's benchmark), 10–50 ns isolated — at
~153 tasks/frame plus ~30 node zones this is single-digit µs/frame, below our
armed-trace cost. The client adds a worker thread + socket and buffers events
in its own rpmalloc heap (MBs while a capture runs).

What we get: live remote timeline of concrete runs; per-zone statistics,
full duration histograms, and "find zone" queries — which subsumes two
`tracing-ideas.md` *consider* items (per-node histogram, exemplar-run capture)
without us storing a sample; lock-contention analysis on the pipe mutex;
callstack sampling alongside instrumentation; memory zones later if wanted.
What stays ours: the average-run view, criticality frequency, CPM/slack, the
B/M/I overhead split — Tracy has no cross-run statistical rendering.

### Perfetto SDK (v57.2, 2026-07; Apache-2.0; active)

Integration: two amalgamated files (`perfetto.h` / `perfetto.cc`, release
artifacts, ~100 kLOC — a heavy TU), C++17, MSVC supported (`/bigobj`,
`/permissive-`, `WIN32_LEAN_AND_MEAN`); written Chromium-style without
exception use. Track events map from the same seam; in-process tracing
session writes a protobuf `.pftrace`.

Cost: 285–630 ns/event enabled, 1–2 ns disabled (their published numbers) —
an order of magnitude above Tracy per event, still small at our volumes.

What we get that Tracy lacks: a *shareable artifact* (`.pftrace` opens in
`ui.perfetto.dev` for anyone, no viewer build) and `trace_processor` — SQL
over the trace, scriptable analysis. No live view in-process mode, no lock
instrumentation equivalent, no per-zone histogram UI comparable to Tracy's.

### Optick — rejected

MIT, but effectively unmaintained: last release 1.4.0 (2022-05), last push
2024-05. Same niche as Tracy with a weaker viewer and no cost/maintenance
advantage. (Remotery and microprofile fall the same way — smaller feature set,
same niche. RAD Telemetry is commercial. Raw ETW/TraceLogging is Windows-only
and pushes all presentation onto WPA — tooling-heavy for no capability gain
over Tracy.)

### Vendor-marker tier (not sample-storing backends — near-free once the seam exists)

- **Intel ITT** (`ittapi`): `__itt_task_begin/end` from the same hooks makes
  our tasks visible in VTune's timeline next to its hardware analysis.
- **Superluminal Performance API**: small free API (BeginEvent/EndEvent,
  thread names); our zones appear in Superluminal's sampling timeline.
- **PIX events** (`WinPixEventRuntime`): PIXBeginEvent/PIXEndEvent; relevant
  when a consumer profiles CPU+GPU frames in PIX.
- **NVTX** (Apache-2.0): `nvtxRangePush/Pop`; Nsight Systems timeline.

Each is a ~30-line adapter behind the same macros; none stores samples itself
— they decorate an external profiler's capture. Worth adding opportunistically
once the seam lands.

## 4. Verdict

Tracy is the clear primary: BSD-3, actively maintained, single-TU vendoring
with no build-system footprint, a C API that fits our stable name pointers,
verified to build without exceptions, per-zone cost an order of magnitude
under the alternative, `TRACY_ON_DEMAND` for always-compiled-in builds, and a
viewer whose per-zone statistics absorb two items we would otherwise build.
Perfetto is not a competitor but a different deliverable — a shareable,
SQL-queryable trace file; add it only if a CI-artifact or cross-team-sharing
workflow materializes, through the same seam. Optick is out (stale). So: not
2–3 candidates to choose among — one winner, one conditional second, and a
cheap vendor-marker tier that the seam makes nearly free. The decision that
matters is the seam itself; the first backend is Tracy.

## 5. Sources

- Tracy repository: https://github.com/wolfpld/tracy
- Tracy releases (v0.13.1): https://github.com/wolfpld/tracy/releases
- Tracy client sources (exception check): https://github.com/wolfpld/tracy/tree/master/public
- Tracy manual (per-zone benchmark): tracy.pdf in the release artifacts
- Tracy FAQ (isolated zone cost): https://github.com/davidwed/tracy/blob/master/FAQ.md
- Perfetto Tracing SDK: https://perfetto.dev/docs/instrumentation/tracing-sdk
- Perfetto track events (overhead numbers): https://perfetto.dev/docs/instrumentation/track-events
- Perfetto releases (v57.2): https://github.com/google/perfetto/releases
- Optick: https://github.com/bombomby/optick
- Intel ITT API: https://github.com/intel/ittapi
- Superluminal Performance API: https://superluminal.eu/docs/documentation.html
- WinPixEventRuntime: https://devblogs.microsoft.com/pix/winpixeventruntime/
- NVTX: https://github.com/NVIDIA/NVTX
