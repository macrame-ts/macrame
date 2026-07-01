# ThreadSanitizer verification

TSan catches data races that ASan and stress-testing miss. It has **no Windows
runtime** — it only works under clang on Linux/macOS. This directory holds a
harness-free stress driver so the portable core (scheduler, `Thread_safe` pipe,
`Static_task_graph` + `parallel_for`, `then`/`when_all`) can be checked off-host.

## One-time host setup (pick one)

- **WSL**: `wsl --install` (admin + reboot), then in the distro install a recent
  clang + libstdc++: `sudo apt install clang libstdc++-14-dev` (Ubuntu 24.04+).
- **Docker**: run a container with clang, mount the repo, run `tsan/run.sh`.
- **CI**: a Linux job that runs `tsan/run.sh` on push.

## Run (after every major change)

From the repo root, in WSL:

```sh
bash tsan/run.sh
```

From Windows (PowerShell), invoke a **non-interactive** shell — never `-lic`
(an interactive login shell blocks on a tty and never runs the script):

```powershell
wsl.exe -e bash -c "bash /mnt/c/src/task_system/tsan/run.sh"
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
