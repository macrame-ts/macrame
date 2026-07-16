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

### Guarded / access
- `[~]` **(P1) Zero-alloc inline `access`** — void→done-sentinel, small-R→SBO handle, large/deferred→heap. [§D1]
- `[ ]` **(P0) Multi-object `ts::access` isn't inline** — schedules like `ts::async`; implement or document the single-vs-multi difference. *(consistency)*
- `[ ]` **(P1) Generic-lambda / `auto&` deduction** — `add_node`/`access` need non-generic lambdas; users hit this immediately.
- `[ ]` **(P2) Access-check completeness** — clang-tidy "every public method calls `TS_CHECK_ACCESS()`" + a `Guarded_value<T>` for POD/no-method types. [§D2]
- `[ ]` **(P2) Adopted `Guarded`** — wrap an existing instance (borrow) via an `adopt` ctor tag; access key = borrowed address.
- `[ ]` **(P2) Sub-object / range harness** — grant on `S` covers `[&S, &S+sizeof)`, or a declarable `Region<T>`.
- `[ ]` **(P3) Standalone public `Pipe`** — expose the reader/writer pipe (UE `FPipe`); defer until a non-single-object use case appears.

### Static task graph
- `[ ]` **(P1) Typed graph chaining** — a node consumes prerequisite-node results (nodes are void-only now); a `Graph_node` may then mint a per-run `Task<R>`.
- `[ ]` **(P2) Ambiguity detection** — `compile({.ambiguity = Warn|Error|Ignore})` determinism diagnostic; needs edge provenance; feeds profiler-guided reorder.
- `[ ]` **(P3) Pipelined execution** — more than one `execute()` in flight (frame overlap).
- `[ ]` **(P3) Profiler-guided optimization** — reorder/rebucket from measured task durations.

### Scheduler
- `[ ]` **(P1) Ambient (overridable) scheduler** — `launch`/`task`/`access` resolve to an *ambient* scheduler; add `Scheduler_scope` override + optional `Launch_options{.scheduler}`. Retires testability / embedding / multi-pool / named-thread limits at once. Additive, but API-shape — consider pre-public. [§D3]
- `[ ]` **(P2) Named-thread affinity** — model a named thread as a single-worker `Scheduler` (unifies with ambient; `co_await resume_on(render)`).
- `[ ]` **(P2) Run-on-all-workers** — broadcast a functor to every worker (per-thread init / flush / warm-up).
- `[ ]` **(P2) Timed / delayed tasks** — delay queue (timing wheel); one-shot / periodic / cancellable; re-arms a reusable task.
- `[ ]` **(P2) M2 stage 5** — promote high/low to per-worker deques (profiling-gated); a proper low-contention worker-submit benchmark.
- `[ ]` **(P2) Platform abstraction (~6 fns)** — `park`/`unpark`(+timeout), thread spawn/name/affinity, cpu_count/topology, `cpu_relax`. Unblocks eventcount timeout + standby workers; console fiber backend later. [§D4]

### Allocation / control block
- `[ ]` **(P2)** Per-type recycling free-list (`Exec`/`Result_block`/bare block).
- `[ ]` **(P2)** Tunable-SBO `Function<Sig, N>` replacing `move_only_function`/`function`.
- `[ ]` **(P2)** Small-vector / intrusive links for block `successors`/`prerequisites`/`continuations` (also shrinks the block ~72 B).
- `[ ]` **(P2)** Shrink `Task_control_block` — `completed`/`cancelled`→bits; a futex wait primitive roughly halves the 288 B block (ties to platform layer).
- `[ ]` **(P3)** Multi-object `async` `std::map`→sorted `vector`.
- `[ ]` **(P2)** Opt-in scoped bump arena (auto for `parallel_for`/graph-run, per-frame opt-in); rebase `journal.h` staging onto it.

### Fork-join / parallel_for
- `[ ]` **(P2) Intra-system entity interactions** — ship the primitive menu: `parallel_gather_apply` (mailbox), `parallel_for_colored` + `Interaction_coloring`, `Accumulator` (commutative), `Union_find` helper, + triage docs. Open author questions. [§D5]
- `[S]` **Priority propagation / inheritance** — designed (pipe / graph / dynamic; opt-in surfaces; not OS thread priority); revisit on a demonstrated inversion. [§D6]
- `[ ]` **(P3) Reserve / standby workers** — sequenced after the platform layer; caller-participation already covers the need.

### Coroutines
- `[ ]` **(P2)** Inline-when-free for awaited accesses — `co_await obj.access(fn)` should try inline at the await (safe: the coroutine would suspend anyway).
- `[ ]` **(P2)** Coroutine-frame / control-block fusion — one alloc for frame + block (coroutines *reduce* allocs, not add).
- `[ ]` **(P3)** Priority setter on the promise (it stores one; no config channel yet).

### Deferred / Versioned
- `[ ]` **(P2) Main chain** ([deferred-versioned-state.md](deferred-versioned-state.md) §6) — journal `mem_profile` baseline → per-journal bump arena → record-stream slots → typed command tier (`Deferred<T,Cmd>`) → sort keys / hooks / dirty-set → render-queue fixture.

### Task chaining
- `[ ]` **(P3) Results-on-`after`** — leaning *no* (`when_all`+`then` covers it); revisit only if a concrete single-result-prerequisite use case appears.

### Research / shelved
- `[S]` **std::execution senders** — shelved; model the concepts for interop, prototype *access-context-as-env* as the one novel spike; do **not** re-found the engine on senders (the monomorphic runtime block earns its keep). [§D7]

### Tooling / infra
- `[ ]` **(P2)** Benchmark regression baseline + compare step (store medians, flag regressions). *(postponed — not blocking)*
- `[ ]` **(P2)** Proper ASan build config (reachable via `/p:EnableASAN=true` today); portable TSan build story.

---

## Inconsistencies to resolve (pre-public sweep)

Small API/doc frictions introduced or exposed this session — cheap to fix, costly to
change after public. Feed the going-public "API-stability pass".

1. **`Task_options::run_inline` is dead for `access`/`async`.** After the verb split, the
   method name chooses inline-eligibility; `run_inline` is only read on the `then`/task path.
   A user passing `{.run_inline = true}` to `access`/`async` is silently ignored. Resolve:
   document it as `then`-only, or split the options type so the access surface can't accept it.
2. **Multi-object `ts::access` ≠ opportunistic.** Single-object `access` runs inline when free;
   multi-object `ts::access` schedules like `ts::async` (inline path unimplemented). Same verb,
   different behavior — implement multi-object inline, or document the difference loudly.
3. **Stale `async` comments after the split.** e.g. `guarded.h` "Ambient scheduler used by
   `async()`", `pipe_try_inline` "opt-in via `Task_options::run_inline`". Sweep comments that
   still say `async` where `access` is now the opportunistic verb.
4. **`pipe` vs `queue` terminology.** README says "reader/writer queue"; guide/design/internals
   say "pipe" (defined in the guide glossary). Pick one public term (lean **pipe**, defined once)
   and use it consistently in user-facing docs.
5. **Reservation path still heap-boxes a closure.** `Pipe::Job::on_acquired`
   (`move_only_function`) allocates on the graph / multi-async reservation path, unlike the
   allocation-free block/async dispatch. Known, minor — fold into the SBO-`Function` work.

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
