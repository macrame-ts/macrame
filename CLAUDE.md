# Task System

## Project

Open-source C++ task/scheduler system, generic but game-engine-focused. Inspired by Unreal Engine's Tasks System (which the author designed and implemented at Epic Games), with improvements and additional functionality. Goals: UX, performance, rich functionality.

## Author

Experienced engine programmer. Designed and implemented UE Tasks System at Epic Games. Expects expert-level C++ discussion — no hand-holding, no over-explaining idioms.

## Stack

- C++23 (MSVC/clang-cl on Windows, Visual Studio solution)
- No external dependencies
- Lock-free/low-contention concurrency preferred

## Current State

Four layers, all built and tested (the harness suite, the sample, and the benchmarks all pass). Sources: core at repo root, plus `tests/`, `benchmarks/`, `sample/`.

- **`Scheduler`** (`scheduler.{h,cpp}`, `worker_thread.{h,cpp}`) — priority queue + `std::jthread` worker pool. Per-instance `Idle_policy` via `Scheduler_config` at construction (block = sleep on a counting semaphore, woken by `submit`; spin = yield-loop on the queue). Runtime, not templated — the `thread_local Scheduler* current_scheduler` rules out templating `Scheduler`. The single mutex-guarded `std::priority_queue` is the known scaling bottleneck (benchmarks confirm).
- **`Thread_safe<T>`** (`thread_safe.{h,cpp}`) — the access-controlled wrapper; the only sanctioned way to touch a thread-unsafe `T` across threads. `async(fn)` runs `fn(T&)` (write) or `fn(const T&)` (read) on a per-object **reader/writer pipe** (concurrent readers, exclusive writer, FIFO, non-blocking completion-driven dispatch). Access mode is deduced from the functor's parameter const-ness. Constructs `T` in place; adopting an existing instance is future work.
- **`Task<R>`** (`thread_safe.h`) — async result handle, the single representation for every result-bearing producer (`async`/`then`/`when_all`/`execute`): `get()` (call once), `is_done()`, `then(fn)` (continuation, fires inline at completion). `when_all(tasks...)` joins typed prerequisites into `Task<tuple<...>>` — void prerequisites are pure ordering and drop out (all-void → `Task<void>`); non-void results are moved into the tuple (move-only supported); consume via `then([](tuple&){...})` or apply-style `then([](A& a, B& b){...})`. `Task` is a completion handle only — it does not carry graph identity (see `Graph_node`). The shared state is `detail::Task_control_block<R>` (monomorphic on `R`; the closure is type-erased at the entry point, no virtual dispatch). **`Signal`** is a bodyless `Task<void>` completed by hand via `trigger()` (idempotent) — an integrated done-signal / barrier / phase gate (UE's `FTaskEvent`). **Cancellation**: `Cancellation_source`/`token` (cooperative); `async`/`then`/`execute` take an optional token; not-yet-started work is skipped and cancellation propagates down continuations/successors as a completion state (`Task::is_cancelled()`); a cancelled value `get()` is fatal (check first), a cancelled `void` get() unblocks. Not yet: cancelling a `when_all` prerequisite.
- **`Static_task_graph`** (`static_task_graph.{h,cpp}`) — build-once / run-many DAG. `add_node(fn, Thread_safe<>&...)` deduces per-arg access from the functor's parameter const-ness (via `Function_traits`; non-generic lambdas / function pointers only) and returns a `Graph_node` — a build-time ordering handle (`after`/`before`), not a completion handle (run-many means a node has no single result). `compile()` derives edges from access conflicts (shared instance + at least one writer, ordered by declaration index) plus explicit `after`/`before`, dedups, Kahn cycle-checks. `execute()` runs the DAG in parallel via atomic indegree counts; re-runnable; returns a completion `Task<void>`. Nodes access their objects **directly** (bypassing the pipe), so `execute()` **reserves** each object's pipe (`pipe_reserve`/`pipe_release`) for the run — a concurrent `Thread_safe::async` on the same object queues behind the run instead of racing a node (the harness does *not* catch that race — both sides declare access). Coarse: reservations held for the whole run. See `docs/task-internals.md` §10 for the mechanism + the scenarios that still break (blocking on same-object async in a node, concurrent/nested runs on shared objects, incomplete declarations).
- **Access harness** (`access.{h,cpp}`) — `TS_CHECK_ACCESS()` at the top of every guarded method checks `this` against a thread-local `Access_context` the scheduler/pipe installs per task; a violation routes to `ts::fatal`. ~1 ns/call. Subtasks (e.g. `parallel_for` chunks) inherit the parent node's context. Gated by `TS_SAFETY_CHECKS`.
- **`fatal.{h,cpp}`** — exceptions are disabled project-wide; all non-recoverable failures call `ts::fatal` (message + `std::stacktrace` + `abort`). Tests are the one place failures are non-fatal (the harness records and continues); fatal paths are checked via subprocess death tests.
- **Sample** (`sample/`) — a 17-system mock game-engine frame exercising all four layers; `run_frames` returns `Frame_stats` for the integration assertions. ~3x speedup over the serial budget at 60 fps.
- **Legacy**: `task.{h,cpp}` (`Static_task`, `Dynamic_task`, `Awaitable_task`, `Ref_counted_task`, `Dependable_task`, `Ref_counted_ptr`) are the original exploration, kept only as a printf demo in `tests/sample_tests.cpp` (`run_sample_tests`). Superseded by the above — don't build on them.

Roadmap and open design questions live in `docs/TODO.md`; the dynamic-task design of record (three-type model, lifecycle, lock-counter, retraction, nested tasks, the access invariant, allocation) in `docs/task-internals.md`; the engine-comparison research in `docs/task-systems-comparison.md`; the original interface sketch in `design/thread_safe_sketch.h`.

## Terminology

Use these terms precisely, in code and discussion:

- **spin waiting** — yielding in a loop until a condition holds (e.g. the worker idling on an empty queue, or a fixed-duration `spin()`). Does no other work.
- **busy waiting** — doing *unrelated* work while waiting, e.g. a thread that pops and executes other queued tasks while waiting for a specific task to complete. Usually a bad idea (stack growth, latency, priority inversion). Not the same as spin waiting.

## Design Principles & Lessons

- **Granularity decides parallelism.** Whole-object (system-level) access serializes every reader against any writer, collapsing the DAG toward sequential. Shard mutable state — the sample double-buffers transforms (`world_xf_prev` read by early systems, `world_xf` written by propagation and read by late systems, swapped per frame) so many readers don't contend with the writer. The central tension: static safety wants coarse access sets, performance wants fine ones.
- **Completeness hazard.** The graph's safety holds only if its access declarations are *complete*. A manual task or an escaped reference touching a system the graph believes it owns races — and the graph can *promote* a latent bug to a live one by manufacturing parallelism around it. The harness is the runtime oracle for undeclared access.
- **Never block inside a graph node / task.** Cross-system async access is fire-and-forget; consuming a result needs a continuation into a downstream node, not a `get()` inside the node. Blocking ties up a worker and risks deadlock under worker exhaustion.
- **Sync state shared across threads must outlive every participant** — heap it (`shared_ptr`), never leave it on a stack frame the initiator may unwind first. (This cost a real use-after-free in `parallel_for`: the caller returned on `completed == chunks` and unwound while a helper still touched the stack counters.)
- Prefer per-instance runtime config over templating when a `thread_local` singleton would fracture across instantiations. Prefer a counting semaphore over a condition variable for worker wake (decouples from the queue mutex; survives a future lock-free queue). Prefer intrusive / zero-alloc on hot paths; avoid virtual dispatch.
- **Debugging flaky crashes**: localize with `main`'s `--tests` / `--bench` / `--stress`, categorize exit codes (1 = a `TS_CHECK` failure via `summary()`; `0xC0000005` = an access violation), and build `/p:EnableASAN=true` to get the faulting stack. Get the stack before theorizing — inspection alone misled twice.

## VS Project

`task_system.vcxproj` must be kept in sync manually — VS does not auto-discover files. Whenever you create a new `.cpp` or `.h` file, add it to the appropriate `<ItemGroup>` in `task_system.vcxproj`: `<ClCompile>` for `.cpp`, `<ClInclude>` for `.h` (use the path including subfolder, e.g. `tests\foo.cpp`).

Layout: core sources at the repo root, tests in `tests/`, benchmarks in `benchmarks/`. The x64 configs set `AdditionalIncludeDirectories` to the root + `tests` + `benchmarks`, so `#include "x.h"` resolves regardless of which folder the file lives in.

Tests use the harness in `tests/harness.h`: `TS_CHECK(cond)` (non-fatal, prints a stacktrace on failure), `run(name, fn)`, `summary()`. Each group lives in its own `tests/<group>_tests.{h,cpp}` with a `run_<group>_tests()` entry, wired into `run_all_tests()` in `tests/tests.cpp`. Fatal paths are exercised via subprocess death tests (`ts::test::expect_death("<scenario>")` + a case in `run_death_scenario`).

`main` takes isolation/stress args (no arg = run everything): `--tests` (suite only, returns the failure count as exit code), `--bench` (benchmarks only), `--stress` (sample only, many frames at a fast scale — dense workload for sanitizers), `--death <scenario>` (internal, the death-test child). These exist to localize flaky failures: run one component in a loop, categorizing exit codes (1 = a `TS_CHECK` failure via `summary()`; `0xC0000005` = an actual access violation), and build with `/p:EnableASAN=true` to get the faulting stack rather than guessing.

## Session Guidance

- Multiple sessions will work on different aspects — read this file for context each time
- Propose design options with tradeoffs before implementing non-trivial changes
- Perf matters: flag any change that adds allocations or contention on hot paths
- After any change to concurrent code (scheduler, `Thread_safe` pipe, graph, `parallel_for`), verify with ThreadSanitizer: `bash tsan/run.sh` on a Linux host (TSan has no Windows runtime — see `tsan/README.md`). On Windows, ASan (`/p:EnableASAN=true`) + stress loops are the fallback (they catch memory bugs/UAF, not pure data races).
- Code style:
  - Type names are `Snake_case`: capitalized first letter, underscores between words — `Static_task_graph`, `Access_context`, `Thread_safe`, `Task`. This applies to all types including class templates (`Task<R>`, not `task<R>`).
  - `snake_case` for members/locals/functions; trailing `_` for private members.
  - Allman braces (opening `{` always on its own line, including namespaces, class/struct declarations, and lambdas):
    ```
    auto lambda = []()
    {
    };
    ```
    Short lambdas may stay one-liners (`[](int x){ return x + 1; }`); use Allman only when the body spans multiple lines.
  - Never align code or comments with padding spaces (no aligning `=`, parameters, trailing args/comments, or braces across lines). Single space only.
  - In comments, wrap references to code identifiers (type, function, variable, member names) in backticks: `Thread_safe`, `pipe_enqueue`, `current_access`.
