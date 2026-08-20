# Contributing

Thanks for your interest in the project. It is an open-source C++23 task system /
parallelisation framework; contributions of all sizes are welcome — bug reports,
docs, tests, and code.

> The library is **Macrame**; the project and file names are now `macrame`.
> Only the GitHub repo and URLs stay `Andriy06/task_system` until the repository
> moves to `macrame-ts/macrame`. The namespace is `ts`.

## Building

C++23, no external dependencies, exceptions disabled project-wide.

- **Visual Studio 2022+** — open `macrame.slnx` (x64). Configurations:
  `Debug`, `Release`, and `Shipping` (Release with the safety harness compiled
  out — see below).
- **CMake** — presets `windows-msvc`, `windows-clang-cl`, `windows-shipping`,
  `linux-clang`, `linux-tsan`:
  ```
  cmake --preset windows-msvc
  cmake --build --preset windows-msvc
  ```

The build produces a single driver executable:

```
macrame --help       # list modes
macrame --tests      # the test suite; exit code = failure count
macrame --version
```

## Running the tests

`macrame --tests` prints a check total ending in `0 failures` (the check
count grows as tests are added; the failure count must stay 0). Fatal paths are
exercised as subprocess *death tests* from the same binary.

### ThreadSanitizer (required for concurrency changes)

Any change to concurrent code — the scheduler, the `Guarded` pipe, the graph,
`parallel_for` — must be verified with ThreadSanitizer. TSan has no Windows
runtime; run it on Linux (or WSL) with a recent clang:

```
CXX=clang++-21 bash tsan/run.sh      # expects "tsan: done (no races)"
```

On Windows, AddressSanitizer (`/p:EnableASAN=true`) plus stress loops
(`macrame --stress`) are the fallback — they catch memory bugs and UAF, not
pure data races.

## The safety harness

Types wrapped in `Guarded<T>` are made race-safe by declaring access; a runtime
**harness** verifies it. Instrument **every public method** of a guarded type
with `TS_CHECK_ACCESS()` at the top — the harness faults (with a stack trace) if a
method runs without the caller holding a declared grant. It costs ~1 ns and is
gated by `TS_SAFETY_CHECKS` (default on). The **Shipping** build defines
`TS_SAFETY_CHECKS=0` and compiles the harness out (CMake: `-DTS_SAFETY_CHECKS=OFF`).
The suite still runs against Shipping — CI has a dedicated job for it — with the
tests that need the harness reported as skipped (`run_if`), not silently dropped.

## No exceptions

Exceptions are disabled (`_HAS_EXCEPTIONS=0` / `-fno-exceptions`). Non-recoverable
failures call `ts::fatal` (message + `std::stacktrace` + `abort`), not `throw`.
Don't introduce code that requires exceptions.

## Code style

A `.clang-format` at the repo root encodes the house style. In short:

- **Type names** are `Snake_case` (capitalised first letter, underscores between
  words) — `Static_task_graph`, `Guarded`, `Task`. This includes class templates.
- `snake_case` for members, locals, and functions; a trailing `_` on private
  members.
- **Allman braces** — the opening `{` on its own line, including for namespaces,
  classes, and multi-line lambdas. Short lambdas may stay one-liners.
- **No alignment padding** — never align `=`, parameters, or trailing comments
  across lines. Single spaces only.
- **Lines up to 120 characters**; prefer one line where it fits.
- **Acronyms stay ALL-CAPS inside identifiers** — `write_DOT_dump`, `write_SVG`,
  `dur_P50`; file names stay lower snake_case (`dot_writer.h`).
- In comments, wrap code identifiers in backticks: `pipe_enqueue`, `current_access`.

## Keeping the Visual Studio project in sync

Visual Studio does not auto-discover files. When you add a `.cpp` or `.h`, keep
**three lists** identical:

- `macrame.vcxproj` — a `<ClCompile>` (for `.cpp`) or `<ClInclude>` (for `.h`)
  entry, path-including-subfolder. Controls what *builds*.
- `macrame.vcxproj.filters` — the same entry with a `<Filter>` matching its
  directory. Controls the Solution Explorer tree; a file added only to the
  `.vcxproj` builds fine but lands ungrouped at the project root.
- `CMakeLists.txt` — the source list. CI builds via CMake, not the `.vcxproj`, so
  a `.cpp` missing here links fine locally but fails CI with unresolved externals.

## Documentation

Public API, behavior, or design changes should update the two public documents in
the same change: `docs/guide.md` (user guide) and `docs/design.md` (design
rationale). See the guide and design docs for the concepts and the *why*.

## Submitting a pull request

- Keep changes focused; explain the motivation.
- Ensure `--tests` passes and, for concurrency changes, TSan is clean.
- Follow the style above and keep the vcxproj / CMake file lists in sync.

By contributing, you agree that your contributions are licensed under the
project's [MIT License](LICENSE).
