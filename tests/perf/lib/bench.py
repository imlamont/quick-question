#!/usr/bin/env python3
"""Timing harness used when hyperfine isn't installed.

Runs a shell command N times (plus warmup), records wall time per run with
CLOCK_MONOTONIC-backed time.perf_counter(), and reports min/p50/mean/p99/
p99.9/stddev. Raw samples (seconds, one per line) are written next to the
summary so percentiles can be recomputed later.

Usage:
    bench.py --name t1_qq_v --warmup 50 --runs 1000 --out results/t1_qq_v \
        -- ./qq -v
"""
import argparse
import json
import math
import statistics
import subprocess
import sys
import time


def percentile(sorted_samples, p):
    if len(sorted_samples) == 1:
        return sorted_samples[0]
    k = (len(sorted_samples) - 1) * p
    f, c = math.floor(k), math.ceil(k)
    if f == c:
        return sorted_samples[int(k)]
    return sorted_samples[f] + (sorted_samples[c] - sorted_samples[f]) * (k - f)


def run_once(cmd):
    t0 = time.perf_counter()
    subprocess.run(["/bin/sh", "-c", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.perf_counter() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--warmup", type=int, default=20)
    ap.add_argument("--runs", type=int, default=200)
    ap.add_argument("--out", required=True, help="output path prefix (writes .json and .samples)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    argv = args.cmd
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        sys.exit("no command given after --")
    cmd = " ".join(argv)

    for _ in range(args.warmup):
        run_once(cmd)

    samples = [run_once(cmd) for _ in range(args.runs)]
    samples_sorted = sorted(samples)

    summary = {
        "name": args.name,
        "cmd": cmd,
        "n": len(samples),
        "min_ms": samples_sorted[0] * 1000,
        "p50_ms": percentile(samples_sorted, 0.50) * 1000,
        "mean_ms": statistics.mean(samples) * 1000,
        "p99_ms": percentile(samples_sorted, 0.99) * 1000,
        "p999_ms": percentile(samples_sorted, 0.999) * 1000,
        "max_ms": samples_sorted[-1] * 1000,
        "stddev_ms": (statistics.stdev(samples) * 1000) if len(samples) > 1 else 0.0,
    }

    with open(args.out + ".json", "w") as f:
        json.dump(summary, f, indent=2)
    with open(args.out + ".samples", "w") as f:
        for s in samples:
            f.write(f"{s * 1000:.6f}\n")

    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
