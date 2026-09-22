#!/bin/bash
# T9: where the CPU time goes. Prefers perf (often unavailable on stock
# WSL2 kernels); falls back to valgrind --callgrind, which gives
# deterministic instruction counts instead of wall-clock sampling.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

export QQ_CONFIG="$QQ_CONFIG_BENCH"
start_fast_server
trap stop_fast_server EXIT

if have perf; then
	perf record -F 999 -g -o "$RESULTS_DIR/t9_perf.data" -- \
		sh -c "for i in \$(seq 500); do $QQ_BIN hi >/dev/null; done"
	perf report --stdio -i "$RESULTS_DIR/t9_perf.data" | head -60 > "$RESULTS_DIR/t9_perf_report.txt"
	perf stat -r 200 -d "$QQ_BIN" hi 2> "$RESULTS_DIR/t9_perf_stat.txt" || true
	echo "perf results in $RESULTS_DIR/t9_perf_*"
else
	echo "perf not installed (expected on stock WSL2 kernels), using valgrind --callgrind instead"
fi

if have valgrind; then
	valgrind --tool=callgrind --callgrind-out-file="$RESULTS_DIR/t9_callgrind.out" \
		"$QQ_BIN" hi > /dev/null 2>"$RESULTS_DIR/t9_callgrind.log" || true
	if have callgrind_annotate; then
		callgrind_annotate "$RESULTS_DIR/t9_callgrind.out" 2>/dev/null | head -60 > "$RESULTS_DIR/t9_callgrind_annotate.txt"
	fi
	echo "callgrind results in $RESULTS_DIR/t9_callgrind*"
fi

echo "T9 done."
