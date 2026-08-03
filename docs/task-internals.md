# Task internals — structure, lifecycle, allocation

(Start with [guide.md](guide.md) for usage and [design.md](design.md) for the
rationale narrative; this document is the deep design-of-record.)

Design of record for the dynamic task object: what a task *is*, the states it
moves through, how pipe turns / nested tasks / coroutine frames work, and how
it is allocated. Distilled from the design discussion; grounded in the Unreal
Engine Tasks system (`Engine/Source/Runtime/Core/.../Tasks/TaskPrivate.{h,cpp}`),
with the differences that matter for *our* access-safety model called out.

**Status:** implemented, coroutine-first (docs/coroutine-first.md). Composition
is `co_await`; the callback vocabulary this document once specified (`then`,
`when_all`, builders, retraction, dynamic-task inline dispatch) was built,
validated, and then deleted in the coroutine-first transformation — those
sections remain below as historical record, marked as such.

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
| Task | `Task_control_block` (+ closure for executable) | 1 alloc | `async`, `launch`, coroutine frames, graph |

There is deliberately **no middle "awaitable-but-no-deps" tier.** You cannot know
at construction whether a task will later be awaited, chained, or made a nested
parent, so a lighter type would have to be upgraded on first use — the racy
late-install. Instead the single Task tier is made *cheap when its features are
unused* (empty dependency lists cost no heap; the lock-counter doubles as the
completion flag; SBO closure), and `submit` covers the "nothing needed" case.
Users extend by writing their own coordinator over `submit`, exactly as the
`Guarded` pipe, `Static_task_graph`, and `parallel_for` do — not by
inheriting a task base class.

---

## 2. Object model

Three types on the Task tier; two private, two public handles (one shared).

- **`Task_control_block`** (private) — **fully monomorphic** (not templated at all):
  the refcounted completion/dependency core — completion signal, `ready`/`cancelled`,
  the gating parent, the lock-counter — plus two type-erased hooks: `void* result_ptr`
  (nullptr ⇒ no result: `void` or bodyless) and
  `void (*execute)(const Task_ptr&, uint64)` (nullptr ⇒ no body: bodyless). Internal
  continuations are stored erased too
  (`move_only_function<void(void* result, bool cancelled)>` — the coroutine awaiter's
  resume hook and detail-level chains attach here). A third fn-ptr, `on_complete`
  (nullptr for most tasks), fires once at `settle` and — unlike a continuation — is
  **not consumed**, so a re-armed block (a graph node, §7.1) keeps it across runs: an
  alloc-free completion hook. All the heavy logic is compiled **once**, regardless of
  how many result/body types exist. The dependency machinery only ever traffics in
  `Task_control_block*`.

- **Storage wrappers** (private, per instantiation) — everything that carries a type
  is composed *around* the block, not into it:
  - `Result_block<R>  { Task_control_block core; std::optional<R> result; }` — for
    results with no body (detail-level producers).
  - `Executable<Body,R> { Task_control_block core; Result_storage<R> storage; Body body; }`
    — for `async` / `launch`.
  - `Task_promise<R>` — the **fused coroutine frame**: the promise embeds the block
    (first member) inside the compiler-allocated frame, so frame + block + result are
    one allocation and the block's `destroy` thunk destroys the whole frame.
  - a bodyless resultless block (`Signal`) is just `Task_control_block`.

  Ownership is **intrusive**: the refcount and a `destroy` thunk live in the block
  (`Task_ptr` is one pointer, half a `shared_ptr` handle), and the thunk deletes the
  enclosing wrapper — the block is the wrapper's first member, so a
  `Task_control_block*` aliases it. `core.result_ptr` points at the wrapper's stored
  result (or null); `core.execute` points at a per-`Body` thunk that casts the block
  back to the wrapper and runs the body.

- **`Task<R>`** (public) — the consumer handle: `co_await` (from tasks), `sync()` /
  `take()` (from blue threads), `is_done()`/`is_cancelled()`. `sync()` reads
  `static_cast<const R*>(core->result_ptr)` by `const&` — the cast is always to the
  right `R` (storage + handle are created together at one site, both on the same `R`;
  confined to `detail`).

- **`Signal`** (public) — a bodyless `Task<void>` completed by hand: `trigger()`.
  Consumer side inherited from `Task<void>` (`co_await`/`sync`, `is_done`);
  `trigger()` is the producer side, idempotent. UE's `FTaskEvent`: a done-signal /
  barrier / phase gate. Allocates just the bare block (no result, no body).

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
  Destroy is the block's `destroy` thunk (one per wrapper type, unavoidable).
- **Per-instantiation codegen** collapses to the wrapper struct + its `execute` thunk
  (~1–2 functions per body/result type) — no `R` on the block, so the heavy machinery
  (lock-counter, close, successor walking, scheduling) is emitted **once**.
- **No `<void>` specialization.** Today two near-identical blocks exist
  (`Task_control_block<R>` and the `<void>` one); monomorphizing collapses them to
  one. `void` = `result_ptr == nullptr`; bodyless = `execute == nullptr`.
- **Bodyless / `void` pay nothing** — no `optional<R>`, no body slot; a `Signal` is
  the bare block.
- One indirect call to run the body (through `execute`) — same cost as a virtual call,
  negligible next to the body.

`R`/`Body` appear only in: the thin `Task<R>` handle, the wrapper structs, the
`execute` thunk, and the awaiter (which casts `result_ptr`). Everything else
is monomorphic.

### The body / no-body seam (and why the *body* still can't be split from an executable)

The body is composed into the *executable's* wrapper, reachable from the block via
`execute`. For an *executable* task the body and the block stay in one allocation
(the wrapper): any handle to the block keeps the not-yet-run body alive, so no
scheduler-side owner can free the closure out from under a dispatch that still
reaches it through `execute` + the cast. (Historically this single-blob rule was
*forced* by retraction — a waiter running the body inline through its own handle —
see §6; the lifetime argument outlives the feature.) A bodyless block has no body
and is never executed, so nothing reaches for a body that isn't there —
composition-in-one-wrapper keeps the invariant while costing bodyless blocks
nothing.

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

- **Created** — mutable; creation sites set dispatch properties here. `async`
  and `ts::launch` are born launched; a coroutine task is born *running* (eager
  start — the body executes on the calling thread to its first suspension).
- **Pending** — launched, waiting on execution prerequisites (pipe turns).
- **Queued** — prerequisites met; owned by the scheduler.
- **Running** — a worker holds execution permission; body runs. A coroutine
  body may suspend and resume across threads; the task stays logically Running
  (no worker is held while suspended).
- **Awaiting-nested** — body returned but nested tasks are still outstanding.
  **Body-return ≠ completion.** Most tasks skip this state.
- **Completed** — result published, waiters released, successors unlocked. This
  is what downstream edges key off — never body-return.

---

## 4. The unified lock-counter

One `std::atomic<uint32> num_locks` tracks *both* pre-execution blockers and
post-execution blockers (nested tasks), distinguished by a high-bit mode flag.
This is the mechanism that makes prerequisites and nested tasks the same thing
seen across the execution boundary — and, coroutine-first, it doubles as the
**implicit per-frame scope's counter**: children gate the frame's completion
through the same locks.

```
execution_flag = 0x8000'0000        // MSB; set at execution start

pipe task        : num_locks = pipe_count        // one lock per pipe turn
turn arrives     : --num_locks; if 0 -> dispatch

execute (claim)  : claim(gen) on run_state       // one-runner CAS; then
                   num_locks = execution_flag+1  // executing + body self-lock
coroutine birth  : num_locks = execution_flag+1  // the promise arms it directly
                                                 // (born executing, eager start)
add nested       : ++num_locks                   // during body; above the flag
body returns     : n = --num_locks; if n == execution_flag -> Close()
nested completes : n = --num_locks; if n == execution_flag -> Close()
```

- Below `execution_flag`, the count is outstanding pipe turns (the pipe rebase's
  `pipe_count` trigger); reaching 0 dispatches. The one-runner claim moved off
  this counter onto `run_state` (generation + claim bit fused in one atomic);
  with one dispatch per run and re-arm only after settle, a failed claim is a
  machinery bug (fatal under `TS_SAFETY_CHECKS`), not a dedup path.
- The `+1` self-lock ensures a nested task completing mid-body cannot `Close`
  the parent before the body finishes. A coroutine promise arms
  `execution_flag + 1` at construction (the body starts eagerly, so the frame is
  executing from birth); the final awaiter drops the self-lock at `co_return`.
- Above `execution_flag`, the count is outstanding nested tasks; the last one to
  hit `execution_flag` closes the parent. This above-the-flag regime *is* the
  implicit scope of docs/coroutine-first.md §4.3 — evaluated against a separate
  scope object during the coroutine-first deletions and kept: the counter
  already counts exactly the scope's members, and a scope object would add
  state without deleting any transition.
- **Historical (deleted 2026-08, coroutine-first):** the below-flag mode also
  counted explicit prerequisites (`ts::task(fn).after(...)`, `then`'s producer
  link), with a frozen-at-launch invariant enforced at `add_prerequisite`.
  Ordering between dynamic tasks is now expressed by awaiting inside a
  coroutine body; the below-flag mode serves pipe turns only.

(Pipe serialization is handled separately by `Guarded`'s reader/writer pipe,
not this counter; ordering-via-pipe is out of scope here.)

---

## 5. Data lifetime

| Data | Live | Notes |
|---|---|---|
| Closure + captures | Created → body-return | exec-bounded; destroy right after the body to release captures early. A coroutine frame's parameters live to Completed (children may read them) |
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

## 6. Waiting: the blue boundary (historical: retraction)

The pool-exhaustion deadlock — a task blocking on other tasks that need a
thread from the same bounded pool — is a documented, recurring failure in every
comparable system (Taskflow, oneTBB, Rayon, Java `ForkJoinPool`, .NET TPL,
GCD). Coroutine-first answers it by construction:

- **Inside a task, waiting is `co_await`.** The frame suspends, the worker is
  freed, the awaited task's settle resumes the frame (on the settling thread,
  through the bounded resume trampoline). No thread is ever parked on task
  progress, so the deadlock's precondition never forms. Arbitrarily deep
  awaited fork-join drains on any pool size — covered by the deep-await-chain
  and awaited-fork-join tests where the old blocking-join deadlock tests stood.
- **On a blue thread (main, dedicated engine threads), `sync()` parks.** A
  non-worker thread parking is harmless; that is the sanctioned blocking
  consumption point, and the only one.
- **An in-task `sync()` about to park is fatal** under `TS_SAFETY_CHECKS`
  (`sync_wait` → `blocking_sync_diagnose`, with a sharper message when the
  target is queued behind the caller's own grant). `parallel_for`'s join is the
  documented exception: the caller drains unclaimed chunks itself and then
  waits only on helpers provably running on workers — bounded, deadlock-free.

**Historical record — retraction (deleted 2026-08, coroutine-first).** The
previous answer let a blocking waiter *run the awaited work itself*: `sync()`
on an un-started task walked its backward links and executed the un-started
prerequisite subtree inline, claiming each task via a one-runner CAS so a
worker could not double-run it — targeted busy-waiting, distinct from both
horns peers take (park and risk deadlock; or help on *arbitrary* queued work —
TBB `isolate`, Taskflow `corun`, FJP helping — and inherit re-entrancy, TLS,
stack-depth, and priority-inversion hazards). It worked, carried the two
hardest races in the project's history in its reuse corner (design.md §4.5),
and became dead weight the moment in-task blocking stopped being legal: a blue
thread can simply park, and a task cannot block at all. The prior-art survey
remains: [retraction-vs-pool-exhaustion.md](retraction-vs-pool-exhaustion.md).
One structural consequence outlived it: work and completion stay one
refcounted object (the single-blob block layout) — the waiter's handle keeps
the closure alive, which any future inline-execution scheme would need again.

---

## 7. Nested tasks

A task launched deep inside a parent's call stack, whose completion the parent's
completion depends on — equivalent to the parent awaiting it at the end of its
body, but without that being lexically expressible where the nested task is
spawned.

- The parent is found via `thread_local Task_ptr current_task`, installed at
  execution start — and, for a coroutine, re-installed around every resumed
  segment (companion to `thread_local Scheduler* current_scheduler` and the
  per-task `Access_context`).
- Registration (`detail::add_nested`, spelled `ts::nested` publicly): the
  nested task `++`s the parent's `num_locks` and records the parent in its
  `nested_parent` slot, so its settle releases the parent; the parent completes only
  once all nested tasks do (§4). In a coroutine segment the child is also
  recorded in the frame's implicit scope list, so `co_await ts::join_nested()`
  can await the children mid-body; `ts::Task_scope` is the explicit-scope
  variant (its own list, `co_await scope.join()`, fatal if dropped unjoined).
- `parallel_for` chunks are a degenerate nested task ("subtasks inherit the
  parent node's context"); the default cheap `parallel_for` stays on the
  group-latch tier (§1).

### 7.1 Nested tasks inside a graph node

A `Static_task_graph` node can spawn nested tasks (`ts::nested`) — needed for
dynamic, data-dependent fan-out a static `parallel_for` can't express (e.g. a
physics node discovering N runtime islands and solving them in parallel). This
required making a graph node a **real task block**, not a bare closure:
`run_node` builds the node body as an `Executable<Body,void>` and submits
`execute` on the run's scheduler, so `Executable::run` installs `current_task`
and the `execution_flag` self-lock (§4) around the body — exactly the state
`add_nested` needs. The node's graph post-logic (early release, successor
release, run completion) fires only after the body *and* all nested tasks
settle — the §8 invariant, structurally. A **coroutine node body** (returning
`Task<void>`) rides the same mechanism: the returned frame is attached as a
nested child of the node block, so the node completes when the frame completes
— suspension neither completes the node nor releases its grants.

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
too. Correct because runs are **sequential** (a `sync()` barrier between them makes
the previous run quiescent before re-arm) and concurrent runs of one graph are
unsupported (§10 scenario 2). One subtlety the reuse exposed: `done` must be kept
alive by the completing worker across its `settle` (a local `shared_ptr` in
`node_complete`), or the woken `sync()` starting the next run — which overwrites
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

**Realized (§7.1).** A graph node runs as a task block whose graph post-logic
(which releases successors and the object holds) is the block's persistent
`on_complete` hook — it fires only at `Completed`, after every nested task. So
a downstream node's `remaining_deps` is not decremented, and the object is not
released, until the node's nested writers are done. The nested writers inherit
the node's grant by value, so they run under the same declared access the hold
covers. A coroutine node body is the same invariant through the same
mechanism: the frame is a nested child, so grants span suspensions and release
at frame completion. The invariant holds structurally, verified under TSan
(`graph nested stress`).

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

## 10. Graph ↔ dynamic access coexistence (pipe reservation)

Graph nodes access their objects **directly** (bypassing the pipe), ordered among
themselves by conflict edges. `Guarded` access (`access`/`async`) goes through the
**pipe**. Two independent serializers over one object → a node and an access on the same object
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

**Access inline (`pipe_try_inline`).** `Guarded::access(fn)` (the opportunistic verb;
`async(fn)` always schedules) runs the body *synchronously on the calling thread* instead
of a worker hop when the pipe is free at call time.
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
- **Handoff (elide the round-trip).** A serial chain on one object — node writes X, successor
  writes X — would otherwise release X and immediately re-acquire it. `node_complete` instead
  **hands X directly** to the sole ready successor that takes it in the *same* mode: it skips
  the release, and a per-node `preheld` bitmask (position in the successor's `pipe_indices`)
  makes that successor's `acquire_next` skip the object (no pipe op — the pipe state, a held
  writer or a reader-count slot, is already right for it). Unique for a writer by the conflict
  edges (two writers of one object are ordered, never both ready). The handed object stays held
  across the edge (no gap), which is fine: the successor just went ready and runs immediately.
  Any object *not* handed off is released — the gap-freeing above. The mask is set (before the
  successor is triggered, so single-writer + happens-before via the trigger) and cleared per run.
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

**Inline node dispatch (opt-in, `Graph_node::set_inline`).** The per-node acquire makes the
inline hand-off natural: when a node's last prerequisite completes on some thread, that thread
runs `acquire_next` for the successor. `acquire_next` threads a `synchronous` flag — true while
the chain stays on that thread, flipped false the moment any acquire *defers* to a
`pipe_acquire` callback (a contended object). At the end, an inline node that stayed
`synchronous` dispatches through the shared `dispatch_ready` trampoline (its block's
`run_inline` bit is set, so the inline path runs it on this thread, bounded/iterative like the
task inline path); otherwise it goes through the queue (`run_node` on the run's scheduler). So
an inline node runs on the settling thread **only if it can acquire all its objects there and
then** — a contended object (async grabbed it in a gap) defers it to the queue, exactly the
"revoke the predecessor's access, acquire the successor's, else defer" hand-off. A chain of
inline nodes on one object trampolines: each releases the object at completion, the next
acquires it synchronously and dispatches inline. Caveats: it runs on a nondeterministic
thread (the caller for a root), bypasses priority, and must not block; an all-inline
graph runs synchronously on the `execute()` caller.

**Multi-object `ts::access` / `ts::async`** reuses the *same* acquire primitive outside the
graph: the free function `ts::access(fn, objs...)` (or `ts::async`, always scheduled) runs
`fn(*objs...)` over several `Guarded`s at once. It
acquires each object's pipe mode-aware, in canonical (pipe-address) order, holding all
(`multi_acquire`, the acquire chain), runs the body under an `Access_context` declaring every
object, then releases at completion (a continuation `attach`ed to the block). Because the
graph's `distinct_pipes_` is address-sorted, graph nodes and multi-object asyncs acquire in
the **same** canonical order — so the two classes of multi-object acquirer can't deadlock
against each other (this is where canonical order stops being belt-and-suspenders and becomes
load-bearing). A repeated object is deduped write-wins. Options are passed first
(`ts::async({.priority=p}, fn, objs...)`) since a parameter pack can't precede a defaulted
argument; `run_inline` is ignored (multi-object inline is a follow-up).

Note there is **no** class of objects that async can't reach: `async()` is public on
every `Guarded`, so any graph object is potentially async-reachable — you can't
statically skip reservation for "async-free" objects. A per-object user assertion
("graph-exclusive, never async'd during a run") could skip it, but that trades the
safety guarantee for ~2 mutex ops per object per run (the reservation cost on an idle
pipe) and reintroduces the silent race if violated — not worth it. If the overhead
ever matters, make the idle-pipe reserve lock-free rather than skipping it.

### Scenarios where the model can still break

Reservation + single-resource async closes the common race, but these remain sharp:

1. **Blocking on a same-object async inside a node** → the node holds the object and
   `.sync()`s an access queued behind that very hold. Still a deadlock shape, but no longer
   silent: an in-task `sync()` on unsettled work is fatal in checked builds, with a sharp
   same-object message when the target is on a pipe the caller's context holds. (Reinforces
   "never block inside a node" — the rule is now enforced, not advised.)
2. **Two runs of ONE graph, concurrently** → fatal (checked builds). The reused `Run_state`
   is single-run, so a second `execute()` while one is in flight would corrupt it; the check
   at `execute()` entry says so instead of letting it happen. Queued/pipelined runs are
   roadmap (TODO 2.3).
   **Two runs of DIFFERENT graphs over overlapping objects, concurrently → SUPPORTED
   (validated 2026-08).** The old blanket "not supported" conflated this with the case above,
   whose real cause is the per-graph run state, not the acquisition protocol. Each graph's
   nodes take their pipe turns in canonical (pipe-address) order over the same address-sorted
   objects, so no wait cycle can form, and the pipe serializes two graphs' conflicting nodes
   against each other exactly as it serializes a node against an async — the graphs simply do
   not know about each other. Validated with two graphs declaring the same objects in
   *opposite* order, run concurrently while asyncs hammer the same objects, under the
   `Rw_probe` overlap oracle and TSan (`stress_concurrent_graphs`), plus a bounded suite test.
   Cross-graph *ordering* is of course nondeterministic — the schedule is whatever the pipes
   admit — so this is "safe, not deterministic": do not build semantics on which graph's node
   reaches a shared object first.
3. **Nested `execute()` on an object a node holds** → **supported since 2026-08** via the
   lend protocol (§4.8 of coroutine-first.md): the inner run does not re-acquire an object its
   caller already holds a covering grant on, it inherits the exclusion. The residual failure
   modes are fatals, not hangs — a caller holding read where the inner graph writes, and
   lending while the caller has live scope children.
4. **Incomplete access declaration** → a node touching `Y` but declaring only `X`
   doesn't take a turn on `Y`; async to `Y` races the node's undeclared `Y` access. The
   protection is only as complete as the declaration. (Unchanged, and the one item on this
   list with no structural fix: it is the completeness hazard by definition.)
5. **Escaped reference** → a raw pointer into `X` used with no `Access_context`
   bypasses both the acquire and the harness. The classic completeness hazard.
6. **Ordering / latency (not races):** whether the graph observes a pre-run async's
   write depends on enqueue timing (nondeterministic).

Contract, therefore: **one `execute()` at a time, no blocking on same-object async inside a
node, and complete access declarations.**

---

## 11. Cancellation

Cooperative, no exceptions. `Cancellation_source` owns an `atomic<bool>` flag; its
`token()` is handed to `async` / `launch` / `execute`. **Cancellation is a completion
state**, not a separate channel: a settled task is either completed or cancelled.

- **Checked when work is about to run.** `async`'s body and each graph `run_node`
  check the token first; if cancelled, the body is **skipped** and the block settles
  as cancelled. Already-running work is *not* interrupted. So cancellation skips
  *not-yet-started* work.
- **Body-level early-out (opt-in).** A task body may take the token so it can poll and
  early-out for cancellation that arrives *while it runs* (the pre-run skip only covers
  work that hasn't started). Declare a trailing `Cancellation_token` parameter —
  `ts::launch([](Cancellation_token t){ ... if (t.is_cancel_requested()) return; ... })`,
  `[](T& v, Cancellation_token t)` for `async`. `Executable::run` forwards the block's
  token when the wrapped body accepts it; a token-taking body isn't invocable at the
  shorter arity, so the result-type traits (`Task_result_t` for bare tasks,
  `Async_result_t` for `async`) pick the result off the token-arity overload. A
  cooperative early-out *returns normally*, so the task settles **completed** (with
  whatever partial result), not cancelled — the token being set doesn't auto-cancel a
  running task. A coroutine body polls its own token (or the awaited tasks'
  `is_cancelled()`) between awaits and `co_return`s early — same semantics, ordinary
  control flow.
- **Propagates as observed state.** A graph successor of a cancelled node settles
  cancelled (`prereq_cancelled` via `release`); an awaiting coroutine *observes* the
  cancellation — a cancelled `Task<void>` await just resumes, a cancelled value await
  is fatal (check `is_cancelled()` first and branch/`co_return`). The awaiter is code
  in the consumer's body, so what "propagation" should mean is written where it
  matters instead of baked into a chain rule.
- **Graph:** `execute({ .token = t })` — pending nodes skip (the DAG still drains so
  the run settles), and the completion `Task<void>` is cancelled. In-flight nodes
  finish.
- **`sync()`:** a cancelled `void` `sync()` unblocks (query `is_cancelled()`); a
  cancelled *value* `sync()` is **fatal** — there is no result, so check
  `is_cancelled()` first. (No exceptions, so this is a precondition, not a throw.)

The token flag is atomic and `settle` is idempotent under the block's mutex, so
`request_cancel` racing a body's check or a completion is race-free (verified under
TSan): the block settles exactly once, either way.

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

- **Done (current model):** the monomorphic block + composed result/body (§2) with
  intrusive refcounting; standalone `ts::launch(fn)`; `Signal`; the unified
  lock-counter, both halves (§4) — pipe turns below the flag, nested tasks /
  the implicit scope above it; nested tasks inside graph nodes + coroutine node
  bodies (§7.1); graph↔async pipe coexistence via mode-aware acquire (§10);
  cooperative cancellation (§11); the fused coroutine frame (`Task_promise`
  embedding the block); the blue-boundary blocking rule with the in-task fatal
  (§6); the waits-for cycle detector for suspended-ABBA deadlocks
  (docs/coroutine-first.md §2); allocation-free graph re-runs (§7.1, ~19% faster
  on the 8-node `graph` benchmark than per-run blocks).
- **Historical record (built, validated, deleted 2026-08 in coroutine-first):**
  `then` rebased onto proper prerequisite-linked tasks; `when_all` with the
  one-allocation intrusive join; `ts::task(fn).after(...)` builders with
  frozen-at-launch enforcement; builder-handle reusable tasks (the fused
  `run_state` generation+claim atomic remains, simplified to one-dispatch-per-run
  with a machinery-bug assert); deep retraction (+ the never-landed pipe-task
  retraction design — its admission-ordering analysis is preserved in the TODO
  §1.14 addendum); dynamic-task inline dispatch (`set_inline`/`run_inline` —
  the flag and per-thread FIFO trampoline survive as the graph's
  `Graph_node::set_inline` mechanism). Rationale and the deletion argument:
  design.md §4.3–§4.5.
- **Compile-time grouping:** schedule an object's accessors close together to shrink
  its hold window (fewer interior gaps), where the DAG allows — trades against
  parallelism / critical path, so profiling-guided.
- Cheaper idle-pipe admission (a lock-free flag instead of the mutex) if the per-object
  mutex cost ever shows up in a profile. The nested/concurrent-run half of this item is
  **done** (2026-08): the nested case is supported via the lend, the same-graph concurrent
  case is a fatal, and the different-graph concurrent case turned out to be safe all along
  (§10 scenario 2/3) — nothing hangs any more.
- **Fold graph scheduling onto the block's lock-counter** — graph nodes run as task
  blocks for execution/nesting/completion (§7.1), but scheduling still uses
  `remaining_deps` + the lazy reservation (`remaining_objects`) rather than
  `num_locks`; the prerequisite half is deliberately not folded in, because the lazy
  reservation needs a separate data-ready signal. Folding the scheduling half is the
  remaining work.
- Group latch tier for the default `parallel_for`.
- Pooled block allocator + the one microbenchmark.
