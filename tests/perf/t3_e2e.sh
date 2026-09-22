#!/bin/bash
# T3: end-to-end request against the fast test server, qq vs raw curl.
# The gap between them is the latency qq's own code adds.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

BODY="$RESULTS_DIR/t3_body.json"
cat > "$BODY" <<'EOF'
{"model":"test-model","messages":[{"role":"user","content":"hi"}]}
EOF

start_fast_server
trap stop_fast_server EXIT

export QQ_CONFIG="$QQ_CONFIG_BENCH"

bench t3_qq_hello 50 1000 -- "$QQ_BIN hi"
bench t3_curl_raw 50 1000 -- "curl -s -o /dev/null -H Content-Type:application/json -d @$BODY http://127.0.0.1:$BENCH_PORT/v1/chat/completions"

echo "T3 done. Overhead = t3_qq_hello minus t3_curl_raw. See $RESULTS_DIR/t3_*.json"
echo "Re-run with tests/perf/t8_memory.sh's PAD_BYTES arg to sweep response size (1KB/100KB/1MB)."
