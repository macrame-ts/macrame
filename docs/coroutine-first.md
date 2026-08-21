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

## 2. The waiting rules

The rule that unifies nested tasks (retired), coroutine node bodies, guard fatals, and
dynamic cross-object access:

> **Under a held grant, a task may suspend on: (a) its own spawned children —
> unconditionally safe (they inherit the grant, never re-acquire it; no cycle can pass
> through the holder's objects); (b) the same object — resolved by reentrancy
> (`writer_owner == current task` runs inline under the grant, never queues behind
> itself); (c) a foreign object — safe iff no circular wait forms, which is a global
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
**circular-wait detector** (TODO 6.5) — at suspension-on-a-pipe the harness knows the
awaiter's held grants (`Access_context`) and each pipe knows its holders; record the edge
at suspend, clear at resume, cycle-check on insert, fatal naming both tasks and both
objects. Blessing pattern (c) in the guide is gated on this detector existing.

Grant lifetime rule (replaces §8's nested phrasing): **a node's grants are held until its
completion, and a coroutine node completes when its frame completes** — suspension does
not release grants; body-return is not completion.

Inheritance is **gated-only**. Rule (a)'s "own spawned children" are the structurally-gated
launches — `parallel_for` chunks and coroutine segments (and, graph-internal, a coroutine
node's frame and a nested graph run) — whose completion is joined to the holder, so the
holder's grant provably outlives them and they inherit it soundly. A detached `ts::launch` is
**not** gated: its handle may be dropped and its grant window can close while it still runs, so
it inherits nothing and runs under an empty context. A body that touches the launcher's guarded
data then faults deterministically as undeclared access on the *first* access, in every checked
run — rather than passing on an inherited-but-not-yet-stale grant and faulting only on a late
touch (or, in a shipping build, never). The sanctioned fan-out over a holder's data is
therefore `ts::parallel_for` (or a fresh acquire via `obj.async` / `co_await obj.access`), never
a bare `ts::launch`. (The `ts::nested` / `Task_scope` verbs that once offered a concurrent
grant-inheriting child were **removed 2026-08** — see §4.3.)

## 3. What is deleted (and what replaces it)

| deleted | replaced by |
|---|---|
| `then(fn, opts)` + apply-style traits | `co_await t;` then the next line |
| `when_all(...)` + `Join_state` + tuple traits | sequential `co_await`s (tasks already run eagerly; last one gates) |
| the `execution_flag` counting mode as USER fan-out; `ts::nested`, `Task_scope`, `ts::join_nested`, `with_inherited_access` (**removed 2026-08 — see §4.3**) | fan out with `ts::parallel_for` (its helpers inherit the caller's grant, join synchronously) or acquire fresh via `obj.async` / `co_await obj.access`. `detail::add_nested`-on-`num_locks` SURVIVES as graph-internal plumbing (a coroutine node's frame and a nested graph run gate their completion through it) |
| `Task_builder`, `after()`, `add_prerequisite`, frozen-at-launch enforcement | `co_await x; co_await y;` at the top of the coroutine (dynamic edges become code) |
| retraction: deep `retract`, `retractable`, hints, `retract_or_wait`, the claim/generation reuse machinery (`run_state` fusion, monotonic-max `dispatch_arg`, release-time gen capture, reuse diagnostics) | waits are suspensions — pool exhaustion is structurally gone; the inline-execution optimization survives as eager start + symmetric transfer (§5.2) |
| reusable executable tasks (`Task_builder::reset`) | call the coroutine again (frames are one-shot); `Signal::reset` stays |
| inline dispatch: `set_inline`, `run_inline`, the `inline_pending` trampoline — **partially: removed from the DYNAMIC surface only** (as landed, 2026-08; `Graph_node::set_inline` and the `dispatch_ready`/`inline_pending` machinery survive graph-internal, see TODO 6.4) | symmetric transfer — the settling thread resumes the awaiting frame directly, tail-call bounded |
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
  `writer_owner == current task` (reentrant, runs under the held grant — waiting rule (b));
  otherwise suspends until the turn arrives. Grant lives only for `fn`.
- `obj.async(fn)` — the eager fire verb, unchanged: returns `Task<R>`, usable from any
  color (fire-and-forget from blue; awaitable from red).
- Multi-object: `co_await ts::access(fn, objs...)` rides the same cascade; the guard
  forms keep the held-across-suspension fatal.

### 4.3 Scoped launches (`ts::nested` / `Task_scope`) — REMOVED (author revision, 2026-08)
An earlier revision of this plan kept `ts::nested` (and an explicit `Task_scope`) as the
scoped-launch verbs: an eager, grant-inheriting child joined to the caller's completion,
carried on an implicit per-frame scope in TLS. **They are removed.** The defect is
structural, not incidental: a nested/scope child runs **concurrently** with its parent while
**inheriting the parent's access grant**, so parent and child can race on the same mutable
guarded state, and the declaration-based harness is structurally blind to it (both accesses
are "declared"). A field survey (Trio, asyncio TaskGroup, Kotlin, Swift SE-0304, Cilk, TBB,
OpenMP, .NET TPL, Legion, OmpSs-2, actors) found structured completion-gating universal but
always motivated by **lifetime/cancellation**, never resource-sharing; inheriting a live
**writable** grant into a concurrent child is something essentially no mainstream system does
silently (Legion requires the parent to unmap the region first; OmpSs-2 requires the parent
to be a non-accessor; Swift considered and refused concurrent mutable capture; .NET makes
attached children opt-in).

Replacement, by shape:
- **Fan out over the holder's data** → `ts::parallel_for` (its helpers inherit the caller's
  grant via an `Access_context` snapshot, and the synchronous join keeps them strictly inside
  the grant window — the one sanctioned in-task wait).
- **Fire independent work** → plain `ts::launch` (detached, inherits nothing).
- **Awaited composition** → `co_await` a `Task` / `Signal` / `parallel_for` / a nested graph run.

**Reintroduce a safe form only against a concrete need.** The field's three safe patterns are
already covered: **read-only inheritance** → `Versioned` snapshots; **parent relinquishes** →
node-splitting (declare the narrower set on a second node ordered after the first, §10.4);
**child re-acquires** → `obj.async` / `co_await obj.access`.

What SURVIVES is graph-internal only: `detail::add_nested` gates a coroutine node's frame and
a nested graph run through the block's `num_locks` (§4.4, §4.8), and a coroutine frame's own
`final_suspend` gates on that counter. The implicit-scope TLS (`current_scope_children`) is
kept solely so the non-quiet-scope lending check (§4.8 fatal 2) can see a still-running nested
run; it is no longer reachable from user code.

### 4.4 Coroutine graph nodes
`add_node` accepts a body returning `Task<void>` (a coroutine): access modes deduce from
the parameters exactly as for functor nodes; the node's completion gates on the *frame's*
completion (one waiter on the returned task). Grants per §2 — suspension frees the worker,
never the grant. Functor nodes stay the zero-frame common case.

```cpp
graph.add_node([&audio](Physics& phys, const Nav& nav) -> ts::Task<void>
{
    auto islands = phys.discover_islands();              // under the node's write grant

    ts::parallel_for(islands.size(), [&](std::size_t i)  // data-dependent fan-out; the helpers
    {                                                    // inherit the node's grant, join here
        phys.solve(islands[i]);
    });

    float mix = co_await audio.access([](const Audio& a) { return a.mix_level(); });
    // foreign read under held grants -- waiting rule (c): short, read-mode, acyclic

    phys.apply(mix);
    co_return;   // frame completes -> node completes -> grants release -> successors unlock
});
```

### 4.5 Cancellation
Tokens unchanged. Awaiting a cancelled `Task<void>` resumes normally (query
`is_cancelled()`); awaiting a cancelled `Task<R>` with non-void `R` is fatal — there is
no result to produce and no exceptions to signal with, so it is a precondition, exactly
as `sync()` on one is today (check `is_cancelled()` first). The
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

### 4.8 Nested graph runs (author-revised: lending instead of a fatal)

**Status: IMPLEMENTED (2026-08), as designed below.** The lend is applied by
`Static_task_graph::bind_links_for_run` (src/static_task_graph.cpp) at every `execute()`:
it intersects the compiled access set with `detail::current_access` and re-binds the
`compile()`-time link slab to the objects the run must actually take turns on — an unbound
link IS a skipped turn, so no bypass flag threads through the pipe. All three fatals landed
with companions, plus the auto-scope-join default and `Execution_options{.detach}`. Two
refinements against the text below: the non-quiet-scope check filters out already-SETTLED
scope children (only live ones are a hazard, and the scope list drops entries at a join, so
plain emptiness would fatal on a fire-and-settle child earlier in the body); and the
in-flight fatal is `TS_SAFETY_CHECKS`-gated like the other two. Worker-less nested runs
(§11.2) fell out working and are covered by a test. Tests: `graph_tests` "nested run *"
(9 cases incl. recursion and worker-less) + three death scenarios; `stress_graph_nested_runs`
under TSan.

`co_await inner.execute()` from a node is supported. An **awaited** inner run is strictly
contained in the outer node's grant window (the outer body is suspended for its duration),
so overlapping objects are **lent**, not re-acquired: at the call, intersect the inner
graph's compiled access set with the caller's `Access_context`; every overlap whose held
mode covers the inner mode (write covers read+write, read covers read) is marked lent for
the run. Lent objects skip the inner nodes' pipe cascades (a per-run lent-mask); inner
conflict edges still order inner nodes among themselves on the lent object; inner contexts
carry the outer's epoch (the inheritance model), so the harness stays live. Recursion
works by construction (a grand-inner intersects against its caller's context, which
already holds the lent entries). The pipe never sees the lend — external asyncs queue
behind the outer's hold exactly as today.

Fatals (each with a companion test):
1. **Mode-incompatible overlap** — outer holds read, inner contains a writer: a read
   grant cannot cover a write and upgrading would re-acquire (the deadlock). Restructure:
   outer declares write, or hoist the writer out of the sub-graph.
2. **Lending with a non-quiet scope** — an earlier **un-awaited nested run** of the caller
   still in flight could race the lent-to inner graph on the same object, both "validly".
   Rule: a lending `execute()` requires that no earlier un-awaited nested run of the caller
   is still running (runtime check: no live child on the implicit scope). `co_await` the
   previous nested run first. (Before the `ts::nested` removal this also caught unjoined
   scope children.) Conservative; relaxations live on the §10 queue.
3. **`execute()` while a run is in flight** (same graph) — explicit fatal (the in-flight
   state already exists for the destructor check).

Un-awaited inner runs are safe **by construction**: the default is auto-join of the
caller's implicit scope (node completion gates on the run; nothing can float); an explicit
detach opts out and structurally receives no lends and no inherited context (cross-frame
rules apply; a detached run overlapping the caller's holds simply queues behind them —
legal, nondeterministic timing, documented). `writer_owner` stays the outer node under a
lend, so `commit()` from an inner node hits the existing inherited-grant fatal — same
contract as scope children. The declared "parameter grants" form (an inner graph asserting
it expects a lend) demotes to optional future sugar for shipped library sub-graphs.

Author note (2026-08): the expected usage is a **pre-compiled** inner graph merely
*executed* inside a parent node (build-once/run-many), not constructed there. All of the
above is unaffected — the lend depends only on the compiled access set and the caller's
grants at await time, and the lent-mask is per-run state — but pre-compilation makes the
in-flight fatal guard a plausible *pattern*, not just misuse: a shared inner-graph
instance invoked from two concurrently-running parents collides with one-run-at-a-time.
v1 rule: one instance per concurrent user (compile a clone, or order the parents via
edges); the relaxation is run-queueing / pipelined runs — existing TODO 2.3, not new
machinery.

## 5. Performance workstream (the added cost and the plan to reduce it)

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
| lending an object to a nested run with an earlier un-awaited nested run in flight | `co_await` the previous nested run first |
| awaiting a cancelled value task | `is_cancelled()` check, then branch |
| circular wait (foreign await under grant; needs TODO 6.5) | the §2 hierarchy: declare / snapshot / stage |
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
   `execution_flag` mode → retraction + claim/generation → reuse → inline trampoline
   (dynamic surface only, as landed) → block slimming (vectors → waiter list; landed as
   `successors` → bare `Task_ptr`, 280 → 264 B — the `continuations` half is TODO 4.7
   step 2). Suite green after each sub-step.
5. **The detector** (TODO 6.5) + the remaining matrix rows; the waiting-rules section into
   task-internals (rewritten §4/§6/§7/§8).
6. **Validation + docs** — benches vs §5.5 targets, full TSan campaigns, ASan/stress,
   Shipping, alloc audit (`mem_profile` on frames); guide/design rewrites (composition
   chapter = coroutines; migration table); CLAUDE.md at merge.

## 8. Flagged changes (behavior/API breaks, per the shakeup mandate)

1. `then`/`when_all`/`ts::task` builder/`after`/`set_inline`/executable `reset()` —
   **removed** (compile errors). `ts::nested`, `ts::Task_scope`, `ts::join_nested`, and
   `detail::with_inherited_access` were reinstated by the §4.3 revision and then **removed
   for good (2026-08)** — see §4.3 for the rationale. `detail::add_nested` is **kept** as
   graph-internal completion-gating (coroutine node frames, nested graph runs).
2. In-task `sync()` — was retraction-or-park, becomes **fatal** (checked builds).
3. Retraction removed — bare-tree fork-join via `sync()` inside tasks no longer exists;
   the pattern is `co_await`/`parallel_for`.
4. `Task<R>` handles: `sync()` demoted to boundary-only; awaiting is the primary verb.
5. Coroutines become **mandatory** (drop `__cpp_impl_coroutine` guards; C++23 baseline
   already assumes a conforming compiler).
6. ~~`access()` becomes awaitable-only (red); blue callers use `async()`.~~ **Not what
   landed (2026-08).** `access(fn, opts)` kept its eager, `Task<R>`-returning form and
   stayed usable from BOTH colors: awaited from a coroutine, `sync()`ed or fire-and-forget
   from a blue thread. The split between `access` and `async` is dispatch policy, not
   color — `access` runs the body inline when the pipe is free right now, `async` always
   enqueues — so there was nothing to take away from blue callers, and the opportunistic
   inline fast path (the thing this flag proposed dropping) is exactly what §4.2 wanted to
   keep. What the transformation actually ADDED to `access` is the reentrant arm
   (`Pipe::writer_owner == current task` → run under the held grant, never queue behind
   yourself), which is waiting rule (b) made real. The awaitable-only surface is the
   separate held-grant form, `co_await ts::read_write(obj)` / `ts::read_only(obj)`, whose
   guard holds the grant for a scope rather than for one body. Net: no behavior was
   removed here; this flag is withdrawn.
7. Allocation profile: +1 frame per dynamic task until fusion (§5.1) restores one-alloc.
8. Async pipe release moved body-return→settle already landed with the evolved pipe;
   under coroutine nodes this generalizes to frame-completion (§2) — same direction.

## 9. Risks

- **`execution_flag` deletion** touches `Executable::run`/`settle` — the §8 invariant
  must be re-proven via scope/frame-gating (stage 4 has a dedicated TSan pass on the
  graph nested→scope conversion).

## 10. Parallel discussion queue (author, 2026-08 — revisit alongside implementation)

Not blockers; to be worked through while the stages land:

1. **Waiting-rule limitations (§2) — ANSWERED (2026-08) by a field survey; one gap
   identified.** ("Doctrine" renamed to **the waiting rules** — author, 2026-08: the old
   name oversold a short list of ordinary rules.)

   The concern was that the rules fight the architecture: we advocate parallelising at the
   highest level (fat nodes holding many objects), and fat nodes look like more deadlock
   surface. **The survey says the risk is misattributed.** A fat DECLARED access set carries
   no deadlock risk at all — it is acquired in canonical order, all-or-nothing, and never
   released mid-acquisition to wait for something outside the set; that is conservative 2PL,
   deadlock-free by construction regardless of cardinality. Every system that predeclares the
   most (Bevy's executor takes the whole set or nothing; BoC proves it for arbitrary cown
   sets; Legion tasks declare dozens of region requirements) is deadlock-free *because* it
   predeclares. All of the risk comes from DYNAMIC acquisition on top of the declared set,
   which is independent of fatness — and the two are in fact anti-correlated: the fatter the
   node, the cheaper it is to declare one more object instead of awaiting it. A thin node
   faces the sharp choice; a fat one has already paid. So "parallelise high" and "don't
   foreign-await" are the same advice, not competing advice.

   Searched specifically for a deadlock caused by declared-access fatness in Bevy, Unity
   DOTS, Legion, Orleans, oneTBB and the Rust ecosystem: **none found, in any system.** Every
   reported deadlock is a wait inside work — Bevy's `block_on` under a starved pool, Unity's
   forbidden `Complete()` from a job, Legion's inline mapping, Orleans grain-call cycles,
   oneTBB's work-stealing re-entrancy (the UE TaskGraph failure mode). The rules target
   exactly what users hit.

   Relative strictness: we are the LOOSE end of the distribution, not the strict end. Bevy,
   Unity, StarPU, OpenMP/OmpSs, Kokkos and BoC all FORBID what we merely discourage — StarPU
   returns `-EDEADLK` for precisely `co_await obj.access(fn)` from inside a task; Unity's
   docs name deadlock as the reason `Complete()` is main-thread only, calling the general
   case "provably impossible to solve". Legion permits it and documents the resulting
   deadlocks (plus an open unfixed hole since 2019). Tier 1–4 maps 1:1 onto the field's
   shipped mitigations (declare = C2PL/Bevy/Legion privileges; snapshot + stage = `Commands`
   / `EntityCommandBuffer` + sync points). What is unusual about our design is that we permit
   tier 4 at all.

   **The one real gap:** the canonical total order covers BATCH acquisition (`multi_acquire`,
   node declared sets) but nothing relates a held grant to a LATER dynamic await. That is the
   whole hole, and the field's cheap answer is a declared rank — see TODO 6.14.

   Two cautions the survey turned up, both recorded against their items: order-learning over
   *wait/completion* edges (as opposed to lock edges) has failed to merge into Linux twice in
   eight years on false positives (cross-release reverted in 4.15; DEPT unmerged after 4+
   years) — keep the circular-wait detector scoped to grant edges and let quiescence (6.13) carry
   the general case. And Go's quiescence check has a documented blind spot (any live
   background thread masks a partial deadlock), which is why 6.13's outstanding-external-
   wakeup counter is load-bearing rather than optional.

   Also noted, not planned: `ww_mutex` (wound-wait with the restartable unit shrunk to a
   side-effect-free acquisition prologue) is the only production precedent under our exact
   constraints — dynamic, caller-ordered acquisition over non-rollbackable in-place objects.
   It is the answer if a workload ever needs runtime-chosen access targets, since it
   *recovers* where we would fatal. Recorded in the design space; not worth building on
   suspicion.
2. **HALO (§5.4)** — currently treated as unavailable; explore what coroutine shapes /
   compiler flags / annotations actually elide frames on MSVC and clang-cl, and whether
   any hot path can be structured to qualify.
3. **Nested graphs** — a graph run launched from within another graph's node (or a
   coroutine): limitations, object-overlap rules (task-internals §10 scenarios 2–3),
   whether the coroutine model makes an inner `co_await g.execute()` safe and useful.
4. **Revoking grants on suspension — discussed 2026-08, parked with an outcome.** Full
   revoke-and-reacquire is coherent (condition-variable `wait` semantics) but carries four
   costs: the torn node view (silent unless epoch revalidation is made always-on),
   structurally unrevocable grants (live scope children / lends — the quiet-scope
   condition), resume re-acquisition latency + priority inversion, and a second "color"
   of await. Its upside (revoked awaits dissolve the suspended-ABBA class) does not
   outweigh them without a demonstrating workload; parked. **The preferred form of the
   same optimization is node-splitting**: declare the narrower access set on a second
   node ordered after the first — statically safe (the second body never receives the
   released object, so a stray touch is impossible by construction, not merely
   harness-caught), visible in the derived edges/DOT/trace, and the object frees at the
   first node's completion. A dynamic `ts::release_early(obj)` (drop a grant permanently
   mid-node; harness evicts the object from the context so later touches fatal in checked
   builds) remains a possible convenience if splitting proves high-friction in practice —
   its cost is that enforcement is runtime-only, so shipping builds would race silently
   on the mistake the split makes unrepresentable. Note only; no action for now.
5. ~~**Library without static graphs**~~ — **ANSWERED (2026-08), with a measurement.**
   `sample/game_frame.cpp` now carries a third composition of the same ~34-system frame,
   `run_frame_graph_free` (same `World`, same `tick_*` bodies, no `Static_task_graph`):
   every system is a multi-object `ts::async`, every edge a `co_await`. Findings, in
   `docs/guide.md` §6.4:
   - **Safety does not weaken at all** — it was never the graph's. A hand-composed system
     takes the same mode-aware pipe turn on everything it declares, and the harness still
     catches undeclared access. What weakens is exactly one thing: nothing derives or
     checks the ORDER.
   - **Pipe FIFO is not a substitute for conflict edges**, which was the tempting
     assumption going in. The multi-object cascade enters links one at a time in canonical
     order, so a system blocked on its first object has not taken its slot on the later
     ones and a later launch overtakes it. Launching the sample's node list in declaration
     order with no explicit awaits mis-orders `frustum_cull` before `camera` and `submit`
     before `cmd_record` (a frame of draw commands lost), silently — every declaration
     correct, harness quiet, and 7.6% faster for it. `compile()` derives 69 edges here;
     by hand that is 17 chain coroutines and 42 `co_await`s.
   - **The graph's perf advantage is NOT allocation amortization**, contrary to the
     standing assumption. Graph-free costs +95 allocations/frame (134 vs 38) — under 2 µs
     at ~17 ns each — but +64 µs/frame on a 4.1 ms frame and +131 µs on a 0.45 ms one
     (+1.6% / +28.7%). The gap is ~50 coroutine suspend/resume round trips at ~1.8 µs
     (`coro chn`): the graph dispatches a successor on the thread that settled its last
     predecessor, an awaited handle does not. If that ever needs closing, the lever is
     resume locality, not the allocator — see §5.
   - Guide text landed as §6.4 "Do I need the static graph?"; benchmark entries
     `graph 1.0 / free 1.0 / graph .05 / free .05` under `--bench`.

## 11. Post-initial-implementation action list (author, 2026-08)

Queued behind the §7 stages; numbers reference the nested-graphs review discussion.

1. ~~**Nested graph runs v1** (§4.8)~~ — **DONE (2026-08)**, see the §4.8 status note: the
   lend protocol (intersection + link re-binding + epoch-carrying contexts),
   auto-scope-join default + explicit detach, and the three fatals with companions
   (mode-incompatible overlap; non-quiet-scope lend; execute while in flight — the last
   also fixed the previously-unguarded concurrent `execute()` on one graph).
2. ~~**(3.3) Worker-less nested runs**~~ — **DONE (2026-08)**, no work needed: inline
   execution through the serial trampoline already handles it (the lend is what keeps it
   from self-deadlocking on the caller's own hold), and a dedicated test pins it.
3. **(3.4) Cancellation composition** — the documented pattern: pass the outer node's
   token into `inner.execute({.token})`; test that outer cancellation drains the inner
   run.
4. **(3.5) Trace attribution across nesting** — the inner graph traces its own nodes;
   audit the outer node's owner-attributed true-busy so inner work is not double-counted
   in the outer trace's fold.
5. ~~**(4) Concurrent shared-object graphs validation**~~ — **DONE (2026-08): it holds.**
   The old §10 scenario-2 line conflated two different things; the "not supported" cause
   was the per-graph single-run `Run_state`, which says nothing about two DIFFERENT graphs.
   With canonical pipe-address ordering over the same address-sorted objects, two graphs
   cannot form a wait cycle, and the pipe serializes their conflicting nodes against each
   other exactly as it does a node against an async. Validated with graphs declaring the
   same objects in OPPOSITE order, run concurrently under an async hammer, with the
   `Rw_probe` overlap oracle: bounded suite test + `stress_concurrent_graphs` under TSan.
   task-internals §10 scenario 2 relaxed to "safe, nondeterministic cross-graph ordering",
   and the same-graph case is now a fatal rather than silent corruption.
6. **Parameter-grants sugar** (§4.8 tail) — the declared expects-a-lend form for shipped
   library sub-graphs; compile-time intent check only, no new mechanism.
- **Allocation regression window** between stages 3 and the fusion landing — fusion is
  stage 1 for exactly this reason.
- **MSVC/clang-cl coroutine codegen** — measure early (stage 1 microbench), not assume.
- **Test churn** — 543 checks reshuffle; the graph/pipe/harness suites are untouched,
  the task/integration suites transform. Wave discipline as in the pipe rebase: green at
  every commit.
- **Debuggability** — suspended frames have no stacks; the circular-wait detector (stage 5)
  and the trace tooling carry more diagnostic weight. Accepted.
