# ThreadSanitizer verification

TSan catches data races that ASan and stress-testing miss. It has **no Windows
runtime** — it only works under clang on Linux/macOS. This directory holds a
harness-free stress driver so the portable core (scheduler, `Guarded` pipe,
`Static_task_graph` + `parallel_for`, `then`/`when_all`) can be checked off-host.

## One-time host setup (pick one)

- **WSL**: `wsl --install` (admin + reboot), then in the distro install a recent
  clang + libstdc++: `sudo apt install clang libstdc++-14-dev` (Ubuntu 24.04+).
- **Docker**: run a container with clang, mount the repo, run `tsan/run.sh`.
- **CI**: a Linux job that runs `tsan/run.sh` on push (`.github/workflows/ci.yml`,
  the `linux-tsan` job).

## Git hooks (per clone, recommended)

`tsan/tsan_main.cpp` is now compiled by the Windows build too (see below), so an
API change that misses it fails the ordinary local build. `tools/hooks/` still
closes the remaining gap — the driver is the only TU whose *Linux* toolchain
behaviour matters, and the hooks run it there:

- **pre-commit**: compile-check `tsan_main.cpp` (`-fsyntax-only`, ~3 s) when the
  staged changes touch C++ sources/headers.
- **pre-push**: full `tsan/run.sh` (~40 s) when the outgoing commits touch C++
  sources/headers.

Both skip with a warning when no Linux clang is reachable (WSL not set up yet).
Enable once per clone:

```sh
git config core.hooksPath tools/hooks
```

## The driver in the Windows build

`macrame.vcxproj` compiles `tsan/tsan_main.cpp` with
`TS_TSAN_NO_MAIN` defined, purely as an API-drift tripwire: an API-tightening
change now fails the developer's own build instead of at commit time or in a
Linux CI job. Everything in the driver lives in an anonymous namespace, so
`main` is the only symbol that would collide with `src/main.cpp` — and it is
also the only thing that *references* those statics, so simply dropping it would
trade a link error for a wall of unused-function warnings. Under the macro the
entry point is renamed to `ts::tsan::run_all` instead: the whole TU compiles and
stays referenced, and the Windows binary gains one function it never calls.

## Run (after every major change)

From the repo root, in WSL:

```sh
bash tsan/run.sh
```

From Windows (PowerShell), invoke a **non-interactive** shell — never `-lic`
(an interactive login shell blocks on a tty and never runs the script):

```powershell
wsl.exe -e bash -c "bash /mnt/c/path/to/macrame/tsan/run.sh"
```

Clean exit + `tsan: done (no races)` = no races found. A race prints a report
and exits nonzero (`halt_on_error=1`). **Verified clean** with clang 21 +
libstdc++ on Ubuntu 26.04 (WSL).

**Read the exit code directly** — do not pipe the run through `tail`/`head` when
you care about pass/fail: a pipeline returns the *last* command's status, which
hid a killed run as "exit 0" once. The script has a watchdog (`timeout`,
`TSAN_TIMEOUT` seconds, default 180) so a deadlock/livelock fails fast instead of
sitting at ~0% CPU forever, and it kills+removes any stale `ts_tsan` before
building so a hung previous run can't block the next build.

## Reproducing a rare race (rr)

TSan detection is schedule-dependent: it only reasons about the interleaving
that actually ran, so a race it reports on one run may not recur on the next
(same binary, same input, different thread timing). When a report is rare and
you need it under a debugger, pair TSan with **`rr`** (record/replay,
Linux-only, `sudo apt install rr`):

1. `rr record ./ts_tsan` — runs the TSan build and records the exact execution
   (all nondeterminism: thread scheduling, syscalls, signals) to a trace. If
   the race fires this run, TSan prints its report as usual *and* the trace is
   captured.
2. `rr replay` — replays that identical execution deterministically, forward
   and backward, under a gdb-compatible interface. The schedule is frozen, so
   the race is now 100% reproducible.
3. From the TSan report take the racing address and the two stacks; in the
   replay set a hardware watchpoint on the address (`watch *addr`) and
   `reverse-continue` to run backward to the *previous* write — landing you on
   the unsynchronized access that TSan flagged, with full state, as many times
   as you need.

Why it works: TSan tells you a race exists and where, but not a reproducible
schedule; `rr` freezes one schedule so the "may-not-recur" problem disappears.
The two are complementary — TSan is the detector, `rr` is the reproducer. This
is the principled alternative to the ad-hoc perturbation approach (core-pinning
with `taskset -c 0,1` to widen preemption windows, plus the in-tree forensic
event ring) used when a race would not surface under plain stress. Caveat: `rr`
needs a CPU with the right perf-counter support (most Intel; some AMD/VM
configs need `--disable-cpuid-features` or fall back to the perturbation route),
and it serializes onto one core, which itself changes timing — record under the
same core-pinning that made the race appear.

## Native Windows alternative

TSan has no Windows runtime and none is planned: the compiler instrumentation
is target-independent, but the runtime (shadow-memory mapping into a fixed VA
layout, interceptors for the Win32 sync primitives, the thread/TLS/SEH model)
has never been ported. ASan was ported (smaller runtime, higher demand); TSan
was not, for priority reasons, not fundamental ones. WSL (above) is the
practical answer for this portable codebase.

The only *native*-Windows substitute is **Intel Inspector** (oneAPI): a
binary-instrumentation dynamic race detector — no recompile, no shadow-mapping
port needed, runs on the MSVC build directly. It is far slower than TSan and
commercial, but it is the one race-adjacent oracle besides the access harness
that runs on the dominant platform. Use it when a race is suspected in
Windows-only code paths that the portable driver here does not exercise;
otherwise WSL + this driver is the primary gate.

## Notes

- The driver (`tsan_main.cpp`) deliberately avoids the test harness
  (`tests/harness.*` uses `windows.h` + subprocess death tests). It exercises the
  same concurrency paths the suite does, plus multi-producer stress the tests
  don't. Extend it as new concurrent features land.
- `fatal.cpp` is built without `<stacktrace>` where the stdlib lacks it; TSan's
  own backtrace covers reports.
- `.gitattributes` forces `*.sh` to LF so the script runs in WSL despite the
  Windows checkout. WSL clears `/tmp` when the distro idles, so build + run in one
  invocation (the script does); the binary builds to `/tmp` to avoid the slow 9p
  mount.
- For symbolized stacks (file:line) in race reports, install `llvm-symbolizer`:
  `sudo apt install -y llvm`. `run.sh` uses it automatically when present;
  without it, reports still fire but show addresses + BuildId only. Verified that
  TSan catches a deliberate publish-index-before-write-slot race (exit 66).
