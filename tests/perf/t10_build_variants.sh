#!/bin/bash
# T10: build/link experiments. Builds each variant into its own out-of-tree
# dir (so they never mix, per the Makefile's B= convention), then measures
# startup (T1 method) and binary size for each.
set -euo pipefail
cd "$(dirname "$0")"
source lib/common.sh

variant() {
	local name="$1" b="$2"
	shift 2
	echo "=== $name ==="
	make -C "$REPO_DIR" B="$b" BIN="$b/qq" "$@" >/dev/null
	size=$(stat -c%s "$REPO_DIR/$b/qq")
	strip -o "$RESULTS_DIR/t10_${name}_stripped" "$REPO_DIR/$b/qq"
	stripped_size=$(stat -c%s "$RESULTS_DIR/t10_${name}_stripped")
	echo "size: $size bytes (unstripped), $stripped_size (stripped)" | tee "$RESULTS_DIR/t10_${name}_size.txt"
	QQ_BIN="$REPO_DIR/$b/qq" bench "t10_${name}" 50 1000 -- "$REPO_DIR/$b/qq -v"
}

variant baseline build/t10-baseline
variant o3 build/t10-o3 CFLAGS=-O3
variant os build/t10-os CFLAGS=-Os
variant lto build/t10-lto CFLAGS="-O2 -flto" LDFLAGS=-flto

echo "T10 done. LD_BIND_NOW comparison is covered by t2_loader.sh."
echo "Fewer-libraries / static-libcurl variants need a custom libcurl build; not automated here."
echo "Keep a variant only if its gain exceeds the baseline's stddev (see t10_*_stddev in the JSON)."
