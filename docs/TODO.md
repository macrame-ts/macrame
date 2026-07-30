# Roadmap

Internal working roadmap. Public-facing docs: [guide.md](guide.md) (user guide),
[design.md](design.md) (rationale). Deep dives: [task-internals.md](task-internals.md),
[deferred-versioned-state.md](deferred-versioned-state.md),
[command-buffer-design.md](command-buffer-design.md),
[task-systems-comparison.md](task-systems-comparison.md).

Legend: `[ ]` open · `[~]` partial/WIP · `[S]` shelved (designed, not scheduled).
Priority: **P0** = pre-public · **P1** = soon · **P2** = later · **P3** = someday.
Fuller notes for the larger items are in [§ Detail](#detail); exhaustive research
survives in git history (this file was condensed 2026-07).

---

## Shipped

All layers built, tested (**504 checks** as of 2026-07, TSan/ASan clean, subprocess
death tests), documented, and CI-gated.

- **Task core** — `launch`/`task`/`after`, `then`, `when_all` (apply-style + void-drop +
  move-only), `const& sync()` + `take()`, cooperative cancellation (+ `Cancel_callback` +
  trailing-`Cancellation_token` body early-out), reusable tasks (hardened — two reuse×retraction
  races fixed: token-immutable-per-block + capture-generation-before-`fetch_sub`), nested tasks,
  retraction / deep-retraction, inline dispatch on all four routes.
- **Guarded / access** — `access` (opportunistic) + `async` (scheduled) verb split, const-ness
  deduction, multi-object `ts::access`/`ts::async` (canonical-order acquire, deadlock-free),
  per-object reader/writer pipe. (MSVC fix: `Async_result` invocable-guard so a rejected
  overload's return type doesn't hard-instantiate `invoke_result_t`.)
- **Static graph** — access-conflict-derived edges + explicit `after`/`before`, per-node
  mode-aware acquire (objects free in the gaps), object handoff between accessors, inline nodes,
  nested tasks in nodes, allocation-free re-runs.
- **Scheduler (M2 stages 1–4)** — per-priority lock-free MPMC queues, Vyukov eventcount,
  per-worker Chase-Lev deques + stealing, low-starvation valve + spinner cap; idle policies
  (`spin` / `spin_then_block` default / `handoff`).
- **Coroutines** — awaitable `Task`, `co_await ts::read_only/read_write` guards, per-segment
  access-context threading, suspension detector, bounded resume trampoline.
- **Deferred / Versioned** — staged-write layer (`Journal`/`Recorder`/`Parallel_recorder`),
  three-phase publish, replay/copy/overwrite resync, single-publisher enforcement.
- **Allocation** — `launch` 1 · `async` 1 · `then` 3.86 · `when_all` 6.15 allocs/op
  (intrusive refcount, per-dispatch `submit_closure` kill, `when_all` collapse).
- **Build / public-prep** — MIT license, whole lib in `ts::`, CMake + presets, CI
  (MSVC / clang-cl / Linux-TSan), Shipping config (`TS_SAFETY_CHECKS=0`), v0.1.0,
  `ts.h` umbrella, `.clang-format`, `CONTRIBUTING` + issue/PR templates.

---

## Open work (index)

Reference items as `area.item` (e.g. `3.1` = ambient scheduler). Numbers are stable
IDs — when an item is done, mark it, don't renumber.

1. **Guarded / access**

   *Standing priority (author, 2026-07): anything that strengthens the harness
   is P1 by default — the runtime oracle is the library's signature safety
   property and its coverage gaps (§13.6 of [research-deepdive.md](research-deepdive.md))
   are where every comparable system's real-world failures concentrated.*

   1. `[~]` **(P1) Zero-alloc inline `access`** — void→done-sentinel, small-R→SBO handle, large/deferred→heap. [§D1]
   2. `[ ]` **(P1) Multi-object `ts::access` inline** — currently schedules like `ts::async` (documented); implement the inline fast path so single- and multi-object `access` match.
   3. `[x]` **(P1) Generic-lambda / `auto&` deduction — DONE (A3+A4, 2026-07).** Introspecting a generic lambda's parameter const-ness is impossible (templated `operator()`), but classification is not: the **rvalue-bindability probe** classifies each bare-arg position at declaration level — `const auto&`/`auto&&` binds an rvalue → read, `auto&` cannot → write — so generic lambdas deduce under the SAME "const = read" spelling rule as non-generic ones, no tags required. **A3**: every read position (deduced, probed, or tagged) invokes the body with `const T&` (`mode_ref`), so mutating under a read classification is a compile error — and the constraint/return-type machinery evaluates the mode gate FIRST (`accessor_mode`, `Accessor_result`) because probing a generic lambda's invocability against `const T&` deduces its return type, which instantiates the BODY — a hard error for a mutating body, not SFINAE. **Q1**: by-value / rvalue-ref resource params are `static_assert`-rejected wherever introspectable (a by-value copy silently discards writes); the generic `auto` by-value spelling is undetectable at declaration level — documented residual. Tags `ts::as_read_only(g)`/`as_read_write(g)` (names ratified; match the `Access` enum and coroutine guards) remain as the explicit-declaration style, the escape for an `auto&&` that must write, and permit conservative write-over-declaration (write tag over a `const T&` param serializes, legally). One unified builder per surface (`async_build_modes` / `fill_node_modes`) serves the deduced/probed/tagged tiers. Tests: probed single-object read-overlap/write-serialization + classification static_asserts, probed multi-object (incl. mixed spellings) + over-declared write, probed graph nodes; `#if 0` blocks document the compile-time rejections. 356→369 checks; clean MSVC + clang-cl + MSBuild, TSan clean.
   4. `[ ]` **(P1, raised 2026-07 — harness item) Access-check completeness** — clang-tidy "every public method calls `TS_CHECK_ACCESS()`" + a `Guarded_value<T>` for POD/no-method types. [§D2]
   5. `[ ]` **(P2) Adopted `Guarded`** — wrap an existing instance (borrow) via an `adopt` ctor tag; access key = borrowed address. **Ownership-transfer note (2026-07, T22):** an adopted object handed between owners needs an explicit re-bind ritual (Chromium's `DETACH_FROM_SEQUENCE` — the first access after detach re-binds the checker), NOT implicit adoption. Frame it as an escape hatch, discouraged for general use; the owned-`Guarded` path (in-place construction, harness-instrumented methods) stays the recommended default.
   6. `[ ]` **(P2) Sub-object / range harness** — grant on `S` covers `[&S, &S+sizeof)`, or a declarable `Region<T>`.
   7. `[ ]` **(P3) Standalone public `Pipe`** — expose the reader/writer pipe (UE `FPipe`); defer until a non-single-object use case appears.
   8. `[ ]` **(P1) `Access_options` vs `Task_options`** — revisit whether the two option aggregates share a base or stay split (flagged in `task.h`); resolve in the code-review API-stability pass.
   9. `[S]` **Checked accessor wrapper (`Granted<T>`) — designed, declined for now (author, 2026-07: "maybe never").** Functors could receive a pointer-like `Granted<T>`/`Granted<const T>` whose `operator->`/`*` runs `access_check` then hands out `T&` — zero-intrusion diagnostics for types without `TS_CHECK_ACCESS()` instrumentation (incl. PODs/third-party types; would subsume 1.4's `Guarded_value`). Semantics: checks are tautological inside the sanctioned body; the signal is stashed/escaped *handles* (post-release, cross-thread, across-suspension use faults at deref); raw-ref extraction (`T& r = *g`) still launders the check away, so method instrumentation stays strictly stronger for owned types. Trust tiers: 0 = none (scheduling stays correct — the harness is diagnostics, not correctness), 1 = wrapper, 2 = instrumented methods (+ clang-tidy). Implementation is cheap (`access_check(T*)`/`(const T*)` overloads already resolve the mode; `TS_SAFETY_CHECKS=0` → raw passthrough); the work is the third signature tier in the deduction machinery (by-value `Granted` exempt from the Q1 static_assert). Revisit only if instrumentation burden proves a real adoption blocker.

   10. `[ ]` **(P2) Whole-object granularity serialization** — think through the mitigation
       menu beyond current guidance (sharding/`Versioned` placement, sub-object grants via 1.6,
       split-along-the-seams doc patterns); [research-deepdive.md](research-deepdive.md) §13/§19
       maps how Bevy (per-component + filters) and Unity (per-container, escape attributes)
       evolved under the same pressure. Flagged by the 2026-07 research pass.
   11. `[x]` **(P1) Grant-generation validity check — DONE (2026-07).** The stale
       inherited-grant hole ([research-deepdive.md](research-deepdive.md) §13.6.5) is closed via
       a per-pipe **write-epoch with seqlock parity** (`Pipe::write_epoch`): bumped at write-grant
       acquire + release (under the pipe mutex, relaxed) and by +2 on a graph write handoff
       (which elides both pipe ops); reader traffic never bumps. Every `Access_context` entry
       declared under a pipe grant captures the epoch (all declaration sites threaded: single-
       object access/async, multi-object, graph nodes, coroutine pipe guards, `Versioned` front
       scopes); `Access_context::check` returns granted/stale/none and `access_check` fatals on
       stale with a dedicated diagnostic naming the fix (`ts::nested`/`add_nested`). One rule
       serves both modes: a write entry is valid while its window is open, a read entry until a
       writer acquires. Compiled out under `TS_SAFETY_CHECKS=0` (bumps, capture, compare all
       gated; the 8-byte pipe field stays for layout stability). Chosen over per-owner
       generations (refcount traffic, per-owner plumbing) and inherit-into-`nested`-only
       (arming-after-launch races the child's first check). Tests: epoch unit verdicts, read-era
       semantics, `stale_inherited_grant` death scenario; suite 470 checks green, Shipping
       compiles, Release+Shipping stress clean (no false positives across handoff /
       `parallel_for` / `Versioned` paths).
   12. `[x]` **(P1) Blocking-`sync()`-under-grant check — DONE (2026-07).** Two pieces, fully
       `TS_SAFETY_CHECKS`-gated: **(a) `TS_ENSURE(expr, message)`** (fatal.{h,cpp}) — the
       UE-`ensure` shape per author: evaluates `expr` once in both configs, yields its bool
       (`if (!TS_ENSURE(...))` recovers); on failure bumps `ts::ensure_failure_count()` on
       EVERY occurrence but reports ONCE PER CALL SITE (captureless-lambda function-local
       static claim — per-frame recurrences stay one stack trace, exact counts). Presentation
       via a swappable handler (`ts::set_ensure_handler`, `std::set_terminate` shape; counting
       stays outside it, so a host dialog can't hide failures); default handler = `ENSURE
       FAILED:` + stacktrace + debugger-break when attached (`detail::is_debugger_present` /
       `debug_break`, the C++26 P2546 pair polyfilled — `IsDebuggerPresent`/`__debugbreak`,
       Linux `TracerPid` + `int3`/`brk`, macOS sysctl `P_TRACED`; feeds the future platform
       layer 3.6). `ts::fatal` also breaks pre-`abort` at the failure site. Cleanliness is
       enforced: the harness fails on failures not consumed via
       `ts::test::consume_ensure_failures(n)`, `--bench`/`--stress` fail their exit on any
       (full no-arg run covered by `summary()`). **(b) the check** at `retract_or_wait`
       (the chokepoint every blocking task wait passes): fires only when genuinely about to park
       (post-retraction, post-serial-drain, target unsettled) under an access scope on
       non-retractable work; sharp same-object message when the target is a single-object pipe
       job on a pipe the context holds (certain deadlock — `Flags::pipe_job` + `dispatch_arg`
       stamped with the `Pipe*` at creation, matched via `Access_context::holds_epoch`;
       `blocking_sync_diagnose` seam task.h→guarded.cpp), general never-block warning otherwise
       (multi-object targets land on the general message — their `dispatch_arg` carries the
       reuse generation, not a pipe). Structurally exempt: `parallel_for` (joins via its own
       counter, never `retract_or_wait`), retractable targets (the waiter runs them), grant-free
       threads. Tests: ensure facility (once-per-site: same site twice → +2 count, one report),
       handler hook (install/restore, custom handler hit), deterministic warns-but-completes
       (blocker spins until the failure lands), `parallel_for`-in-node zero failures,
       `sync_own_object_deadlock` death scenario (child aborts on observing the sharp report;
       bounded poll so a miss fails instead of hanging). 503 checks green; full run (samples +
       benchmarks) zero failures; Shipping compiles.
   13. `[S]` **Escaped-reference hardening (T21) — analyzed, SHELVED (author, 2026-07).**
       Decision document: [escaped-refs-hardening.md](escaped-refs-hardening.md). Findings:
       (a) `Granted<T>` confirmed in the noise quadrant — five-system precedent survey
       (Chromium raw_ptr conceding `.get()` and winning via quarantine not deref checks; folly
       stating the launder hole verbatim; MSVC's checked-in-release default reversed) shows
       checked handles succeed only as the sole access path, tool-enforced, or with guarantees
       that survive extraction — 1.9 stays shelved with receipts. (b) Option A
       (interval-containment grants = 1.6's implementation; unlocks instrumented member types
       under a parent grant) and Option B (grant-scoped ASan poisoning off the pipe's
       acquire/release edges — catches stashed raw `T&` on Windows where TSan doesn't exist)
       both viable; author verdict: B "interesting but too limited, maybe in the future"; A
       shelved with it. Page protection rejected on numbers. Escaped-ref coverage remains
       TSan's job per [limits.md](limits.md); revisit on a real adoption-blocking incident.
   14. `[ ]` **(P2, designed 2026-07, benchmark-gated) Rebase the pipe onto the block machinery
       (pipes-as-edges — UE `FPipe` generalized to reader/writer).** UE's pipe is lock-free not
       by clever atomics but by *not being a scheduler structure*: one atomic `LastTask`;
       push = exchange + `AddSubsequent` on the previous task — serialization compiled into
       dependency edges, with the existing task machinery doing all dispatch (verified in
       Pipe.cpp). Our generalization: writers chain FPipe-style (atomic tail, prerequisite edge
       on the previous holder); a reader batch becomes a **reader-group sentinel** — a bare
       block whose nested-task counter counts active readers (existing `num_locks` machinery
       verbatim); a subsequent writer takes one edge from the sentinel; last reader out
       completes it. Reservations (graph acquire) become bodyless blocks whose dispatch fires
       `on_acquired` and whose completion IS the release. `wait_until_idle` = task-count +
       eventcount (copy UE's pattern incl. its last-decrement use-after-free caution). Gains:
       structurally inline-safe (nothing dispatches under a lock), lock-free producers, reader
       join/leave = `fetch_add` not mutex, and unification — subsumes the pending graph/
       `when_all` rebase thread; 1.7's standalone `Pipe` falls out. Costs: the tail-push
       protocol (writer arrival closes the group to joiners; a reader's failed join-CAS
       re-reads the tail — a known queued-RW-lock construction, ~100 careful lines), one
       pooled bare block per reader batch (rides 4.1's free-list), write-epoch bump migration,
       a serious TSan campaign. GATE: measure pipe-mutex contention on a reader-heavy fixture
       first — no rewrite without evidence. (A `shared_mutex` is NOT the interim answer:
       every pipe op mutates admission state, so all would take it exclusive.)
       **Addendum (2026-07, from the `Deferred::last_commit_` race):** the pipe does not
       expose queue facts (last-enqueued write, subset completion), so a wrapper that
       fire-and-forgets jobs and later needs "are my jobs done" must reconstruct pipe
       ordering externally — and the reconstruction is subtle: handle-store order need not
       match enqueue order, so recording "my last job" requires a lock spanning enqueue +
       store (a plain or even atomic store of the newest handle can leave an earlier job's
       handle recorded, and a settled-check on it then misses a pending job). Incident:
       `Deferred::commit_async` raced exactly this way (caught by the pre-push TSan hook;
       fixed with `commit_mutex_` spanning enqueue + store). Inventory: `Versioned::chain_`
       is sound by construction (the `seq_mutex_`'d chain, not the pipe, is its ordering
       source). This rebase dissolves the class: the atomic tail IS the last-job handle —
       expose it (e.g. `Pipe::last_write()` completion, or a settled-check) and delete
       `Deferred`'s external mutex; any future fire-and-forget wrapper gets ordering for
       free. Until then the rule stands: never record pipe ordering without a lock spanning
       enqueue + record.
       **Addendum (2026-07, retraction): the split-structure pipe is why pipe/`async` jobs
       are non-retractable.** Pipe ordering lives in a separate structure (`pipe.jobs` deque
       + reader/writer admission in `dispatch`), NOT in the block's `prerequisites`.
       `retract` (`task.h`) walks only `prerequisites`, so it is blind to pipe admission
       order — a pipe job flagged `retractable` could be run out of turn while still queued
       behind a conflicting writer (the exact race [task-internals.md](task-internals.md) §6
       cites). So pipe blocks are marked non-retractable (`run_pipe_job`, `guarded.cpp`:
       "pipe blocks are not retractable and never inline-dispatched"), and a blocking
       `sync()` on one parks a worker — caught only by the blocking-sync diagnostic, never
       made safe. UE gets pipe retraction *for free* precisely because it does NOT keep a
       separate structure: the previous piped task IS registered as a prerequisite
       (`FTaskBase::Prerequisites` is populated "by piping, when the previous piped task is
       added as a prerequisite"), so `TryRetractAndExecute` covers pipe chains through the
       same recursive prerequisite walk and respects order structurally (predecessor
       retracted first — never out of turn). The catch: UE's `FPipe` is strictly serial, so
       each piped task has exactly ONE pipe-predecessor, which maps to a single prerequisite
       edge. This rebase restores retraction the same way — but only cleanly for **writers**:
       a writer chains via one prerequisite edge on the previous holder, so it becomes
       retractable like any prerequisite (walk the edge, run inline once it clears).
       **Readers do not map**: the reader-group sentinel has N predecessors and a reader
       block carries no single ordering edge to walk, so reader retraction stays a separate
       design question even post-rebase. Net: the rebase reframes §6's parked "Retraction of
       pipe/async tasks" blocker from "admission-ordering plumbing" to "pipe ordering is not
       a prerequisite chain," and delivers writer retraction as a side effect — reader
       retraction not.

   15. `[ ]` **(P1, author 2026-07) Audit the two access routes (sync/async): defaults, low-level
       launches, zero-cost.** A systematic review of how a two-way data access is performed across
       the whole surface: `access` (opportunistic-inline) vs `async` (always-scheduled), and the
       same choice for coroutine `co_await` accesses, with three goals. (a) Review the DEFAULTS,
       especially for coroutines: is the default route (inline-when-free vs schedule) the right one
       at each entry, and consistent across single-object / multi-object / graph / coroutine access?
       Coroutine defaults are the least-examined and most likely wrong. (b) Minimise task launches
       at LOWER levels of abstraction: the design stance is that parallelism concentrates at the
       HIGHER levels (the frame graph, `parallel_for` over real work), so a low-level access that
       silently spawns a scheduled task where an inline body would serve is a cost with no
       parallelism win. Find and cut those, and consider stating this as a design principle
       (design.md / the CLAUDE.md principles list). (c) Drive `access` and inline dispatch toward a
       zero-cost abstraction: where the object is free, an `access` / awaited access should approach
       the cost of a direct call. Subsumes and should be done WITH 1.1 (zero-alloc inline `access`)
       and 6.1 (inline-when-free for awaited accesses); pairs with 10.11. Deliverable first: a
       route-by-route table (entry x default x inline-eligibility x alloc/dispatch cost) before
       changing anything.

2. **Static task graph**
   1. `[ ]` **(P1) Typed graph chaining** — a node consumes prerequisite-node results (nodes are void-only now); a `Graph_node` may then mint a per-run `Task<R>`.
   2. `[ ]` **(P2, raised within-band) Ambiguity detection** — `compile({.ambiguity = Warn|Error|Ignore})` determinism diagnostic; needs edge provenance; feeds profiler-guided reorder. **Research validation (2026-07, [research-static-vs-dynamic.md](research-static-vs-dynamic.md)):** ordering ambiguity is the top user-facing failure of access-derived schedules — Bevy shipped exactly this diagnostic (`ambiguity_detection`) after its stageless rework because users hit nondeterministic system order in practice. **PARKED (author, 2026-07).** Full analysis in [ordering-ambiguity.md](ordering-ambiguity.md): our declaration-index orientation is deterministic, so we lack Bevy's per-frame-nondeterminism bug class — the residual is *hidden, unratified* orientation (a refactor that swaps two `add_node` lines silently flips gameplay). The proposed feature (conflict provenance + a fragile-orientation lint + a commutativity annotation feeding the optimizer) is rescoped as optimizer infrastructure, not a safety feature — but the annotation-cost question (pairwise = combinatorial; object-level = the mitigation, unproven) is unresolved. Do nothing until real usage data (start with the tiebreak-only pair count on `game_frame`). Provenance itself is still needed by 2.4/2.5 and the DOT dump regardless.
   3. `[ ]` **(P3) Pipelined execution** — more than one `execute()` in flight (frame overlap).
   4. `[ ]` **(P2) Profiler-guided optimization** — reorder/rebucket from measured durations. **Manual dry-run done (2026-07, game_frame); the headline finding is about measurement, not levers.** Five configurations were tried (placement of staleness-tolerant `Versioned` readers early-vs-tail × priority ranking none/high-low/spine-high): all cells measured 4.9–5.6 ms within one session window, while the SAME baseline configuration measured 4.15 ms in an earlier window and 5.3 ms later — **cross-window ambient drift (~25%) dwarfed every within-window delta (~5–10%)**, so no lever's sign is established at single-run resolution. Everything below is therefore prerequisites + hypotheses to re-test under proper methodology:
      - **Measurement methodology is prerequisite #1**: interleaved runs (A/B/A/B to cancel drift), medians over N, controlled load, and the per-lane timeline capture from the graph-viz session (docs/graph-viz-handoff.md) — without this, optimization conclusions are noise. Ties to 10.1 (benchmark regression infra). **Capture landed (2026-07)**: `Graph_trace` streams per-node medians/variance, per-edge binding gaps, measured critical-path frequency, dispatch waits, and the critical-dead-time headline (makespan − critical work). First validated findings, from the permanent 4-worker starved run: dead time 22.5% vs 2.4% on 12 workers (scheduling- vs dependency-bound, now a one-line diagnosis); the priority lever is REFUTED for the occupancy case, and the ordering lever half-confirmed (gap closed, makespan −10% regression — `after` is completion-to-start). Full experiment record, lessons (makespan is the objective, dead time the diagnostic; the chain-extension guard; the missing yield/start-ordering primitive), and the three-tier tuner design in [profiler-guided-optimization.md](profiler-guided-optimization.md). Remaining prerequisite: interleaved A/B methodology.
      - **Mechanistic hypotheses recorded** (plausible, magnitudes unproven): (1) naive early gap-filling delays the serial spine — a ready critical-path node waits for a worker to finish its current slice, nothing evicts a runner → keep-out zones / finer filler granularity / reservation may be needed; (2) the current `Priority` enum cannot serve as rank: `low` is valve-gated background (a mislabeled dependency — hand-labeling got nav→ai and ui→submit wrong, which automated upward-rank would not — stalls its dependents), and `high` dispatches via the global MPMC queues instead of the per-worker deques (M2: only `normal` goes local) — a structural cost. Prerequisite either way: a rank mechanism native to the local-deque path (rank-ordered pop within `normal`, or M2 stage 5), plus **slice priority inheritance** — done (2026-07): `parallel_for`/`async_parallel_for` helpers now inherit the calling task's priority by default (`Parallel_options::priority` overrides), so a node's slices dispatch at the node's class rather than always `normal`.
      - **Info the optimizer needs**: per-node wall duration as (total_work, parallel_width) — a sliced node's wall depends on free workers, a scalar misleads; medians + variance; the edge list WITH provenance (conflict-derived = semantic, immovable; explicit = intent; version-choice reader→flip / flip→reader = movable IFF the user annotates the read staleness-OK); per-lane timelines; worker count.
      - **Sanctioned lever taxonomy**: (a) dispatch order among ready nodes — HEFT-style list scheduling by upward rank (blocked on the rank mechanism); (b) DAG shape via user-annotated semantic relaxations only — staleness-OK `Versioned` readers become a binary placement choice per reader, a small discrete search evaluable OFFLINE by simulating the captured cost model; (c) commutative-pair reordering (needs 2.2's provenance); (d) node fusion — merge nodes below the dispatch-cost floor that share an edge without losing parallelism (a PGO-time lever; see also 2.10's per-node cost numbers). The optimizer proposes; the user ratifies semantic moves.
      - **Conflict-edge orientation as the core static-graph lever (author 2026-07, T18).** The framing that makes this a headline selling point: a W-W or R-W conflict edge is a *disjunctive arc* (job-shop scheduling) — the pair must be ordered, either direction is race-free, and the direction is a scheduling *choice*. A pipes-only / dynamic system decides that direction online by arrival (no lookahead, nondeterministic) and can never plan around it because it never sees the arc before contention. A materialized graph sees every arc up front, so it can (1) compute critical-path / upward ranks and orient/dispatch to shorten the makespan, and (2) even for edges it must not flip, *report* "edge X (R->W on `physics`) adds Y µs to the critical path — consider `Versioned` or deferral" (the restructuring-suggestion mode). This is the strongest concrete answer to "what does a static graph buy that scheduling can't." **Prerequisite = edge provenance** (which arcs are semantic/immovable vs orientation-choices), which is needed here regardless of 2.2's parked annotation work — do provenance with this track; the *flip* lever for non-commutative pairs stays gated on the (parked) commutativity annotation. Names for nodes/objects already exist, so provenance labeling is cheap — but review the naming approach first (see Inconsistency 6: `ts::Named` not yet ratified).
      - Validation fixture: game_frame (hand-derived chain ≈2.6 ms, 26.9 ms work); a simulator must reproduce measured timelines before its proposals are trusted.
   5. `[ ]` **(P2) Compile-time rank → native dispatch shaping.** Compute upward rank per node at `compile()` (longest path to sink; weights = node priority now, measured durations once 2.4's capture exists; reuses the Kahn order, O(V+E) once). At run the graph shapes dispatch WITHOUT the scheduler's priority classes — avoiding `high`'s global-queue detour and `low`'s valve (2.4's blockers): (a) rank-aware successor submission order when one completion releases several ready nodes (owner deques pop LIFO — submit lowest-rank first so the settling worker pops the highest; thieves take the other end); (b) inline-successor selection by rank (`set_inline` currently picks without regard to importance). Converts `Graph_node::priority` from a queue-class request into a rank weight. Related: D6's static effective-priority pass should ride this mechanism (not the Priority classes) until M2 stage 5; priority-as-conflict-tiebreak belongs behind 2.2's opt-in (it changes observed values). Sequence AFTER the graph-viz timing capture — 2.4's dry-run showed dispatch-shaping effects are unmeasurable without it.
   6. `[ ]` **(P3, downgraded 2026-07 — author unconvinced; revisit on a demonstrated
      disabled-writer-stalls-readers case) Per-run node enable predicate (conditional execution).** The research pass's one design challenge ([research-static-vs-dynamic.md](research-static-vs-dynamic.md) — every mature static-graph system grew a dynamic escape; pure DAGs fail on data-dependent control flow first): the graph has no in-graph skip/branch. The cheap 80%: a per-run predicate on a node, evaluated before acquisition — a disabled node acquires nothing, completes immediately, releases successors (render graphs' conditional-pass-execution pattern). Preserves acyclicity, the derived-edge story, and the safety model; deliberately NOT Taskflow-style condition tasks (their weak/strong-dependency semantics carry documented race/deadlock pitfalls). Full loops/branches stay out of scope — dynamic tasks and `co_await` are the escape for data-dependent shape, as designed.
   7. `[x]` **(P1) Serial execution mode — DONE as GLOBAL worker-less mode (2026-07).**
      Rescoped from graph-only `execute({.serial=true})` to the UE shape after reading UE's
      source (zero workers when threads unavailable; `LaunchInternal` executes inline at launch,
      looping the continuation chain): `Scheduler_config{.single_threaded = true}` = no workers,
      every submit executes inline on the submitting thread via a bounded FIFO trampoline
      (per-thread pending vector + shared head, reentrant); blocking waits drain the thread's
      pending entries before parking (`drain_serial_pending` seam in task.h → scheduler.cpp), so
      a body that admits work then `sync()`s it doesn't deadlock. Serves debugging/bisection,
      deterministic tests, thread-less platforms, low-end fallback. Enabler shipped with it
      (Tier 0): pipe job submission moved OUTSIDE `pipe.mutex` (`dispatch` collects the admitted
      batch under the lock; `submit_admitted` after unlock) — inline-at-submit would deadlock on
      a body releasing its own pipe under the held mutex. Worker-less graph runs skip the trace
      fold (their timings describe the trampoline, not scheduling). `execute` now takes
      `Execution_options{.token}` (no bare-token overload — author decision); 2 call sites
      migrated. Graph-scoped serial (`.serial` per run, compiled-topo order, rest of app stays
      parallel) DEFERRED — revisit only if serialize-one-graph-in-a-live-app materializes; it
      would reuse this routing core plus a TLS gate. Tests: inline-at-submit, FIFO chain order,
      end-to-end (launch/then/async/parallel_for/graph+nested all on the caller thread),
      sync-inside-body drain, deterministic run-twice order; 486 checks green, Shipping + stress
      clean.
   8. `[x]` **(P1) Graph/`Guarded` lifetime fatals — DONE (2026-07).** Two fatals, fully
      `TS_SAFETY_CHECKS`-gated (field included — `Pipe::graph_refs` compiles out entirely, per
      author): (1) `~Static_task_graph` (and a move-assign overwrite, via destroy+placement-new
      move-assign so member moves stay maintenance-free; and `compile()` on recompile) fatals
      while a run is in flight (`run_->remaining_nodes != 0` — inits to 0, so pre-first-run
      destruction is clean); (2) `compile()` registers the graph on every `distinct_pipes_`
      pipe, the same paths release, and `~Guarded` fatals while any compiled graph still
      references it, naming the object via `debug_name`. Moves are balanced by construction
      (registrations ride the `distinct_pipes_` vector; moved-from is empty)
      ([research-deepdive.md](research-deepdive.md) §10.2; Taskflow #82 precedent). Tests:
      `guarded_outlived_by_graph` + `graph_destroyed_mid_run` death scenarios, and a
      recompile/move/move-assign-overwrite balance test; 490 checks green, Shipping + stress
      clean.
   9. `[ ]` **(P3) Over-declaration diagnostic** — report objects a node declared but never
      accessed (silent lost parallelism; RDG's unused-declaration warnings are the precedent —
      [research-deepdive.md](research-deepdive.md) §7.4). Caveat (author, 2026-07): conditional
      early-out bodies legitimately skip declared accesses — aggregate across runs ("never
      accessed in any of N runs") and provide a per-node "may-skip" annotation before warning.
   10. `[ ]` **(P2) Publish cost numbers** — measured `compile()` cost, per-node dispatch cost
       (queued / `set_inline` / handoff), and a "merge nodes below X µs" guideline; makes
       per-frame rebuild a measured option instead of an anti-pattern and answers the first
       sophisticated-evaluator question ([research-deepdive.md](research-deepdive.md) §9.4,
       §12.2). Ties to 10.1.
   11. `[ ]` **(P2, author 2026-07) Yield points inside long-running nodes.** The 4-worker
       game_frame trace made the failure concrete: a ready critical-path node (economy) waited
       ~0.9 ms behind a long off-path runner (audio) — nothing evicts a runner, and priority
       cannot help work that is already running (measured: audio at `low` changed neither its
       placement nor the gap; occupancy, not arbitration). A cooperative yield point inside a
       long node body — splitting it into resumable segments the scheduler can interleave ready
       higher-rank work between — is the eviction-shaped lever. Design space: explicit
       `ts::yield()` polling a "critical work pending" signal vs coroutine nodes (`co_await`
       suspension already exists for tasks) vs auto-slicing via `parallel_for` guidance. Relates
       to 2.4's keep-out-zone hypothesis and 2.5's rank (what "higher-rank pending" means).
   12. `[ ]` **(P2, author 2026-07 — raised from the 2.3 adjacency) Frame-boundary overlap for
       designated nodes.** Distinct from 2.3 (whole-graph pipelining): let specific off-path
       tails (audio mix, streaming finalization) spill past the run's settle into the next
       frame's early idle — per-node "may straddle the frame fence" annotation rather than two
       full runs in flight. Interacts with the run-quiescence invariant (one run at a time, full
       barrier between) — likely needs the spilling node to detach into a tracked dynamic task
       at the fence. Speculative; validate demand on the 4-worker fixture first.
   13. `[ ]` **(P3, doc + demonstrate — T20) Pre-compiled graph variants pattern.** For discrete
       mode sets (game-loading vs in-game driving vs boss fight vs cutscene), the intended answer
       to occasional shape change is "keep N compiled graphs, pick one per mode" rather than
       rebuild machinery ([research-deepdive.md](research-deepdive.md) §9.4). Document the
       pattern — but only worth promoting if a fixture demonstrates a *significant* per-variant
       scheduling difference (a cutscene graph genuinely out-scheduling the driving graph on the
       same work); build that demonstration before writing it up, else it is unsubstantiated
       advice.
3. **Scheduler**
   0. `[x]` **Single global scheduler — DONE (2026-07, `d173d9a`+`2b48a5b`).** Author chose single-global over the ambient-multi model (3.1). Exactly one scheduler process-wide, reachable via `global_scheduler()` (renamed from `default_scheduler`); reconfigurable by teardown+recreate (`configure_scheduler(config)`, a coarse quiescent-point op, NOT thread-safe against concurrent use) + a scoped `Scheduler_scope(config)` RAII (snapshot→reconfigure→restore) for running a block on a specific pool. `execute()` drops its `Scheduler&` arg (uses the global); `parallel_for` fans out on `current_scheduler` (Phase-1 fix `ea471dd` — a parallel_for inside a task uses that task's pool, not the global default, which had silently oversubscribed the sample's variant traces). Supersedes 3.1's ambient-override idea and its `Launch_options{.scheduler}` (nothing to select among). **Residual (P3): compile-time ban on ad-hoc `ts::Scheduler`.** The public ctor still compiles, so `ts::Scheduler s{...}` is constructible but INERT (nothing routes to it — a footgun). Finish via a `detail::make_scheduler(config)` factory (returns `unique_ptr`; class non-movable) used by the global holder AND the internal scheduler tests, public ctor removed. Cost: `scheduler_tests.cpp` unit-tests the class directly (ctor/dtor drain, 3 idle policies, valve — 13 sites) so it migrates to the factory; benchmarks/tsan likewise (~21 sites, mechanical). Author 2026-07: leave the ctor public for now (behavioral single-global suffices), do the ban later.
   1. `[~]` **(P1, superseded by 3.0) Ambient (overridable) scheduler** — was: `launch`/`task`/`access` resolve to an *ambient* scheduler + `Scheduler_scope` override + `Launch_options{.scheduler}`. The single-global model (3.0) took the simpler road: one reconfigurable global, `Scheduler_scope` reconfigures it rather than overriding among many. Kept for the record. [§D3]
   2. `[ ]` **(P3, rescoped 2026-07 — author: avoid named threads as much as possible) Task→worker affinity (escape hatch, NOT named threads).** Named threads are the anti-pattern the access-declared graph exists to replace: a named render thread encodes "render after game, on this thread" as a *thread boundary*, where the graph encodes the same ordering as a *dependency edge* derived from data access — which scales (submission fans across all workers) where the thread does not (serial funnel, cores that can't lend to each other, cross-thread hand-off latency). The sample's `Versioned` render pipeline reading last frame's transforms IS "the render thread with one frame of latency," expressed as tasks with a version-choice dependency rather than a pinned thread — the named-thread-free pattern, generalized. So do NOT model named threads. The only real need is IRREDUCIBLE affinity — single-thread graphics APIs (GL, older D3D), OS main-thread requirements (macOS UI / window message pump), external libs that demand a fixed thread — met by a minimal "pin these tasks to worker N" mechanism (a reserved/pinned worker or an affinity tag) *inside* the one scheduler, off by default. NOT a separate `Scheduler` instance: consistent with the single-global-scheduler model (variety lives inside the scheduler *type*, one live *instance*). Replaces the old "named thread as a single-worker `Scheduler`" framing.
   3. `[ ]` **(P2) Run-on-all-workers** — broadcast a functor to every worker (per-thread init / flush / warm-up).
   4. `[ ]` **(P2) Timed / delayed tasks** — delay queue (timing wheel); one-shot / periodic / cancellable; re-arms a reusable task.
   5. `[ ]` **(P2) M2 stage 5** — promote high/low to per-worker deques (profiling-gated); a proper low-contention worker-submit benchmark.
   6. `[ ]` **(P2) Platform abstraction (~6 fns)** — `park`/`unpark`(+timeout), thread spawn/name/affinity, cpu_count/topology, `cpu_relax`. Unblocks eventcount timeout + standby workers; console fiber backend later. [§D4]
   7. `[ ]` **(P2, raised 2026-07) Pluggable scheduler + a foreground/background two-pool variant.** The `Scheduler` is one implementation of an interface; ship alternatives behind the same `Task` API (the ambient-scheduler work in 3.1 is the seam). The motivating variant: a **separate background worker pool on OS-low-priority threads**, deliberately OVERSUBSCRIBING the cores. The idea is OS-scheduler-assisted gap filling — background threads run only when the foreground pool leaves a core idle, and the OS *preempts them out* the instant foreground work is ready, so background work costs the foreground nothing (unlike userspace `low`, which can't evict a runner — the exact failure we measured: `audio` at `low` still occupied a core the critical spine needed). This is distinct from 3.5 (promoting high/low to per-worker deques) and from D6 (userspace priority propagation): it moves the arbitration to the OS for the *background* band only, where preemption is legitimate. Prior art + adjacent notes: UE's two-pool foreground/background design with per-pool OS priority + affinity ([task-systems-comparison.md](task-systems-comparison.md) §UE), the mobile QoS-over-fixed-pool note (§D4), and D6's "a fixed per-pool QoS-background attribute is the only legit OS-priority use." Costs to weigh: thread explosion / oversubscription tuning, the syscall/permission cost of OS priority, affinity interplay, and determinism (background timing becomes OS-dependent — keep it off the traced/asserted paths). Also relevant to this session's finding that a normal task buried in a local deque loses to global `low` because `find_work` steals last (see 3.5 and the pending steal-before-global-low reorder) — a two-pool split would let the background band be genuinely non-interfering instead of merely deprioritized. **Scope guidance (author, 2026-07, T26):** ship at most 2–3 interchangeable scheduler implementations behind the shared `Task` API, not a plugin zoo. Each may expose a wake/idle policy (as the current one does) but nothing fancier at this stage. The engineering constraint that matters: factor the scheduler so its reusable parts (eventcount, deques, MPMC queues, valve, the submit/steal loop) are shared building blocks a new implementation composes, rather than each scheduler being a monolith — a fancy future scheduler should reuse most of what exists. Design-doc note added (design.md §3).

4. **Allocation / control block**
   1. `[ ]` **(P2)** Per-type recycling free-list (`Exec`/`Result_block`/bare block).
   2. `[ ]` **(P2)** Tunable-SBO `Function<Sig, N>` replacing `move_only_function`/`function` (also fixes the reservation-path closure alloc — inconsistency #5).
   3. `[ ]` **(P2)** Small-vector / intrusive links for block `successors`/`prerequisites`/`continuations` (also shrinks the block ~72 B).
   4. `[ ]` **(P2)** Shrink `Task_control_block` — `completed`/`cancelled`→bits; a futex wait primitive roughly halves the 288 B block (ties to platform layer).
   5. `[ ]` **(P3)** Multi-object `async` `std::map`→sorted `vector`.
   6. `[ ]` **(P2)** Opt-in scoped bump arena (auto for `parallel_for`/graph-run, per-frame opt-in); rebase `journal.h` staging onto it.

5. **Fork-join / parallel_for**
   1. `[ ]` **(P2) Intra-system entity interactions** — ship the primitive menu: `parallel_gather_apply` (mailbox), `parallel_for_colored` + `Interaction_coloring`, `Accumulator` (commutative), `Union_find` helper, + triage docs. Open author questions. [§D5]
   2. `[S]` **Priority propagation / inheritance** — designed (pipe / graph / dynamic; opt-in surfaces; not OS thread priority); revisit on a demonstrated inversion. [§D6]
   3. `[ ]` **(P3) Reserve / standby workers** — sequenced after the platform layer; caller-participation already covers the need.

6. **Coroutines**
   1. `[ ]` **(P2)** Inline-when-free for awaited accesses — `co_await obj.access(fn)` should try inline at the await (safe: the coroutine would suspend anyway).
   2. `[ ]` **(P2)** Coroutine-frame / control-block fusion — one alloc for frame + block (coroutines *reduce* allocs, not add).
   3. `[ ]` **(P3)** Priority setter on the promise (it stores one; no config channel yet).

7. **Deferred / Versioned**
   1. `[ ]` **(P2) Main chain** ([deferred-versioned-state.md](deferred-versioned-state.md) §6) — journal `mem_profile` baseline → per-journal bump arena → record-stream slots → typed command tier (`Deferred<T,Cmd>`) → sort keys / hooks / dirty-set → render-queue fixture.
   2. `[ ]` **(P2) Lock-free `stage()`** — kill the per-slot mutex (it exists ONLY for the dynamic stage-vs-cut race; single producer per slot otherwise — handoff doc §5). Falls out of 7.1's arena step: single-producer chunked bump allocation makes `stage` a lock-free bump, and the cut becomes a chain-head exchange. `Parallel_recorder` already gives thread-keyed slots (per-worker + overflow lane); this removes the last lock on the staging path. Split out of 7.1 for referenceability — implement together with the arena.

8. **Task chaining**
   1. `[ ]` **(P3) Results-on-`after`** — leaning *no* (`when_all`+`then` covers it); revisit only if a concrete single-result-prerequisite use case appears.

9. **Research / shelved**
   1. `[S]` **std::execution senders** — shelved; model the concepts for interop, prototype *access-context-as-env* as the one novel spike; do **not** re-found the engine on senders (the monomorphic runtime block earns its keep). [§D7]
   2. `[ ]` **(research note — T13.4) Async I/O story.** Untouched so far. The library is CPU-compute-first; async I/O (file/socket/GPU-transfer completion) is a different axis — a blocked I/O wait must not tie up a worker. Today the sanctioned bridge is `Signal`: an external completion (OS overlapped-IO callback, GPU fence, `io_uring` CQE) calls `signal.trigger()`, and a `then`/`when_all` resumes CPU work — fire-the-IO, gate-the-continuation, never block a worker. That covers "react to completion" without an I/O runtime. What we deliberately do NOT provide (and probably shouldn't, cf. Rayon/Tokio being separate pools by design): an I/O reactor, readiness polling, or a socket/timer API. Open question to revisit only on demonstrated demand: whether a thin `Signal`-from-OS-completion helper (register an OVERLAPPED / fd / fence, get a `Signal`) is worth packaging, or stays a documented idiom. Ties to the Signal-examples doc item (10.4).
   3. `[ ]` **(research note — T25) CPU transient aliasing.** Render graphs reuse one block of memory for two scratch resources whose lifetimes don't overlap ([research-deepdive.md](research-deepdive.md) §4.4), derived from declared first-write/last-read. The CPU analogue for us would be per-node *declared transient buffers* with `compile()` computing [first-writer, last-reader] windows and aliasing storage across non-overlapping windows — a potential differentiator, but it needs a task-system feature we don't have: a *declared transient-resource* concept distinct from a `Guarded` object (a scratch buffer owned by the graph, not a persistent guarded instance). The coarse version already exists (the per-run bump arena, 4.6 — everything dies at run end); the fine version is speculative and gated on that new concept. Record only; act only if a concrete workload needs graph-derived scratch-memory reuse.

10. **Tooling / infra**
    1. `[ ]` **(P2)** Benchmark regression baseline + compare step (store medians, flag regressions). *(postponed — not blocking)* Also assert **allocation-free re-runs** here (T19): `execute()` on a compiled graph should show 0 allocs via `--memprofile` on the 2nd+ run — a cheap regression guard for the re-arm design, folded into this step rather than a standalone test.
    2. `[ ]` **(P2)** Proper ASan build config (reachable via `/p:EnableASAN=true` today); portable TSan build story.
    3. `[ ]` **(P3, docs — T13.1/T13.2) Renderer-pattern samples.** Single-file samples, not out-of-box machinery (the author's call — these are idioms over existing primitives, not new API): (a) **parallel command-list recording** — fork records into an indexed slot array (chunk i → slot i), a sequential tail submits slots in order (GPU exec order = submission order); shows `parallel_for` + ordered-slot join. (b) **dynamic reprioritization** — the PSO-compile pattern: a queued background task that must jump the queue when its result becomes needed this frame; cancel-and-relaunch-at-higher-priority is the viable mechanism and the user can already do it — the sample documents the idiom; only add a helper if the raw form proves error-prone. (c) **RHI-stream-as-pipe** — the render/RHI split as a `Pipe`/`Deferred` ordered stream fed from many recording threads (incl. inside `parallel_for`), rather than a dedicated named thread — reinforcing 3.2's named-thread-free stance.
    4. `[ ]` **(P2, docs — T13.5) `Signal` usage examples.** `Signal` is under-documented for how much it enables: external-completion bridge (GPU fence / OS async-IO callback → `trigger()` → `then`), manual join/barrier, phase gate, one-shot broadcast, reusable phase gate via `reset()`. A worked set of examples in guide.md (and the async-IO idiom from 9.2). Author flagged Signals as an interesting, under-covered primitive.
    5. `[ ]` **(P3, docs — T24) Same-program comparison appendix.** A short "same task graph in ours / Taskflow / TBB flow graph" appendix — we win the wiring count by construction (derived edges = zero `precede`/`make_edge` calls), the measurable form of the ergonomics claim ([research-deepdive.md](research-deepdive.md) §14). Low priority; informative value uncertain until written.
    6. `[ ]` **(P1-cheap, docs) "Declaration order is program order" contract.** One paragraph in guide.md making explicit the contract the graph already implements (conflicting nodes run in `add_node` order — the STF/pipe-FIFO philosophy applied to the graph). Removes the "invisible edge" surprise the derived-edge model risks; prerequisite framing for the ordering-ambiguity discussion ([ordering-ambiguity.md](ordering-ambiguity.md) §4.1). Do with the next docs pass.
    7. `[ ]` **(P2, docs) Layered-disclosure front page + scope statement.** guide.md should let a reader be productive with the six core functions before meeting `Deferred`/`Versioned`/coroutines/`parallel_for` (API-surface width is the adoption risk, [research-deepdive.md](research-deepdive.md) §14); and state once, plainly, the scope boundary (O(100)-node coarse frame skeletons; problem-given million-node graphs are Taskflow/HPC territory — §16).
    8. `[ ]` **(P2, docs) Known-limits section.** Draft written in [limits.md](limits.md) (2026-07): the honest catalog of what the harness and access declarations do NOT catch — coverage gaps (uninstrumented methods, PODs, escaped refs, sub-object escapes, stale inherited grants, shipping builds) and the failures no declaration system can catch (semantic/order races, deadlock-by-misuse, the completeness hazard), plus the TSan-as-complement posture. Refine and fold into guide.md.
    9. `[ ]` **(P2, trace — handoff to the tracing session) Serial-baseline trace lane.**
       Worker-less (`single_threaded`) graph runs are excluded from the parallel trace fold
       (their starts are cumulative serial offsets; dispatch-wait measures the trampoline;
       the critical path degenerates; utilization has no workers) — but their per-node body
       durations are the CLEANEST cost-model input there is (no contention, no co-runner
       cache interference): exactly the per-node `total_work` 2.4's simulator needs. Add a
       separate duration-only per-node aggregate for worker-less runs + a serial-vs-parallel
       delta view in the SVG (a node much slower in parallel = contention/false-sharing
       suspect — a diagnostic nothing else provides). Lives in `graph_trace.h` /
       `trace_stamps.h` (the tracing session's files; the fold-skip hook in
       `static_task_graph.cpp` already passes the skip flag).
    10. `[ ]` **(P2, trace) Internal profiling: decompose M to optimise the machinery.**
        The ranked 6-point plan in
        [profiler-guided-optimization.md](profiler-guided-optimization.md) ("Internal
        profiling" section). Start with 1+2: (1) split machinery into named phases —
        dispatch/submit already have bridges; add per-worker **acquire** (`pipe_acquire`/
        handoff) and **completion** (successor release + dep-counter + object release)
        accumulators, same armed/relaxed gating; (2) **queue-lock-wait** per worker
        (request→hold on the priority-queue mutex) + MPMC CAS-retry counts — prices the
        sharded/lock-free queue redesign before doing it. (3) cheap ratio counters ride
        along (find_work hit/miss, local-pop vs steal, park/unpark) — overlaps the
        per-kind/scheduling-counters trace item; land together. Then 4–6: per-node/per-kind
        M attribution (reuse the `trace_owner` seam), per-feature microbench ladder over
        the ~194 ns floor, wake-latency histogram (idle split: no-work vs
        parked-while-work-available).
    11. `[ ]` **(P2, docs/samples, author 2026-07) Lead users to coroutines.** Review every sample
        (game_frame, physics, blackboard, and the renderer-pattern samples from 10.3) and identify
        places where a coroutine (`co_await ts::read_only`/`read_write`, awaited accesses,
        suspension across a dependency) would read cleaner than the current `then` / `when_all` /
        continuation shape, then convert or add a coroutine variant wherever it genuinely improves
        clarity, so the sample set actively demonstrates coroutines instead of treating them as an
        advanced afterthought. Goal: coroutines become a first-class, encouraged style in the
        samples and in the guide's disclosure order (10.7), not a footnote. Pairs with 1.15: cleaner
        high-level coroutine code is the intended alternative to low-level task launches.

---

## Inconsistencies to resolve (pre-public sweep)

Small API/doc frictions introduced or exposed this session — cheap to fix, costly to
change after public. Feed the going-public "API-stability pass".

1. **`run_inline` dead for `access`/`async` — RESOLVED (Access_options split).** The access
   surface now takes `Access_options{token, priority}` (task.h), separate from
   `Task_options{token, priority, run_inline}` (then/task builders) — passing `run_inline` to
   `access`/`async` is now a compile error, not a silent no-op. *Revisit during code review*
   (flagged in-code): whether the two option types should share a base or stay split.
2. **Multi-object `ts::access` ≠ opportunistic — documented (implement later).** Single-object
   `access` runs inline when free; multi-object `ts::access` schedules like `ts::async` (inline
   path unimplemented). Documented at the call site so it isn't a silent surprise; the
   multi-object inline fast path is deferred (Guarded/access "generic-lambda"-adjacent follow-up).
3. **Stale `async` comments after the split — FIXED.** Swept `guarded.h` (`default_scheduler`,
   `pipe_try_inline`, the two access/async doc blocks).
4. **`pipe` vs `queue` terminology — deferred to the docs review.** Decision: use **queue** in
   README + user guide (no "pipe"); keep **pipe** in the deeper docs (design/internals), and on
   first use there explain the term + reference UE task pipes (with a doc link) as the exemplar.
5. **Reservation path still heap-boxes a closure — folded into the allocation campaign.**
   `Pipe::Job::on_acquired` (`move_only_function`) allocates on the graph / multi-async
   reservation path (unlike the allocation-free block/async dispatch); the tunable-SBO
   `Function<Sig,N>` item is the fix. Not a standalone task.
6. **Safety-field gating convention — RESOLVED (fully gated, 2026-07).** The convention is
   ratified in CLAUDE.md's style section: safety-only state is fully gated by
   `TS_SAFETY_CHECKS`, fields included; the only unconditional exception is genuinely free
   storage (a spare bit in an existing packed byte, e.g. `Flags::pipe_job`). T1's two style-1
   spots aligned: `Pipe::write_epoch` and `Access_context::Entry`'s epoch pair are now gated
   (capture sites go through `detail::pipe_epoch`, which returns null with the harness off —
   shipping contexts shrink by 16 bytes/entry). The "layout stability" argument was hollow —
   mixed-config TUs violate ODR through inline-function bodies regardless — and is now
   enforced instead of argued: a link-time tripwire in `access.h` (`#pragma detect_mismatch`
   on MSVC-family; a config-named anchor symbol, defined in `access.cpp`, elsewhere) makes a
   mixed-config link fail with a diagnostic. The same convention governs the shelved
   interval-grant entry layout ([escaped-refs-hardening.md](escaped-refs-hardening.md) §6.1)
   if revived.
7. **Entity-naming approach — review before it hardens (author 2026-07, T18).** We name nodes
   (`Node_name` / `std::source_location`) and objects (`ts::Named` leading ctor arg), which is
   what makes edge provenance and the DOT dump cheap — but the author is "not totally sold on
   `ts::Named`" (the distinct-wrapper-to-avoid-`T`-ctor-ambiguity design, design.md §2.3). Since
   provenance-driven optimization (2.4/2.5, the orientation lever) leans on these names, ratify
   or revise the naming surface in the API-stability pass *before* more tooling depends on it.
   Question: is a leading `ts::Named{"..."}` wrapper the right spelling, or a `.name("...")`
   setter / a `Named<Guarded<T>>` alias / something else? Cheap to change now, costly post-public.

---

## Detail

### D1. Zero-alloc inline `access`
Today every `access` heap-allocates a `Task_control_block` (~288 B) even when it runs inline;
the coroutine guard is the proof it needn't (stack-only `Pipe_guard` under a scoped grant).
Three tiers: **(1)** `void` `access` on a free grant → run under a stack-scoped grant, return an
*already-done* `Task<void>` sentinel (no block; ships independently). **(2)** small non-`void` R →
`Task<R>` becomes a discriminated union *inline-value | block-ptr | done-void*, storing R inline
below an SBO threshold (lean fixed-small ~2–3 words; a discriminant branch in
`sync`/`then`/`is_done`, not hot-path); larger R → heap. **(3)** large R / deferred → heap (the
floor). Composes with the SBO-`Function` and per-type free-list items.

### D2. Access-check completeness
The harness only fires from instrumented methods — a forgotten `TS_CHECK_ACCESS()` is a silent
hole. Belts: a **clang-tidy** check (every public method of a guarded type opens with the macro;
needs a way to mark guarded types), and a **POD wrapper** — a POD in `Guarded` has no methods to
instrument, so undeclared field access is invisible; offer a value-mediating `Guarded_value<T>`
or lean on the range harness (D-adjacent) so a grant covers the POD's bytes.

### D3. Ambient scheduler
`launch`/`task`/`access` are hardwired to a process-wide `default_scheduler()`; only
`graph.execute(scheduler)` is explicit. The global default is the right terse default, but the
*hard* global blocks testability (deterministic/inline scheduler), embedding (a library imposes
its pool on the host), multiple pools, and named-thread targeting. Fix (the `thread_local
current_scheduler` half-exists): resolve to the *ambient* scheduler, add a scoped
`Scheduler_scope guard(s)` (tokio `enter()`), optional per-op `Launch_options{.scheduler}`;
`Guarded` binds ambient at construction. Overridable ambient — not full-explicit, not hard-global.

### D4. Platform abstraction
~6 functions behind a header: `park(addr, expected, timeout?)` / `unpark_one` / `unpark_all`
(Win `WaitOnAddress`, Linux `futex`, macOS `__ulock`/`os_sync`; `std::atomic::wait` lacks the
timeout needed for standby workers/watchdogs → plan the raw-futex swap), `thread_spawn` /
`set_name` / `set_affinity`, `cpu_count` + minimal `topology`, `cpu_relax`/`yield`. Everything
else (eventcount, deques, MPMC queues, loop) stays portable. Console = fiber backend behind the
same `Task` API; mobile = QoS + oversubscription over a fixed pool.

### D5. Intra-system entity interactions
Inside one coarse `Guarded` grant, an internal `parallel_for` where entity *i* reads+writes
entity *j*. Per-entity locks are the anti-pattern (contention/convoying/SIMD-hostile/non-det —
loses to privatization even at 8 threads). Cross-domain survey (Box2D v3, Jolt, Unity DOTS,
Avian, LAMMPS/GROMACS, PRISM, Galois, Bevy/Flecs) → ship: **(1)** `parallel_gather_apply<Msg>`
(two-pass mailbox: record→bin-by-target→apply; deterministic via sorted inboxes; the intra-loop
sibling of the graph command buffer) — the general default; **(2)** `parallel_for_colored` +
persistent `Interaction_coloring` (greedy bitset, overflow→serial) — freshest-value Gauss-Seidel;
**(3)** `Accumulator<T>` per-worker slabs + reduce — commutative subset; **(4)** `Union_find`
helper + doc pattern for islands (not an executor); **(5)** triage docs + the anti-pattern note.
All run inside the coarse grant — same trust model as disjoint `parallel_for` chunks; no
pipe/graph/harness changes. Open author questions: commutativity share; Gauss-Seidel freshness
load-bearing?; pair provenance (persistent→coloring vs discovered→mailbox); hot-entity max degree;
determinism default; POD-first vs closure.

### D6. Priority propagation / inheritance
Priority flows *backward* along dependency edges (consumer→producer, taking max) — the dual of
cancellation's forward flow. Keep order, only raise dispatch priority (never reorder). Three
instantiations: **pipe** (dispatch head at `max(own, queued-behind)`, O(1), on by default),
**static graph** (reverse-topo max once at `compile()`, no residual window — do first), **dynamic
chains** (donate at launch, bounded, opt-in on the consumer). Caveat: unbounded propagation floods
the high queue and starves background → opt-in/bounded, per-workload. Not OS thread priority
(syscall cost, permission-gated; a fixed per-pool QoS-background attribute is the only legit
OS-priority use → platform layer).

### D7. Senders (P2300)
Lazy typed sender + receiver + `connect`→op-state (stack-allocatable) + `start`; algorithms fold
into one nested op-state. Could give: zero-alloc *structured* composition (the structural answer to
allocation, but only for the awaited subset), and — the distinctive angle — **env/queries as the
`Access_context` carrier** (typed, composable replacement for `thread_local current_access`;
opens compile-time access checking). Tensions: clashes with the deliberately not-templated
monomorphic block (compile-time + error-message tax), doesn't fix fire-and-forget allocation,
different usage model, P2300 still settling. Verdict: coroutines first (done); prototype
access-context-as-env as the one spike; don't rebuild the engine.
