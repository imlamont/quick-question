#!/bin/bash
# T6: does qq reuse its TCP connection across tool rounds? Expect 1 connect()
# against a keep-alive (HTTP/1.1) server, 2 against the HTTP/1.0 mock.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

if ! have strace; then
	echo "strace not installed, cannot run T6" >&2
	exit 1
fi

echo "=== against fast HTTP/1.1 keep-alive server ==="
start_fast_server
trap stop_fast_server EXIT
QQ_CONFIG="$QQ_CONFIG_BENCH" strace -f -e trace=connect,socket -o "$RESULTS_DIR/t6_http11.txt" \
	"$QQ_BIN" -r -p lister "list the directory" || true
n11=$(grep -c 'connect(' "$RESULTS_DIR/t6_http11.txt" || true)
echo "connect() calls (HTTP/1.1 server): $n11"
stop_fast_server
trap - EXIT

echo "=== against tests/mock_openai.py (HTTP/1.0) ==="
MOCK_LOG="$RESULTS_DIR/t6_mock.log"
python3 "$REPO_DIR/tests/mock_openai.py" "$MOCK_LOG" > "$RESULTS_DIR/t6_mock_port.txt" 2>/dev/null &
MOCK_PID=$!
sleep 0.5
MOCK_PORT=$(cat "$RESULTS_DIR/t6_mock_port.txt")
cat > "$RESULTS_DIR/t6_mock_config.json" <<EOF
{"default":"g/lister","timeout":30,"gateways":{"g":{"endpoint":"http://127.0.0.1:$MOCK_PORT/v1","tools":["read"],"models":{"lister":{}}}}}
EOF
QQ_CONFIG="$RESULTS_DIR/t6_mock_config.json" strace -f -e trace=connect,socket -o "$RESULTS_DIR/t6_http10.txt" \
	"$QQ_BIN" -r "list the directory" || true
kill "$MOCK_PID" 2>/dev/null || true
n10=$(grep -c 'connect(' "$RESULTS_DIR/t6_http10.txt" || true)
echo "connect() calls (HTTP/1.0 mock): $n10"

echo "T6 done. HTTP/1.1: $n11 connect(s), HTTP/1.0: $n10 connect(s)."
[ "$n11" -gt 1 ] && echo "FINDING: expected 1 connect() with keep-alive, got $n11 -- see $RESULTS_DIR/t6_http11.txt"
