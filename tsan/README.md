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

Clean exit + `tsan: done (no races)` = no races found. A race prints a report
and exits nonzero (`halt_on_error=1`). **Verified clean** with clang 21 +
libstdc++ on Ubuntu 26.04 (WSL).

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
