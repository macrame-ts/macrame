#!/usr/bin/env bash
# Build + run the ThreadSanitizer stress driver.
#
# Requires clang with the ThreadSanitizer runtime and a C++23 stdlib that has
# std::move_only_function / std::jthread / std::counting_semaphore (verified with
# clang 21 + libstdc++ on Ubuntu 26.04). TSan has NO Windows runtime -- run this
# on Linux/macOS (e.g. WSL). From anywhere:
#   bash tsan/run.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
OUT="${TMPDIR:-/tmp}/ts_tsan"   # build off the (slow) Windows mount
SRC="scheduler.cpp worker_thread.cpp thread_safe.cpp static_task_graph.cpp access.cpp fatal.cpp \
     sample/systems.cpp sample/frame.cpp sample/engine.cpp tsan/tsan_main.cpp"

"$CXX" -std=c++23 -fsanitize=thread -fno-exceptions -O1 -g -pthread \
    -I. -Isample $SRC -o "$OUT"

# Symbolize reports (file:line) if llvm-symbolizer is available -- `sudo apt
# install -y llvm`. Without it, reports still fire but show addresses only.
OPTS="halt_on_error=1 second_deadlock_stack=1"
SYM="$(command -v llvm-symbolizer || true)"
[ -n "$SYM" ] && OPTS="$OPTS external_symbolizer_path=$SYM"

TSAN_OPTIONS="$OPTS" "$OUT"
