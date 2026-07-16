## Summary

What this change does and why.

## Checklist

- [ ] `task_system --tests` passes (`N checks, 0 failures`).
- [ ] For concurrency changes (scheduler, `Guarded` pipe, graph, `parallel_for`):
      ThreadSanitizer is clean (`CXX=clang++-21 bash tsan/run.sh` → no races).
- [ ] Public API / behavior / design changes update `docs/guide.md` and
      `docs/design.md` (the public-docs contract).
- [ ] New `.cpp` / `.h` files are added to `task_system.vcxproj` and the
      `CMakeLists.txt` source lists.
- [ ] Code follows the house style (`.clang-format`; Snake_case types, Allman
      braces, no alignment padding).
- [ ] No new use of exceptions (the project builds with exceptions disabled).

## Notes

Anything reviewers should know — trade-offs, follow-ups, benchmarks.
