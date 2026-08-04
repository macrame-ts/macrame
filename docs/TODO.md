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

All layers built, tested (**501 checks** as of 2026-08, TSan/ASan clean, subprocess
death tests), documented, and CI-gated.

- **Task core (coroutine-first, 2026-08)** — `Task<R>` is the one completion primitive:
  awaitable from any task, `sync()`/`take()` legal ONLY outside tasks (in-task blocking is
  fatal under `TS_SAFETY_CHECKS`). Composition is `co_await`; `ts::launch` (free-running) and
  `ts::nested` (joins the caller's implicit scope) are the launch verbs, `Task_scope` the
  explicit nursery. Cooperative cancellation (+ `Cancel_callback` + trailing-
  `Cancellation_token` body early-out), `Signal` (+ `reset()`) as the awaitable event.
  Deleted with the transformation: `then`, `when_all`, the `ts::task` builder + `after`,
  retraction/deep-retraction, executable task reuse, the inline-dispatch trampoline
  (`set_inline`/`run_inline`) — see [coroutine-first.md](coroutine-first.md) §3/§8.
- **Guarded / access** — awaitable `co_await obj.access(fn)` (inline when the pipe is free or
  the caller already owns the write grant, else suspends) + eager `async` (returns `Task<R>`,
  callable from any color), const-ness deduction, multi-object `ts::access`/`ts::async`
  (canonical-order cascade, deadlock-free), per-object reader/writer pipe.
- **Pipe (evolved, 2026-08)** — one mutex per pipe with rebuilt internals: intrusive FIFO of
  embedded `Pipe_link`s (no `Job`/`std::deque`/closures), pipe turns as `num_locks`
  prerequisites, one unified multi-object cascade serving dynamic multi-object `async` AND
  graph nodes (deleting `Multi_async_state`, `acquire_next`, `preheld`, the explicit graph
  handoff), always-on `writer_owner` grant ownership. Design of record:
  [pipe-rebase.md](pipe-rebase.md) §0.
- **Static graph** — access-conflict-derived edges + explicit `after`/`before`, per-node
  mode-aware acquire (objects free in the gaps), coroutine node bodies, per-node implicit
  scope, allocation-free re-runs.
- **Scheduler (M2 stages 1–4)** — per-priority lock-free MPMC queues, Vyukov eventcount,
  per-worker Chase-Lev deques + stealing, low-starvation valve + spinner cap; idle policies
  (`spin` / `spin_then_block` default / `handoff`).
- **Coroutines** — mandatory, not optional: awaitable `Task`, `co_await ts::read_only/
  read_write` guards, promise-carried access context reinstalled at every resumption,
  implicit per-frame scope + `join_nested`, `Task_scope`, coroutine graph nodes, frame/block
  fusion (one allocation — the promise embeds the block), the bounded resume trampoline, the
  waits-for cycle detector for suspended-ABBA.
- **Deferred / Versioned** — staged-write layer (`Journal`/`Recorder`/`Parallel_recorder`),
  ONE auto-dispatching `commit()` (inline under a held write grant, else an enqueued write —
  `commit_async` removed), three-phase publish, replay/copy/overwrite resync,
  single-publisher enforcement.
- **Allocation** — `launch` 1 · `async` 1 · coroutine task 1 (frame/block fusion) allocs/op
  (intrusive refcount, per-dispatch `submit_closure` kill). The `then`/`when_all` multi-alloc
  offenders are gone with the verbs themselves; re-baseline `mem_profile` on the coroutine
  shapes (10.14 territory).
- **Build / public-prep** — MIT license, whole lib in `ts::`, CMake + presets, CI
  (MSVC / clang-cl / Linux-TSan), Shipping config (`TS_SAFETY_CHECKS=0`), v0.1.0
  (+ [CHANGELOG.md](../CHANGELOG.md); the library goes public as 0.1.0),
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
   14. `[x]` **DONE, but NOT as designed (2026-08) — the lock-free chain was implemented,
       stress-tested, and RETIRED; what shipped is the evolved mutex pipe.** The R10 gate came
       back negative (22 producers on one pipe: 798 ns/op vs ~1050 ns/op uncontended — the
       mutex was never the bottleneck), and every reader-group piece of the chain burned
       (walk double-claim, three custody/lifetime UAFs, claim-without-fire, the tenure/era ABA
       that exists only because the chain is lock-free), while every serial UE-verbatim piece
       worked immediately. What shipped instead: one mutex per pipe with rebuilt internals —
       intrusive FIFO of embedded `Pipe_link`s, pipe turns as `num_locks` prerequisites, the
       unified multi-object cascade (one path for dynamic multi-object `async` and graph nodes;
       `Multi_async_state`/`acquire_next`/`preheld`/explicit handoff deleted), always-on
       `writer_owner`, and the `wait_until_idle` drain kept as a CV notified under the mutex
       (verified immune to the UE `FPipe` teardown UAF, which is a lock-free-notify artifact).
       Design of record + the retirement evidence: [pipe-rebase.md](pipe-rebase.md) §0;
       tests [pipe-rebase-tests.md](pipe-rebase-tests.md). The addenda below are kept as the
       engineering record — note that the `Deferred::last_commit_` race they describe IS
       dissolved (the enqueue-and-record seam under the pipe mutex; `commit_mutex_` deleted),
       and the retraction addendum is moot (retraction is gone with the coroutine-first
       transformation, 6.4). Original design text follows.
       UE's pipe is lock-free not
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
       **Partly answered (2026-08, 6.4).** (a) is settled for the dynamic surface: there is one
       awaitable access verb whose default IS inline-when-free (plus reentrant-owner inline),
       and one eager `async` — the split is now by *color*, not by policy, so there is no
       inconsistent default left to pick between. What survives of this item: multi-object
       `access` still has no inline arm (1.2), (b) the low-level-launch minimisation pass has
       not been done (and should now count coroutine frames, not just task blocks — 6.2), and
       (c) the zero-cost target is unmeasured on the new verbs. Redo the route table against
       the coroutine surface before acting.

   16. `[ ]` **(P1, author 2026-07) Nested `Guarded`: investigate, then sample or hazard doc.**
       The case: a guarded resource whose member is itself guarded (a `Guarded<T>` where `T` holds
       a `Guarded<U>`), and accessing the inner `Guarded` from inside the outer access scope. Work
       through whether it composes cleanly: does the per-object `Access_context` nest correctly (an
       inner `access`/`async` under an outer grant), do the pipes compose without deadlock (inner
       acquire while holding the outer), and do mode deduction + the write-epoch stale-grant
       machinery behave? If it works smoothly, add a sample or test demonstrating the pattern; if it
       does not, document the hazards (which nestings deadlock, defeat the harness, or serialize
       unexpectedly) AND add tests pinning the failure modes so they cannot silently regress. Either
       outcome ships coverage. Relates to 1.6 (sub-object grants) and the completeness-hazard story.

2. **Static task graph**
   1. `[ ]` **(P1) Typed graph chaining** — a node consumes prerequisite-node results (nodes are void-only now); a `Graph_node` may then mint a per-run `Task<R>`.
   2. `[ ]` **(P2, raised within-band) Ambiguity detection** — `compile({.ambiguity = Warn|Error|Ignore})` determinism diagnostic; needs edge provenance; feeds profiler-guided reorder. **Research validation (2026-07, [research-static-vs-dynamic.md](research-static-vs-dynamic.md)):** ordering ambiguity is the top user-facing failure of access-derived schedules — Bevy shipped exactly this diagnostic (`ambiguity_detection`) after its stageless rework because users hit nondeterministic system order in practice. **PARKED (author, 2026-07).** Full analysis in [ordering-ambiguity.md](ordering-ambiguity.md): our declaration-index orientation is deterministic, so we lack Bevy's per-frame-nondeterminism bug class — the residual is *hidden, unratified* orientation (a refactor that swaps two `add_node` lines silently flips gameplay). The proposed feature (conflict provenance + a fragile-orientation lint + a commutativity annotation feeding the optimizer) is rescoped as optimizer infrastructure, not a safety feature — but the annotation-cost question (pairwise = combinatorial; object-level = the mitigation, unproven) is unresolved. Do nothing until real usage data (start with the tiebreak-only pair count on `game_frame`). Provenance itself is still needed by 2.4/2.5 and the DOT dump regardless.
   3. `[ ]` **(P3, pulled up 2026-08) Pipelined execution** — more than one `execute()` in flight (frame overlap). **Now also the relaxation path for nested graph runs** (6.9a): the v1 rule there is one instance per concurrent user, because a pre-compiled inner graph invoked from two concurrently-running parents collides with one-run-at-a-time. Run-queueing / pipelined runs is the general fix, so a demonstrated shared-inner-graph case promotes this item rather than adding new machinery.
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
   14. `[ ]` **(P1, author 2026-08) Declaration order must not carry intent — explicit edges
       for logical ordering.** `compile()` derives an edge for every access conflict and
       directs it by declaration index. That direction is currently load-bearing in the
       sample: baseline `cmd_record`/`particles`/`UI` precede `submit` *only* because they are
       declared first. **Author's ruling: we rely on the determinism of a COMPILED graph, not
       on node declaration order.** A derived edge exists for safety (no data race); either
       direction is race-free, so an optimiser is free to reorder independent conflicting
       nodes. If node A must logically precede node B, that is intent and must be an explicit
       `after`/`before` edge. Work: (a) audit `sample/game_frame.cpp` for every place where a
       derived edge's *direction* encodes intent and make those explicit — the baseline
       render-producer → `submit` edges are the known set, and note the optimised variant
       already declares them explicitly because staging removes the conflict; (b) state the
       rule in `docs/guide.md` and `docs/design.md` (declaration order resolves the direction
       of a derived edge, but is not a specification — do not build semantics on it);
       (c) consider a `compile()` diagnostic that flags a derived edge whose direction is the
       only thing ordering two nodes, so reliance is visible rather than implicit. Enables the
       reordering optimiser this rule is reserving room for.

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
   **Two distinct oversubscription mechanisms (UE, verified from source — keep them separate, both are SCHEDULER-impl concerns behind the unchanged `Task` API, NOT task-layer):** (i) *background-band gap-filling* — the variant above: a standing low-OS-priority pool the OS preempts out the instant foreground work is ready. (ii) *dynamic oversubscription around a blocking region* — `FOversubscriptionScope` bumps a count that wakes a parked standby worker (or spawns one under dynamic thread creation, `IncrementOversubscription`); surplus workers self-retire via `ConditionalStandby` when the count drops. (ii) keeps the pool alive when a task genuinely BLOCKS (mutex / I/O / `Wait` on unrelated work) — the case our current scheduler does NOT cover. (2026-08: awaits no longer belong on that list — a `co_await` suspends the frame and frees the worker, and retraction is gone. What is left uncovered is opaque blocking inside a body, with no suspension point for us to use; see 5.3.) (ii) is the prior art for closing that gap in the alternative scheduler; see 5.3.
   8. `[ ]` **(P3, micro-opt, raised 2026-07) De-correlated per-worker park-spin timing.** UE gives each worker a distinct prime-ish spin duration from a small table (`{719,991,1361,…}[worker_id % 8]`, the `YieldCycles` arg to the pre-park spin) so parked workers don't re-scan the queues in lockstep — a cheap thundering-herd mitigation. Our `spin_then_block`/`handoff` idle loops spin a uniform `spin_cycles` on every worker. Cheap to add (a per-worker offset seeded at construction); measure whether it cuts contended eventcount re-scans under a fully-parked burst/drain. Prior art: UE `FWaitingQueue::Park` ([task-systems-comparison.md](task-systems-comparison.md) §UE).

4. **Allocation / control block**
   **Prior art (UE `FTask`, verified from source — concrete shapes for the items below):** `FTask` is exactly one cache line (`LOWLEVEL_TASK_SIZE = PLATFORM_CACHE_LINE_SIZE`) with the body stored INLINE via an SBO `TTaskDelegate` sized to the remaining bytes — validates 4.2 (size the tunable-SBO `Function` buffer so the common task functor never heaps). Per-size-class recycling is `TLockFreeFixedSizeAllocator_TLSCache<256, cacheline>` (TLS-cached, fixed-size, lock-free) — the concrete shape for 4.1 (each `Exec<Body,R>`/`Result_block<R>` instantiation is constant-size per type, so one free-list per size class). An oversized/overaligned closure falls back to a 64 KB-block linear (arena) allocator (`TConcurrentLinearAllocator`) — validates 4.6's SBO-overflow-to-arena path. Executable-task init refcount is 2 (one external handle + one in-system).
   1. `[ ]` **(P2)** Per-type recycling free-list (`Exec`/`Result_block`/bare block).
   2. `[ ]` **(P2)** Tunable-SBO `Function<Sig, N>` replacing `move_only_function`/`function` (also fixes the reservation-path closure alloc — inconsistency #5).
   3. `[~]` **(P2, mostly overtaken 2026-08)** Small-vector / intrusive links for the block's edge vectors. `prerequisites` is gone (deleted with retraction — it existed only for the retraction walk), pipe turns never touched the vectors, and `successors` collapsed to a single `nested_parent` pointer: 320 → 280 → 264 B. Only `continuations` is left, and its fix is 4.7 step 2 (an intrusive waiter list with dependent-side nodes), not a small-vector.
   4. `[ ]` **(P2)** Shrink `Task_control_block` — `completed`/`cancelled`→bits; a futex wait primitive roughly halves the block (now 264 B, of which `std::mutex` + `condition_variable` are the dominant term; ties to the platform layer).
   5. `[ ]` **(P3)** Multi-object `async` `std::map`→sorted `vector`.
   6. `[ ]` **(P2)** Opt-in scoped bump arena (auto for `parallel_for`/graph-run, per-frame opt-in); rebase `journal.h` staging onto it.
   7. `[ ]` **(P1, author 2026-08) Review `continuations`; fold into `successors` (then-as-normal-task) + intrusive edge storage.** `Task_control_block::continuations` (type-erased `move_only_function` callbacks fired at settle) is a THIRD dependency mechanism next to `successors`/`prerequisites` — it predates the `then` rebase and doesn't feel right. Target: `then` launches a normal task everywhere (it already is a real block with the producer as a `num_locks` prerequisite — audit the remaining `attach()` users: `when_all`'s join, multi-async release, `Deferred`/`Versioned` internals) so `continuations` folds into `successors` and the block loses one of its three vectors. Pair with the storage fix (supersedes half of 4.3): a dependent knows its edge count at wiring time, so embed the edge nodes in the DEPENDENT's allocation and thread the producer's successor list intrusively through them (producer holds one head pointer, allocates nothing) — `successors` can never be a fixed array on the producer (fan-in is unbounded), which is exactly why the storage must live dependent-side. Nested-task edges stay dynamic by nature (data-dependent fan-out, count unknowable at allocation). The pipe rebase (1.14, branch `pipe-rebase`) already makes pipe-turn prerequisites vector-free (embedded `Pipe_link`s + the `pipe_count` trigger; no `successors`/`prerequisites` traffic); this item does the same for ordinary edges. Touches `then`/`when_all`/`settle`/retraction — its own project, after the pipe rebase lands.
      **Re-scoped (2026-08, post-6.4).** Both landed prerequisites are in: the pipe is rebased
      (1.14) and `then`/`when_all`/retraction are deleted, which took `prerequisites` with them
      (block 320 → 280 B).
      **Step 1 DONE (2026-08): `successors` → a single `nested_parent` slot, 280 → 264 B**
      (clang-cl x64, measured). The audit that motivated the note found `successors` had
      exactly one producer left — `add_nested` — and it runs once per child (`ts::nested`
      launches its own task; a coroutine node's frame and a nested graph run are each attached
      once), so the fan-out was structurally 0 or 1. A `std::vector<Task_ptr>` was costing 24
      bytes plus a heap allocation on first push to hold one pointer. Now a bare `Task_ptr`,
      with a `TS_SAFETY_CHECKS` fatal on a second attachment (which would previously have
      silently queued a second parent, and would now silently drop one).
      **Step 2, the remainder — designed, NOT implemented.** `continuations`
      (`std::vector<move_only_function<void(void*, bool)>>`, 24 B + a closure allocation past
      SBO) is the last edge vector. Three users: the coroutine `Task_awaiter` (one per awaiting
      frame), `Join_awaiter` (one per joined child), and `Versioned::publish`'s internal chain.
      Fan-in is unbounded in principle (N coroutines may await one handle) but is 0 or 1 in
      every measured path. The target shape is an **intrusive waiter list with the nodes in the
      DEPENDENT**: an awaiting frame knows its own edge at `await_suspend` time and already has
      a frame to embed the node in (this is exactly where 6.2's fusion pays off a second time),
      so the producer keeps one head pointer and allocates nothing, and `settle` walks a list
      instead of draining a vector. Estimated win: 16 B of block plus one allocation per
      suspended await. **Why it is not done here:** unlike step 1 it is not a local change —
      it rewrites `settle`'s drain (the most concurrency-sensitive function in the core, whose
      current shape is load-bearing: everything is moved out UNDER the lock and fired after the
      notify, for the reset-race reason documented at the site), and it must re-prove the
      awaiter's two-state resume handshake against a list the awaiter now owns storage for.
      That is a project with its own TSan campaign, not a slimming pass. Sequence it after
      4.1/4.2 (a size-class pool and an SBO callable would independently remove the allocation
      half, which may make the remaining 16 B not worth the risk — measure first).
      **Note (author, 2026-08) — "the shipping block size" is no longer one number.** This item
      has been assuming 264 B is what ships. That stops being true once users take up the
      standing invitation to leave part of the harness enabled in a shipping build, because two
      independent things then follow the rule policy rather than `TS_SAFETY_CHECKS`:
      (a) the task's `Named` (24 B, `task.h`) is currently gated on `TS_SAFETY_CHECKS`, but the
      rules that *print* task identity are not all dev-only — `deadlock_net` (6.13) is available
      in shipping and its entire value is dumping the suspended frames, which without names is a
      list of pointers; `in_task_sync` (6.10) is likewise shipping-available. So the field's
      gate should follow "is any name-printing rule compiled in", not `TS_SAFETY_CHECKS`, and
      the block's shipping size becomes a function of `TS_ENABLED_RULES`. (Object and node names
      are already kept in shipping, so `access_rank`'s and the waits-for detector's *object*
      naming is unaffected — this is only about task identity.)
      (b) 6.14's rank field on `Guarded` has the same shape.
      Consequences for this item: size targets must be stated per rule-policy, not absolutely;
      and the interning idea rejected in the 2026-08 naming discussion (a global registry +
      an 8-byte pointer, dismissed because shipping paid nothing for names) **acquires a real
      target** in exactly this configuration — that was the recorded trigger condition, and this
      is it. Do not act now; settle it when 6.13 lands and the shipping-harness story is
      concrete.
   8. `[ ]` **(P2, author 2026-08) Cache-line alignment audit across components.** A systematic pass over every hot shared structure for cache-line placement: separate fields written by different threads onto distinct lines (`alignas(std::hardware_destructive_interference_size)` where warranted), keep fields read/written together on one line, and check array elements for false sharing between adjacent entries. Inventory to cover: `Task_control_block` (the size-ordered cluster is packing-motivated, not sharing-motivated — e.g. `num_locks` (contended decrements) shares a line with `refcount`; check whether that pairing helps or hurts), the evolved `Pipe` (all queue state now lives under one mutex, so the interesting question shifted: is `writer_owner` — read lock-free by every `commit()` ownership check — on the right line relative to the mutex and the queue head, and does a coroutine frame's embedded link share a line with hot promise state), `Pipe_link` arrays (adjacent links of one task live on one line; different lines' traffic collides — measure before padding, links are per-task not global), scheduler queues/deques (Chase-Lev top/bottom, MPMC slots; `Busy_slot`/`Bucket_row` are already padded — verify the rest), journal slots, `Event_count`. Measure with the existing benchmarks (contention series + R10 pipe fixture) — padding trades memory for isolation, so each change needs a number, not a vibe.

5. **Fork-join / parallel_for**
   1. `[ ]` **(P2) Intra-system entity interactions** — ship the primitive menu: `parallel_gather_apply` (mailbox), `parallel_for_colored` + `Interaction_coloring`, `Accumulator` (commutative), `Union_find` helper, + triage docs. Open author questions. [§D5]
   2. `[S]` **Priority propagation / inheritance** — designed (pipe / graph / dynamic; opt-in surfaces; not OS thread priority); revisit on a demonstrated inversion. [§D6]
   3. `[ ]` **(P3, re-scoped 2026-08) Reserve / standby workers** — sequenced after the platform layer. The framing changed with 6.4: retraction is gone, and a coroutine wait SUSPENDS rather than blocking, so the fork-join case this used to cover is structurally handled (a suspended frame holds no worker). What remains is genuine blocking a task system cannot see through — an OS mutex, a synchronous file read, a driver call inside a node body — which still occupies a worker with no suspension point. UE's answer is dynamic oversubscription: `FOversubscriptionScope` around the blocking region wakes/spawns a standby worker (`IncrementOversubscription`), `ConditionalStandby` retires it after (verified from source). This is scheduler-impl territory — belongs to the alternative two-pool scheduler (3.7), not the task layer; both mechanisms sit behind the unchanged `Task` API.

6. **Coroutines**
   1. `[x]` **DONE (2026-08, subsumed by 6.4).** Inline-when-free for awaited accesses is what
      the awaitable access verb does: `co_await obj.access(fn)` is `await_ready` when the pipe
      is free (runs `fn` on the caller) or when `writer_owner == current task` (reentrant,
      under the held grant), and suspends only when it would actually have to wait.
   2. `[x]` **DONE (2026-08, 6.4 stage 1).** Coroutine-frame / control-block fusion — one alloc for frame + block, not two. `Task_promise<R>` carries `Task_control_block core` as its FIRST member (so the block pointer doubles as the promise pointer) plus the result storage, and the block's `destroy` thunk destroys the coroutine frame. A coroutine task therefore allocates exactly the frame; coroutines reduce allocations rather than adding them, as the item required. Remaining allocation work is 4.1 (routing `operator new` on the promise to a size-class pool).
   3. `[ ]` **(P3)** Priority setter on the promise (it stores one; no config channel yet).
   4. `[x]` **DONE (2026-08, branch `pipe-rebase`) — coroutine-first transformation.** The shakeup: static graph + coroutines for everything dynamic; `then`/`when_all`/builder-`after`/retraction/reuse/inline-trampoline removed; `Task_scope` nursery + implicit per-frame scopes + coroutine graph nodes + awaitable access verb added; every illegal case a fatal with a companion how-to test. Design of record + staged plan: [coroutine-first.md](coroutine-first.md). Landed §7 stages 1–6; the remaining §11 action list is tracked as 6.9 below. Subsumed 6.1 (the awaitable access verb IS inline-when-free) and delivered 6.2 (frame/block fusion — the promise embeds the block, so a coroutine task is one allocation). Note the inline-dispatch trampoline was removed only from the DYNAMIC surface: `Graph_node::set_inline` and the `dispatch_ready`/`inline_pending` machinery survive as graph-internal.
   5. `[x]` **DONE (2026-08) — waits-for cycle detector** (`TS_SAFETY_CHECKS`). The suspended-ABBA deadlock (a task holding G1 suspends awaiting G2's turn while a G2-holder awaits G1) parks no thread — both frames suspended, all workers free, the run silently never completes; graph-invisible by definition (the accesses are undeclared). At suspension-on-a-pipe record edge {holder's grants -> awaited pipe} (the harness knows both), clear at resume, cycle-check on insert, fatal naming both tasks + both objects. Gates blessing waiting-rule case (c) (coroutine-first.md §2) in the guide.
      **Scope ruling (2026-08, field survey): keep it on GRANT edges; do not generalize it to
      arbitrary `Task`/`Signal` await edges.** That generalization is what Linux has failed to
      merge twice in eight years — lockdep's cross-release (covering `wait_for_completion`)
      landed in 4.14 and was reverted in 4.15 for false positives under a zero-false-positive
      policy, and its successor DEPT, which models exactly this class of event/completion
      edges, is unmerged after 4+ years because "the tool generates so many reports that it is
      difficult to get a real signal out of the noise" ([LWN 1036222](https://lwn.net/Articles/1036222/)).
      Our grant edges are structurally lock-like with known holders, so the narrow form behaves;
      the general case is covered by 6.13's quiescence net instead, which is O(1) and cannot
      false-positive on an ordering it merely finds surprising.
   6. `[ ]` **(P2, scoped 2026-08 — needs the platform layer, 3.6) Signal-from-OS-completion helper** — register an OVERLAPPED / fd / fence, get a `Signal` (the 9.2 packaging question, pulled by the first-class cross-frame pattern, coroutine-first.md §4.7).
      **Design note (2026-08).** The bridge itself is already trivial and needs nothing new: a
      `Signal` is a refcounted handle, so an OS callback captures one by value and calls
      `trigger()` — that is the whole "get a `Signal`" story, and 6.7's `Frame_gate` now covers
      the realignment half. What is NOT trivial, and what any packaged helper must answer, is
      **who owns the waiting**. Three shapes, in increasing commitment:
      (a) **Nothing** (today): the user registers the completion with the OS themselves and
      calls `trigger()` from whatever callback context the OS gives them. Zero API, zero
      portability surface; the user carries the caveat below.
      (b) **A thin per-platform adapter** — `Signal signal_from(HANDLE overlapped_event)` /
      `signal_from(int fd, events)` / a GPU-fence variant. Each needs a THREAD to do the
      waiting (a `WaitForMultipleObjects` pool, an epoll loop), i.e. exactly the reactor the
      library has deliberately not built (9.2), and it multiplies with every platform. Not
      worth it until a real workload asks.
      (c) **A reactor.** Explicitly rejected in 9.2 (Rayon/Tokio are separate pools by design).
      **The caveat that matters regardless of shape**, and the reason this is not purely a
      packaging question: `trigger()` releases awaiting frames on the TRIGGERING thread via the
      resume trampoline. An OS completion callback (an APC, an IOCP worker, a driver callback)
      is the worst possible place to run arbitrary user coroutines — it may be a restricted
      context, and it is certainly not a place to run unbounded work. So an OS-triggered
      `Signal` should hop: `ts::launch([s]() mutable { s.trigger(); })` from the callback, which
      is one line and is exactly what `Frame_gate::open()` does for the same reason. **Action:
      document (a) + the hop as the sanctioned idiom in the guide; revisit (b) only alongside
      the platform layer (3.6), which is where the waiting threads would live.**
   7. `[x]` **DONE (2026-08) — per-frame gate.** `ts::Frame_gate` (`frame_gate.h`, not in the
      umbrella — include it): `co_await gate.next()` parks a task until the frame loop's next
      `open()`. Shipped as a type rather than a documented `Signal::reset` idiom because the
      hand-rolled version has two real hazards: a missed-wakeup window (a task reading the
      signal just before a boundary can attach to a gate about to be re-armed) and
      `Signal::reset`'s precondition that every waiter is already released (fatal otherwise).
      The gate hands out the CURRENT frame's signal under a mutex and installs a fresh one at
      `open()`, so a waiter always names a specific frame's gate and no re-arm race exists —
      trading one bare block per frame, which is noise at frame scale (`Signal::reset` stays
      available for the zero-alloc case). `open()` releases through the scheduler rather than
      inline, at `Priority::low` by default (`set_release_priority` overrides): an inline
      trigger would run every parked frame on the frame loop's own thread before `open()`
      returned, stalling frame start by an unbounded amount — the low-priority-resumption
      default this item asked for, made structural. 3 tests.
   8. `[ ]` **(P2, author 2026-08 — promoted to P1 on an aggregate, then DEMOTED by the
      decomposition that aggregate asked for) Symmetric task switching for coroutine
      resumption.**
      *Correction (2026-08):* the P1 promotion below reasoned from "~95% of a chained stage
      is the wake/dispatch round trip, and symmetric transfer is the lever for it". The first
      half is true; the second does not follow. The measured split of the ~1930 ns default
      stage is frame 91 ns (5%) · same-thread queue hop 248 ns (13%) · first cross-thread hop
      331 ns (17%) · idle-pool scale 770 ns (40%) · wake+park 490 ns (25%). Symmetric transfer
      can claim at most the same-thread queue hop — ~13%, and less in practice since the
      resume trampoline already skips the queue. Everything else is paid dispatching the stage
      *outward*, before any resume exists to transfer to. The real lever for the remaining
      ~1600 ns is **locality**, and the obvious shape (awaiting a not-yet-started task runs it
      inline on the awaiting thread) is exactly the retraction coroutine-first deleted on
      purpose — so it needs a design answer, not an optimization. Caveat on the measurement:
      the benchmark is a strictly serial chain, which maximally rewards locality and is
      therefore the most favourable possible case for that lever, not a representative one.
      *Why P1:* the resume round trip is now the largest measured cost in the coroutine core,
      established twice without looking for it. (a) The `coro chn` decomposition (N3): 88 ns/stage
      of actual coroutine machinery against 1809–2139 ns/stage for a chained wait — ~95% is the
      wake + dispatch + resume-on-another-thread round trip, not the frame. (b) The graph-free
      frame measurement (`sample/game_frame.cpp`, 2026-08): graph-free costs +56–131 µs/frame,
      matching ~50 suspend/resume round trips at ~1.8 µs; the +95 allocations/frame are under
      2 µs of it. So the graph's performance advantage over hand-composed coroutines is **resume
      locality, not allocation amortization** — and this item is the lever, not the allocator.
      Not yet decomposed: how the ~1.8 µs splits between the wake syscall, the queue/eventcount
      hop, and the cache-cold resume. Measure that first (an `Idle_policy::spin` vs
      `spin_then_block` comparison isolates the wake syscall) — it decides how much of the gap
      symmetric transfer can actually claim. UE's runnable signature is `FTask*(bool)` — a body/segment can RETURN the next task, which the worker loop runs immediately on the same thread with zero queue/eventcount interaction ("Continuations were not themselves dequeued from any queue"); `CallAndMove` relocates the delegate storage in one step, so no second alloc for the returned continuation. This is the natural shape for a coroutine `co_await` handing back "resume me here next" — cleaner than routing the resume through our thread-local inline trampoline (`dispatch_ready`/`inline_pending`, task.h), which already gives same-thread + stack-safe execution but via a push+drain rather than a direct return-and-run. Scope the borrow to the coroutine path (pairs with 6.2's frame/control-block fusion — a fused frame that returns its own next segment); NOT a rework of general dispatch, which stays framework-driven so a queued resumption still gets a priority and the scheduler can interleave. Verified against UE source (`FTask::ExecuteTask`, `FScheduler::ExecuteTask` loop). Prior art: [task-systems-comparison.md](task-systems-comparison.md) §UE.
      **Rescoped (2026-08, post-6.4):** the coroutine path now has its own equivalent — a
      bounded thread-local *resume trampoline* (`resume_pending`/`schedule_resume`,
      coroutine_support.h) that resumes a released frame on the settling/granting thread with
      no queue hop, iteratively so the stack stays O(1). `dispatch_ready`/`inline_pending`
      survive but are graph-internal (`Graph_node::set_inline`) and no longer carry resumption.
      So the borrow narrows to the *worker-loop* half — a runnable that RETURNS its next
      runnable, which would let a resume skip even the trampoline's push+drain. Measure the
      trampoline first (10.14) -- **done, and it promotes this item**: `coro chn` is ~95%
      wake/dispatch latency (2138/1959/1809 ns per stage, of which only ~88 ns is coroutine
      machinery), so a resume that skips the queue entirely is where the remaining cost is.

      **Decomposed (2026-08, and it RESCOPES this item again).** The measurement this item
      asked for first is done: the same `chain_coro` benchmark run on differently-configured
      global schedulers (`bench_coro_chain_on` + `Scheduler_scope`, benchmarks.cpp — five of
      the configurations are kept as a permanent `--bench` section). Idle policy isolates the
      wake syscall, worker count isolates the cross-thread hop. Release, 22 hw threads, quiet
      machine, 3 full runs; ns per awaited stage, median of 4 measured rounds each:

      | configuration | run 1 | run 2 | run 3 |
      |---|---|---|---|
      | 22 w, `spin_then_block` (the `coro chn` line) | 1932 | 1996 | 1814 |
      | 22 w, `spin` | 1414 | 1439 | 1458 |
      | 4 w, `spin` (not kept) | 810 | 819 | 800 |
      | 2 w, `spin` | 681 | 668 | 659 |
      | 2 w, `spin_then_block` (not kept) | 906 | 892 | 902 |
      | 1 w, `spin_then_block` | 360 | 366 | 366 |
      | 1 w, `spin` | 335 | 337 | 344 |
      | `coro nst` (frame only, no scheduler) | 91 | 91 | 95 |

      Split of the ~1930 ns default-configuration stage:

      | component | isolation | ns | share |
      |---|---|---|---|
      | coroutine frame (alloc, promise, settled await, destroy) | `coro nst` | 91 | 5% |
      | queue round trip on ONE thread (submit, own-deque pop, dispatch, claim, settle, resume trampoline) | 1 w spin − `coro nst` | 248 | 13% |
      | first cross-thread hop (one possible thief: steal + cache-cold block/frame/result) | 2 w spin − 1 w spin | 331 | 17% |
      | idle-pool scale (steal contention on the deques + a more distant core) | 22 w spin − 2 w spin | 770 | 40% |
      | wake + park | 22 w s+blk − 22 w spin | 490 | 25% |

      The wake-syscall component the item predicted is real but is a quarter of the cost, and
      it shrinks with the pool: the same delta is 230 ns at 2 workers and 25 ns at 1 (nothing
      parks when the single worker always has work). **The dominant term (57%) is that the
      awaited stage leaves the awaiting thread at all**, and it gets worse the more idle
      workers there are to take it.

      Consequence for this item: **symmetric transfer can claim at most the 248 ns same-thread
      queue round trip, ~13%** — and less than that in practice, because the resume already
      skips the queue via the resume trampoline; the worker-loop borrow only removes that
      trampoline's push+drain. It can claim NONE of the other 87%, because wake and migration
      are paid dispatching the awaited stage OUTWARD, before any resume exists to transfer to.
      The lever for the 1600 ns is locality, not transfer: keep a task its submitter is about
      to await on the submitter's thread. The obvious shape — an await of a not-yet-STARTED
      task runs it inline on the awaiting thread — is exactly the retraction that
      coroutine-first deleted (deliberately: it was unsound for pipe/`async` work and made the
      blue/red boundary mushy), so reviving it needs a design pass, not a patch; a weaker,
      purely scheduler-side variant is to make the just-submitted-and-about-to-be-awaited task
      unstealable for a short window. Note the benchmark is a strictly serial chain, which
      maximally rewards locality — a policy that keeps work local must not do so when the
      launcher does NOT immediately await, or it converts parallelism into latency. Downgrade
      candidate: on this evidence the symmetric-transfer borrow is a P2 (13% of a cost the
      trampoline mostly already collects), and the locality question is the P1 that inherits
      its "why".

   9. `[~]` **(P1, author 2026-08) Coroutine-first post-initial action list.** The queue behind
      the landed transformation, from [coroutine-first.md](coroutine-first.md) §11. In order:
      (a) `[x]` **nested graph runs v1 — DONE (2026-08).** The lend protocol landed as
      designed: `bind_links_for_run` intersects the compiled access set with the caller's
      `Access_context` at every `execute()` and re-binds the `compile()`-time link slab to the
      objects the run must actually take turns on — an unbound link IS a skipped turn, so no
      bypass flag threads through the pipe and admission keeps one code path. Inner conflict
      edges are untouched (they order inner nodes among themselves on a lent object) and the
      pipe never sees the lend (external `async`s queue behind the caller's hold as before).
      Auto-scope-join default + `Execution_options{.detach}` (a detached run gets no lend --
      it is not contained in the caller's grant window); all three fatals with companions.
      Two refinements vs the design text: the non-quiet-scope check filters SETTLED scope
      children (the scope list only drops entries at a join, so plain emptiness would fatal on
      a fire-and-settle child), and the in-flight fatal is `TS_SAFETY_CHECKS`-gated like its
      siblings. 10 tests + 3 death scenarios + `stress_graph_nested_runs` under TSan;
      (b) `[x]` worker-less nested runs — DONE, no work needed (the serial trampoline already
      handles it; test added); (e) `[x]` **concurrent shared-object graphs — DONE (2026-08),
      and it holds.** The old task-internals §10 scenario-2 line conflated two cases: the "not
      supported" cause was the per-graph single-run `Run_state`, which says nothing about two
      DIFFERENT graphs. Canonical pipe-address ordering over the same address-sorted objects
      makes a cross-graph wait cycle impossible, and the pipe serializes two graphs'
      conflicting nodes exactly as it does a node against an async. Validated with graphs
      declaring the same objects in OPPOSITE order, run concurrently under an async hammer,
      `Rw_probe` oracle: suite test + `stress_concurrent_graphs` under TSan. §10 scenario 2
      relaxed to "safe, nondeterministic cross-graph ordering"; the same-GRAPH case is now a
      fatal instead of silent `Run_state` corruption; (c) cancellation
      composition (pass the outer token into `inner.execute({.token})`, test that outer
      cancellation drains the inner run); (d) trace attribution across nesting (inner work must
      not double-count in the outer trace's fold); (e) concurrent shared-object graphs
      validation (task-internals §10 scenario 2 predates the evolved pipe — with per-node
      admission + globally canonical acquisition it should now be deadlock-free; dedicated
      stress with the `Rw_probe` oracle + TSan, and if it holds, relax the task-internals
      contract line to "safe, nondeterministic cross-graph ordering"); (f) parameter-grants
      sugar for shipped library sub-graphs (compile-time intent check only).
      Open discussion queue: [coroutine-first.md](coroutine-first.md) §10 (waiting-rule
      relaxations, HALO reality on MSVC/clang-cl, the graph-free usage model).

   10. `[x]` **(P1, author 2026-08) Check the rule, not the incident — structural in-task
       `sync()` fatal — DONE (2026-08).** `sync_wait`'s check dropped its `!blk->ready` gate:
       `sync()`/`take()` inside a task is fatal whether or not the target has settled, so the
       first execution of a bad path fails deterministically. `blocking_sync_diagnose` is now
       `[[noreturn]]`, gated on `TS_RULE_ON(TS_RULE_IN_TASK_SYNC)` rather than
       `TS_SAFETY_CHECKS` (only its sharp same-object arm needs the harness), and both
       messages name the participating task and object by `ts::Named` plus the escapes:
       `co_await`, `try_take()` (6.12), and `ts::Relaxed_scope{ts::Rule::in_task_sync}`.
       Rule class **advisory**; shipping default **compiled out**.
       Two in-tree call sites were relying on the old timing gate and are the interesting
       evidence that the item was right: `test_access_reentrant_under_own_grant` did
       `r.sync()` on a provably-settled task (now `try_take()`), and the worker-less
       `test_single_threaded_sync_inside_body` genuinely relies on the serial trampoline's
       drain hook — a claim the library cannot verify, so it now carries a `Relaxed_scope`
       and doubles as the opt-out's companion test. New death scenario
       `sync_settled_in_task` covers exactly the shape that used to pass. Original text follows.
       The diagnostic in `sync_wait` is gated on `!blk->ready`, so it fires
       only when the wait would *genuinely park*. That inverts its coverage: an in-task
       `sync()` whose target is usually already settled never trips in dev (checked builds
       skew further toward "already done" via their own overhead), then parks a worker on the
       one frame a prerequisite runs long — and in shipping the check is compiled out entirely.
       A safety check whose trigger condition is the hazard's *timing* inherits the hazard's
       nondeterminism. Fix: make it unconditional — `sync()` inside a task is illegal whether
       or not the target has settled — which is what the waiting rules already say (coroutine-first
       §8 flag 4, `sync()` demoted to boundary-only). Deterministic: the first execution of the
       path fails, regardless of timing. **Depends on 6.12** — the legitimate
       `if (t.is_done()) v = t.sync();` idiom needs a non-blocking spelling before the
       unconditional fatal removes it. `parallel_for` joins stay structurally exempt.

   11. `[x]` **(P1, author 2026-08) Same fix for the guard-across-suspension fatal — DONE
       (2026-08).** The `pipe_guard_depth > 0` check moved from `await_suspend` to
       `await_ready` in all three awaiters (`Task_awaiter`, `Pipe_guard_awaiter`,
       `Join_awaiter`), so an await under a live guard is illegal whether or not it suspends.
       For `Join_awaiter` the check runs BEFORE the settled children are erased -- whether the
       scope happens to be drained at that instant is precisely the timing the rule must not
       depend on. Each of the three carries its own message naming the sanctioned form
       (functor `co_await obj.access(fn)`; split the scope; multi-object
       `co_await ts::access(fn, a, b)` instead of nested guards) and stating that the rule is
       structural -- no runtime opt-out, only `TS_ENABLED_RULES` (rule class **structural**;
       shipping default **kept**, as before the policy existed).
       **The reentrancy exemption is now stated rather than emergent**, which was the item's
       real content: `detail::reentrant_under_held_grant` returns true when every pipe of the
       awaited block is write-owned by the current task -- i.e. `Guarded::access` took its
       reentrant arm and ran the body inline under the held grant, so the task is settled
       before the `co_await` is evaluated and cannot suspend by construction. It keys off
       `Pipe::writer_owner`, which is always-on state, so the exemption works in every build.
       Deliberately narrow: a READ access under a READ guard is NOT exempt (it reaches the
       pipe, and a queued writer -- blocked by our own read hold -- makes it suspend). New
       death scenario `await_settled_under_guard` covers the shape §2.3(f) of
       [static-order-checking-and-ww-mutex.md](static-order-checking-and-ww-mutex.md)
       identified as passing undiagnosed; companions `co await under guard, split` and
       `co reentrant access under guard`. Original text follows.
       The `pipe_guard_depth > 0` check lives in `await_suspend`, so a `co_await` that happens to
       complete synchronously is never examined — the same timing-luck coverage as 6.10, one
       degree less bad (the check is not `TS_SAFETY_CHECKS`-gated, so the latent case aborts in
       shipping rather than silently serializing). Fix: hoist the test to `co_await` *entry*
       (`await_ready`) so any await under a held guard is illegal whether or not it suspends;
       cost is one branch on a TLS counter on a path that already branches. Not purely
       mechanical: today the reentrancy exemption is **emergent** (a reentrant same-object
       access never suspends, so it never reaches the check). Hoisting forces it to be stated —
       an awaiter for an object already in the grant set skips the check, everything else does
       not — which makes the rule legible rather than implicit. Land with 6.10; same principle,
       same test-matrix rows.
       **Confirmed load-bearing (2026-08), not cosmetic:** the ww_mutex evaluation found that
       because the fatal sits in `await_suspend`, a hold-then-await runs *fine* whenever the
       target pipe happens to be free at that moment — so the illegal pattern executes
       undiagnosed on exactly the runs where timing is friendly, which is the A.1 inversion in
       its purest form. It also found that both *guard-form* ABBA shapes are already closed by
       these fatals; what survives is node/pipe-job declared grants plus a `co_await`, which is
       6.14's territory. That narrows the residual hole and raises this item's value.

   12. `[x]` **(P1, author 2026-08) Non-blocking result accessors — DONE (2026-08).**
       `Task<R>::try_take()` returns `std::optional<R>`, never blocks (empty when unsettled
       OR cancelled) and is therefore legal inside a task -- the escape valve 6.10 needs;
       `co_await t.as_optional()` performs the same wait as `co_await t` but yields an empty
       optional instead of fatalling on a cancelled task (a `detail::Optional_awaitable`
       marker from task.h, made awaitable by an `operator co_await` in `ts::detail` -- ADL
       looks where the argument type lives -- resolving to an `Optional_awaiter` that reuses
       `Task_awaiter`'s handshake and only differs in `await_resume`). `sync()`/`take()` are
       unchanged. **`ts::Maybe<R>` dropped by the author**: both verbs simply do not exist for
       `void`, since a void task has no result to be missing (`is_done()` answers the first
       question, and awaiting a cancelled void task already resumes normally). Both MOVE the
       result, so they join `take()` under the existing "at most one mover, and it must be
       last" rule rather than adding a third kind of read. Original text follows.
       Fatal-on-cancelled is the wrong default for a value `sync()`: the
       caller cannot check-then-take without a race, and the fatal punishes them for a state
       the callee chose. Do NOT change `sync()`'s return type — every call site that cannot be
       cancelled (no token in play, the common case) would pay an unwrap forever. Three
       spellings instead: `sync()` unchanged (blocking, boundary-only, fatal on cancelled — the
       "this cannot be cancelled" assertion); **`try_take()`** — non-blocking, empty when
       unsettled *or* cancelled, and legal inside a task (it never parks), which is also the
       escape valve 6.10 needs; **`co_await t.as_optional()`** — the red-side equivalent,
       replacing the fatal-on-awaiting-cancelled with a branch. Void asymmetry: define
       `ts::Maybe<R>` as `optional<R>` for non-void and **`optional<std::monostate>`** for void
       — presence-testing *and* dereference stay uniform, so generic code needs no
       `if constexpr`. (The `bool`-for-void variant buys only the declaration: `*m` is invalid
       for `bool`, so every generic consumer that extracts still branches — barely worth a name.)

   13. `[x]` **(P1, author 2026-08) Global quiescence deadlock detector — DONE (2026-08).**
       Implemented in the three-part decomposition the author asked for, so the task layer
       stays uncoupled from the scheduler:
       (a) **quiescence is scheduler-local** — `Scheduler::quiescent()` = every worker in the
       idle path AND every queue empty, exposed through the plain function seam
       `detail::scheduler_quiescent()` (defined in guarded.cpp, where the global holder is;
       same shape as `drain_serial_pending` / `blocking_sync_diagnose`). Idleness is an
       explicit busy<->idle **transition** count in `Worker_thread::main`, not "parked":
       under `Idle_policy::spin` a worker never parks, so a park-based test would never
       report idle. Two atomics per crossing, none per task. The seam returns false when no
       scheduler exists yet -- a blue wait before the pool is created must not conjure one as
       a side effect of a safety check.
       (b) **the task layer interprets it from the boundary waiter** — no global per-task
       counter, nothing on the hot path: `Task_control_block::wait()` already blocks, so it
       polls while it is there. Quiescence must hold CONTINUOUSLY for the window (default
       2 s over 8 samples, `ts::set_deadlock_net_window`; 0 disables): one sample is
       worthless because a worker sits briefly between finding work and marking itself busy,
       and a long window is free because a real deadlock is permanent while a short one would
       fire on a slow-but-legitimate blue-to-blue handoff.
       (c) **`ts::External_wait`** (RAII, task.h) is the outstanding-external-wakeup counter,
       and `Frame_gate` holds one from the first `next()` of a frame until the matching
       `open()`. The fatal names it, per the item's own warning that a forgotten registration
       reports a correct program as deadlocked; it also names
       `ts::set_deadlock_net_window(0ms)` and `TS_ENABLED_RULES`.
       Rule class **net** (no scoped opt-out -- it observes the whole process, so there is no
       call site to attribute a relaxation to); shipping default **compiled out**. Death
       scenario `deadlock_net` plus two companions (`External_wait` suppresses it; the window
       switch disables it).
       **Scoped out, deliberately:** the item wanted the fatal to dump every suspended frame.
       That needs a registry of live suspended frames, i.e. per-suspension bookkeeping --
       exactly what (b) was designed to avoid -- so the report names the blocked waiter (by
       `ts::Named`, from 6.19) and the state that justified the verdict, and the frame dump is
       deferred until a debugging session actually demands it. Original text follows.
       The waits-for
       detector (6.5) fires *before* a deadlock and names the participants, but only for the
       shapes it models: an edge is recorded solely when a task suspends on a pipe job while
       holding grants. Cycles mediated by a task-await edge are invisible — N holds G1 and
       awaits foreign task T (not a pipe job, no edge), while T awaits G1 (holds nothing, no
       edge); both frames suspend forever and nothing fires. The sound complement is Go's
       `all goroutines are asleep` check: **every worker idle + un-settled work outstanding +
       nothing external able to wake us ⇒ progress is impossible ⇒ fatal**, dumping every
       suspended frame. O(1), no waits-for graph, and it misses no deadlock shape — including
       lost-wakeup bugs that are not cycles at all. Two requirements: (a) an
       **outstanding-external-wakeup counter** — anything completable by a non-worker thread
       (I/O, a `Signal` triggered off-pool, `Frame_gate`) registers while pending, so
       quiescence with a zero count is a genuine deadlock and not a legitimate wait; (b) idle
       must be an explicit idle-worker count + empty queues + zero in-flight, since `spin`
       workers never park. Relationship: quiescence is the safety net (always-on candidate),
       6.5's graph stays the precise dev-mode diagnostic. Ranks first of 6.10–6.14 — it also
       retires the blind spot above.
       **Field evidence (survey, 2026-08):** Go's equivalent check has a documented blind spot
       — it fires only when *every* goroutine is asleep, so any live background thread masks a
       partial deadlock ([golang/go#13759](https://github.com/golang/go/issues/13759)). Our
       version is better positioned only *because* of the outstanding-external-wakeup counter,
       which supplies the "nothing can wake us" predicate Go lacks. The counter is load-bearing,
       not an optimization — and its failure mode (a forgotten registration ⇒ a FALSE deadlock
       report) means the fatal message must name its own escape hatch.

   14. `[x]` **(P1, author 2026-08 — the one gap the field survey identified) Declared rank
       for dynamically-awaited objects — DONE (2026-08).** `ts::Rank{n}` (access.h) as an
       optional second leading argument on `Guarded`/`Versioned` after `ts::Named`; stored on
       the pipe and carried into every `Access_context` entry alongside the grant epoch, so
       the held side is read from the context that already exists. Checked at `co_await`
       ENTRY (`await_ready`, next to 6.11's guard check) in both awaiters, so it fires
       deterministically on the first offending await rather than needing two halves of a
       cycle to interleave.
       As the item required, **the rank is NOT defaulted** (to address order or anything
       else) and unranked is the strict state: holding an unranked object forbids dynamic
       awaits entirely. Two rejected shapes, two death scenarios (`access_rank_descends`,
       `access_rank_unranked`); two companions (climb; per-scope opt-out). Rule class
       **advisory** -- `ts::Relaxed_scope{ts::Rule::access_rank}` is the escape; shipping
       default **compiled out**, and like `waits_for_cycle` it is masked off entirely without
       `TS_SAFETY_CHECKS`, because its held-side input IS the access context.
       Two things fell out of implementing it, both kept:
       (1) **Stale grants must be skipped.** A detached coroutine carries its launcher's grant
       snapshot for its whole life; without the staleness filter (the same epoch test `check`
       uses) it would be treated as a holder forever and every later await rejected. This
       showed up immediately in `game_frame`'s fire-and-forget streaming coroutine.
       (2) **The rule makes a genuine ABBA unrepresentable**, which means the `waits_for_cycle`
       death scenario can no longer be *constructed* without opting out of the rank rule --
       `abba_body` now carries a `Relaxed_scope`. That is the honest relationship between the
       two: rank prevents, the detector is what remains for programs that opted out.
       Adoption in-tree: three `game_frame` objects (`input`/`assets` low, `asset_source`
       high) and the coroutine-graph-node test's trio. Everything else never awaits outside
       its declared set and needed no rank -- which is the intended shape. Original text follows.
       The canonical pipe-address order covers BATCH
       acquisition (`multi_acquire`, node declared sets) and makes it deadlock-free. Nothing
       relates a grant a task already HOLDS to an object it awaits LATER — that single missing
       ordering constraint is the entire suspended-ABBA hole. The field's cheap answer is a
       lock rank: `Guarded` takes an optional `ts::Rank{n}` (alongside `ts::Named`), and every
       dynamic await asserts `rank(target) > max(rank of everything currently held)`. Havender's
       argument then makes the cycle unrepresentable.
       Why this shape over the alternatives: **O(1)** (one TLS max against one field;
       `Access_context` already tracks held entries — Go's `lockrank` is a load and a branch,
       and is off by default only for a struct-size reason, not a speed one), and it fires
       **deterministically on the first offending await**, where a waits-for cycle detector
       needs both halves to actually interleave — a scheduling coin-flip. Driver Verifier makes
       the same choice explicitly: it bugchecks on *"the hierarchy violation… not when an actual
       deadlock situation is occurring"*.
       Composes with what exists: batch acquisition keeps address order; only the sequential
       held-then-await edge needs ranks, so only objects that are actually awaited need one.
       **Design detail that is load-bearing:** do NOT default the rank to address order —
       rejection would become ABI-dependent and non-reproducible across builds. Require an
       explicit rank for any object that may be dynamically awaited; default = "no dynamic
       await permitted while held", the strictest and most common case.
       Honest cost, the standard one for this family: a strict order rejects some correct
       programs; the escape is to restructure (split the node — the §10.4 preferred form) or
       opt out per scope. Prior art: Go `runtime/lockrank.go`, Williams' `hierarchical_mutex`,
       Linux `CONFIG_PROVE_RAW_LOCK_NESTING`, Boyapati et al. OOPSLA'02 (the type-level version).
       **Free static complement — EVALUATED (2026-08), adopt narrowly.** Full report with
       compiler output: [static-order-checking-and-ww-mutex.md](static-order-checking-and-ww-mutex.md).
       clang-cl **22.1.3 ships in VS 18 Community**, so `ACQUIRED_BEFORE` is default-on under
       plain `-Wthread-safety`, and the flag is silent over the current headers — enabling it
       costs nothing today. Two claims in the earlier draft of this item were **wrong** and are
       corrected here: (1) it does NOT cycle-check the declarations themselves — there is no
       declaration-level check at all (verified under `-Weverything`); the analysis is entirely
       use-driven, so an unused inverted rank pair is never diagnosed. (2) "named globals only"
       is false — the attribute fires on *struct members* through a `World&` parameter and
       through lambda captures, so all 38 of `game_frame`'s objects are coverable.
       Real limits, measured: **function locals cannot carry the attribute at all** (hard
       `-Wignored-attributes` rejection) — 268 of the repo's ~410 `Guarded` objects, giving ~9%
       repo coverage against ~75% of the samples. And the *held* side is unautomatable: a node
       body must hand-write `REQUIRES(world.phys, world.nav)` duplicating what `compile()`
       already derives (42 sites in `game_frame`), with silent drift when it diverges. One
       defect worth knowing: `ACQUIRED_AFTER` is silently order-dependent — identical code
       warns or not depending on source position relative to an unrelated acquisition.
       Verdict: ship `TS_CAPABILITY`/`TS_ACQUIRED_BEFORE` macros (no-ops on MSVC, verified
       zero-noise) plus a guide recipe, as a free static mirror of the ranks this item makes
       users write anyway. Strictly a subset of 6.14 at higher annotation cost — **6.14 first.**

   15. `[x]` **(P1, author 2026-08 — big, design first) Escape hatches for the waiting rules,
       with a declared shipping policy — DONE (2026-08).** Design of record:
       [waiting-rule-policy.md](waiting-rule-policy.md); mechanism in `include/ts/rules.h`.
       Landed: a flat `ts::Rule` enum mirroring `TS_RULE_*` preprocessor bits (so a disabled
       rule's code AND state vanish), three classes over it, `ts::Relaxed_scope` +
       `ts::set_default_relaxed_rules`, and `TS_ENABLED_RULES` with the same
       `detect_mismatch` ODR tripwire as `TS_SAFETY_CHECKS`. Defaults reproduce today's
       behavior exactly (everything checked; `await_under_guard` only in shipping). The two
       existing checks were rewired onto it: `await_under_guard` and `waits_for_cycle`.
       **The design question the item left open answered itself, against the assumption in
       (a):** the runtime opt-out cannot be uniform. Relaxing `await_under_guard` is not
       merely permissive but unsound — a `Pipe_guard` installs its own `Access_context` as
       the thread's current grant, so a frame suspended under one resumes with the promise's
       snapshot installed over it and then restores a `current_access` pointer captured on a
       different thread. That splits the rules into **advisory** (scoped opt-out +
       compile-out), **structural** (compile-out only; escape is the sanctioned form) and
       **net** (6.13; no call site to scope, escape is `ts::External_wait`). Inheritance
       resolved as "yes, but not via `Access_context`": the relaxation is its own
       thread-local, carried across coroutine segments by the promise (`Relaxed_carrier`) and
       captured into launched sub-work by `with_inherited_access` — `Access_context` is
       snapshotted at frame CREATION, so a mask living in it would be silently dropped by any
       `Relaxed_scope` opened mid-body. Obligations on a new check are the doc's §8 checklist.
       Original text follows.
       Raised by the observation that a user may uphold a
       rule by means we cannot see — an external lock discipline, a phase invariant, a
       platform guarantee — in which case our check is a false positive and there must be a
       way out. Two axes, both needed:
       (a) **Opt-out granularity** — a scoped RAII form (`ts::Relaxed_scope{ts::Rule::foreign_await}`
       or similar: documents the claim at the site, keeps the rest of the program checked) and
       a global default for teams that want the rules as advice. Scoped is the primary form;
       global is the blunt instrument.
       (b) **Compile-out policy** — which checks a shipping build keeps. This is the reason
       every rule check must be *capable* of being compiled out even when it is currently
       always-on (the guard-across-suspension fatal is always-on today with no way to disable
       it, which is exactly the case that motivated this item). The user should be able to
       declare the set: e.g. keep the cheap structural checks, drop the expensive diagnostics.
       Design questions to settle: the rule taxonomy (what is individually switchable — a flat
       enum of rules, or classes like structural/diagnostic/net?); interaction with 6.10–6.14
       (each new check must state its escape and its shipping default *when it lands*, not
       retrofitted); whether an opt-out is inherited by nested/child tasks (probably yes, via
       `Access_context`, but that makes it wider than the lexical scope suggests); and how an
       opt-out interacts with the quiescence net (6.13), which cannot be locally suppressed
       because it fires globally. Prior art: TSan suppressions, `[[clang::no_sanitize]]`,
       lockdep's per-class annotations, Orleans' scoped `AllowCallChainReentrancy()`.

   16. `[ ]` **(P2, author 2026-08) `when_all` as an AWAITER (not a Task combinator).** The
       deleted `when_all` was a `Task`-level join (its own block, a result tuple, ~6 allocs —
       the worst per-op offender in the old allocation audit). The awaiter form is a different
       and much cheaper animal: a countdown plus one resume, no join block, no tuple. It is
       also a popular and expected feature in coroutine libraries independent of any static
       graph, so its absence is a gap in the graph-free story (guide §6.4). Concretely wanted
       for the cross-frame pattern, where `co_await io_done; co_await gate.next();` is two
       suspensions and two potential worker wakes for what should be one wait — see the
       narrower `gate.next_after(io_done)` idea in 6.9(g). Design: variadic awaitables, resume
       once when all have fired, results by reference from the individual handles rather than
       moved into a tuple (avoids the old allocation profile entirely).

   17. `[ ]` **(P2, author 2026-08 — reopened by the 6.8 decomposition) Retraction, or another
       locality lever.** The resume decomposition (6.8) shows ~57% of a chained coroutine stage
       is the work *leaving the awaiting thread* — the awaited task is dispatched outward to
       whatever worker steals it, and the resume is dispatched again afterwards. That cost
       grows with the number of idle workers (1930 ns at 22 workers, 681 ns at 2, 335 ns at 1).
       The classic lever is exactly what coroutine-first deleted on purpose: **awaiting a
       not-yet-started task runs it inline on the awaiting thread** (retraction). It was deleted
       because as a *blocking* mechanism it could not be made safe under access control (see
       [retraction-vs-pool-exhaustion.md](retraction-vs-pool-exhaustion.md) and coroutine-first
       §3) — but the suspension-based form is a different proposition: a suspended awaiter holds
       no thread, so the pool-exhaustion argument that killed it does not apply. What must be
       re-derived from scratch: whether running the awaited task inline is safe with respect to
       the awaiter's held grants (it inherits the context, so this is waiting-rule case (a) —
       plausibly yes), whether the claim protocol can be reintroduced without the
       claim/generation machinery that was removed, and what it does to determinism. Measure
       before building: the 6.8 benchmark is a strictly serial chain, the case that maximally
       rewards locality; a realistic fan-out workload may show much less. Do not reintroduce
       blocking retraction.

   18. `[ ]` **(P3, evaluated 2026-08) `TS_CAPABILITY` / `TS_ACQUIRED_BEFORE` macros — a free
       static mirror of 6.14's ranks.** Report + compiler output:
       [static-order-checking-and-ww-mutex.md](static-order-checking-and-ww-mutex.md).
       Ship thin macros wrapping Clang's thread-safety attributes (no-ops on MSVC, verified
       zero-noise; clang-cl 22.1.3 ships in VS 18, so `ACQUIRED_BEFORE` is default-on under
       plain `-Wthread-safety` and is silent over the current headers) plus a guide recipe for
       the one shape that works: node body annotated `REQUIRES(<declared set>)` + a
       `SCOPED_CAPABILITY` order token at the dynamic await, which produces the exact ABBA
       diagnostic with zero false positives in free-function, lambda and coroutine forms.
       P3 because it is strictly a subset of 6.14 at higher cost: function locals cannot carry
       the attribute at all (~9% repo coverage, ~75% of samples), the held side must be
       hand-written per body (42 sites in `game_frame`) and can drift silently from what
       `compile()` derives, there is no declaration-level cycle check (use-driven only), and
       `ACQUIRED_AFTER` is silently source-order-dependent. Do 6.14 first; revisit this as an
       opt-in extra for users who want a compile-time signal on their own named state.

   19. `[x]` **(P2, author 2026-08) Debug name on every task — DONE (2026-08), as
       author-specified.** One `ts::Named` (`include/ts/named.h`) for nodes, objects and tasks;
       `Node_name` deleted. It stores `{literal, file, line}` — not a whole `source_location`,
       since `function_name()` is never used — displayed as the literal else
       `basename(file):line`. Required for nodes and objects (their unnamed constructors and
       the `node<N>`/`objN` fallbacks are gone; `ts::Named{}` is the "identify me by my call
       site" spelling); optional for tasks, where the verb captures the site by default.
       The three design points settled:
       (a) **cost** — node and object names stay in shipping (they feed the DOT dump and the
       trace, both shipping-capable, over a handful of entities); the task's `Named` is
       `TS_SAFETY_CHECKS`-gated, field included, so the 264 B block is unchanged in shipping.
       (b) **the coroutine case** — a promise sees the coroutine's arguments, not its call site,
       so a frame cannot capture one; it INHERITS the identity of the task it was created
       inside, which for a coroutine node body is the node — exactly the participant a
       diagnostic should name.
       (c) **plumbing** — a defaulted `source_location` captures the CALLER of the function
       declaring it, so it is declared only on the outermost verb the user calls
       (`launch`/`nested`/`Guarded::access`/`async`/`Deferred::commit`) and the resulting
       `Named` is passed down every internal layer explicitly; `tests/named_tests.cpp` asserts
       captured sites land in the test file rather than a library header. The pack-ending
       multi-object `ts::access`/`ts::async` cannot take a trailing default, so they carry only
       an explicit `{.name = "..."}` — documented, not worked around.
       Payoff landed with it: the waits-for cycle fatal now reads `task 'nodeA' holding 'objA'
       awaits 'objB', while task 'nodeB' holding 'objB' awaits 'objA'`. Also resolves
       Inconsistency 7. Original text follows.
       Graph nodes are named (`Node_name`, implicit from a string literal, `{}`
       captures the `add_node` call site via `std::source_location`) and objects are named
       (`ts::Named`), but **dynamic tasks are anonymous**. Since coroutine-first made
       coroutines the whole dynamic vocabulary, and since their ergonomics are a stated goal,
       every task should carry a name the same way: **default = the `std::source_location` of
       the creation site, with an optional user-supplied literal overriding it** — the exact
       `Node_name` pattern, reused rather than reinvented.
       *Why it pays off now:* the diagnostics landing in 6.10–6.14 all print tasks. The
       waits-for fatal currently reads `task (block 0000018EB62717B0) holding 'objB' awaits
       'objA'` — the objects are named because `ts::Named` exists, the tasks are raw pointers
       because nothing equivalent does. 6.13's quiescence net is meant to **dump every
       suspended frame** when it fires, which is only useful if the frames identify
       themselves. Trace attribution (`trace_owner`) has the same gap for non-node work.
       Design points to settle: (a) **cost** — a `const char*` plus a `source_location` is
       ~3 pointers + 2 ints per block, against a control block deliberately shrunk to 264 B
       (4.7 is still trying to shrink it further), so decide between always-on (like
       `Node_name`, kept in shipping) and `TS_SAFETY_CHECKS`-gated, and consider storing the
       `source_location` alone and formatting lazily; (b) **the coroutine case is the
       interesting one** — a coroutine's useful name is its function, which `source_location`
       at the *creation* site gives only indirectly; check what `source_location::current()` as
       a defaulted parameter actually captures for `co_await`ed frames versus `launch`/`async`
       call sites, and whether the promise can capture it at `get_return_object` instead;
       (c) plumbing through `launch`/`async`/`nested`/access verbs without adding a parameter
       to every signature (a leading name arg is not expressible after an object pack — the
       same constraint that forced `Node_name` to lead). Ratify alongside Inconsistency 7
       (the naming surface review, which explicitly wants this settled *before* more tooling
       depends on it) — this item is exactly that dependency arriving.

7. **Deferred / Versioned**
   1. `[ ]` **(P2) Main chain** ([deferred-versioned-state.md](deferred-versioned-state.md) §6) — journal `mem_profile` baseline → per-journal bump arena → record-stream slots → typed command tier (`Deferred<T,Cmd>`) → sort keys / hooks / dirty-set → render-queue fixture.
   2. `[ ]` **(P2) Lock-free `stage()`** — kill the per-slot mutex (it exists ONLY for the dynamic stage-vs-cut race; single producer per slot otherwise — handoff doc §5). Falls out of 7.1's arena step: single-producer chunked bump allocation makes `stage` a lock-free bump, and the cut becomes a chain-head exchange. `Parallel_recorder` already gives thread-keyed slots (per-worker + overflow lane); this removes the last lock on the staging path. Split out of 7.1 for referenceability — implement together with the arena.

8. **Task chaining**
   1. `[—]` **MOOT (2026-08, 6.4).** Results-on-`after` — `after` and the whole builder are
      deleted; a dynamic task that needs a prerequisite's result writes `auto r = co_await t;`.
      Nothing left to design. (Typed results INTO a graph node remain open as 2.1.)

9. **Research / shelved**
   1. `[S]` **std::execution senders** — shelved; model the concepts for interop, prototype *access-context-as-env* as the one novel spike; do **not** re-found the engine on senders (the monomorphic runtime block earns its keep). [§D7]
   2. `[ ]` **(research note — T13.4) Async I/O story.** Untouched so far. The library is CPU-compute-first; async I/O (file/socket/GPU-transfer completion) is a different axis — a blocked I/O wait must not tie up a worker. Today the sanctioned bridge is `Signal`: an external completion (OS overlapped-IO callback, GPU fence, `io_uring` CQE) calls `signal.trigger()`, and a `co_await` on that `Signal` resumes CPU work — fire-the-IO, suspend, never block a worker. (2026-08: the coroutine-first cross-frame pattern, coroutine-first.md §4.7, makes this the first-class shape rather than an idiom, which is what pulled 6.6 out of this note.) That covers "react to completion" without an I/O runtime. What we deliberately do NOT provide (and probably shouldn't, cf. Rayon/Tokio being separate pools by design): an I/O reactor, readiness polling, or a socket/timer API. Open question to revisit only on demonstrated demand: whether a thin `Signal`-from-OS-completion helper (register an OVERLAPPED / fd / fence, get a `Signal`) is worth packaging, or stays a documented idiom. Ties to the Signal-examples doc item (10.4).
   3. `[ ]` **(research note — T25) CPU transient aliasing.** Render graphs reuse one block of memory for two scratch resources whose lifetimes don't overlap ([research-deepdive.md](research-deepdive.md) §4.4), derived from declared first-write/last-read. The CPU analogue for us would be per-node *declared transient buffers* with `compile()` computing [first-writer, last-reader] windows and aliasing storage across non-overlapping windows — a potential differentiator, but it needs a task-system feature we don't have: a *declared transient-resource* concept distinct from a `Guarded` object (a scratch buffer owned by the graph, not a persistent guarded instance). The coarse version already exists (the per-run bump arena, 4.6 — everything dies at run end); the fine version is speculative and gated on that new concept. Record only; act only if a concrete workload needs graph-derived scratch-memory reuse.

   4. `[ ]` **(P2, research, author 2026-07) Survey high-level parallelisation patterns to
      generalise.** A research pass over the broad parallel-programming pattern catalog
      (structured/nested parallelism, dataflow + pipeline, map-reduce / scan, fork-join variants,
      actor / mailbox, work-stealing idioms, GPU-style dispatch, ECS scheduling, render-graph
      patterns) to FARM for patterns this library could generalise into first-class primitives or
      documented idioms, the way `Versioned` / `Deferred` generalised double-buffering and command
      buffers. Output: a ranked candidate list with "already covered / cheap idiom / worth a
      primitive / out of scope" verdicts, feeding 5.1's primitive menu and the samples. Complements
      the existing engine research ([task-systems-comparison.md](task-systems-comparison.md),
      [research-deepdive.md](research-deepdive.md)) which studied task SYSTEMS; this studies parallel
      PATTERNS.
   5. `[ ]` **(P1, research, author 2026-07) Find a real integration / validation target.** A deep
      research pass for potential applications of the library, with the concrete goal of finding an
      OPEN-SOURCE project to integrate with and validate the access-declared-parallelism approach on
      a real workload (not a synthetic sample): ideally a game engine, renderer, simulation, or
      other frame-structured C++ codebase with exploitable coarse-grained parallelism and
      thread-unsafe shared state. Deliverable: a shortlist of candidates with fit assessment
      (workload shape, existing threading model, integration surface, license, community activity)
      and a recommended target to prototype against. The strongest available validation of the
      design and the best source of real API pressure, so prioritize it. Ties to the going-public
      story ([going-public.md](going-public.md)).

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
    12. `[ ]` **(P2, docs/samples, author 2026-07) Multiple / nested static graphs sample.** A
        single-file sample showing more than one `Static_task_graph` in play, and graphs nested
        within graphs: a node whose body drives a sub-graph (via nested tasks so it does not block a
        worker), and independent graphs composed at a higher level. Demonstrates the composition
        story (large frames built from reusable sub-graphs) and surfaces the sharp edges: object
        lifetime across graph boundaries (a `Guarded` touched by two graphs), the run-quiescence
        invariant per graph, and whether driving a sub-graph from inside a node risks worker
        exhaustion (the never-block-in-a-node rule; establish whether retraction/nested cover it).
        Investigate feasibility first: if a clean composition pattern falls out, ship the sample,
        else document the constraints. Relates to 2.3 (pipelined execution) and 10.3.
        **Update (2026-08):** the mechanism this sample needs is 6.9a (nested graph runs v1 —
        `co_await inner.execute()` with the lend protocol); sequence the sample after it, and
        the "does driving a sub-graph from a node risk worker exhaustion" question is already
        answered (it does not: an awaited inner run suspends the outer frame, holding no worker).

    13. `[x]` **DONE (2026-08) — Wave-2 `writer_owner` tests F1–F4**
        ([pipe-rebase-tests.md](pipe-rebase-tests.md) §F), in `pipe_tests` §F. Re-scoped for the
        evolved pipe: **F1** (owner set inside a write body, null outside — plus the other half
        of the invariant, that a READER hold publishes no owner, or `commit()` would take its
        inline arm under a read grant) and **F4** (each pipe of a multi-object write names the
        same block; an untouched object stays unowned) as written; **F3** re-aimed at the two
        inline arms — an `access` on a free pipe is a real admission and publishes its own
        block, while the REENTRANT arm must leave the outer block as owner (republishing or
        clearing on the inner settle would mis-dispatch `commit()` for the rest of the outer
        body); **F2 re-scoped** — the explicit graph write handoff it tested is deleted, so it
        now pins release-then-admit (two chained writes each name their own block, never the
        predecessor's, never null). No `Pipe_probe` type was needed: `Guarded_access::pipe()`
        plus a local `owner_of()` reads the field directly. 517 → 536 checks.

    14. `[x]` **DONE (2026-08) — the coroutine chain re-measured and decomposed.** `coro chn`
        reads ~1800–2100 ns/op against ~104 ns/op for a single `co_await`, which looked like a
        coroutine cost worth chasing. It is not. Its figure is per STAGE (50 awaited launched
        stages, 50 ops reported), and each stage is a full scheduler round trip. A new
        `coro nst` benchmark runs the identical 50-stage shape with each stage a plain
        coroutine call — eager tasks complete on the calling thread, so the await takes the
        `await_ready` fast path and only coroutine machinery is measured: **87.9/88.5/87.9 ns
        across three quiet-host runs (±0.6%)** vs `coro chn` **2138.9/1959.2/1809.3 (±8%)**.
        Coroutine cost is therefore under 5% of the chain figure; the rest is wake/dispatch
        latency (cross-checked against `launch` at 900–1070 ns — a chain stage costs ~2x, which
        matches paying a wake in each direction). So there is no allocation-shaped gap here
        (6.2 already landed), and the lever if it ever matters is the worker-loop half of 6.8
        or the idle policy, NOT 4.1/6.2. Full verdict in
        [pipe-rebase.md](pipe-rebase.md) §0.4; keep both benchmark lines.

    15. `[ ]` **(P1-cheap, CI) Gate Shipping (`TS_SAFETY_CHECKS=0`) on every push, not by hand.**
        The safety-gated surface grew substantially (waits-for detector state, grant epochs,
        scope bookkeeping, `writer_owner`'s always-on exception), and the convention is that
        safety-only state is FULLY compiled out — a mis-gated field or an `#if`-invisible
        behavioral dependency only shows as a Shipping build/link/behaviour break. Shipping is
        currently built and stressed manually at merge points. Add it to CI alongside the
        Release and TSan legs (build + `--tests` + `--stress`), so a mixed-config mistake fails
        the push rather than the next release.

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
7. **Entity-naming approach — RESOLVED (author-designed, 2026-08).** Ratified as the leading
   `ts::Named` wrapper, unified across all three entity kinds: `Node_name` is deleted and
   `add_node` takes `ts::Named` in the same leading position; `Guarded`/`Versioned` keep theirs;
   tasks carry one too. `Named` is a literal **or** a captured call site
   (`{literal, file, line}`, implicit from a string literal, `ts::Named{}` for the site alone),
   displayed as the literal else `basename(file):line`. Required for nodes and objects — their
   unnamed constructors and the `node<N>`/`objN` ordinal fallbacks are gone — and optional for
   tasks, where the verb captures the creation site by default. The load-bearing implementation
   rule (the defaulted `source_location` must sit on the OUTERMOST function the user calls, with
   the resulting `Named` passed down explicitly) is stated in `ts/named.h` and pinned by
   `tests/named_tests.cpp`. Specifies 6.19.

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
**Pipe-rebase addendum (2026-08):** the tail-chain pipe (1.14, branch `pipe-rebase`) makes
pipe order strictly FIFO by construction — a task's priority applies at dispatch (queue
placement once its turn arrives) but never jumps the line, so a high-priority task queued
behind a long low-priority body waits out the body. That is today's semantics too, but the
pipe instantiation above ("dispatch head at `max(own, queued-behind)`") must be re-expressed
on the chain when this item lands: the "head" is now the running link's owner, and propagation
= walking `next` links backward-in-priority to raise the owner's dispatch priority at its
turn. Review the interaction then; the chain makes the walk cheap (the links are right there)
but the raise must happen before the owner's `dispatch_ready` reads `flags.priority`.

### D7. Senders (P2300)
Lazy typed sender + receiver + `connect`→op-state (stack-allocatable) + `start`; algorithms fold
into one nested op-state. Could give: zero-alloc *structured* composition (the structural answer to
allocation, but only for the awaited subset), and — the distinctive angle — **env/queries as the
`Access_context` carrier** (typed, composable replacement for `thread_local current_access`;
opens compile-time access checking). Tensions: clashes with the deliberately not-templated
monomorphic block (compile-time + error-message tax), doesn't fix fire-and-forget allocation,
different usage model, P2300 still settling. Verdict: coroutines first (done); prototype
access-context-as-env as the one spike; don't rebuild the engine.
