#!/bin/bash
# T8: large responses and memory. Sweeps response size and records wall
# time, peak RSS, and realloc counts.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

export QQ_CONFIG="$QQ_CONFIG_BENCH"

for size_name in 1KB:1024 100KB:102400 1MB:1048576 10MB:10485760; do
	name="${size_name%%:*}"
	bytes="${size_name##*:}"
	echo "=== $name ($bytes bytes) ==="

	start_fast_server "$bytes"
	trap stop_fast_server EXIT

	if have /usr/bin/time; then
		/usr/bin/time -v "$QQ_BIN" hi > /dev/null 2> "$RESULTS_DIR/t8_${name}_time.txt" || true
		grep -E 'Maximum resident|Elapsed' "$RESULTS_DIR/t8_${name}_time.txt" || true
	fi

	if have valgrind; then
		valgrind --tool=massif --massif-out-file="$RESULTS_DIR/t8_${name}_massif.out" \
			"$QQ_BIN" hi > /dev/null 2>"$RESULTS_DIR/t8_${name}_massif.log" || true
		ms_print "$RESULTS_DIR/t8_${name}_massif.out" 2>/dev/null | head -30 > "$RESULTS_DIR/t8_${name}_massif_summary.txt" || true
	fi

	if have heaptrack; then
		heaptrack -o "$RESULTS_DIR/t8_${name}_heaptrack" "$QQ_BIN" hi > /dev/null 2>&1 || true
	elif have ltrace; then
		ltrace -c -e 'realloc+malloc+free' -o "$RESULTS_DIR/t8_${name}_ltrace.txt" "$QQ_BIN" hi > /dev/null 2>&1 || true
	fi

	stop_fast_server
	trap - EXIT
done

echo "T8 done. See $RESULTS_DIR/t8_*"
