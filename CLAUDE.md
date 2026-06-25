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

## Design Principles

- Prefer intrusive ref-counting over `shared_ptr` for tasks (cache locality, explicit control)
- Avoid virtual dispatch on hot paths
- Worker threads set `thread_local Scheduler* current_scheduler` — subsequents submitted inline from worker
- `subsequents_closed_` flag handles the race between `then()` called before vs after task completion

## VS Project

`task_system.vcxproj` must be kept in sync manually — VS does not auto-discover files. Whenever you create a new `.cpp` or `.h` file, add it to the appropriate `<ItemGroup>` in `task_system.vcxproj`: `<ClCompile>` for `.cpp`, `<ClInclude>` for `.h`.

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
