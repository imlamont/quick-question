#!/bin/bash
# T1: process-start floor. Compares `qq -v` against an empty dynamic and
# static C program so the loader/libc floor can be subtracted out.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

record_env

TMP="$RESULTS_DIR/t1"
mkdir -p "$TMP"
echo 'int main(void){return 0;}' > "$TMP/empty.c"
cc -O2 -o "$TMP/empty_dyn" "$TMP/empty.c"
cc -O2 -static -o "$TMP/empty_static" "$TMP/empty.c" 2>/dev/null || echo "static link unavailable, skipping empty_static"

bench t1_empty_dyn 50 1000 -- "$TMP/empty_dyn"
[ -x "$TMP/empty_static" ] && bench t1_empty_static 50 1000 -- "$TMP/empty_static"
bench t1_qq_v 50 1000 -- "$QQ_BIN -v"

echo "T1 done. See $RESULTS_DIR/t1_*.json"
