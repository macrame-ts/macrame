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
- `Task`: define get()/then() interaction (currently get() consumes the result; mixing with then() is unsupported). Multiple concurrent blocking `get()` waiters: **done for `void`** (`Task_control_block<void>` wakes N waiters via a condition variable — a `Signal` is a barrier); the value overload stays single-consumer by design (a moved result can't go to two waiters).
- Cancellation: **done** — `Cancellation_source`/`token`; cooperative, checked when a task/node is about to run (not-yet-started work is skipped). Cancellation is a completion state that propagates down continuations and graph successors (`Task::is_cancelled()`); `async`/`then`/`execute` take an optional token; a cancelled `void` get() unblocks, a cancelled value get() is fatal (check `is_cancelled()`). **Not** yet: cancelling a `when_all` prerequisite (would hang — its internal join isn't cancel-aware); a cancel *callback*/notification.

## Task types
- Unify the handle returned from `Static_task_graph::add_node` with regular async `Task`s: **done** — split, not merged. `add_node` now returns a distinct `Graph_node` (build-time ordering identity: `after`/`before`); `Task<R>` is purely a completion handle (`get`/`then`/`is_done`/`when_all`/`execute`). The run-many model means a node has no single result, so it can't be a single-shot `Task`. Revisit when typed graph chaining lands (a `Graph_node` may then mint a per-run `Task<R>`).
- Reusable tasks + a task builder: construct a task once, configure prerequisites/access, run it repeatedly (ties into the graph's build-once/run-many model and reusable node handles).

## Thread_safe / access
- `Thread_safe<T>` adopting an **existing** instance (reference/adopt) instead of always constructing T in place. Design ownership/lifetime (borrowed vs owned).
- Pipe FIFO semantics: think hard about whether strict FIFO is the right ordering guarantee, and what it costs.
- Reader/writer prioritisation policy for max throughput (e.g. batch readers vs. favor writers vs. fairness), potentially configurable per `Thread_safe` instance.
- **Dynamic multi-object `async(fn, objs...)`** with canonical-order acquisition (deadlock-free; never incremental). Static graph already covers the declared/multi-object case.
- Deferred / command-buffer write option (read snapshot, apply writes at a sync point) as an alternative to in-place serialized writes.
- Harness: sub-object / region ownership. Currently guards only top-level wrapped systems; a `Body&` handed out by a system isn't covered.
- clang-tidy check enforcing that every public method of a wrapped type calls `TS_CHECK_ACCESS()`.
- Generic-lambda / `auto&` support for access-mode deduction (graph `add_node` currently needs non-generic lambdas so parameter const-ness is introspectable).

## Static_task_graph
- `compile()`: ambiguity detection (two nodes with conflicting access and no explicit order edge) — currently auto-ordered by declaration index.
- Pipelined / concurrent execution (more than one execute() in flight) for frame overlap.
- Profiler-guided graph optimization (reorder/rebucket from measured task durations).

## Fork-join / parallel_for
- Promote a `parallel_for` / fork-join to the core. The sample's `sample/parallel.h` is deadlock-free by having the caller participate (spin-wait), but a core version should use scheduler oversubscription (a standby worker wakes when a task blocks) instead, so a waiting node doesn't tie up a worker. Subtasks must inherit the parent's access context (as the sample's does) so the harness passes.

## Scheduler
- Spin-then-block hybrid idle (UE-style: spin N cycles, then park on an EventCount) instead of the binary spin/block policy.
- Per-worker work-stealing deques + lock-free global overflow queue to replace the single mutex-guarded priority queue (the known scaling bottleneck per benchmarks).

## Tooling / infra
- Benchmark regression monitoring: `benchmarks/` prints a stable median ns/op per public feature (`--bench`). Wire a baseline + compare step (store the numbers, fail/flag on a regression beyond a threshold) so changes are tracked over time.
- Sanitizer build configs: ASan is reachable via `/p:EnableASAN=true`; add a proper config. TSan has no Windows runtime — would need a portable (WSL/clang) build of the non-Windows-specific parts to catch data races.
- `TS_SAFETY_CHECKS=0` smoke build (confirm the guards compile out).
