# Task internals — structure, lifecycle, allocation

Design of record for the dynamic task object: what a task *is*, the states it
moves through, how prerequisites / nested tasks / retraction work, and how it is
allocated. Distilled from the design discussion; grounded in the Unreal Engine
Tasks system (`Engine/Source/Runtime/Core/.../Tasks/TaskPrivate.{h,cpp}`), with
the differences that matter for *our* access-safety model called out.

**Status:** design/target. Current code has `Task_state<R>` (a plain
completion+result blob) and `Static_task_graph`; this doc describes what they
evolve into. Not all of it is implemented yet — retraction and nested tasks in
particular are follow-ups the layout below is designed to admit.

---

## 1. Tiers: one primitive, coordinators on top

Richness is not a property of "the task" — to the scheduler every task is just a
closure it runs. Richness lives in the **coordination structure** the closure is
wired into. The tiers are selected at the **entry point** (which statically knows
the capability set), never upgraded at runtime (a live upgrade is race-prone).

| Tier | Coordinator | Per-task cost | Entry point |
|---|---|---|---|
| Primitive | none | 0 alloc (closure rides the queue slot / SBO) | `Scheduler::submit` |
| Group | one shared latch (counter + one completion) | 0 alloc/task, 1 for the group | `parallel_for` |
| Task | `Task_control_block` (+ closure for executable) | 1 alloc | `async`, `then`, `when_all`, graph |

There is deliberately **no middle "awaitable-but-no-deps" tier.** You cannot know
at construction whether a task will later be awaited, chained, or made a nested
parent, so a lighter type would have to be upgraded on first use — the racy
late-install. Instead the single Task tier is made *cheap when its features are
unused* (empty dependency lists cost no heap; the lock-counter doubles as the
completion flag; SBO closure), and `submit` covers the "nothing needed" case.
Users extend by writing their own coordinator over `submit`, exactly as the
`Thread_safe` pipe, `Static_task_graph`, and `parallel_for` do — not by
inheriting a task base class.

---

## 2. Object model

Three types on the Task tier; two private, two public handles (one shared).

- **`Task_control_block<R>`** (private) — the refcounted core: completion signal,
  result, successors, prerequisite/nested backlinks, the lock-counter, and an
  **optional type-erased body** (`std::move_only_function<void()>`, empty for a
  bodyless block). **One monomorphic type** — parameterized on `R` only; the
  closure type is erased at the entry point and never reaches it (see §2.1). The
  dependency machinery only ever traffics in `Task_control_block<R>*`, so bodyless
  blocks and executable tasks interoperate as prerequisites/successors with no
  special-casing.

- **`Task<R>`** (public) — the consumer handle (a future): `get()`, `is_done()`,
  `then()`, and use as a prerequisite. Same type whether the block is executable
  (from `async`/`then`/graph) or bodyless (a `Signal`).

- **`Signal`** (public) — a bodyless `Task<void>` completed by hand: `trigger()`.
  Both producer and consumer in one handle — the consumer side (`get`/`wait`,
  `is_done`, `then`, use-as-prerequisite) is inherited from `Task<void>`, the
  producer side is `trigger()`. Copyable; copies share one block. `trigger()` is
  idempotent (first call wins). This is UE's `FTaskEvent`: real usage is
  overwhelmingly a `void` done-signal / barrier / pipeline-phase gate, passed
  around as one handle that some code triggers and other code waits on — which is
  why it is *not* a producer-only `Promise` and *not* a promise/future pair.
  `trigger()` lives only on `Signal`, so a task with a body cannot be completed by
  hand. (Value-carrying out-of-band completion is deferred; add `set_value` /
  revisit a promise/future split if it is ever needed — genuine usage is all void.)

### 2.1 One monomorphic type, no inheritance, no virtual

The "bodyless core vs body" distinction is **conceptual**, not a base/derived
template hierarchy. UE implements it by inheritance (`FTaskBase` +
`TExecutableTask<Body>` + a `virtual ExecuteTask`), which forces a virtual
destructor and, per closure-typed `TExecutableTask` instantiation, a vtable +
typeinfo + the Itanium destructor triple (D0/D1/D2) — plus any templated-derived
code duplicated per body type. That is the per-closure code / i-cache blowup UE
hit.

We avoid it entirely: the body is a **type-erased member** of a single
non-polymorphic `Task_control_block`. Consequences:

- No polymorphism → no virtual destructor, no vtable, no typeinfo, no D0/D1/D2
  triple. The block's destructor is emitted **once** and destroys the erased body
  via the `move_only_function`'s own manager.
- Per-closure codegen collapses to the `move_only_function`'s ~2 thunks
  (invoke, move/destroy) — the irreducible minimum for any type-erased callable.
- All the heavy logic (lock-counter, retraction, `Close`, successor walking,
  scheduling) stays **monomorphic** — emitted once, not per closure type. This is
  the actual i-cache win, and it is better than the inheritance model on that axis.
- Body invocation is one indirect call through the erased function pointer — same
  cost as a virtual call, negligible next to the body.

**The discipline that keeps it bounded:** erase the closure at the entry point.
`async`/`then` are templated on the closure only long enough to wrap it into the
erased body and call a non-templated launch path; the persistent types
(`Task_control_block`, `Task<R>`) are parameterized on **`R` only**, so the number
of instantiations is bounded by the number of *result types*, not closure types.
The closure type never leaks past the entry point.

*Current status:* the erased body already lives outside the type system (today the
closure is a `move_only_function` captured in the scheduler/pipe submission, not a
member of the block). Moving it *into* the block is only required for retraction
(the waiter reaches the un-executed body through the block) and lands with that
work; the no-virtual / monomorphic-on-`R` property already holds.

*Event size note:* a bodyless block carries an unused body slot (~the
`move_only_function` SBO). Acceptable with a pooled block allocator; if event
density ever makes it matter, split events into a smaller type using a manual
`destroy`/`execute` function-pointer pair (still no C++ virtual). Do not
pre-optimize.

### The body / no-body seam

The closure is an *add-on* to the bodyless core, not an intrinsic field. This is
why a bodyless block pays no closure cost — and events are used constantly as sync
points, so that cost is systematic.

It is also the *one* place separation is safe. The general rule (§6) is that work
and completion must stay fused, because a retracting waiter reaches from the
completion handle into the un-executed closure. A bodyless block has no closure
and is never executed, so that constraint vanishes — bodyless is not "an
executable task with its work split off" (still forbidden), it is a node with no
work to split.

---

## 3. Lifecycle

```
Created ──launch──> Pending ──(last prereq)──> Queued ──dispatch──> Running
   │                                                                   │
   │ (zero prereqs: launch goes straight to Queued)          body returns
   │                                                                   │
   │                                    ┌── no nested pending ─────────┤
   │                                    │                              │
   └──────────────────────────> Completed <── all nested done ── Awaiting-nested
```

- **Created** — mutable; the builder / `Signal` path attaches prerequisites and
  properties here. `async` skips this: it is born launched.
- **Pending** — launched, waiting on execution prerequisites.
- **Queued** — prerequisites met; owned by the scheduler.
- **Running** — a worker (or a retractor) holds execution permission; body runs.
- **Awaiting-nested** — body returned but nested tasks are still outstanding.
  **Body-return ≠ completion.** Most tasks skip this state.
- **Completed** — result published, waiters released, successors unlocked. This
  is what downstream edges key off — never body-return.

---

## 4. The unified lock-counter

One `std::atomic<uint32> num_locks` tracks *both* pre-execution blockers
(prerequisites) and post-execution blockers (nested tasks), distinguished by a
high-bit mode flag. This is the mechanism that makes prerequisites and nested
tasks the same thing seen across the execution boundary.

```
execution_flag = 0x8000'0000        // MSB; set at execution start

construct        : num_locks = 1                 // the "not launched" lock
add prerequisite : ++num_locks                   // (Created only)
launch           : --num_locks; if 0 -> schedule
prereq completes : --num_locks; if 0 -> schedule

claim/execute    : CAS num_locks 0 -> execution_flag + 1   // one winner; +1 self-lock
add nested       : ++num_locks                   // during body; now above the flag
body returns     : n = --num_locks; if n == execution_flag -> Close()
nested completes : n = --num_locks; if n == execution_flag -> Close()
```

- Below `execution_flag`, the count is outstanding prerequisites; reaching 0
  schedules. The claim CAS (`0 -> execution_flag + 1`) is the single linearization
  point for "who runs this," shared by worker dispatch and retraction.
- The `+1` self-lock on claim ensures a nested task completing mid-body cannot
  `Close` the parent before the body finishes.
- Above `execution_flag`, the count is outstanding nested tasks; the last one to
  hit `execution_flag` closes the parent.

(Pipe serialization is handled separately by `Thread_safe`'s reader/writer pipe,
not this counter; ordering-via-pipe is out of scope here.)

---

## 5. Data lifetime

| Data | Live | Notes |
|---|---|---|
| Closure + captures | Created → body-return | exec-bounded; destroy right after the body to release captures early |
| Backlink array | Created → Completed | reused: prerequisites (released at claim), then nested (released at Close) |
| `num_locks` | Created → Completed | mode flips at `execution_flag` |
| Result | body-return → last reader | **written** at body-return, **published** at Close |
| Successors, waiters | Created → Completed(+) | released at Close |

The asymmetry: the closure is execution-bounded; everything else on the block is
completion-bounded or longer. We do **not** put the whole thing in an arena (a
long-held block would pin a page). Instead the block is individually refcounted
(reclaimed exactly when the last handle drops), and at body-return we *destroy the
closure in place* — reclaiming the captured resources (e.g. a `shared_ptr` to a
large asset) immediately, without freeing the block. That is the answer to "dead
work bytes": release the captures early, not the bytes.

---

## 6. Retraction

A thread waiting on `T` does not block a worker — it walks `T`'s backlinks and
executes the un-started prerequisites inline (claiming each via the §4 CAS so a
worker cannot double-run it), recursively, then runs `T`. This is *targeted*
busy-waiting (only your own dependency subtree, in order) — not the general
"execute arbitrary queued work," which invites stack growth and priority
inversion.

**Retraction is why work and completion must be one refcounted object.** The
waiter holds the completion handle and from it must reach the un-executed closure.
If the closure lived in separate single-owner (e.g. arena) memory, the retractor
could dereference it just as the winning executor frees it — the reclamation
use-after-free we have already been bitten by. A single refcounted block dissolves
it: the waiter's handle keeps the whole block (closure included) alive, and the
claim is a CAS on a field of a guaranteed-live object. This decides the
single-blob layout on *safety* grounds, independent of the (hard-to-isolate)
allocation-count comparison.

---

## 7. Nested tasks

A task launched deep inside a parent's call stack, whose completion the parent's
completion depends on — equivalent to the parent awaiting it at the end of its
body, but without that being lexically expressible where the nested task is
spawned.

- The parent is found via `thread_local Task_control_block* current_task`,
  installed at execution start (companion to `thread_local Scheduler*
  current_scheduler` and the per-task `Access_context`).
- Registration reuses the prerequisite mechanism: the nested task `++`s the
  parent's `num_locks` and is pushed into the parent's backlink array (also
  enabling nested-task retraction); the parent completes only once all nested
  tasks do (§4).
- `parallel_for` chunks are a degenerate nested task today ("subtasks inherit the
  parent node's context"). First-class nested tasks generalize them; the default
  cheap `parallel_for` stays on the group-latch tier (§1) — nested-task-block
  chunks are an opt-in richer mode (retraction + uniform access inheritance) at
  per-chunk cost.

---

## 8. The access invariant (ours, not UE's)

UE has no access model, so this constraint is unique to us:

> **A node's declared access grant is held until its nested tasks complete.**

If a graph node declares access to system `X` and its body spawns a nested task
still writing `X` after the body returns, a downstream node — unlocked on the
parent's *completion* — must not run until the nested writer is done. This is
automatic **iff** successor edges key off `Completed` (body + all nested), which
§4 provides. Concretely: the parent's `Access_scope` must span to **Close**, not
body-return, whenever nested tasks exist. Otherwise a downstream reader races a
live nested writer — the completeness hazard in a new guise. Nested-completion
gating is therefore a **safety invariant**, not a convenience.

---

## 9. Allocation

- **Executable task**: one heap allocation for the `Task_control_block`, SBO
  closure (no second alloc for small bodies). Refcounted; individually reclaimed,
  so no page pinning.
- **Bodyless block** (`Signal`): one allocation; the body slot is unused (see the
  event size note in §2.1).
- **Primitive / group tiers**: zero per-task allocation (§1).
- **`Static_task_graph`**: owns its nodes in a slab, reused every `execute()`; a
  minority of tasks and out of scope for the dynamic allocator decision.
- **Pluggable pool hook**: a fixed-size-per-`R`-size-class free list behind the
  block allocation, for extreme fan-out / non-gamedev workloads. Transparent (does
  not change the model); individual recycling avoids the arena pinning that ruled
  out a bump allocator for the whole block.

The single cleanly-isolable perf question is **pooled block allocation vs raw
`make_shared`** (one microbenchmark, fixed `R`) — it doesn't change the model, so
it can be measured without the cross-cutting effects that make the architectural
comparison hard.

---

## 10. Graph ↔ dynamic `async` coexistence (pipe reservation)

Graph nodes access their objects **directly** (bypassing the pipe), ordered among
themselves by conflict edges. `Thread_safe::async` goes through the **pipe**. Two
independent serializers over one object → a node and an async on the same object
would run concurrently → **data race**. The access harness does **not** catch this:
both sides hold a valid declared `Access_context`, and the harness only catches
*undeclared* access, never two-declared-concurrent. So the race is silent
(TSan/ASan only) — which is exactly why it needs a structural fix.

**Mechanism (implemented):** `pipe_reserve(pipe, on_acquired)` holds a pipe as an
exclusive writer that does **not** auto-complete — idle → acquired synchronously
(returns true); else the reservation queues behind pending work (FIFO) and
`on_acquired` fires when admitted. Async jobs queue behind a held reservation, so
`X.async(...)` concurrent with a run touching `X` runs *after* the run's use of `X`,
never alongside a node. One authority per object per frame.

Reservation is **lazy on acquire and early on release**, so an object is held only
for `[first accessor's dispatch, last accessor's completion]` — not the whole run:

- **Lazy acquire.** An object is reserved when its *first data-ready accessor* is
  dispatched — not up front. A node runs only once **two** gates are open: its data
  prerequisites (`remaining_deps`) *and* its object reservations
  (`remaining_objects`). When a node becomes data-ready (`on_data_ready`) it
  `ensure_reserved`s each of its objects (a per-pipe `object_initiated` CAS makes the
  first accessor the sole initiator); when a pipe is acquired (`on_object_reserved`)
  every accessor's `remaining_objects` drops and `maybe_run` re-checks both gates
  (a run-once `launched` guard). So a late-touched object (e.g. `audio`, written at
  end of tick) stays free for async through the early frame.
- **Early release.** Each object is freed by its *last accessor*, not at run
  completion: `run_node` decrements `remaining_accessors[pipe]` (initialized to the
  accessor count from `pipe_accessors_`) and calls `pipe_release` at 0. Safe — 0 is
  reached only after every accessor has completed.

**Deadlock-freedom** no longer needs "reserve all up front / sole multi-object
acquirer": reservation is **run-level and shared** (a pipe is reserved once per run;
a node needing an already-reserved object just proceeds — it never waits for a holder
to release), and acquiring a reservation only ever waits on *that pipe's own* queued
async, which is single-object and independent. No cross-object hold-and-wait, so no
cycle — even without a canonical order. (The one real deadlock remains blocking on a
same-object async *inside* a node; see below.)

**Gaps:** the window is `[first accessor, last accessor]` *including* any interior
gap where no node touches the object — a mid-run gap still holds the reservation
(async blocked through it), because "last accessor" is the release trigger and
releasing/re-acquiring mid-run could then delay the object's later node (and gaps
aren't statically precise under parallelism). The way to *shrink* gaps is
**compile-time grouping** — scheduling an object's accessors close together — which
trades against parallelism and is a follow-up.

Note there is **no** class of objects that async can't reach: `async()` is public on
every `Thread_safe`, so any graph object is potentially async-reachable — you can't
statically skip reservation for "async-free" objects. A per-object user assertion
("graph-exclusive, never async'd during a run") could skip it, but that trades the
safety guarantee for ~2 mutex ops per object per run (the reservation cost on an idle
pipe) and reintroduces the silent race if violated — not worth it. If the overhead
ever matters, make the idle-pipe reserve lock-free rather than skipping it.

### Scenarios where the model can still break

Reservation + single-resource async closes the common race, but these remain sharp:

1. **Blocking on a same-object async inside a node** → deadlock: the async sits
   behind the run's reservation (released only at run end) while the node `.get()`s
   it. (Reinforces "never block inside a node.")
2. **Two runs over overlapping objects, concurrently** → reservation deadlock (two
   multi-object acquirers, opposite orders). One run per object-set at a time.
3. **Nested `execute()` on a reserved object** → the inner run's reservation waits
   for the outer's release, which waits for the triggering node → deadlock.
4. **Incomplete access declaration** → a node touching `Y` but declaring only `X`
   doesn't reserve `Y`; async to `Y` races the node's undeclared `Y` access. The
   reservation is only as complete as the declaration.
5. **Escaped reference** → a raw pointer into `X` used with no `Access_context`
   bypasses both reservation and harness. The classic completeness hazard.
6. **Ordering / latency (not races):** whether the graph observes a pre-run async's
   write depends on enqueue timing (nondeterministic); coarse release backs up async
   to a graph object for the whole frame.

Contract, therefore: **one run per object-set at a time, no blocking on same-object
async inside a node, and complete access declarations.**

---

## 11. Cancellation

Cooperative, no exceptions. `Cancellation_source` owns an `atomic<bool>` flag; its
`token()` is handed to `async` / `then` / `execute`. **Cancellation is a completion
state**, not a separate channel: a settled task is either completed or cancelled.

- **Checked when work is about to run.** `async`'s body and each graph `run_node`
  check the token first; if cancelled, the body is **skipped** and the block settles
  as cancelled. Already-running work is *not* interrupted (a long body can poll the
  token cooperatively). So cancellation skips *not-yet-started* work.
- **Propagates automatically.** Continuations carry the outcome — `void(bool
  cancelled)` for `void`, `void(R*)` (nullptr = cancelled) for a value. `complete`
  fires them with the result; `cancel` fires them with the cancel signal, and each
  continuation's closure then cancels *its* subsequent. So cancelling one task
  cancels the whole downstream chain, and a cancelled prerequisite cancels its
  continuations even if they weren't given the token. `then(fn, token)` also lets the
  token cancel *at that link* even when the producer succeeded.
- **Graph:** `execute(scheduler, token)` — pending nodes skip (the DAG still drains so
  the run settles), and the completion `Task<void>` is cancelled. In-flight nodes
  finish.
- **`get()`:** a cancelled `void` `get()` unblocks (query `is_cancelled()`); a
  cancelled *value* `get()` is **fatal** — there is no result, so check
  `is_cancelled()` first. (No exceptions, so this is a precondition, not a throw.)

The token flag is atomic and `settle` is idempotent under the block's mutex, so
`request_cancel` racing a body's check or a completion is race-free (verified under
TSan): the block settles exactly once, either way.

**Not covered:** cancelling a `when_all` prerequisite — its internal join
continuations aren't cancel-aware, so a cancelled prerequisite would stall the join
(the remaining counter never reaches 0). A cancel *callback* (notify on request) is
also future work.

---

## Open items

- **Done:** `Task_state → Task_control_block` rename; idempotent `complete()`;
  `Signal` (bodyless triggerable `Task<void>`); graph↔async pipe reservation, lazy on
  acquire + early on release — window `[first accessor, last accessor]` (§10);
  cooperative cancellation (§11).
- **when_all cancellation:** make the internal join cancel-aware so a cancelled
  prerequisite cancels the join instead of stalling it (§11).
- **Cancel callback:** notify (a continuation) when cancellation is requested.
- **Compile-time grouping:** schedule an object's accessors close together to shrink
  its reservation window (fewer interior gaps), where the DAG allows — trades against
  parallelism / critical path, so profiling-guided.
- Reservation follow-ups: cheaper idle-pipe reserve (lock-free flag vs mutex) if the
  per-object mutex cost matters; detect nested/concurrent-run reservation deadlock
  (§10 scenarios 2–3) instead of hanging.
- Move the erased body *into* the block (currently in the scheduler/pipe
  submission) — required only for retraction; the monomorphic-on-`R` / no-virtual
  property already holds (§2.1).
- The lock-counter with the `execution_flag` mode bit (§4) — replaces the plain
  `ready`/`completed` completion flag and the graph's separate `remaining_deps`.
  This is the load-bearing step for the static-graph / dynamic-task unification:
  graph indegree and dynamic prerequisites become one counter on one block type.
- Retraction (the claim CAS is specified; the waiter-side walk is not built).
- Nested tasks + `current_task` TLS; re-base `parallel_for`'s rich mode on them.
- Group latch tier for the default `parallel_for`.
- Pooled block allocator + the one microbenchmark.
