#!/bin/bash
# T11 (optional): tail latency under load. Re-runs T3 while a CPU hog runs
# on a different core, then again pinned to the same core.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

export QQ_CONFIG="$QQ_CONFIG_BENCH"
start_fast_server
trap stop_fast_server EXIT

if ! have taskset; then
	echo "taskset not installed, cannot pin cores for T11" >&2
	exit 1
fi

hog() { taskset -c "$1" bash -c 'while :; do :; done' & echo $!; }

echo "=== baseline (no hog) ==="
bench t11_baseline 50 500 -- "$QQ_BIN hi"

echo "=== hog on a different core ==="
HOG_PID=$(hog 1)
bench t11_diff_core 50 500 -- "$QQ_BIN hi"
kill "$HOG_PID" 2>/dev/null || true

echo "=== hog pinned to the same core as qq (taskset -c 0) ==="
HOG_PID=$(hog 0)
bench t11_same_core 50 500 -- "taskset -c 0 $QQ_BIN hi"
kill "$HOG_PID" 2>/dev/null || true

echo "T11 done. Compare p99/p99.9 across the three result files."
