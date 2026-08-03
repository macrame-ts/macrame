# Coroutine-first — design of record and transformation plan

Author decision (2026-08): the dynamic-task layer becomes coroutine-first. High-level
parallelization stays on the static graph; **everything dynamic composes with `co_await`**.
The callback/edge vocabulary that simulated suspension with blocks (`then`, `when_all`,
nested tasks, dynamic prerequisites, retraction, inline dispatch) is removed. This is a
deliberate shakeup: no existing mechanism is sacred; every change that alters shipped
behavior or API is flagged in §8. Guiding goals (CLAUDE.md): UX, performance, rich
functionality.

Companions: `docs/pipe-rebase.md` §0 (the evolved pipe this builds on),
`docs/retraction-vs-pool-exhaustion.md` (+ its §7 outcome), `docs/task-internals.md`
(to be rewritten at the end of this plan).

## 1. The end-state model

- **Static graph** — unchanged role: compiled, access-derived, zero-alloc-per-run
  high-level schedule.
- **Coroutines** — the one dynamic composition vocabulary. Our tasks are **eager**
  (`initial_suspend = suspend_never`): the body runs on the caller's thread until its
  first real suspension; awaiting a settled task (`await_ready`) never suspends. Eager
  preserves `launch` semantics and gives run-inline-until-you-must-wait for free.
- **Pipes + grants + harness** — unchanged role; a granted turn now resumes a frame or
  dispatches a block through the same `release()` trigger.
- **`Task<R>`** — the one completion primitive: awaitable from coroutines, `sync()`-able
  ONLY from outside tasks (the blue boundary: main loop, external threads, tests).
- **`Signal`** — the awaitable event; keeps `reset()` (the per-frame gate idiom needs a
  reusable phase gate; bodyless re-arm carries none of the executable-reuse hazards).
- **`parallel_for`** — unchanged (group tier, caller participation). Its join waits only
  on RUNNING helpers (work that provably occupies a worker) — the one sanctioned in-task
  wait, documented as the structured exception.
- **`Deferred`/`Versioned`** — unchanged contracts; returned tasks become awaitable like
  any `Task`.

## 2. The suspension-under-grant doctrine

The rule that unifies nested tasks (retired), coroutine node bodies, guard fatals, and
dynamic cross-object access:

> **Under a held grant, a task may suspend on: (a) its own spawned children —
> unconditionally safe (they inherit the grant, never re-acquire it; no cycle can pass
> through the holder's objects); (b) the same object — resolved by reentrancy
> (`writer_owner == current task` runs inline under the grant, never queues behind
> itself); (c) a foreign object — safe iff no waits-for cycle forms, which is a global
> property the caller cannot always see locally.**

For (c), the preference hierarchy (highest first — this is "parallelism concentrates at
high levels" applied to communication):

1. **Declare the object** on the node and let `compile()` order it — no suspension, no
   cycle risk, cheapest.
2. **`Versioned` snapshot** when last-frame data suffices — no grant, no wait.
3. **`Deferred` staging** for outward writes — grant-free, applies at the commit slot.
4. **Awaited dynamic access** (`co_await obj.access(fn)`) as the residual — short bodies,
   read mode where possible.

The (c) failure mode is a **suspended ABBA deadlock**: N holds G1 awaiting G2, M holds G2
awaiting G1 — no thread parks (both frames suspended, all workers free), the frame's
`execute()` silently never completes; harder to debug than a blocked thread. The graph
cannot prevent it (the accesses are undeclared by definition). Mitigation: the
**waits-for cycle detector** (TODO 6.5) — at suspension-on-a-pipe the harness knows the
awaiter's held grants (`Access_context`) and each pipe knows its holders; record the edge
at suspend, clear at resume, cycle-check on insert, fatal naming both tasks and both
objects. Blessing pattern (c) in the guide is gated on this detector existing.

Grant lifetime rule (replaces §8's nested phrasing): **a node's grants are held until its
completion, and a coroutine node completes when its frame completes** — suspension does
not release grants; body-return is not completion.

## 3. What is deleted (and what replaces it)

| deleted | replaced by |
|---|---|
| `then(fn, opts)` + apply-style traits | `co_await t;` then the next line |
| `when_all(...)` + `Join_state` + tuple traits | sequential `co_await`s (tasks already run eagerly; last one gates) |
| nested tasks: `ts::nested`, `add_nested`, the `execution_flag` counting mode | the coroutine doesn't `co_return` until it awaited its children; deep fan-out collects into a `Task_scope` (§4.3) |
| `Task_builder`, `after()`, `add_prerequisite`, frozen-at-launch enforcement | `co_await x; co_await y;` at the top of the coroutine (dynamic edges become code) |
| retraction: deep `retract`, `retractable`, hints, `retract_or_wait`, the claim/generation reuse machinery (`run_state` fusion, monotonic-max `dispatch_arg`, release-time gen capture, reuse forensics) | waits are suspensions — pool exhaustion is structurally gone; the inline-execution optimization survives as eager start + symmetric transfer (§5.2) |
| reusable executable tasks (`Task_builder::reset`) | call the coroutine again (frames are one-shot); `Signal::reset` stays |
| inline dispatch: `set_inline`, `run_inline`, the `inline_pending` trampoline | symmetric transfer — the settling thread resumes the awaiting frame directly, tail-call bounded |
| `access()` vs `async()` as behavioral split | one awaitable access verb + one eager fire verb (§4.2) |
| blocking-sync diagnostics (`blocking_sync_diagnose`, the sharp/general ensures) | in-task `sync()` is a hard fatal (§4.1); nothing to diagnose-and-park |
| `Task_control_block`'s three vectors + two counting regimes | slim shared-completion state: refcount, settled/cancelled, result, token, waiter list (usually one frame handle). TODO 4.7 resolves by annihilation |

Everything deleted at the API level is a **compile error** after this plan (the functions
no longer exist); everything deleted at the *rule* level (in-task blocking, guard across
suspension, scope misuse) gets a **fatal + a companion test** showing the sanctioned form
(§6).

## 4. New/changed surface (the decisions, flagged)

### 4.1 The blue boundary
`Task<R>::sync()`/`take()` are legal only outside tasks. Inside a task
(`current_task != nullptr`), any would-be park is **fatal in all TS_SAFETY_CHECKS
builds** (shipping: parks, documented UB-risk as today). `parallel_for`'s internal join is
the explicit exemption (waits on running work only).

### 4.2 Access verbs
- `co_await obj.access(fn)` — the awaitable form: `await_ready` when the pipe is free
  (runs `fn` inline on the caller — the old opportunistic `access`) or when
  `writer_owner == current task` (reentrant, runs under the held grant — doctrine (b));
  otherwise suspends until the turn arrives. Grant lives only for `fn`.
- `obj.async(fn)` — the eager fire verb, unchanged: returns `Task<R>`, usable from any
  color (fire-and-forget from blue; awaitable from red).
- Multi-object: `co_await ts::access(fn, objs...)` rides the same cascade; the guard
  forms keep the held-across-suspension fatal.

### 4.3 `Task_scope` (the nursery)
For dynamic fan-out spawned deep in call stacks (the old nested-task motivation):
`scope.spawn(fn)` launches eagerly and records; `co_await scope.join()` awaits all.
Children inherit the owner's grants (the `with_inherited_access` model, promise-carried).
Destroying a scope with unjoined children is **fatal** (lost children — the analogue of
the journal's lost-writes fatal).

### 4.4 Coroutine graph nodes
`add_node` accepts a body returning `Task<void>` (a coroutine): the node's completion
gates on the frame's completion (one waiter on the returned task — the mechanism that
replaces nested-gating for graphs). Grants per §2. Functor nodes stay the zero-frame
common case.

### 4.5 Cancellation
Tokens unchanged. Awaiting a cancelled `Task<void>` resumes normally (query
`is_cancelled()`); awaiting a cancelled value task is fatal (mirror of `sync`). The
`prereq_cancelled` propagation machinery shrinks to the awaiter (no successor edges left
to propagate through). A coroutine's own token rides its promise.

### 4.6 Context and resumption
The ambient `Access_context` and `current_task` are promise-carried and reinstalled at
every resumption (resume may land on a different worker). Resumption is scheduled at the
task's priority; symmetric transfer applies when the settling thread may run the frame
directly (default on; priority-respecting fallback to the queue).

### 4.7 Cross-frame tasks (first-class pattern)
Fire I/O, `co_await` a `Signal`, resume frames later, re-enter the schedule via `Deferred`
staging — zero workers held while suspended. Rules: no grants across the wait (the guard
fatal enforces), never gate a node/run on cross-frame work (nested is gone, so the footgun
is unexpressible), captures own their data, carry a token. Sugar TODOs: Signal-from-OS
helper (6.6), per-frame gate idiom (6.7), low-priority resumption default (doc).

## 5. Performance workstream (the price and its payment)

1. **Frame/block fusion first** (TODO 6.2, promoted to the critical path): one allocation
   per coroutine task — the promise embeds the shared-completion state; `operator new`
   on the promise routes to the (future) size-class pool (TODO 4.1). Without fusion,
   coroutine-first regresses the one-alloc `async` path to two.
2. **Symmetric transfer** in all awaiters (§4.6) — replaces the inline trampoline's win.
3. **`await_ready` fast paths** — settled task, free pipe, reentrant owner: zero suspension.
4. HALO is treated as unavailable (MSVC/clang-cl reality); no design relies on frame elision.
5. **Benchmarks**: coroutine equivalents replace the `then`/`when_all` benches; targets =
   current numbers (then 1919 ns/op, when_all 5238 ns/op are the marks to beat — both
   carry multi-alloc machinery today, so fusion + symmetric transfer should win); R10
   pipe series re-run; `game_frame` (converted per §7.4) must hold its trace numbers and
   `transform0` determinism, including worker-less.

## 6. Illegal cases: fatal + companion (the test matrix)

Every row = one death test + one adjacent test demonstrating the sanctioned form.

| illegal (fatal) | companion (how to do it) |
|---|---|
| `sync()`/`take()` inside a task | `co_await` it |
| guard held across suspension (exists) | functor-form `co_await obj.access(fn)`, or split: release, await, re-acquire |
| `commit()` from a grant-inheriting child (exists) | `commit()` from the grant-holding task |
| `Task_scope` destroyed with unjoined children | `co_await scope.join()` before scope exit |
| awaiting a cancelled value task | `is_cancelled()` check, then branch |
| waits-for cycle (foreign await under grant; needs TODO 6.5) | the §2 hierarchy: declare / snapshot / stage |
| `Signal` re-armed while awaited (reset misuse; verify existing guard) | settle, then `reset()` |

Deleted APIs need no fatals (compile errors); the guide's migration table (§7.5) covers
them.

## 7. Staged plan (each stage lands green: suite, TSan, Shipping)

1. **Foundation** — coroutine core hardening BEFORE any deletion: promise-carried
   context/token + resumption reinstall; symmetric transfer; awaiting-cancelled semantics;
   frame/block fusion (§5.1); the in-task-`sync()` fatal (+ matrix rows 1); awaitable
   access verb incl. reentrant `await_ready` (+ row 2 companion).
2. **Replacement capabilities** — `Task_scope` (+ row 4); coroutine graph nodes (§4.4);
   per-frame gate + Signal helpers as thin utilities.
3. **Migration** — samples (`game_frame` streaming + HUD to coroutines; physics;
   blackboard) and the test suite off `then`/`when_all`/nested/builder onto coroutine
   forms. Largest mechanical stage; determinism checks must hold.
4. **The deletions** (§3, in dependency order): then/when_all → builder/after → nested +
   `execution_flag` mode → retraction + claim/generation → reuse → inline trampoline →
   block slimming (vectors → waiter list). Suite green after each sub-step.
5. **The detector** (TODO 6.5) + the remaining matrix rows; doctrine section into
   task-internals (rewritten §4/§6/§7/§8).
6. **Validation + docs** — benches vs §5.5 targets, full TSan campaigns, ASan/stress,
   Shipping, alloc audit (`mem_profile` on frames); guide/design rewrites (composition
   chapter = coroutines; migration table); CLAUDE.md at merge.

## 8. Flagged changes (behavior/API breaks, per the shakeup mandate)

1. `then`/`when_all`/`ts::nested`/`add_nested`/`ts::task` builder/`after`/`set_inline`/
   executable `reset()` — **removed** (compile errors).
2. In-task `sync()` — was retraction-or-park, becomes **fatal** (checked builds).
3. Retraction removed — bare-tree fork-join via `sync()` inside tasks no longer exists;
   the pattern is `co_await`/`Task_scope`/`parallel_for`.
4. `Task<R>` handles: `sync()` demoted to boundary-only; awaiting is the primary verb.
5. Coroutines become **mandatory** (drop `__cpp_impl_coroutine` guards; C++23 baseline
   already assumes a conforming compiler).
6. `access()` becomes awaitable-only (red); blue callers use `async()`. (The old blue
   opportunistic-inline `access` disappears; measure whether a blue inline fast path is
   ever missed before re-adding.)
7. Allocation profile: +1 frame per dynamic task until fusion (§5.1) restores one-alloc.
8. Async pipe release moved body-return→settle already landed with the evolved pipe;
   under coroutine nodes this generalizes to frame-completion (§2) — same direction.

## 9. Risks

- **`execution_flag` deletion** touches `Executable::run`/`settle` — the §8 invariant
  must be re-proven via frame-gating (stage 4 has a dedicated TSan pass on the graph
  nested→frame conversion).
- **Allocation regression window** between stages 3 and the fusion landing — fusion is
  stage 1 for exactly this reason.
- **MSVC/clang-cl coroutine codegen** — measure early (stage 1 microbench), not assume.
- **Test churn** — 543 checks reshuffle; the graph/pipe/harness suites are untouched,
  the task/integration suites transform. Wave discipline as in the pipe rebase: green at
  every commit.
- **Debuggability** — suspended frames have no stacks; the waits-for detector (stage 5)
  and the trace tooling carry more diagnostic weight. Accepted.
