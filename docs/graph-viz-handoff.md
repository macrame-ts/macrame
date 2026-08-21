# Static-graph DAG visualization / frame profiling

What the library can report about a `Static_task_graph`: its structure (the compiled DAG)
and its behavior (per-node timing, the measured critical path, utilization). This started
as a mission brief for a dedicated session; the work landed, so what follows is the status
and the one piece that was never built.

## 1. Status

Shipped, all of it under `TS_PROFILING` (defined in `static_task_graph.h`, default 1;
`=0` compiles the dump and the trace out while the parameters stay in the signatures):

- **DAG dump.** `compile(DOT_path)` writes the compiled graph as Graphviz DOT through the
  header-only `tools/dot_writer.h` (`ts::tools::DOT_writer`). Edge provenance is recorded at
  derivation and rendered as line style — solid for explicit `after`/`before`, dashed for
  conflict-derived — with the object and modes as an SVG hover tooltip (`physics: RW->RO`);
  an edge that is both renders explicit-styled with the conflict tooltip. This is the
  provenance data TODO 2.2 (ambiguity detection) also wants.
- **Node and object identity.** `ts::Named` (`include/ts/named.h`) is the one debug-identity
  type: a leading argument on `add_node` and a required leading constructor argument on
  `Guarded`/`Versioned`. A literal names the thing; `{}` captures the call site through
  `std::source_location` and labels `file:line`. Kept when `TS_DEBUG_NAMES` is on.
- **Runtime trace.** `set_trace(tools::Graph_trace*)` (requires `compile()`; nullptr
  detaches) attaches an aggregating trace: `Graph_trace` (`tools/graph_trace.h`) streams
  per-node Welford mean/variance, P² P50/P95, min/max, per-worker histogram and dispatch
  wait, per-edge mean meet point, plus global makespan and critical work — O(nodes+edges)
  state, no per-run sample buffer. The graph's own involvement is a `Trace_stamps` object
  with a handful of mark calls and one fold at run settle.
- **Derived reports.** The measured critical path is a per-run backward walk through the
  binding predecessor, aggregated into a criticality frequency rather than a single path;
  a CPM pass at `write_SVG` time yields per-node slack, so divergence between the two
  identifies scheduling-bound versus dependency-bound structure. Core utilization comes
  from the scheduler's time-bucketed busy counters, and the framework-overhead split
  (`M = busy - B`) from the body/machinery accounting — see
  `docs/profiler-guided-optimization.md`.
- **Output.** `Graph_trace::write_SVG(path)` renders the average run directly: median bars
  packed into anonymous concurrency rows, edges between them, critical dead time as
  full-height bands, and formatted hover tooltips from a self-contained overlay script
  embedded in the SVG. No external viewer, no post-processing step.
- **Driver.** `macrame_playground --dot [path]` dumps the `game_frame` structure;
  `--trace [n]` runs the sample traced and writes the SVGs alongside the DOT.
  `show_graph.bat [file.dot]` at the repo root renders and opens.

## 2. The one open item: export to an external viewer

Per-run event export (chrome://tracing / Perfetto JSON: a `"traceEvents"` array of `ph:"X"`
duration events keyed by `tid` = worker lane) was in the original brief and was never built.
The trace deliberately went the other way — aggregate statistics, no per-run sample
retention — so an exporter is an addition, not a refactor: it needs a per-run event sink
next to the aggregating fold, which is a real cost decision rather than a formatting one.
`docs/third-party-tracing.md` covers the adjacent question (vendoring a Tracy or Perfetto
backend behind trace hooks) and is the place to start; the two deliverables are different —
a shareable timeline for a specific run, versus the average-run picture `write_SVG` gives.

## 3. The fixture: `sample/game_frame.cpp`

A mock game-engine frame of ~30 systems over a comparable number of guarded stores, built
in three compositions from the same system bodies: `baseline`, `optimised` (the worked
exercise in `docs/example-frame-optimization.md`), and `graph_free` (the same frame with no
`Static_task_graph`, the measurement in `docs/coroutine-first.md` §10.5). It exercises every
primitive in a natural role — `Guarded` stores with a single writer each, `Versioned<Transforms>`
so the render pipeline reads last frame's transforms and overlaps the sim, `Deferred<Draw_lists>`
so culling/particles/UI stage draw batches grant-free, `ts::parallel_for` inside the heavy
systems, and a fire-and-forget streaming coroutine. The explicit `after` edges are exactly
the grant-free orderings, which is what makes it a good provenance test: those render dashed
where the conflict-derived ones do not.

Exports (no header - consumers forward-declare): `game_frame_stats`, `game_frame_free_stats`,
`game_frame_draw_count`, `run_game_frame_sample`, `dump_game_frame_dot`, `trace_game_frame`,
`stress_game_frame_optimised`.

Current measurements are not kept here — they move with the machine and the workload. The
figures of record are in `docs/example-frame-optimization.md` (the baseline-to-optimised
comparison, with its worker count and scale stated) and `docs/profiler-guided-optimization.md`
(the overhead metric and its ground truth).

## 4. Constraints that still govern this area

- **Profiling is opt-in and zero-cost when off.** A graph run allocates only its `done`
  handle with capture detached; the scheduler's busy counters are armed only while a traced
  run's window is open, and disarmed cost is one relaxed load and a branch. Anything added
  here is measured before it lands.
- **The gating lives in the tools headers, not in the run logic.** `static_task_graph.cpp`
  includes `tools/graph_introspect.h` unconditionally; every `TS_PROFILING` check and null
  check is inside the tools headers, which stub to no-ops when it is 0. Keep new hooks that
  shape: no `#if` in the run path.
- **Verification** for anything touching graph or scheduler paths: the suite green under
  MSVC and clang-cl (CMake presets `windows-msvc` / `windows-clang-cl`; stale objects have
  masked MSVC-only breakage before, so build clean), plus TSan via `tsan/run.sh` on a Linux
  host.
- **New files** must be added to the right `.vcxproj` and its `.filters` AND to
  `CMakeLists.txt` — CI builds via CMake, so a file present only in the `.vcxproj` links
  locally and fails CI. See CLAUDE.md's VS Project section.
