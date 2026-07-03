# TODO / Roadmap

Future work, captured as it comes up. Not ordered by priority.

The dynamic-task internal design (three-type model, lifecycle, unified
lock-counter, retraction, nested tasks, access invariant, allocation) is speced
in `docs/task-internals.md` — the items below under "Task types" / "Task chaining"
are the incremental steps toward it.

## Task chaining / results
- Typed prerequisite -> subsequent chaining (dynamic path): **done** — `Task::then` (single) and `when_all` (multi, results as a tuple).
- Apply-style `when_all`: **done** — `then` unpacks a tuple result into separate continuation args (`then([](A& a, B& b){...})`) when the functor doesn't take the tuple by reference; falls back to the tuple form otherwise.
- `when_all` with void prerequisites and move-only results: **done** — void prerequisites act as pure ordering and drop out of the result (all-void → `Task<void>`); non-void results are carried in a tuple and **moved** (move-only supported). The consumer of a prerequisite's result is `when_all` (results move out), consistent with the single-consumer model.
- Typed chaining in `Static_task_graph` (graph nodes are void-only for now): let a node consume prerequisite-node results.
- `Task`: define get()/then() interaction (currently get() consumes the result; mixing with then() is unsupported). Multiple concurrent blocking `get()` waiters: the monomorphic block waits on a condition variable, so any number of waiters wake (a `Signal` is a barrier); a value `get()` still moves the result, so value tasks stay single-consumer by design.
- Cancellation: **done** — `Cancellation_source`/`token`; cooperative, checked when a task/node is about to run (not-yet-started work is skipped). Cancellation is a completion state that propagates down continuations and graph successors (`Task::is_cancelled()`); `async`/`then`/`execute` take an optional token; a cancelled `void` get() unblocks, a cancelled value get() is fatal (check `is_cancelled()`). A cancelled `when_all` prerequisite cancels the join (it can't form a tuple) instead of stalling — **done**. Cancel *callback* (push notification) — **done**: `Cancel_callback(token, fn)`, RAII/`std::stop_callback`-style; `request_cancel()` fires registered callbacks synchronously (for work that blocks rather than polls).

## Task types
- Unify the handle returned from `Static_task_graph::add_node` with regular async `Task`s: **done** — split, not merged. `add_node` now returns a distinct `Graph_node` (build-time ordering identity: `after`/`before`); `Task<R>` is purely a completion handle (`get`/`then`/`is_done`/`when_all`/`execute`). The run-many model means a node has no single result, so it can't be a single-shot `Task`. Revisit when typed graph chaining lands (a `Graph_node` may then mint a per-run `Task<R>`).
- Reusable tasks: **done** — no new type. `Task_control_block::reset()` re-arms a settled block in place (monomorphic; scalars only — reuse is a block capability, not a result-type concern), and the existing `Task_builder<R>` (from `ts::task(fn)`) is the reusable handle: it already retains the block after `launch()`, and now also has `reset()`/`get()`/`is_done()`. Pattern `t.reset().after(x).launch(); r = t.get();` re-runs one block/body/result-storage across runs (prerequisites re-established each run; one run in flight, `reset()` after the prior settled+consumed, guarded by the block's `ready` flag). `Signal::reset()` falls out (a reusable phase gate). Pooling doesn't help — reuse is the point. Timed-task launch is under Scheduler.

## Control block / allocation
- **Keep `Task_control_block` as small as possible; fuse flags into bits.** The one-bit static properties should share a byte, not each cost a `bool`: `run_inline` (planned), `retractable`, and `priority` (2 bits) — all set at creation, and `run_inline`+`priority` are read *together* at dispatch, so fusing also improves locality. Audit the rest for packing (`completed`/`cancelled` live under the mutex; `ready`/the claim/generation are in `run_state`). A smaller block matters — every task allocates one, and they sit on hot paths.
- **Reconsider every `shared_ptr`; prefer intrusive ref-counting.** The block, its `successors`/`prerequisites`/`continuations` vectors, and the public handles are all `std::shared_ptr<Task_control_block>` — each a separate heap control block and an extra atomic per copy. Move to an **intrusive** refcount (a count word in the block itself + a small `Task_ptr` handle): one allocation instead of two, smaller/faster handles, fewer atomics, and the aliasing tricks (`Result_block`/`Executable` wrappers) get simpler. Cross-cutting — do it deliberately, with TSan/ASan coverage.

## Thread_safe / access
- **Standalone public `Pipe`?** Expose the reader/writer pipe as a public serialization primitive (UE's `FPipe`): serialize a stream of tasks without wrapping a single `T`. Open question — does it earn its keep over `Thread_safe<T>`, which *is* a pipe + an owned `T` + access deduction + harness, just friendlier? It only adds value when the serialized resource isn't one wrapped object: several objects that must move in lockstep, external/global state, or pure task-stream ordering. Defer until such a use case shows up; if your resource is one object, `Thread_safe<T>` already covers it.
- `Thread_safe<T>` adopting an **existing** instance (reference/adopt) instead of always constructing T in place. Design ownership/lifetime (borrowed vs owned).
- Pipe FIFO semantics: think hard about whether strict FIFO is the right ordering guarantee, and what it costs.
- Reader/writer prioritisation policy for max throughput (e.g. batch readers vs. favor writers vs. fairness), potentially configurable per `Thread_safe` instance.
- **Dynamic multi-object `async(fn, objs...)`** with canonical-order acquisition (deadlock-free; never incremental). Static graph already covers the declared/multi-object case.
- Deferred / command-buffer write option (read snapshot, apply writes at a sync point) as an alternative to in-place serialized writes.
- Harness: sub-object / region ownership. Currently guards only top-level wrapped systems; a `Body&` handed out by a system isn't covered.
- clang-tidy check enforcing that every public method of a wrapped type calls `TS_CHECK_ACCESS()`.
- Generic-lambda / `auto&` support for access-mode deduction (graph `add_node` currently needs non-generic lambdas so parameter const-ness is introspectable).

## Static_task_graph
- Nested tasks inside a graph node: **done** — a node runs as a real `Executable` block (`current_task` set, `execution_flag` self-lock), so its body may `ts::nested(...)`; its completion (and thus successors + object release) gates on all nested tasks (§7.1/§8), and nested sub-work inherits the node's `Access_context` by value so it may touch the node's owned guarded data. Enables dynamic, runtime-sized fan-out over a node's store (what a static `parallel_for` can't express). Node blocks + `Run_state` are built at `compile()` and re-armed per run (dispatch via the raw scheduler API; completion via the block's persistent `on_complete` fn-ptr), so a run allocates only its `done` handle — ~19% faster than per-run blocks. Still one run at a time.
- `compile()`: ambiguity detection (two nodes with conflicting access and no explicit order edge) — currently auto-ordered by declaration index.
- Pipelined / concurrent execution (more than one execute() in flight) for frame overlap.
- Profiler-guided graph optimization (reorder/rebucket from measured task durations).

## Fork-join / parallel_for
- Promote a `parallel_for` / fork-join to the core. The sample's `sample/parallel.h` is deadlock-free by having the caller participate (spin-wait), but a core version should use scheduler oversubscription (a standby worker wakes when a task blocks) instead, so a waiting node doesn't tie up a worker. Subtasks must inherit the parent's access context (as the sample's does) so the harness passes.

## Scheduler
- **Task priorities: exposed** — the scheduler always ordered by `Priority {high, normal, low}`; now every queued route threads it to `scheduler.submit`: `ts::launch(fn, token, priority)`, `ts::task(fn).priority(p)`, `Thread_safe::async(fn, token, priority)`, `Graph_node::priority(p)`. Stored on the block (applied at dispatch, incl. prerequisite-gated). `then`/`when_all` continuations are inline (not queued) so carry no priority yet — they get one when `then` is rebased onto the prerequisite path. **Future — priority propagation:** a task's priority optionally raising its prerequisites/continuations (UE-style).
- **Inline execution mode** (planned; detailed plan discussed). Run a ready task on the *settling thread*, not the queue — a mode orthogonal to priority, for latency-sensitive/small dependents. API `ts::task(fn).set_inline().after(...)` (name TBD, not `inline_`). Dispatch forks at `num_locks == 0`: inline → run on this thread, else `submit_ready`. **Trampoline** to bound the stack: a thread-local retained-capacity vector + a head index, drained **FIFO** (advance the index, `clear()` at end — no per-item pop, no steady-state alloc), so deep inline chains run iteratively, O(1) stack. Notes for when built:
  - **External-thread execution.** An inline task may run on a thread with no worker context — a `Signal::trigger()` from a UI thread, a retractor. Whatever ambient context our workers rely on (current or future), an externally-run task must either cope with its absence or **detect and fail clearly**.
  - **Diagnostic — long-running inline tasks.** They monopolize the completer and invert priority; detect + warn (part of future profiling/diagnostics).
  - **Diagnostic — blocking task holding a worker** (generic, not inline-specific, but inline makes it easier to hit): detect a `get()` that parks a worker.
  - Scope: `after`/builder path only. `async` inline hits the pipe wall; graph-node inline holds a reservation + runs on a nondeterministic thread. Both come with the unification below.
- **Unify continuations, then routes** (roadmap order): (1) inline on the `after` path → (2) rebase `then` (and `when_all` consumption) onto proper prerequisite tasks — a continuation is a normal scheduled task (queued by default, inline opt-in), consuming the producer's result, with a cancel-propagating edge for the result-consuming case; retires the legacy inline `then`-callback and the retraction hint → (3) static graph → (4) async. See the discussion for the "task = prerequisites + dispatch-route (scheduler/pipe/inline) + priority" model.
- Spin-then-block hybrid idle (UE-style: spin N cycles, then park on an EventCount) instead of the binary spin/block policy.
- Per-worker work-stealing deques + lock-free global overflow queue to replace the single mutex-guarded priority queue (the known scaling bottleneck per benchmarks).
- **Run-on-all-workers**: submit a functor to run once on *every* worker thread (broadcast). Uses: per-thread init/teardown, flushing thread-local state, per-worker warm-up. Needs a way to fan out to N workers and join.
- **Timed / delayed tasks**: "run this in 300ms" — a delay queue (min-heap by fire-time or a timing wheel) that launches a task when due; one-shot vs periodic; cancellable pending timer; interacts with `Reusable_task` (re-arm on a timer).

## Tooling / infra
- Benchmark regression monitoring: `benchmarks/` prints a stable median ns/op per public feature (`--bench`). Wire a baseline + compare step (store the numbers, fail/flag on a regression beyond a threshold) so changes are tracked over time.
- Sanitizer build configs: ASan is reachable via `/p:EnableASAN=true`; add a proper config. TSan has no Windows runtime — would need a portable (WSL/clang) build of the non-Windows-specific parts to catch data races.
- `TS_SAFETY_CHECKS=0` smoke build (confirm the guards compile out).
