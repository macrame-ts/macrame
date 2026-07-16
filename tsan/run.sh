#!/usr/bin/env bash
# Build + run the ThreadSanitizer stress driver.
#
# Requires clang with the ThreadSanitizer runtime and a C++23 stdlib that has
# std::move_only_function / std::jthread / std::counting_semaphore (verified with
# clang 21 + libstdc++ on Ubuntu 26.04). TSan has NO Windows runtime -- run this
# on Linux/macOS (e.g. WSL). From WSL:
#   bash tsan/run.sh
# From Windows (PowerShell), invoke a NON-interactive shell -- do NOT use `-lic`,
# an interactive login shell blocks on a tty and never runs:
#   wsl.exe -e bash -c "bash /mnt/c/path/to/task_system/tsan/run.sh"
# Check the exit code directly; do NOT pipe through `tail` etc. when you need it
# (a pipeline returns the LAST command's status and hides a failure/kill here).
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
OUT="${TMPDIR:-/tmp}/ts_tsan"
TIMEOUT="${TSAN_TIMEOUT:-180}"   # watchdog seconds; a deadlock fails fast, never hangs
SRC="scheduler.cpp worker_thread.cpp guarded.cpp static_task_graph.cpp access.cpp fatal.cpp \
     sample/systems.cpp sample/frame.cpp sample/engine.cpp sample/physics.cpp sample/blackboard.cpp tsan/tsan_main.cpp"

# A previous run that deadlocked keeps the binary busy (a build can't overwrite a
# running executable) and burns no CPU while looking "alive". Kill any leftover and
# start from a fresh binary so a stale process can't block or be mistaken for a run.
pkill -9 -f "$OUT" 2>/dev/null || true
rm -f "$OUT"

"$CXX" -std=c++23 -fsanitize=thread -fno-exceptions -O1 -g -pthread \
    -I. -Isample $SRC -o "$OUT"

# Symbolize reports (file:line) if llvm-symbolizer is available -- `sudo apt
# install -y llvm`. Without it, reports still fire but show addresses only.
OPTS="halt_on_error=1 second_deadlock_stack=1"
SYM="$(command -v llvm-symbolizer || true)"
[ -n "$SYM" ] && OPTS="$OPTS external_symbolizer_path=$SYM"

# Run under `timeout` (bounds a hang; SIGKILLs after a grace period) and capture
# the binary's own output to a log, so the last stage it printed is always visible
# -- even when the invoking shell drops stdout through a pipe. The driver runs with
# unbuffered stdout, so on a hang the log ends at the stage that deadlocked.
LOG="${TMPDIR:-/tmp}/ts_tsan.log"
status=0
TSAN_OPTIONS="$OPTS" timeout --kill-after=10 "$TIMEOUT" "$OUT" >"$LOG" 2>&1 || status=$?
cat "$LOG"
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
    echo "tsan: TIMED OUT after ${TIMEOUT}s -- deadlock in the stage above; FAIL" >&2
    exit 1
fi
exit "$status"
