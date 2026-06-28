# TODO / Roadmap

Future work, captured as it comes up. Not ordered by priority.

## Task chaining / results
- Typed prerequisite -> subsequent chaining (dynamic path): **done** — `Task::then` (single) and `when_all` (multi, results as a tuple).
- Apply-style `when_all`: unpack the result tuple into separate continuation args (`fn(r1, r2, ...)`) instead of `then([](tuple&){...})`.
- `when_all` with void prerequisites (pure-ordering joins) and move-only / non-copyable results (currently results must be copyable).
- Typed chaining in `Static_task_graph` (graph nodes are void-only for now): let a node consume prerequisite-node results.
- `Task`: define get()/then() interaction (currently get() consumes the result; mixing with then() is unsupported); allow multiple waiters; cancellation.

## Task types
- Unify the handle returned from `Static_task_graph::add_node` with regular async `Task`s (currently the graph node is a state-less `Task<void>` carrying a `Graph_node_ref`; the dynamic `Task` carries result state). One coherent task type / hierarchy.
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

## Scheduler
- Spin-then-block hybrid idle (UE-style: spin N cycles, then park on an EventCount) instead of the binary spin/block policy.
- Per-worker work-stealing deques + lock-free global overflow queue to replace the single mutex-guarded priority queue (the known scaling bottleneck per benchmarks).
