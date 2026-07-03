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

- **`Task_control_block`** (private) — **fully monomorphic** (not templated at all):
  the refcounted completion/dependency core — completion signal, `ready`/`cancelled`,
  successors, prerequisite/nested backlinks, the lock-counter — plus two type-erased
  hooks: `void* result_ptr` (nullptr ⇒ no result: `void` or bodyless) and
  `void (*execute)(Task_control_block*)` (nullptr ⇒ no body: bodyless). Continuations
  are stored erased too (`move_only_function<void(void* result, bool cancelled)>`). A
  third fn-ptr, `on_complete` (nullptr for most tasks), fires once at `settle` and —
  unlike a continuation — is **not consumed**, so a re-armed reusable block (a graph
  node, §7.1) keeps it across runs: an alloc-free completion hook.
  All the heavy logic is compiled **once**, regardless of how many result/body types
  exist. The dependency machinery only ever traffics in `Task_control_block*`.

- **Storage wrappers** (private, per instantiation) — everything that carries a type
  is composed *around* the block, not into it:
  - `Result_block<R>  { Task_control_block core; std::optional<R> result; }` — for
    results with no body (`then`, `when_all`, a promise).
  - `Executable<Body,R> { Task_control_block core; std::optional<R> result; Body body; }`
    — for `async` / `launch`.
  - a bodyless resultless block (`Signal`, a pure join) is just `Task_control_block`.

  The wrapper is allocated with `make_shared`; the handle is a `shared_ptr`
  **aliasing** `&wrapper.core`, so the one refcount owns the whole wrapper (result +
  body destroyed with it) while everything downstream sees only the monomorphic
  block. `core.result_ptr` points at the wrapper's `result` (or null); `core.execute`
  points at a per-`Body` thunk that `static_cast`s the block back to `Executable`
  (block is the first member) and runs the body.

- **`Task<R>`** (public) — the consumer handle (a future): `get()`, `is_done()`,
  `then()`, use as a prerequisite. `get()` does `static_cast<R*>(core->result_ptr)`
  and moves out — the cast is always to the right `R` (storage + handle are created
  together at one site, both on the same `R`; confined to `detail`).

- **`Signal`** (public) — a bodyless `Task<void>` completed by hand: `trigger()`.
  Consumer side inherited from `Task<void>` (`get`/`wait`, `is_done`, `then`,
  prerequisite); `trigger()` is the producer side, idempotent. UE's `FTaskEvent`:
  a done-signal / barrier / phase gate. Allocates just the bare block (no result, no
  body).

### 2.1 Fully monomorphic block, no inheritance, no virtual, no result/body tax

UE splits body from core by **inheritance** (`FTaskBase` + `TExecutableTask<Body>` +
`virtual ExecuteTask`), which keeps bodyless `FTaskEvent`s small but forces, per
closure-typed `TExecutableTask`, a vtable + typeinfo + the Itanium destructor triple
(D0/D1/D2), plus virtual dispatch — the per-body code / i-cache blowup, against our
"avoid virtual" grain.

We get UE's small-events property **without** that: composition + a single `execute`
function pointer + `shared_ptr` aliasing (above). And we go further than "erase the
body" — **erase the result too**, so the block is parameterized on *nothing*:

- **No polymorphism** → no virtual dtor, no vtable, no typeinfo, no D0/D1/D2 triple.
  Destroy is handled by `make_shared`'s deleter for the wrapper (one per wrapper
  type, unavoidable).
- **Per-instantiation codegen** collapses to the wrapper struct + its `execute` thunk
  (~1–2 functions per body/result type) — no `R` on the block, so the heavy machinery
  (lock-counter, retraction, close, successor walking, scheduling) is emitted **once**.
- **No `<void>` specialization.** Today two near-identical blocks exist
  (`Task_control_block<R>` and the `<void>` one); monomorphizing collapses them to
  one. `void` = `result_ptr == nullptr`; bodyless = `execute == nullptr`.
- **Bodyless / `void` pay nothing** — no `optional<R>`, no body slot; a `Signal` is
  the bare block.
- One indirect call to run the body (through `execute`) — same cost as a virtual call,
  negligible next to the body.

`R`/`Body` appear only in: the thin `Task<R>` handle, the wrapper structs, the
`execute` thunk, and the `then`-closures (which cast `result_ptr`). Everything else
is monomorphic.

### The body / no-body seam (and why the *body* still can't be split from an executable)

The body is composed into the *executable's* wrapper, reachable from the block via
`execute`. This is safe precisely because it's the **one** separation the retraction
rule (§6) allows: a bodyless block has no body and is never executed, so nothing
reaches for a body that isn't there. For an *executable* task the body and the block
stay in one allocation (the wrapper) — a retracting waiter holding the block reaches
the body through `execute` + the cast, so the body must not live in separately-owned
memory. Composition-in-one-wrapper keeps that invariant while still costing bodyless
blocks nothing.

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

### 7.1 Nested tasks inside a graph node

A `Static_task_graph` node can now spawn nested tasks (`ts::nested` /
`ts::add_nested`) — needed for dynamic, data-dependent fan-out a static
`parallel_for` can't express (e.g. a physics node discovering N runtime islands
and solving them in parallel). This required making a graph node a **real task
block**, not a bare closure: `run_node` builds the node body as an
`Executable<Body,void>` and submits `execute` on the run's scheduler, so
`Executable::run` installs `current_task` and the `execution_flag` self-lock (§4)
around the body — exactly the state `add_nested` needs. The node's graph
post-logic (early release, successor release, run completion) runs as a
**continuation** on that block, so it fires only after the body *and* all nested
tasks settle — the §8 invariant, structurally.

This is a **scoped** version of "graph nodes are blocks": nodes get a block for
their *execution / nesting / completion* only. Scheduling stays as it was — data
prerequisites via `remaining_deps` + the lazy reservation via `remaining_objects`
(§10), *not* folded into `num_locks`. So `num_locks` on a node block is used only
in its `execution_flag` (post-execution) mode; the intricate part of a full rebase
(folding prerequisite counting into `num_locks` against the lazy reservation) is
avoided.

**Allocation-free re-runs.** The node block is a `Graph_node_block` (the
`Task_control_block` + a `graph`/`index` back-pointer), **allocated once at
`compile()`** and re-armed each `execute()` (reset `started`/`completed`/`ready`/
`num_locks`/`token` — all scalars, no alloc). It carries no body: `run_graph_node`
reaches it via `graph->nodes_[index].run`, so there is no per-run body closure
either, and nodes dispatch through the **raw scheduler API** (`submit(fn-ptr,
Node*)`), not a heap-allocated closure. Completion runs via a **persistent
`on_complete` fn-ptr** on the block (`graph_node_completed` → `node_complete`) —
*not* a continuation, whose `move_only_function` in the block's `continuations`
vector would allocate per run. Net: a run allocates only its completion handle
(`done`, inherently per-run since the caller may outlive the run) plus, when a
graph object is contended at reserve time, a small reservation closure. The reused
`Run_state` (values reset, vector capacity kept) removes the per-run state alloc
too. Correct because runs are **sequential** (a `get()` barrier between them makes
the previous run quiescent before re-arm) and concurrent runs of one graph are
unsupported (§10 scenario 2). One subtlety the reuse exposed: `done` must be kept
alive by the completing worker across its `settle` (a local `shared_ptr` in
`node_complete`), or the woken `get()` starting the next run — which overwrites
`run.done` and drops its own handle — destroys the block mid-`notify_all` (found
under TSan). The graph is movable (build-and-return, e.g. `build_frame_graph`);
`execute()` refreshes the blocks' `graph` back-pointers, so a moved graph is valid
on its next run.

**Access inheritance.** For nested sub-work to touch the node's *owned* guarded
data (the point of fanning out over it), it must run under the node's grant. A
worker running a nested task otherwise has no `Access_context` → the harness fires.
So `ts::launch` (and thus `ts::nested`) now snapshots the launcher's
`Access_context` **by value** (`detail::snapshot_access()`) and installs it around
the body (`Inherited_access_scope`). By value, not by pointer: the node's context
is a stack local in the body, and body-return ≠ completion — the nested task may
run after the body unwinds, so a pointer would dangle. The copy is bounded to the
node's declared instances, so a nested task touching an *undeclared* object still
faults (completeness hazard preserved). Consistent with the reservation: the
node's objects stay reserved until its block completes (post-nested), and the
nested tasks run under the node's grant on those same objects.

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

**Realized (§7.1).** A graph node runs as an `Executable` block whose graph
post-logic (which releases successors and the object reservation) is a
*continuation* — it fires only at `Completed`, after every nested task. So a
downstream node's `remaining_deps` is not decremented, and the object is not
released, until the node's nested writers are done. The nested writers inherit the
node's grant by value, so they run under the same declared access the reservation
holds. The invariant holds structurally, verified under TSan (`graph nested
stress`).

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

**Mechanism (implemented):** per-node **mode-aware acquire/release**. `pipe_acquire(pipe,
mode, on_acquired)` holds a pipe in `mode` without auto-completing — a `read_only` holder
joins concurrent readers, a `read_write` holder is exclusive; admissible at the front (FIFO
+ reader/writer rules) → acquired synchronously (returns true), else it queues and
`on_acquired` fires when admitted. `pipe_release(pipe, mode)` drops the hold (mode-aware) and
re-dispatches. A node acquires each object it touches before running and releases it at
completion, so an object is held only over each accessor's `[acquire, complete]` window —
free in between. Async on a graph object thus coexists **per node**: it can't overlap a node
holding the object incompatibly (a writer node is exclusive), a read async overlaps a read
node, and any async runs in the gaps. Generalises the old writer-only whole-run `pipe_reserve`.

**Async inline (`pipe_try_inline`).** `Thread_safe::async(fn, {.run_inline = true})` opts
the body into running *synchronously on the calling thread* instead of a worker hop.
`pipe_try_inline(pipe, mode, fn)` is the sibling of `pipe_reserve`'s try-acquire, but
mode-aware and run-then-release: under the pipe mutex it admits the job only if the pipe is
immediately free for that mode — no queued jobs (FIFO preserved) and the reader/writer rules
allow (`read_only` joins as a concurrent reader, `read_write` as an exclusive writer) — then
runs `fn()` inline (the body installs its own access scope), re-locks, releases, `dispatch`es
the remainder, and returns true. Not admissible → returns false and leaves `fn` untouched for
the normal `pipe_enqueue`. Caveats (documented, not enforced): it **blocks the caller** for
the body's duration and **stacks the access scope** on the caller's thread — so opting in from
inside a graph node (or any worker you can't afford to block) is the anti-pattern above.

Acquire is **per node, mode-aware, canonical-order incremental**, so an object is held
only over each accessor's `[acquire, complete]` window — not the whole run:

- **Per object, not up front.** A node runs once **two** gates are open: its data
  prerequisites (`remaining_deps`) *and* all its objects acquired. When it becomes data-ready
  (`on_data_ready`) it walks its objects in ascending pipe-index (canonical) order —
  `acquire_next` holds each: an immediate acquire recurses on synchronously, a contended one
  defers to `pipe_acquire`'s callback (fires when the object frees). Once the last is held it
  runs (`run_node`); at completion (`node_complete`, after any nested sub-work) it releases
  them all (`pipe_release`, mode-aware). No per-pipe run-level bookkeeping — each node owns
  its own acquire/release.
- **Gaps are free.** An object touched by an early node and a late node with a gap node
  between is released after the early node and re-acquired only at the late node — free
  during the gap. (The old `[first accessor, last accessor]` reservation held it continuously
  across the gap; a late-touched object like `audio` was likewise held from the first
  accessor. Now both are free until an accessor actually runs.)
- **Mode-aware.** A reader node holds its object as a reader, so two reader nodes — or a
  reader node and an async reader — overlap; a writer node is exclusive. (The old reservation
  was writer-only, blocking even a read async for the whole run.)

**Deadlock-freedom** comes from **canonical order** — a node acquires objects in ascending
pipe-index order, holding as it goes (the classic ordered-acquisition result), not from
atomicity. In the graph it's belt-and-suspenders: conflict edges serialise every conflicting
node pair, so no two *concurrent* nodes ever contend an object, and single-object async holds
one object and waits for none — no wait-cycle can form regardless. Canonical order becomes
load-bearing only once multi-object `async` (a second class of multi-object acquirer) lands;
it's baked in now (pipe index = canonical id) for that.

Note there is **no** class of objects that async can't reach: `async()` is public on
every `Thread_safe`, so any graph object is potentially async-reachable — you can't
statically skip reservation for "async-free" objects. A per-object user assertion
("graph-exclusive, never async'd during a run") could skip it, but that trades the
safety guarantee for ~2 mutex ops per object per run (the reservation cost on an idle
pipe) and reintroduces the silent race if violated — not worth it. If the overhead
ever matters, make the idle-pipe reserve lock-free rather than skipping it.

### Scenarios where the model can still break

Reservation + single-resource async closes the common race, but these remain sharp:

1. **Blocking on a same-object async inside a node** → deadlock: the node holds the
   object and `.get()`s an async on it, which is queued behind that very hold. (Reinforces
   "never block inside a node.")
2. **Two runs over overlapping objects, concurrently** → not supported: the reused
   `Run_state` is single-run (one `execute()` at a time), so a second concurrent run would
   corrupt it. (Per-node acquire is canonical-order, so it would *not* deadlock — the block
   is the shared run state, not the acquire order.)
3. **Nested `execute()` on an object a node holds** → the inner run's node waits for the
   object, held by the outer node that triggered the inner run → deadlock.
4. **Incomplete access declaration** → a node touching `Y` but declaring only `X`
   doesn't acquire `Y`; async to `Y` races the node's undeclared `Y` access. The
   protection is only as complete as the declaration.
5. **Escaped reference** → a raw pointer into `X` used with no `Access_context`
   bypasses both the acquire and the harness. The classic completeness hazard.
6. **Ordering / latency (not races):** whether the graph observes a pre-run async's
   write depends on enqueue timing (nondeterministic).

Contract, therefore: **one `execute()` at a time, no blocking on same-object async inside a
node, and complete access declarations.**

---

## 11. Cancellation

Cooperative, no exceptions. `Cancellation_source` owns an `atomic<bool>` flag; its
`token()` is handed to `async` / `then` / `execute`. **Cancellation is a completion
state**, not a separate channel: a settled task is either completed or cancelled.

- **Checked when work is about to run.** `async`'s body and each graph `run_node`
  check the token first; if cancelled, the body is **skipped** and the block settles
  as cancelled. Already-running work is *not* interrupted. So cancellation skips
  *not-yet-started* work.
- **Body-level early-out (opt-in).** A task body may take the token so it can poll and
  early-out for cancellation that arrives *while it runs* (the pre-run skip only covers
  work that hasn't started). Declare a trailing `Cancellation_token` parameter —
  `ts::launch([](Cancellation_token t){ ... if (t.is_cancel_requested()) return; ... })`,
  `[](T& v, Cancellation_token t)` for `async`, or on a `then` continuation in any shape
  (void / whole-result / apply-style). `Executable::run` forwards the block's token when
  the wrapped body accepts it; a token-taking body isn't invocable at the shorter arity, so
  the result-type traits (`Task_result_t` for bare tasks, `Async_result_t`/`Async_accessor`
  for `async`, `Apply_invocable_tok`/`Invoke_result_tok` for `then`) pick the result off the
  token-arity overload. A `then` body gets the continuation's own token (`opts.token`); the
  `chain` body always takes a token and forwards it to `produce`, which passes it to `fn`
  only when `fn` declared it. A cooperative early-out *returns normally*, so the task settles
  **completed** (with whatever partial result), not cancelled — the token being set doesn't
  auto-cancel a running task.
- **Propagates automatically.** Continuations carry the outcome — `void(bool
  cancelled)` for `void`, `void(R*)` (nullptr = cancelled) for a value. `complete`
  fires them with the result; `cancel` fires them with the cancel signal, and each
  continuation's closure then cancels *its* subsequent. So cancelling one task
  cancels the whole downstream chain, and a cancelled prerequisite cancels its
  continuations even if they weren't given the token. `then(fn, {.token = ...})` also
  lets the token cancel *at that link* even when the producer succeeded.
- **Graph:** `execute(scheduler, token)` — pending nodes skip (the DAG still drains so
  the run settles), and the completion `Task<void>` is cancelled. In-flight nodes
  finish.
- **`get()`:** a cancelled `void` `get()` unblocks (query `is_cancelled()`); a
  cancelled *value* `get()` is **fatal** — there is no result, so check
  `is_cancelled()` first. (No exceptions, so this is a precondition, not a throw.)

The token flag is atomic and `settle` is idempotent under the block's mutex, so
`request_cancel` racing a body's check or a completion is race-free (verified under
TSan): the block settles exactly once, either way.

- **`when_all`:** if any prerequisite settles cancelled, the join settles **cancelled**
  (it can't form a complete tuple) rather than stalling. The join attaches to each
  prerequisite *directly* (not via `.then`, which skips its continuation on cancellation
  and would leave the counter stuck): on cancel it flags the join and still decrements,
  so the last prerequisite to settle runs `finish`, which cancels the result. Downstream
  `.then` off the join propagates it.

- **Cancel callback (push).** `Cancel_callback(token, fn)` (RAII, `std::stop_callback`-
  style) registers `fn`; `request_cancel()` fires all registered callbacks synchronously
  on the requesting thread. For work that *blocks* rather than polls the token — wake a
  condition variable, abort an I/O — it is the push complement to the pull check. If
  cancellation was already requested at registration, `fn` runs in the constructor; the
  destructor deregisters, and waits out a callback firing on another thread (so `fn`'s
  captures stay valid) except for a re-entrant self-destroy (detaches, to avoid deadlock).
  The source/token/callback share a `Cancel_state` (atomic flag for the lock-free
  `is_cancel_requested`, mutex-guarded list for the callbacks).

---

## Open items

- **Done:** `Task_state → Task_control_block` rename; idempotent `complete()`;
  `Signal` (bodyless triggerable `Task<void>`); graph↔async pipe reservation, lazy on
  acquire + early on release — window `[first accessor, last accessor]` (§10);
  cooperative cancellation (§11); the **monomorphic block** + composed result/body
  (§2) — body in an `Executable<Body,R>` reached via `core.execute`; **standalone
  `ts::launch(fn)`**; the **lock-counter (pre-execution half)** — `num_locks` +
  `successors` on the block, with dynamic prerequisites via
  `ts::task(fn).after(x,y).launch()` (a settled prerequisite releases the dependent; a
  *cancelled* one propagates — marks `prereq_cancelled` so the dependent settles cancelled
  rather than running, uniform with `then`). `submit_ready` bridges the counter
  to the scheduler. The **post-execution half** too (§7): the `execution_flag` mode
  bit — a running body sets flag + a self-lock; `ts::nested(fn)` / `ts::add_nested(t)`
  (via `current_task` TLS) add completion-locks; the parent completes only once its
  self-lock and all nested tasks have released. **Retraction** (§6): a blocking
  `get()` on a *retractable* (bare-scheduler), ready, not-yet-started task runs it
  **inline** on the waiting thread — the `started` claim (`exchange`) ensures a worker
  and a retractor never both run it. **Deep**: `get()` on a *dependent* (a builder task
  with prerequisites) walks its `prerequisites` (backward links) recursively, runs the
  un-started subtree inline, then the dependent — so waiting on a join, not just a leaf,
  is deadlock-free. This breaks the oversubscription deadlock (nested fork-join where
  parents block all workers while children queue). Non-retractable prerequisites (pipe
  tasks, externally-triggered `Signal`s) are left to complete on their own.
  **Nested tasks inside graph nodes** (§7.1): a node runs as an `Executable` block
  (so `current_task` is set and completion gates on nested tasks), its graph
  post-logic a continuation firing at `Completed`; `ts::launch`/`nested` inherit the
  launcher's `Access_context` by value, so nested sub-work may touch the node's owned
  guarded data. Realizes the §8 invariant structurally.
  **Inline dispatch** — `ts::task(fn).set_inline().after(...)` runs a ready task on the
  thread that settled its last prerequisite, not the queue (latency-sensitive dependents).
  The dispatch forks in `release` at `num_locks == 0` (`dispatch_ready`): inline → run
  here, else `submit_ready`. A **per-thread FIFO trampoline** (`inline_pending` vector +
  head index; `clear()` retains capacity) makes a chain of inline tasks run iteratively —
  O(1) stack instead of `settle → release → execute → settle …` recursion. Caveats
  (documented, not enforced): runs on a nondeterministic / possibly external thread,
  bypasses priority, must not block. `run_inline` is a packed bit in `Flags`
  (with `priority`/`retractable`). `then` takes the same knobs via a `Continuation_options`
  aggregate (`t.then(fn, {.priority = high})`, `{.run_inline = true}`, `{.token = ...}`) —
  `chain` sets the continuation's `flags.priority`/`flags.run_inline` before `release`, so
  a `then` dispatches inline / at a priority exactly as a builder task does. Graph/async
  inline still pending.
  **Reusable tasks** — `Task_control_block::reset()` re-arms a settled block in place
  (monomorphic, scalars only: reuse is a *block* capability, so no new `<R>` type). The
  existing `Task_builder<R>` (from `ts::task(fn)`) is the reusable handle — it already
  retains the block after `launch()`, now also has `reset()`/`get()`/`is_done()`:
  `t.reset().after(x).launch(); r = t.get();` re-runs one block/body/result-storage
  (prereqs re-established each run; one run in flight; `reset()` guarded by the block's
  `ready` flag). `Signal::reset()` gives a reusable phase gate. Subtlety: retraction
  leaves a *duplicate* dispatch (it runs the body inline while `release` also queued
  one), harmlessly deduped by the one-runner claim — but reuse re-arms the block, so a
  leftover duplicate could re-run the body against the *next* run. Fixed by fusing the
  claim and the reuse generation into **one atomic** (`run_state`: `[63:1]` generation,
  bit 0 claimed): a dispatch captures the generation it was queued for and `claim(gen)`s;
  a stale duplicate (generation bumped by `reset`) fails the CAS. Two separate atomics
  would race — a stale dispatch could observe the old generation but the new unclaimed
  bit and wrongly run (caught under TSan on the reuse+prerequisites+retraction stress).
- **Compile-time grouping:** schedule an object's accessors close together to shrink
  its reservation window (fewer interior gaps), where the DAG allows — trades against
  parallelism / critical path, so profiling-guided.
- Reservation follow-ups: cheaper idle-pipe reserve (lock-free flag vs mutex) if the
  per-object mutex cost matters; detect nested/concurrent-run reservation deadlock
  (§10 scenarios 2–3) instead of hanging.
- **Re-base the graph and `when_all` onto the block's lock-counter** — the graph's
  `remaining_deps`/`Node.successors` and `when_all`'s counter are now the *same*
  mechanism as `num_locks`/`successors`; fold them in so there's one implementation
  (the graph↔dynamic unification, realized). *Partial:* graph nodes now run as
  `Executable` blocks for execution/nesting/completion (§7.1), but scheduling still
  uses `remaining_deps` + the lazy reservation (`remaining_objects`) rather than
  `num_locks` — the prerequisite half is deliberately *not* folded in, because the
  lazy reservation needs a separate data-ready signal. Folding the scheduling half
  (and letting graph nodes be `after`/`then` prerequisites, and be retractable) is the
  remaining work.
- **Reuse graph node blocks across runs:** **done** (§7.1) — node blocks and the
  `Run_state` are built at `compile()` and re-armed each `execute()`, nodes dispatch
  via the raw scheduler API, and completion uses the block's persistent `on_complete`
  fn-ptr instead of a per-run continuation. A run now allocates only its `done` handle
  (+ a reservation closure only when an object is contended). ~19% faster on the
  8-node `graph` benchmark vs the per-run-block version.
- **Retraction of pipe/async tasks** — today only bare-scheduler tasks (`launch`/
  `task`/`nested`) are `retractable`. It is *not fundamental*: the pipe's completion
  bookkeeping (`--active_readers`, `dispatch`, `notify`) rides the `submit_job` **wrapper**,
  which runs independently of whether the body was claimed — so a body run inline by
  retraction wouldn't corrupt pipe state (the wrapper still does the accounting; the
  body just no-ops its claim). The real blocker is *admission ordering*: retraction can
  reach a block whose pipe job is still **queued behind a conflicting writer** (not yet
  admitted) and run it inline out of turn — racing that writer. Making async retractable
  therefore means gating retractability on **pipe admission** (e.g. set `retractable`
  in `submit_job`, when the pipe has granted the turn), so retraction only ever runs a
  task the pipe already cleared. Bounded plumbing, and only helps *admitted* jobs (a job
  still in the pipe deque isn't a scheduler task and is unreachable by retraction anyway).
  Parked until async-retraction is a real need.
- **`then` rebased onto proper tasks. Done.** A `then` continuation is no longer an
  inline callback in the producer's `settle`; it's a real `Executable` with the producer
  as a `num_locks` **prerequisite** — queued by default (so it carries a priority and the
  scheduler can interleave), inline opt-in. Its body reads `producer->result_ptr` (the
  producer is kept alive as the prerequisite + the body's capture). A cancelled prerequisite
  still `release`s → dispatches the continuation, but sets the block's `prereq_cancelled`
  flag, so `Executable::run` cancels instead of consuming a missing result — the *same*
  cancel-propagation `after` now uses (no `then`-specific mechanism). Deep-retractable
  *natively* now (the producer is in `prerequisites`, so `retract`
  walks to it and — after it settles, `num_locks == 0` — runs the continuation inline),
  which replaced the earlier retraction-hint hack for `then`. Subtlety found in the build:
  the continuation must have `num_locks == 1` until the producer settles — with `num_locks
  == 0` (hint-only), `retract` ran it *before* the producer, reading a null `result_ptr`.
  `when_all`'s **join** stays a bodyless aggregator + retraction hint; its **consumption**
  (the `.then` off it) is this rebased path.
- Nested tasks + `current_task` TLS; re-base `parallel_for`'s rich mode on them.
- Group latch tier for the default `parallel_for`.
- Pooled block allocator + the one microbenchmark.
