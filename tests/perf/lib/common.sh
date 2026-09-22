# Shared helpers for tests/perf/*.sh. Source this, don't run it directly.

PERF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_DIR="$(cd "$PERF_DIR/../.." && pwd)"
RESULTS_DIR="$PERF_DIR/results"
BENCH_PORT=8971
QQ_BIN="${QQ_BIN:-$REPO_DIR/qq}"

mkdir -p "$RESULTS_DIR"

have() { command -v "$1" >/dev/null 2>&1; }

# bench NAME WARMUP RUNS -- CMD...
# Uses hyperfine if installed (exports its JSON too), else falls back to
# lib/bench.py. Either way, results/<NAME>.json + .samples get written.
bench() {
	local name="$1" warmup="$2" runs="$3"
	shift 3
	[ "$1" = "--" ] && shift
	if have hyperfine; then
		hyperfine -N --warmup "$warmup" --runs "$runs" \
			--export-json "$RESULTS_DIR/$name.json" \
			--command-name "$name" -- "$*"
	else
		echo "hyperfine not found, using lib/bench.py (adds Python-side fork overhead," \
			"fine for relative comparisons, don't quote it as an absolute number)" >&2
		python3 "$PERF_DIR/lib/bench.py" --name "$name" --warmup "$warmup" --runs "$runs" \
			--out "$RESULTS_DIR/$name" -- "$@"
	fi
}

record_env() {
	{
		echo "commit: $(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null)"
		echo "kernel: $(uname -r)"
		echo "cpu: $(lscpu | grep 'Model name' | head -1)"
		echo "gcc: $(gcc --version | head -1)"
		echo "curl: $(curl --version | head -1)"
		if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
			echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
		fi
		grep -qi microsoft /proc/version 2>/dev/null && echo "platform: WSL2 (timings are noisy, prefer native Linux)"
	} | tee "$RESULTS_DIR/env.txt"
}

# start_fast_server [PAD_BYTES] -> prints PID, sets FAST_SERVER_PID
start_fast_server() {
	python3 "$PERF_DIR/server/fast_server.py" "$BENCH_PORT" "${1:-0}" &
	FAST_SERVER_PID=$!
	for _ in $(seq 1 50); do
		curl -s -o /dev/null "http://127.0.0.1:$BENCH_PORT/" && break
		sleep 0.1
	done
}

stop_fast_server() {
	[ -n "${FAST_SERVER_PID:-}" ] || return 0
	kill "$FAST_SERVER_PID" 2>/dev/null || true
	wait "$FAST_SERVER_PID" 2>/dev/null || true
	FAST_SERVER_PID=""
}

export QQ_CONFIG_BENCH="$PERF_DIR/bench-config.json"
