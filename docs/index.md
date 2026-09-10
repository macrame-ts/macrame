# Macrame

*Threads, knotted on purpose.*

Macrame is a C++23 high-level parallelisation framework built around controlled access to shared
resources.

[Source on GitHub](https://github.com/macrame-ts/macrame)

## Start here

- [Quick start](quickstart.md) — how to get the framework running.
- [User guide](guide.md) — a complete tour of every layer.
- [Design rationale](design.md) — why the library is shaped the way it is.

## Examples

- [Worked example: reading a frame trace and optimising it](example-frame-optimization.md) — the
  `game_frame` sample as an optimisation exercise.
- [The `game_frame` trace](game_frame_trace.html)

## Background

- [Task and job systems: a comparative survey](task-systems-comparison.md) — Unreal Engine Tasks
  System, Taskflow, TBB, HPX, Folly, Go and others.
- [Roadmap](roadmap.md) — where Macrame is going.
- [Limits of the model](internals/limits.md) — what the harness and access declarations do not
  catch.

## Case studies

- [Parallelising Godot to get 6x speedup](https://github.com/Andriy06/godot) — Godot 4.8-dev
  parallelised with Macrame. An experiment, not for production.
