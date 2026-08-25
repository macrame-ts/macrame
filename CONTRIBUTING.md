# Contributing

Thanks for your interest in the project. It is an open-source C++23 task system and
parallelisation framework, and contributions of all sizes are welcome: bug reports,
docs, tests, and code.

The library is called Macrame, the project and file names are `macrame`, and the
namespace is `ts`.

## Building

The project requires C++23 and has no external dependencies.

- Visual Studio 2022 and later: open `macrame.slnx` (x64). There are two projects,
  `macrame` (the static library) and `macrame_playground` (the dev driver that links
  it; set it as the startup project to run the tests and samples). The configurations
  are `Debug`, `Release`, and `Shipping` (Release with the safety harness compiled
  out; see below).
- CMake: presets are available for `windows-msvc`, `windows-clang-cl`,
  `windows-shipping`, `linux-clang`, and `linux-tsan`:
  ```
  cmake --preset windows-msvc
  cmake --build --preset windows-msvc
  ```

The library is `macrame` (static, consumed as `macrame::macrame`). The dev driver
that runs the tests, samples, and benchmarks is `macrame_playground`:

```
macrame_playground --help       # Lists the available modes.
macrame_playground --tests      # Runs the test suite; the exit code is the failure count.
macrame_playground --version
```

## Running the tests

`macrame_playground --tests` runs the suite and returns the failure count as its exit
code. Expect `0 failures`. Fatal paths are exercised as subprocess death tests from the
same binary.

### ThreadSanitizer (required for concurrency changes)

Any change to concurrent code must be verified with ThreadSanitizer. TSan has no
Windows runtime, so run it on Linux (or WSL) with a recent clang:

```
CXX=clang++-21 bash tsan/run.sh      # Expects "tsan: done (no races)".
```

On Windows, AddressSanitizer (`/p:EnableASAN=true`) plus stress loops
(`macrame_playground --stress`) are the fallback. They catch memory bugs and
use-after-free, not pure data races.

## The safety harness

Types wrapped in `Guarded<T>` are made race-safe by declaring access, and a runtime
harness verifies it. Instrument every public method of a guarded type with
`TS_CHECK_ACCESS()` at the top. The harness faults with a stack trace if a method
runs without the caller holding a declared grant. It costs about 1 ns and is gated by
`TS_SAFETY_CHECKS` (default on). The Shipping build defines `TS_SAFETY_CHECKS=0` and
compiles the harness out (in CMake, `-DTS_SAFETY_CHECKS=OFF`). The suite still runs
against Shipping, and CI has a dedicated job for it; the tests that need the harness
are reported as skipped (`run_if`) rather than silently dropped.

## No exceptions

The library never throws, but it is otherwise exception-neutral. It builds with
exception support on or off. The default Debug and Release builds have exceptions
enabled. The Shipping configuration and `-DMACRAME_NO_EXCEPTIONS=ON` build with them
disabled (`_HAS_EXCEPTIONS=0` / `-fno-exceptions`). Non-recoverable failures call
`ts::fatal`, which prints a message and a `std::stacktrace` and then aborts. Don't
introduce code that requires exceptions, and an exception must not escape a task body.

## Code style

A `.clang-format` file at the repository root encodes the house style. In short:

- Type names are `Snake_case` (a capitalised first letter with underscores between
  words): `Static_task_graph`, `Guarded`, `Task`. This includes class templates.
- Use `snake_case` for members, locals, and functions, with a trailing `_` on private
  members.
- Allman braces: the opening `{` goes on its own line, including for namespaces,
  classes, and multi-line lambdas. Short lambdas may stay one-liners.
- No alignment padding. Never align `=`, parameters, or trailing comments across
  lines; use single spaces only.
- Lines may run up to 120 characters; prefer one line where it fits.
- Acronyms stay ALL-CAPS inside identifiers (`write_DOT_dump`, `write_SVG`,
  `dur_P50`), while file names stay lower snake_case (`dot_writer.h`).
- In comments, wrap code identifiers in backticks: `pipe_enqueue`, `current_access`.

## Keeping the Visual Studio projects in sync

Visual Studio does not auto-discover files, and the build is split across two
projects. When you add a `.cpp` or `.h` file, add it to the right project and keep
its lists identical with CMake:

- Pick the project by what the file is. A core `src/*.cpp` or a public or detail
  header (`include/ts/**`, `tools/**`) goes in `macrame.vcxproj` (the library); a
  test, sample, or benchmark `.cpp` goes in `macrame_playground.vcxproj` (the
  driver). Add a `<ClCompile>` entry for a `.cpp` or a `<ClInclude>` entry for a
  `.h`, with the path including the subfolder. This controls what builds.
- Add the same entry to that project's `.filters` file with a `<Filter>` matching its
  directory. This controls the Solution Explorer tree; a file added only to the
  `.vcxproj` builds fine but lands ungrouped at the project root.
- Add it to the matching list in `CMakeLists.txt` (`TS_CORE_SOURCES` for a core
  `.cpp`, `TS_SAMPLE_SOURCES` or `TS_DRIVER_SOURCES` for a driver `.cpp`). CI builds
  via CMake, not the `.vcxproj`, so a `.cpp` missing here links fine locally but
  fails CI with unresolved externals.

## Documentation

Changes to public API, behavior, or design should update the two public documents in
the same change: `docs/guide.md` (the user guide) and `docs/design.md` (the design
rationale). See those two documents for the concepts and the reasoning behind them.

## Submitting a pull request

- Keep changes focused and explain the motivation.
- Ensure `--tests` passes and, for concurrency changes, that TSan is clean.
- Follow the style above and keep the vcxproj and CMake file lists in sync.

By contributing, you agree that your contributions are licensed under the project's
[MIT License](LICENSE).
