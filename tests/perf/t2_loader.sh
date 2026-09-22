#!/bin/bash
# T2: dynamic loader and dependency cost.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

{
	echo "=== ldd ==="
	ldd "$QQ_BIN"
	echo "library count: $(ldd "$QQ_BIN" | wc -l)"
	echo
	echo "=== NEEDED ==="
	readelf -d "$QQ_BIN" | grep NEEDED
} | tee "$RESULTS_DIR/t2_deps.txt"

echo "=== LD_DEBUG=statistics ===" | tee "$RESULTS_DIR/t2_ld_debug.txt"
LD_DEBUG=statistics "$QQ_BIN" -v >> "$RESULTS_DIR/t2_ld_debug.txt" 2>&1 || true

bench t2_qq_v_normal 50 1000 -- "$QQ_BIN -v"
LD_BIND_NOW=1 bench t2_qq_v_bindnow 50 1000 -- "$QQ_BIN -v"

if have strace; then
	strace -f -ttt -c -o "$RESULTS_DIR/t2_strace_counts.txt" "$QQ_BIN" -v || true
	strace -f -ttt -o "$RESULTS_DIR/t2_strace_timeline.txt" "$QQ_BIN" -v || true
	echo "openat calls: $(grep -c openat "$RESULTS_DIR/t2_strace_timeline.txt" || true)"
else
	echo "strace not installed, skipping syscall trace"
fi

echo "T2 done. See $RESULTS_DIR/t2_*"
