# Task System

## Project

Open-source C++ task/scheduler system, generic but game-engine-focused. Inspired by Unreal Engine's Tasks System (which the author designed and implemented at Epic Games), with improvements and additional functionality. Goals: UX, performance, rich functionality.

## Author

Experienced engine programmer. Designed and implemented UE Tasks System at Epic Games. Expects expert-level C++ discussion — no hand-holding, no over-explaining idioms.

## Stack

- C++23 (MSVC/clang-cl on Windows, Visual Studio solution)
- No external dependencies
- Lock-free/low-contention concurrency preferred

## Current State (as of 2026-06)

Core scaffolding in `main.cpp` and `scheduler.h` (scheduler.cpp is empty placeholder):

- `Scheduler` — priority queue + single worker thread (`std::jthread`), busy-spin (no condition variable, removed intentionally)
- `Priority` enum — high / normal / low
- Task variants explored so far:
  - `Static_task<F>` — zero-alloc, function pointer trampoline
  - `Dynamic_task` — `move_only_function`, heap body
  - `Awaitable_task` — adds `binary_semaphore` wait
  - `Ref_counted_task` / `Ref_counted_task_impl` — intrusive ref-count, self-submits and self-destructs
  - `Dependable_task` / `Dependable_task_impl` — prerequisite counting + subsequent chain, `then()` API
- `Ref_counted_base<T>` / `Ref_counted_ptr<T>` — intrusive smart pointer utilities

## Terminology

Use these terms precisely, in code and discussion:

- **spin waiting** — yielding in a loop until a condition holds (e.g. the worker idling on an empty queue, or a fixed-duration `spin()`). Does no other work.
- **busy waiting** — doing *unrelated* work while waiting, e.g. a thread that pops and executes other queued tasks while waiting for a specific task to complete. Usually a bad idea (stack growth, latency, priority inversion). Not the same as spin waiting.

## Design Principles

- Prefer intrusive ref-counting over `shared_ptr` for tasks (cache locality, explicit control)
- Avoid virtual dispatch on hot paths
- Worker threads set `thread_local Scheduler* current_scheduler` — subsequents submitted inline from worker
- `subsequents_closed_` flag handles the race between `then()` called before vs after task completion

## VS Project

`task_system.vcxproj` must be kept in sync manually — VS does not auto-discover files. Whenever you create a new `.cpp` or `.h` file, add it to the appropriate `<ItemGroup>` in `task_system.vcxproj`: `<ClCompile>` for `.cpp`, `<ClInclude>` for `.h` (use the path including subfolder, e.g. `tests\foo.cpp`).

Layout: core sources at the repo root, tests in `tests/`, benchmarks in `benchmarks/`. The x64 configs set `AdditionalIncludeDirectories` to the root + `tests` + `benchmarks`, so `#include "x.h"` resolves regardless of which folder the file lives in.

Tests use the harness in `tests/harness.h`: `TS_CHECK(cond)` (non-fatal, prints a stacktrace on failure), `run(name, fn)`, `summary()`. Each group lives in its own `tests/<group>_tests.{h,cpp}` with a `run_<group>_tests()` entry, wired into `run_all_tests()` in `tests/tests.cpp`. Fatal paths are exercised via subprocess death tests (`ts::test::expect_death("<scenario>")` + a case in `run_death_scenario`).

`main` takes isolation/stress args (no arg = run everything): `--tests` (suite only, returns the failure count as exit code), `--bench` (benchmarks only), `--stress` (sample only, many frames at a fast scale — dense workload for sanitizers), `--death <scenario>` (internal, the death-test child). These exist to localize flaky failures: run one component in a loop, categorizing exit codes (1 = a `TS_CHECK` failure via `summary()`; `0xC0000005` = an actual access violation), and build with `/p:EnableASAN=true` to get the faulting stack rather than guessing.

## Session Guidance

- Multiple sessions will work on different aspects — read this file for context each time
- Propose design options with tradeoffs before implementing non-trivial changes
- Perf matters: flag any change that adds allocations or contention on hot paths
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
