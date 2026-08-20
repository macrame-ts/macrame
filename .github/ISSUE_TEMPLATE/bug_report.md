---
name: Bug report
about: Report a defect — a crash, a wrong result, a data race, or a harness abort
title: ''
labels: bug
assignees: ''
---

## What happened

A clear description of the bug.

## Steps to reproduce

Minimal steps or a small code snippet that triggers it:

```cpp
// ...
```

## Expected vs actual

- **Expected:**
- **Actual:**

## Environment

- **Build configuration:** Debug / Release / Shipping
- **Compiler + version:** (MSVC / clang-cl / clang; e.g. clang 21)
- **OS:** (Windows 11 / Ubuntu 24.04 / ...)
- **Commit / version:** (`macrame --version`, or the git SHA)

## Sanitizer / diagnostic output

If applicable, paste the ThreadSanitizer / AddressSanitizer report, the
`ts::fatal` message + stack trace, or a harness access-violation diagnostic.

```
// ...
```

## Additional context

Anything else that helps — frequency (deterministic vs flaky), core count, etc.
