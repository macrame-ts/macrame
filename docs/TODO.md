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

All layers built, tested (**345 checks**, TSan/ASan clean, subprocess death tests),
documented, and CI-gated.

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
   1. `[~]` **(P1) Zero-alloc inline `access`** — void→done-sentinel, small-R→SBO handle, large/deferred→heap. [§D1]
   2. `[ ]` **(P1) Multi-object `ts::access` inline** — currently schedules like `ts::async` (documented); implement the inline fast path so single- and multi-object `access` match.
   3. `[x]` **(P1) Generic-lambda / `auto&` deduction — DONE (A3+A4, 2026-07).** Introspecting a generic lambda's parameter const-ness is impossible (templated `operator()`), but classification is not: the **rvalue-bindability probe** classifies each bare-arg position at declaration level — `const auto&`/`auto&&` binds an rvalue → read, `auto&` cannot → write — so generic lambdas deduce under the SAME "const = read" spelling rule as non-generic ones, no tags required. **A3**: every read position (deduced, probed, or tagged) invokes the body with `const T&` (`mode_ref`), so mutating under a read classification is a compile error — and the constraint/return-type machinery evaluates the mode gate FIRST (`accessor_mode`, `Accessor_result`) because probing a generic lambda's invocability against `const T&` deduces its return type, which instantiates the BODY — a hard error for a mutating body, not SFINAE. **Q1**: by-value / rvalue-ref resource params are `static_assert`-rejected wherever introspectable (a by-value copy silently discards writes); the generic `auto` by-value spelling is undetectable at declaration level — documented residual. Tags `ts::as_read_only(g)`/`as_read_write(g)` (names ratified; match the `Access` enum and coroutine guards) remain as the explicit-declaration style, the escape for an `auto&&` that must write, and permit conservative write-over-declaration (write tag over a `const T&` param serializes, legally). One unified builder per surface (`async_build_modes` / `fill_node_modes`) serves the deduced/probed/tagged tiers. Tests: probed single-object read-overlap/write-serialization + classification static_asserts, probed multi-object (incl. mixed spellings) + over-declared write, probed graph nodes; `#if 0` blocks document the compile-time rejections. 356→369 checks; clean MSVC + clang-cl + MSBuild, TSan clean.
   4. `[ ]` **(P2) Access-check completeness** — clang-tidy "every public method calls `TS_CHECK_ACCESS()`" + a `Guarded_value<T>` for POD/no-method types. [§D2]
   5. `[ ]` **(P2) Adopted `Guarded`** — wrap an existing instance (borrow) via an `adopt` ctor tag; access key = borrowed address.
   6. `[ ]` **(P2) Sub-object / range harness** — grant on `S` covers `[&S, &S+sizeof)`, or a declarable `Region<T>`.
   7. `[ ]` **(P3) Standalone public `Pipe`** — expose the reader/writer pipe (UE `FPipe`); defer until a non-single-object use case appears.
   8. `[ ]` **(P1) `Access_options` vs `Task_options`** — revisit whether the two option aggregates share a base or stay split (flagged in `task.h`); resolve in the code-review API-stability pass.
   9. `[S]` **Checked accessor wrapper (`Granted<T>`) — designed, declined for now (author, 2026-07: "maybe never").** Functors could receive a pointer-like `Granted<T>`/`Granted<const T>` whose `operator->`/`*` runs `access_check` then hands out `T&` — zero-intrusion diagnostics for types without `TS_CHECK_ACCESS()` instrumentation (incl. PODs/third-party types; would subsume 1.4's `Guarded_value`). Semantics: checks are tautological inside the sanctioned body; the signal is stashed/escaped *handles* (post-release, cross-thread, across-suspension use faults at deref); raw-ref extraction (`T& r = *g`) still launders the check away, so method instrumentation stays strictly stronger for owned types. Trust tiers: 0 = none (scheduling stays correct — the harness is diagnostics, not correctness), 1 = wrapper, 2 = instrumented methods (+ clang-tidy). Implementation is cheap (`access_check(T*)`/`(const T*)` overloads already resolve the mode; `TS_SAFETY_CHECKS=0` → raw passthrough); the work is the third signature tier in the deduction machinery (by-value `Granted` exempt from the Q1 static_assert). Revisit only if instrumentation burden proves a real adoption blocker.

2. **Static task graph**
   1. `[ ]` **(P1) Typed graph chaining** — a node consumes prerequisite-node results (nodes are void-only now); a `Graph_node` may then mint a per-run `Task<R>`.
   2. `[ ]` **(P2) Ambiguity detection** — `compile({.ambiguity = Warn|Error|Ignore})` determinism diagnostic; needs edge provenance; feeds profiler-guided reorder.
   3. `[ ]` **(P3) Pipelined execution** — more than one `execute()` in flight (frame overlap).
   4. `[ ]` **(P2) Profiler-guided optimization** — reorder/rebucket from measured durations. **Manual dry-run done (2026-07, game_frame); the headline finding is about measurement, not levers.** Five configurations were tried (placement of staleness-tolerant `Versioned` readers early-vs-tail × priority ranking none/high-low/spine-high): all cells measured 4.9–5.6 ms within one session window, while the SAME baseline configuration measured 4.15 ms in an earlier window and 5.3 ms later — **cross-window ambient drift (~25%) dwarfed every within-window delta (~5–10%)**, so no lever's sign is established at single-run resolution. Everything below is therefore prerequisites + hypotheses to re-test under proper methodology:
      - **Measurement methodology is prerequisite #1**: interleaved runs (A/B/A/B to cancel drift), medians over N, controlled load, and the per-lane timeline capture from the graph-viz session (docs/graph-viz-handoff.md) — without this, optimization conclusions are noise. Ties to 10.1 (benchmark regression infra).
      - **Mechanistic hypotheses recorded** (plausible, magnitudes unproven): (1) naive early gap-filling delays the serial spine — a ready critical-path node waits for a worker to finish its current slice, nothing evicts a runner → keep-out zones / finer filler granularity / reservation may be needed; (2) the current `Priority` enum cannot serve as rank: `low` is valve-gated background (a mislabeled dependency — hand-labeling got nav→ai and ui→submit wrong, which automated upward-rank would not — stalls its dependents), and `high` dispatches via the global MPMC queues instead of the per-worker deques (M2: only `normal` goes local) — a structural cost. Prerequisite either way: a rank mechanism native to the local-deque path (rank-ordered pop within `normal`, or M2 stage 5), plus **slice priority inheritance** — done (2026-07): `parallel_for`/`async_parallel_for` helpers now inherit the calling task's priority by default (`Parallel_options::priority` overrides), so a node's slices dispatch at the node's class rather than always `normal`.
      - **Info the optimizer needs**: per-node wall duration as (total_work, parallel_width) — a sliced node's wall depends on free workers, a scalar misleads; medians + variance; the edge list WITH provenance (conflict-derived = semantic, immovable; explicit = intent; version-choice reader→flip / flip→reader = movable IFF the user annotates the read staleness-OK); per-lane timelines; worker count.
      - **Sanctioned lever taxonomy**: (a) dispatch order among ready nodes — HEFT-style list scheduling by upward rank (blocked on the rank mechanism); (b) DAG shape via user-annotated semantic relaxations only — staleness-OK `Versioned` readers become a binary placement choice per reader, a small discrete search evaluable OFFLINE by simulating the captured cost model; (c) commutative-pair reordering (needs 2.2's provenance). The optimizer proposes; the user ratifies semantic moves.
      - Validation fixture: game_frame (hand-derived chain ≈2.6 ms, 26.9 ms work); a simulator must reproduce measured timelines before its proposals are trusted.
   5. `[ ]` **(P2) Compile-time rank → native dispatch shaping.** Compute upward rank per node at `compile()` (longest path to sink; weights = node priority now, measured durations once 2.4's capture exists; reuses the Kahn order, O(V+E) once). At run the graph shapes dispatch WITHOUT the scheduler's priority classes — avoiding `high`'s global-queue detour and `low`'s valve (2.4's blockers): (a) rank-aware successor submission order when one completion releases several ready nodes (owner deques pop LIFO — submit lowest-rank first so the settling worker pops the highest; thieves take the other end); (b) inline-successor selection by rank (`set_inline` currently picks without regard to importance). Converts `Graph_node::priority` from a queue-class request into a rank weight. Related: D6's static effective-priority pass should ride this mechanism (not the Priority classes) until M2 stage 5; priority-as-conflict-tiebreak belongs behind 2.2's opt-in (it changes observed values). Sequence AFTER the graph-viz timing capture — 2.4's dry-run showed dispatch-shaping effects are unmeasurable without it.

3. **Scheduler**
   1. `[ ]` **(P1) Ambient (overridable) scheduler** — `launch`/`task`/`access` resolve to an *ambient* scheduler; add `Scheduler_scope` override + optional `Launch_options{.scheduler}`. Retires testability / embedding / multi-pool / named-thread limits at once. Additive, but API-shape — consider pre-public. [§D3]
   2. `[ ]` **(P2) Named-thread affinity** — model a named thread as a single-worker `Scheduler` (unifies with ambient; `co_await resume_on(render)`).
   3. `[ ]` **(P2) Run-on-all-workers** — broadcast a functor to every worker (per-thread init / flush / warm-up).
   4. `[ ]` **(P2) Timed / delayed tasks** — delay queue (timing wheel); one-shot / periodic / cancellable; re-arms a reusable task.
   5. `[ ]` **(P2) M2 stage 5** — promote high/low to per-worker deques (profiling-gated); a proper low-contention worker-submit benchmark.
   6. `[ ]` **(P2) Platform abstraction (~6 fns)** — `park`/`unpark`(+timeout), thread spawn/name/affinity, cpu_count/topology, `cpu_relax`. Unblocks eventcount timeout + standby workers; console fiber backend later. [§D4]

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

10. **Tooling / infra**
    1. `[ ]` **(P2)** Benchmark regression baseline + compare step (store medians, flag regressions). *(postponed — not blocking)*
    2. `[ ]` **(P2)** Proper ASan build config (reachable via `/p:EnableASAN=true` today); portable TSan build story.

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
