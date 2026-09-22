#!/bin/bash
# Runs the mechanically-runnable tests (T1, T2, T3, T6, T8, T9, T10;
# T11 opt-in). See README.md for what each test measures, and for T4/T5/T7
# which need source instrumentation not yet added.
set -uo pipefail
cd "$(dirname "$0")"
source lib/common.sh

make -C "$REPO_DIR" >/dev/null

SCRIPTS=(t1_process_start.sh t2_loader.sh t3_e2e.sh t6_connection_reuse.sh t8_memory.sh t9_cpu.sh t10_build_variants.sh)
[ "${WITH_T11:-0}" = "1" ] && SCRIPTS+=(t11_tail_latency.sh)

fail=0
for s in "${SCRIPTS[@]}"; do
	echo
	echo "############ $s ############"
	if ! ./"$s"; then
		echo "!!! $s failed (rc=$?)" >&2
		fail=1
	fi
done

echo
echo "All raw results and summaries are in $RESULTS_DIR."
echo "Copy the numbers you trust into docs/PERF.md (see README.md's 'How to report' section)."
exit $fail
