#!/usr/bin/env bash
# Forensic hunt for the stress_reuse `dep.sync() == i` flake. Builds the isolated driver
# (TS_REUSE_FORENSICS + the TS_REUSE_ONLY entry in tsan_main.cpp) in TWO variants -- TSan
# and plain -- and runs a small matrix of concurrent processes, streaming any CAPTURE line
# the moment it appears. Diagnostic tool; does not touch tsan/run.sh.
#
#   bash tsan/reuse_hunt.sh [iters-per-proc]
#
# Runs with halt_on_error=0 so a TSan report doesn't kill the hunt before a capture.
set -uo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
ITERS="${1:-50}"
# NOT /tmp: WSL's tmpfs /tmp is wiped when the utility VM idles out between invocations.
OUT_TSAN="$HOME/ts_reuse_tsan"
OUT_PLAIN="$HOME/ts_reuse_plain"
LOGDIR="$HOME/ts_reuse_logs"
mkdir -p "$LOGDIR"
SRC="scheduler.cpp worker_thread.cpp guarded.cpp static_task_graph.cpp access.cpp fatal.cpp \
     sample/systems.cpp sample/frame.cpp sample/engine.cpp tsan/tsan_main.cpp"

pkill -9 -f "$OUT_TSAN" 2>/dev/null || true
pkill -9 -f "$OUT_PLAIN" 2>/dev/null || true
rm -f "$OUT_TSAN" "$OUT_PLAIN"

echo "building TSan variant..."
"$CXX" -std=c++23 -DTS_REUSE_FORENSICS -fsanitize=thread -fno-exceptions -O1 -g -pthread \
    -I. -Isample $SRC -o "$OUT_TSAN"
echo "building plain variant..."
# -mcx16: 16-byte atomics (std::atomic<Task_entry>) inline via cmpxchg16b -- without the
# TSan runtime there is no __atomic_load_16/__atomic_store_16 fallback to link against.
"$CXX" -std=c++23 -DTS_REUSE_FORENSICS -mcx16 -fno-exceptions -O1 -g -pthread \
    -I. -Isample $SRC -o "$OUT_PLAIN"

SYM="$(command -v llvm-symbolizer || true)"
OPTS="halt_on_error=0 second_deadlock_stack=1"
[ -n "$SYM" ] && OPTS="$OPTS external_symbolizer_path=$SYM"

PIDS=""
run_one()   # $1 = tag, $2 = binary, $3 = taskset prefix ("" for none)
{
    local tag="$1" bin="$2" pin="$3"
    local log="$LOGDIR/$tag.log"
    : > "$log"
    ( TSAN_OPTIONS="$OPTS" TS_REUSE_ONLY=1 TS_REUSE_ITERS="$ITERS" \
        $pin "$bin" > "$log" 2>&1
      echo "EXIT $? $tag" >> "$log" ) &
    PIDS="$PIDS $!"
}

echo "launching matrix (iters/proc = $ITERS)..."
run_one tsan_pin_a "$OUT_TSAN" "taskset -c 0,1"
run_one tsan_pin_b "$OUT_TSAN" "taskset -c 0,1"
run_one tsan_free_a "$OUT_TSAN" ""
run_one tsan_free_b "$OUT_TSAN" ""
run_one plain_pin "$OUT_PLAIN" "taskset -c 0,1"
run_one plain_free "$OUT_PLAIN" ""

# Stream CAPTURE/heartbeat/done lines from all logs while the jobs run.
tail -q -n +1 -F "$LOGDIR"/*.log 2>/dev/null | grep --line-buffered -E "CAPTURE|done:|EXIT" &
TAILPID=$!
wait $PIDS
sleep 1
kill "$TAILPID" 2>/dev/null || true

echo "=== summary ==="
for f in "$LOGDIR"/*.log; do
    echo "$f: $(grep -c CAPTURE "$f") captures, $(grep -c heartbeat "$f") heartbeats, $(tail -1 "$f")"
done
