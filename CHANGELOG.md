# Changelog

Notable changes per version.

## 0.1.0 — 2026-08-27

The library goes public as 0.1.0 (author, 2026-08): nothing was ever tagged, so the
pre-transformation API shape has no released version to be distinguished from. What
follows is the state 0.1.0 ships as, not a delta against a published predecessor.

Two large transformations, developed together on the `pipe-rebase` branch and merged as a
unit. Both change public API. Designs of record: [docs/coroutine-first.md](docs/coroutine-first.md)
and [docs/pipe-rebase.md](docs/pipe-rebase.md) §0.

### Coroutine-first: composition is `co_await`

**Removed** (all now compile errors):

- `Task<R>::then(...)` — replace with `co_await t;` and the next statement.
- `ts::when_all(...)` — replace with sequential `co_await`s (tasks already run eagerly, so
  the last await gates).
- The `ts::task(...)` builder, `Task_builder`, `.after()`, `.set_inline()`, `.launch()`,
  and `Task_options` — dynamic ordering is now code: `co_await x; co_await y;`.
- Reusable executable tasks (`Task_builder::reset()`). `Signal::reset()` stays.
- Retraction, deep retraction, retraction hints, and the claim/generation reuse machinery.
  A wait is now a suspension, so the pool-exhaustion deadlock it existed to break cannot
  form.
- The dynamic inline-dispatch surface (`run_inline` on task options). `Graph_node::set_inline`
  and the underlying trampoline remain as a graph-internal mechanism.
- `ts::nested`, `ts::Task_scope`, and `co_await ts::join_nested()` — the scoped-launch verbs.
  A nested/scope child runs concurrently with its parent while inheriting the parent's access
  grant, so the two can race on shared mutable state and the declaration-based harness cannot
  see it. Fan out over a holder's data with `ts::parallel_for` (grant-inheriting helpers,
  synchronous join) or acquire fresh via `obj.async` / `co_await obj.access`; the field's safe
  patterns (read-only inheritance, parent relinquishes, child re-acquires) are covered by
  `Versioned`, node-splitting, and the access verbs. `detail::add_nested` survives internally
  as the completion-gating for coroutine graph nodes and nested graph runs. See
  [docs/coroutine-first.md](docs/coroutine-first.md) §4.3.

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
- Coroutine graph node bodies: a node body returning `ts::Task<void>` completes when the
  frame completes (gated through the internal `detail::add_nested`), so grants are held across
  suspensions.
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

### Attended access: `Access_op`

- `Guarded::access` returns the caller-owned `ts::Access_op<T, Body>` instead of a heap-backed
  task handle: the operation's whole state — completion core, result storage, body, pipe
  entry — lives in the returned object, so an access **allocates nothing**. `async` remains the
  detached verb and keeps returning a `Task<R>`.
- Consume the result exactly once: `co_await op`; `op.sync()` — ref-qualified, `&` is a
  non-consuming `const R&` peek, `&&` returns `R` by value so the temporary form
  `obj.access(fn).sync()` stays dangle-free; `take()` — the explicit consuming move; or the
  non-blocking, cancellation-tolerant `try_take()`. Destroying an unsettled op is a reported
  bug (`TS_ENSURE`) that then blocks until the access settles.
- Deferred spellings for members: an unbound op (`Access_op()`, then `bind()` + `start()`) and
  a bound-but-dormant one (`Access_op(ts::dormant, obj, body)`); `start()` also refires a
  settled op with the same storage — a zero-alloc steady state.
- `Access_options{.queued}`: attended but never inline, for a heavy body whose result the
  caller still stays for. Skips only the inline-when-free arm; a lent object stays inline
  regardless (correctness, not opportunism).
- The multi-object verb returns the same handle: `ts::access(fn, a, b)` yields
  `ts::Access_op<A, B, Body>`, not a heap `Task<R>`. The type is variadic, objects first and
  body last, so `Access_op<T, Body>` still names exactly the single-object op it always did.
  One consume vocabulary, one dispatch policy, and no allocation at any arity. `ts::async`
  is unchanged - it remains the detached verb at every arity.
  See [docs/multi-access-op-design.md](docs/multi-access-op-design.md).
- Dispatch, at every arity: objects the calling task **already holds** are *lent* - no turn is
  taken on them, because the access runs inside the caller's grant window, which is already the
  exclusion those objects need (the same protocol a nested `graph.execute()` uses; at one
  object it is what the reentrant arm was). The remaining pipes are then probed **all or
  nothing** - every one free right now, or none admitted and the whole set enqueues through the
  canonical cascade. A queued entry is never jumped, on any object.
- A calling task holding only a *read* grant on an object the body **writes** is fatal in
  checked builds: a read grant cannot be lent to a writer, and enqueueing would wait behind the
  caller's own hold. Same rule, and same message shape, as a nested graph run.
- `Access_options::name` / `Launch_options::name` are now `ts::Named` rather than `const char*`.
  A literal still works; `{.name = ts::Named{} }` captures the call site, which is the only way
  a multi-object verb can be identified by site (an object pack cannot be followed by a
  defaulted `source_location`).

### Also in 0.1.0

- Debug identity is required at construction: `Guarded`/`Versioned` take a leading `ts::Named`
  (a literal, or `ts::Named{}` to capture the construction site), and `add_node` takes a
  leading name the same way — it is what every diagnostic, DOT tooltip and trace row prints.
- `ts::Event_bus` (public header, in the `ts.h` umbrella): per-domain pub/sub over
  `Guarded`+`Deferred` — grant-free staged `publish`/`subscribe`, a `dispatch_fn()` delivery
  node, pinned subscriptions with auto-disconnect on owner death.
- The waiting-rule policy (`ts/rules.h`): the coroutine-first waiting rules as compile-selectable
  runtime checks (`TS_ENABLED_RULES` / `ts::Rule`), scoped opt-out via `ts::Relaxed_scope`,
  declared lock ranks (`ts::Rank`), and the three-tier deadlock report.
- Build-once is literal: `add_node`/`after`/`before` after `compile()`, and a second
  `compile()`, are fatal — structural change means a new graph.

### Internals

- `Task_control_block` shrank 320 → 248 bytes (the `prerequisites` vector went with
  retraction; `successors` collapsed to a single `nested_parent` slot; the generation/reuse
  substrate retired with reuse itself).
- Resumption runs on a bounded thread-local trampoline on the settling/granting thread — no
  queue hop, iterative so the stack stays O(1).

### Samples and measurement

- `sample/game_frame.cpp` gained a **third composition of the same frame**,
  `run_frame_graph_free` — the ~34 systems as multi-object `ts::async` calls with every
  derived edge written out as a `co_await`, no `Static_task_graph`. It answers "what does
  the graph buy" with a measurement ([docs/guide.md](docs/guide.md) §6.4): safety is the
  pipe's either way, order is the graph's (69 derived edges become 17 chain coroutines and
  42 awaits, and pipe FIFO does *not* stand in for the conflict edges), and the runtime gap
  is coroutine resume round trips (+1.6% on a 4.1 ms frame, +28.7% on a 0.45 ms one), not
  the +95 allocations per frame.
- New `--bench` section (`graph 1.0` / `free 1.0` / `graph .05` / `free .05`, µs/frame) and
  `--memprofile` entries (`frame graph` / `frame graph-free`, allocations per frame).

## Pre-0.1.0 — never tagged

The pre-coroutine state: the callback composition vocabulary (`then` / `when_all` / task
builders / `after`), retraction, reusable tasks, and the `std::deque`-based pipe. Described
by the docs as they stood before the transformations above; kept here as the reference point
the removals are measured against.
