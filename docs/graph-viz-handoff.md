# Handoff: static-graph DAG visualization / frame profiling

Mission brief for a dedicated session. Goal: **profiling and visualization of the
frame composition** — primarily a visualization of `Static_task_graph`'s DAG
(structure), plus per-run timing capture (behavior) so the picture can show what
actually happened: node durations, worker lanes, the measured critical path,
utilization. Read `CLAUDE.md` first (the authoritative internal doc — layout,
style, verification); this file adds what CLAUDE.md doesn't carry: the mission,
the code pointers, current measurements, and session-learned practicalities.

## 1. Why now / what exists

The `game_frame` sample just gained a utilization printout
(`serial_budget / workers` vs measured ms/frame) and went through a
hand-derived critical-path analysis (24% → 43% → 54% by restructuring). All of
that analysis was done BY HAND in chat — the graph knows its own structure and
could report it. The natural deliverables, smallest first:

1. **DAG dump** (compile-time structure): nodes, edges, per-edge provenance
   (conflict-derived vs explicit `after`/`before`), per-node declared accesses
   (object, mode). Output: Graphviz DOT first (`dot -Tsvg` renders everywhere);
   an ASCII adjacency summary is a cheap byproduct for consoles.
2. **Per-run timing capture** (runtime): per node — dispatch time, start, end,
   worker index. Output: chrome://tracing / Perfetto JSON (`"traceEvents"`
   array; ph "X" duration events keyed by tid = worker lane) — gives a frame
   timeline with zero custom UI work.
3. **Derived reports**: measured critical path (longest path over the DAG using
   measured durations), per-node slack, lane occupancy / utilization — the
   things computed manually this session (see §4 for the hand-derived versions
   to validate against).

Roadmap synergies (docs/TODO.md): **2.2** ambiguity detection needs the same
edge-provenance data — record provenance while you're in `compile()` and 2.2
becomes cheap later; **2.4** profiler-guided graph optimization consumes exactly
this timing data; the utilization printout in `game_frame` is the first consumer
of a real report.

## 2. Code pointers (what's introspectable today, what's missing)

- `include/ts/static_task_graph.h` + `src/static_task_graph.cpp` (~200 + ~470
  lines). `add_node(fn, objs...)` returns `Graph_node` (build-time handle:
  `after`/`before`/`priority`/`set_inline`). `compile()` derives edges from
  access conflicts (shared object + ≥1 writer, declaration-index tiebreak) plus
  explicit edges, dedups, Kahn cycle-check. `execute()` runs via atomic
  indegree; node blocks + `Run_state` built at compile, re-armed per run.
- **Edges**: stored as successor lists + indegree counts after `compile()`.
  **Provenance (derived-vs-explicit) is NOT recorded** — the derivation
  discards it. Recording it at derivation time is step one.
- **Node identity: there are NO node names.** `add_node` takes only functor +
  objects. Visualization needs labels → API decision: a `Graph_node::name(str)`
  fluent setter (matches `priority()`/`set_inline()` house style) with an
  auto-generated fallback (`node<index>`). Flag this API addition to the author
  before building on it.
- **Object identity**: nodes hold pipe indices into an address-sorted
  `distinct_pipes_`; objects also have no names — same naming question, likely
  a per-graph `name(obj, str)` registration or names on the edge labels only
  (object index + mode may be enough for v1).
- **Timing hooks**: nodes dispatch through the block's raw path and complete
  via a persistent `on_complete` fn-ptr (`node_complete` in
  static_task_graph.cpp). Start/end stamps belong around the node body
  invocation; worker index from `ts::detail::current_worker_index` (TLS,
  `scheduler.h`). Keep capture **opt-in** (a per-graph flag or a
  compile()/execute() option) and zero-cost when off — house rule: flag any
  hot-path cost. Timing storage: a per-run array indexed by node id (the
  `Run_state` is the natural home; it is re-armed per run).
- `include/ts/scheduler.h`: `worker_count()`, `current_worker_index` — for
  lanes and utilization math.

## 3. The fixture: `sample/game_frame.cpp`

The visualization's target and test subject. 18 systems + a publish node over
~20 guarded stores; all three primitives in natural roles:

- `Guarded` stores, single writer each; three parallel gameplay systems
  (combat/economy/quests); heavy systems internally parallel via
  `ts::parallel_for` (8 `cost_slices` — an artifact of mocking cost with spins,
  documented in-file).
- `Versioned<Transforms>`: early systems + audio read last frame's version
  (declared before the flip), late systems the fresh one. The flip is
  `ts::publish_fn` + `flip.after(propagation)`.
- `Deferred<Draw_lists>`: culling/particles/UI stage draw batches grant-free;
  `submit.after(culling).after(particles).after(ui)` commits. The explicit
  edges are exactly the grant-free orderings — provenance would render these
  distinctly.
- Exports (no header; consumers forward-declare): `game_frame_stats(frames,
  scale, avg_ms&, serial_ms&, transform0&)`, `run_game_frame_sample(frames,
  scale)`.

## 4. Current measurements (validate the tool against these)

Machine: 12 workers. Scale 1.0, 20 frames, Debug:

```
4.15 ms/frame; 26.90 ms of work across 12 workers -> ideal 2.24 ms/frame, 54% utilization
27000 draw commands staged by 3 producers (1350/frame, deterministic)
```

Hand-derived critical path (~2.6 ms; wall = budget/8 for sliced nodes):

```
t=0.0  input (0.1 serial)
t=0.1  net (0.5 serial)
t=0.6  combat | economy | quests (0.7 serial, parallel trio)
t=1.3  ai (1.5 -> ~0.2 wall)      [nav done ~0.3, parallel from t=0]
t=1.5  animation (3.0 -> ~0.4)    [physics 4.5 finishes under it]
t=1.9  propagation (1.0 -> ~0.15)
t=2.05 flip -> culling (~0.2) -> submit (~0.33)
```

Off-path: nav (2.5 parallel, t=0), audio (1.5 serial, t≈0, reads prev
transforms — deliberately moved off the post-flip tail), streaming (1.5),
cloth (2.0), particles (2.0), ui (0.5), debug (0.2). A correct measured-CP
report should reproduce this shape; utilization gap vs the 86% ceiling is slice
granularity + scheduling overhead (~70 slice-tasks/frame on 12 workers).

## 5. Constraints & author preferences (hard-won this session)

- **Comment tone rule** (CLAUDE.md, enforced): technical, concise, no all-caps
  emphasis, no history narration ("replaces the old X"), no repo
  meta-commentary. Samples are single self-contained `.cpp`, no headers.
- **Perf discipline**: profiling must be opt-in, zero-cost when off; flag any
  allocation/contention added to hot paths. The graph run currently allocates
  only its `done` handle — keep it that way when capture is off.
- **Verification gate** for anything touching graph/scheduler paths: clean
  MSBuild Debug x64 (`"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" macrame.vcxproj /p:Configuration=Debug /p:Platform=x64`)
  + `x64\Debug\macrame.exe --tests` → **369 checks, 0 failures**; CMake
  presets `windows-msvc`/`windows-clang-cl` for clean cross-compiler builds
  (stale objects have masked MSVC-only breakage before — clean builds); TSan
  via `wsl.exe -e bash -c "cd /mnt/c/src/task_system && CXX=clang++-21 bash tsan/run.sh"`
  → "tsan: done (no races)". CI: 3 jobs, all green at handoff; `gh` CLI is
  installed+authed (`gh run list`, `gh run watch <id> --exit-status`; PATH may
  need `/c/Program Files/GitHub CLI`).
- **Git discipline**: commit with EXPLICIT paths (`git commit -- <files>`) — a
  bare commit once swept another session's staged deletion and broke CI.
  Push may need `GIT_EXEC_PATH="/c/Program Files/Git/mingw64/libexec/git-core"`.
  End commit messages with the Co-Authored-By line per CLAUDE.md.
- **Environment quirks**: `.md` files must be edited with the Edit/Write tools
  only (PowerShell `Get-Content|Set-Content` round-trips corrupted UTF-8
  em-dashes; check `grep -cE "aooess|â€" <file>` → 0). Intermittent `EPERM` on
  file rename = a lingering `cl.exe` or VS re-indexing — kill `cl` / retry.
  Debug `abort()` exits with code 3 and pops a CRT dialog (death-test children
  suppress it; a fatal in the main process does not). `Start-Process`'s
  `$p.ExitCode` reads `$null` on success — use direct invocation or `cmd /c`.
- **Vcxproj is manually maintained** — new files must be added to
  `macrame.vcxproj` (+ `.filters`) AND `CMakeLists.txt` AND (if compiled on
  Linux) `tsan/run.sh`'s `SRC` list.
- **Library name is now decided**: `macrame` (namespace stays `ts::`) — see
  `docs/naming.md`; don't invent branding.

## 6. Coordination

The main session is mid **code review** (top-down; Deferred/Versioned public
API next; running log in its scratchpad). To avoid collisions: keep this work
scoped to NEW files (a `tools/` or `include/ts/` addition for the dump/trace
writers) plus minimal, clearly-marked hooks in `static_task_graph.{h,cpp}` and
the sample; commit early with explicit paths; the API additions (node naming,
provenance, capture option) should be presented to the author as a short design
before implementation — the house pattern is propose-options-with-tradeoffs
first (CLAUDE.md).

## 7. Suggested first moves

1. Design note to the author: node/object naming API, capture opt-in surface
   (`execute` option vs graph flag), output formats (DOT + Perfetto JSON),
   provenance recording in `compile()` (shared with TODO 2.2).
2. Implement the DOT dump (structure only, no timing) — smallest complete
   deliverable, immediately useful on `game_frame`.
3. Add opt-in per-node timing + the Perfetto trace writer; validate the
   measured critical path against §4.
4. Wire a `--trace` (or similar) mode into the sample/driver so the frame
   composition can be regenerated in one command.
