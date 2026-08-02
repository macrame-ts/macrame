#!/usr/bin/env bash
cd "$(dirname "$0")/.."
OUT="$HOME/ts_tsan_pipe"
if [ ! -x "$OUT" ]; then
  echo "building..."
  clang++ -std=c++23 -fsanitize=thread -fno-exceptions -O1 -g -pthread -DTS_PIPE_TAIL=1 \
    -Iinclude -Isample -Itools -Itests \
    src/scheduler.cpp src/worker_thread.cpp src/guarded.cpp src/pipe_tail.cpp \
    src/static_task_graph.cpp src/access.cpp src/fatal.cpp \
    sample/game_frame.cpp sample/physics.cpp sample/blackboard.cpp tsan/tsan_main.cpp -o "$OUT" || { echo BUILD_FAIL; exit 1; }
fi
SYM="$(command -v llvm-symbolizer || true)"
TSAN_OPTIONS="halt_on_error=1 external_symbolizer_path=$SYM" timeout 1200 "$OUT" > "$HOME/tsp.log" 2>&1
echo "EXIT=$?"
grep -nE 'tsan:|WARNING: ThreadSanitizer|SUMMARY|Read of size|Write of size|heap block of' "$HOME/tsp.log" | head -45
