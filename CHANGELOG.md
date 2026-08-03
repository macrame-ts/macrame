# Changelog

Notable changes per version. Dates are the version's completion, not a release date —
nothing has been tagged or published yet.

## 0.2.0 — unreleased (2026-08)

Two large transformations, developed together on the `pipe-rebase` branch and merged as a
unit. Both change public API. Designs of record: [docs/coroutine-first.md](docs/coroutine-first.md)
and [docs/pipe-rebase.md](docs/pipe-rebase.md) §0.

### Coroutine-first: composition is `co_await`

**Removed** (all now compile errors):

- `Task<R>::then(...)` — replace with `co_await t;` and the next statement.
- `ts::when_all(...)` — replace with sequential `co_await`s (tasks already run eagerly, so
  the last await gates), or `co_await ts::join_nested()` / `co_await scope.join()`.
- The `ts::task(...)` builder, `Task_builder`, `.after()`, `.set_inline()`, `.launch()`,
  and `Task_options` — dynamic ordering is now code: `co_await x; co_await y;`.
- Reusable executable tasks (`Task_builder::reset()`). `Signal::reset()` stays.
- Retraction, deep retraction, retraction hints, and the claim/generation reuse machinery.
  A wait is now a suspension, so the pool-exhaustion deadlock it existed to break cannot
  form.
- The dynamic inline-dispatch surface (`run_inline` on task options). `Graph_node::set_inline`
  and the underlying trampoline remain as a graph-internal mechanism.

**Changed:**

- `sync()` / `take()` are **boundary-only**. Calling them inside a task on unsettled work is
  fatal under `TS_SAFETY_CHECKS` (it was previously a retract-or-park with a warning). Inside
  a task, `co_await`. `parallel_for`'s internal join is the one structural exemption.
- Coroutines are **mandatory**, not an optional layer; the `__cpp_impl_coroutine` guards are
  gone.
- Awaiting a cancelled value task is fatal (check `is_cancelled()` first); awaiting a
  cancelled `void` task resumes normally.

**Added:**

- Frame/block fusion: a coroutine task's promise embeds its control block, so it is **one**
  allocation, not two.
- Implicit per-frame/per-node scopes, with `ts::nested` as the scoped-launch verb and
  `co_await ts::join_nested()` for a mid-body join. `ts::Task_scope` (`ts/task_scope.h`) is
  the explicit nursery; destroying one with unjoined children is fatal.
- Coroutine graph node bodies: a node body returning `ts::Task<void>` completes when the
  frame completes, so grants are held across suspensions.
- Awaitable held-grant guards: `auto g = co_await ts::read_write(obj)` / `ts::read_only(obj)`.
  `co_await`ing anything while a guard is live is fatal.
- A waits-for cycle detector for the suspended-ABBA deadlock (two frames each holding a grant
  and awaiting the other's object — no thread parks, so nothing else can see it).
- Nested graph runs: `co_await inner.execute()` from a node, with the caller's grants **lent**
  to the inner run rather than re-acquired. Un-awaited inner runs join the caller's scope;
  `execute({.detach = true})` opts out of both. Three fatals guard the cases where the
  containment argument fails.
- `ts::Frame_gate` (`ts/frame_gate.h`): `co_await gate.next()` parks cross-frame work until
  the frame loop's next `open()`.

### The evolved pipe

- The per-object pipe is rebuilt: one mutex, an intrusive FIFO of embedded `Pipe_link`s (no
  `Job` type, no `std::deque`, no per-entry closures), pipe turns as `num_locks`
  prerequisites, and one canonical pipe-address-ordered cascade serving both dynamic
  multi-object `async` and graph nodes. `Multi_async_state`, `acquire_next`, `preheld`, and
  the explicit graph write handoff are gone.
- **`Deferred::commit_async()` is removed.** `commit()` is now one auto-dispatching verb: it
  applies inline when the calling task holds the target's write grant, and enqueues an
  ordinary async write otherwise. Committing from sub-work running under a parent's
  *inherited* grant is fatal (call it from the grant holder).
- `Guarded::access` gains a reentrant arm: an access from the task that already holds the
  object's write grant runs under that grant instead of queueing behind it.
- A second `execute()` on a graph whose previous run is still in flight is fatal. It was
  previously unguarded and corrupted the shared run state. Concurrent runs of *different*
  graphs over shared objects are supported (validated), with nondeterministic cross-graph
  ordering.

### Internals

- `Task_control_block` shrank 320 → 264 bytes (the `prerequisites` vector went with
  retraction; `successors` collapsed to a single `nested_parent` slot).
- Resumption runs on a bounded thread-local trampoline on the settling/granting thread — no
  queue hop, iterative so the stack stays O(1).

## 0.1.0 — never tagged

The pre-coroutine state: the callback composition vocabulary (`then` / `when_all` / task
builders / `after`), retraction, reusable tasks, and the `std::deque`-based pipe. Described
by the docs as they stood before 0.2.0; kept here as the reference point the removals above
are measured against.
