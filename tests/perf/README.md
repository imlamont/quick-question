# qq performance test harness

Implements the tests proposed in `qq-suggested-tests.md` (repo root). Goal:
turn "starts in 5-10 ms" into real, re-runnable numbers, find out where
qq's time actually goes, and get one measured before/after improvement.

Nothing here has been run yet on this machine — run it and fill in
`docs/PERF.md` with what you get. This machine currently has `strace` and
`valgrind` but not `hyperfine` or `perf`; scripts fall back automatically
(see "What's missing" below) but the fallbacks are noisier, so install the
real tools if you can (`sudo apt install hyperfine linux-tools-generic
linux-tools-common heaptrack ltrace`) before trusting a resume-worthy number.

## The tests, in one line each

| # | Script | Question |
|---|---|---|
| T1 | `t1_process_start.sh` | How much of `qq -v`'s time is just starting any process, vs. qq itself? Compares against an empty static/dynamic C binary. |
| T2 | `t2_loader.sh` | How much does the dynamic loader + libcurl/libcjson cost? `ldd`, `LD_DEBUG=statistics`, `LD_BIND_NOW`, strace timeline. |
| T3 | `t3_e2e.sh` | With a fast local server, how long does a real `qq hi` take vs. raw `curl` hitting the same endpoint? The gap is qq's own overhead. |
| T4 | *(not scripted — needs source changes)* | Phase breakdown inside one request (config load, curl init, request build, transfer, parse, print). Needs `QQ_TRACE`-style `CLOCK_MONOTONIC` trace points added to `src/qq.c` and `CURLINFO_*` timing pulled from libcurl. See "Not yet built" below. |
| T5 | *(not scripted — needs T4's trace points)* | Isolated cost of `curl_global_init`. |
| T6 | `t6_connection_reuse.sh` | Does qq reuse its TCP connection across tool rounds? Counts `connect()` via strace against an HTTP/1.1 keep-alive server and the HTTP/1.0 `tests/mock_openai.py`. |
| T7 | *(not scripted — needs source changes)* | Tool-loop latency (`-x`, `-r`/`-w`) from child-exit to next request, `fork`/`exec` vs `posix_spawn`. Needs the same trace-point instrumentation as T4, around `proc.c`/`tools.c`'s `poll()` loops. |
| T8 | `t8_memory.sh` | Wall time, peak RSS, and allocation counts across 1 KB/100 KB/1 MB/10 MB responses. Uses `/usr/bin/time -v`, valgrind massif, and heaptrack/ltrace if installed. |
| T9 | `t9_cpu.sh` | Where CPU time goes: `perf` if installed, else `valgrind --callgrind` (deterministic instruction counts, works without `perf`). |
| T10 | `t10_build_variants.sh` | Startup/size for `-O2` (baseline) vs `-O3`/`-Os`/LTO, each in its own `build/t10-*` dir. |
| T11 | `t11_tail_latency.sh` | (Optional) p99/p99.9 with a CPU hog on a different core vs. the same core, via `taskset`. |

## Layout

```
tests/perf/
  README.md              this file
  bench-config.json       qq config pointing at the local test server (gateway "bench": models test-model, lister)
  lib/common.sh            shared helpers: bench(), start/stop_fast_server(), record_env()
  lib/bench.py              hyperfine fallback: N runs, min/p50/mean/p99/p99.9/stddev + raw samples
  server/fast_server.py    HTTP/1.1 keep-alive responder, no logging, no per-request I/O
  t*.sh                    one script per test, runnable standalone or via run-all.sh
  results/                 raw samples + JSON summaries land here (gitignored-worthy, local only)
```

`tests/mock_openai.py` (existing, HTTP/1.0, logs every request) is kept as-is
for the correctness suite (`make test`) and reused by T6 specifically to
compare against the fast server.

## Running

```sh
cd tests/perf
./run-all.sh                 # T1, T2, T3, T6, T8, T9, T10
WITH_T11=1 ./run-all.sh       # also run T11
./t3_e2e.sh                  # or run one test at a time
```

Each script writes JSON summaries and raw sample files to `results/`, plus
`results/env.txt` (commit, kernel, CPU, compiler, governor) so numbers stay
tied to the machine and build they came from.

For best numbers: run on native Linux (not WSL2 — its timers and scheduler
add jitter, and `perf` usually doesn't work there at all), set the CPU
governor to `performance`, and consider `taskset -c N` to pin runs to one
core. The scripts don't do this for you; set it up first if you want
tighter numbers.

## Not yet built: T4/T5/T7 trace points

These three need actual instrumentation in `src/qq.c`, `src/proc.c` and
`src/tools.c` — a `QQ_TRACE=1` env var that prints `CLOCK_MONOTONIC`
timestamps at the phase boundaries listed in `qq-suggested-tests.md`, plus
printing libcurl's `CURLINFO_NAMELOOKUP_TIME` etc. after
`curl_easy_perform`. That's a source change to the main binary, not a test
script, so it wasn't added here — ask for it as a follow-up if you want the
phase breakdown; it's the test most likely to explain *why* T1-T3 look the
way they do.

## How to report

For each test, record: build (commit + flags), machine (CPU, kernel, native
vs WSL2), N, min/p50/mean/p99/p99.9/stddev, and a pointer to the raw sample
file in `results/`. Put the tables in `docs/PERF.md`, next to
`docs/TESTING.md`. Only report a before/after if the change is bigger than
the noise (compare against stddev), and say plainly what you didn't
measure — `qq-suggested-tests.md` has the full rationale.
