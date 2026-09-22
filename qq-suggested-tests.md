# qq: suggested performance tests

Goal: turn "starts in 5–10 ms" into a defensible set of numbers, find where the time goes, and produce one measured before-and-after improvement you can put on a resume and talk through in an interview.

Written 2026-09-21 from a read of `imlamont/quick-question` at the latest commit (2026-09-16). I could not build or run qq where I wrote this (no libcurl or cJSON headers, no profiling tools installed), so **nothing below is measured**. Every command is a suggestion to run.

## What the repo already has

`docs/TESTING.md` records, on WSL2:

- Stripped binary: 39,120 bytes.
- `qq -v` startup: 5.1 ms averaged over 100 runs, with an earlier reading of 10–11 ms ("WSL timing varies").
- Tests: unit and integration suites pass, including under ASan/UBSan and valgrind.

## Read this first: three things that limit the existing numbers

1. **`qq -v` does not measure a real request.** `-v` is handled in the option loop (`src/qq.c:81`) and exits before config loading and before `curl_global_init` (`src/qq.c:230`). So the 5 ms figure covers process start, the dynamic loader and libc init only. Config parsing, curl and TLS initialization, request building and response parsing are not in it.
2. **The mock server is not a fair baseline.** `tests/mock_openai.py` uses Python's `ThreadingHTTPServer`, which speaks HTTP/1.0 by default (no keep-alive), and it appends every request to a log file. Both add noise, and HTTP/1.0 hides whether qq reuses its connection across tool rounds.
3. **A mean over 100 runs hides the tail.** Report min, p50, p99, p99.9 and standard deviation, and keep the raw samples.

## Setup

- **Prefer native Linux.** WSL2 timers and scheduling add jitter, and `perf` usually does not work with the stock WSL2 kernel. If you must use WSL2, say so next to every number.
- Packages (Debian/Ubuntu): `sudo apt install build-essential libcurl4-openssl-dev libcjson-dev valgrind strace ltrace hyperfine linux-tools-generic linux-tools-common heaptrack`.
- Record with every run: `git rev-parse --short HEAD`, `uname -r`, `lscpu | head -20`, `gcc --version`, `curl --version | head -1`, CPU governor (`cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`).
- Where you can, set the governor to `performance` and pin runs to one core: `taskset -c 2 <cmd>`.
- Build variants go in separate dirs, for example `make B=build/o2`, `make B=build/o3 CFLAGS="-O3"`, so results stay comparable.
- Save raw samples (one number per line) next to each summary, so you can recompute percentiles later.

**Test server.** Write a minimal fast responder for the network tests: an HTTP/1.1 keep-alive server that returns a fixed chat-completion JSON, does no logging, and does no per-request file I/O. About 40 lines of Python with `protocol_version = "HTTP/1.1"` and a plain `http.server` handler works, or a small C or Go program if Python jitter shows up in your baseline. Keep the existing mock for the correctness tests. Point qq at it with a config file and `QQ_CONFIG`:

```json
{ "default": "bench", "timeout": 30,
  "profiles": { "bench": { "endpoint": "http://127.0.0.1:PORT/v1", "model": "test-model" } } }
```

## Tests

### T1. Process-start floor (what does `qq -v` actually cost?)

Question: how much of the 5 ms is qq, and how much is just starting any process?

```sh
# floor: an empty C program, dynamically linked, and a static one
echo 'int main(void){return 0;}' > /tmp/empty.c
cc -O2 -o /tmp/empty_dyn /tmp/empty.c
cc -O2 -static -o /tmp/empty_static /tmp/empty.c

hyperfine -N --warmup 50 --runs 1000 --export-json t1.json \
  '/tmp/empty_static' '/tmp/empty_dyn' './qq -v'
```

Record min, mean, p50, p99, stddev for each. The gap between `empty_dyn` and `qq -v` is roughly the cost of loading libcurl and libcjson and their dependencies. The gap between `empty_static` and `empty_dyn` is the dynamic loader itself.

### T2. Dynamic loader and dependency cost

```sh
ldd ./qq | wc -l                       # how many libraries get pulled in
readelf -d ./qq | grep NEEDED
LD_DEBUG=statistics ./qq -v 2>&1 | head -30     # relocation and load time
hyperfine -N --warmup 50 --runs 1000 './qq -v' 'env LD_BIND_NOW=1 ./qq -v'
strace -f -ttt -c ./qq -v              # syscall counts and time
strace -f -ttt ./qq -v 2>&1 | head -80 # timeline: where does time go before main()?
```

libcurl pulls in TLS, HTTP/2, compression and more, and that usually dominates startup. Note how many `openat` calls hit library files.

### T3. End-to-end request with no model (client-side latency)

Question: with the model out of the picture, how long does one full `qq "hi"` take, and how much of that is qq?

```sh
# start your fast test server, then:
export QQ_CONFIG=/path/to/bench-config.json
hyperfine -N --warmup 50 --runs 1000 --export-json t3.json \
  './qq hello' \
  "curl -s -o /dev/null -H 'Content-Type: application/json' -d @body.json http://127.0.0.1:PORT/v1/chat/completions"
```

Use a request body that matches what qq sends. The `-L` log shows it, or read `src/openai.c`. **Overhead = qq minus curl.** That gap is the number worth reporting: it is the latency your code adds. Repeat with the response body at 1 KB, 100 KB and 1 MB (T8).

### T4. Phase breakdown inside one request

Question: which phase is slow?

qq's `-L` log uses `CLOCK_REALTIME` at millisecond resolution (`src/log.c:36`), which is too coarse. Add optional nanosecond `CLOCK_MONOTONIC` trace points, for example behind an env var such as `QQ_TRACE=1`, at:

1. `main()` entry
2. config loaded
3. after `curl_global_init`
4. request JSON built
5. just before `curl_easy_perform`
6. after `curl_easy_perform`
7. response parsed
8. output printed

You can also get network phases from libcurl for free after `curl_easy_perform`, using `curl_easy_getinfo` with `CURLINFO_NAMELOOKUP_TIME`, `CURLINFO_CONNECT_TIME`, `CURLINFO_APPCONNECT_TIME`, `CURLINFO_PRETRANSFER_TIME`, `CURLINFO_STARTTRANSFER_TIME` and `CURLINFO_TOTAL_TIME`. Print them in the trace. Produce a table of phase versus median and p99 over 1,000 runs.

### T5. Cost of `curl_global_init`

`qq.c` calls `curl_global_init(CURL_GLOBAL_DEFAULT)`. Time it in isolation with the T4 trace points. Then test whether other flags change anything for `http://` endpoints. Modern libcurl may treat the SSL flag as a no-op, so treat this as a measurement, not an assumption. Do not change flags in a way that breaks `https://` endpoints, and re-run the suite if you do.

### T6. Connection reuse across tool rounds

qq reuses one `curl_easy` handle (`src/http.c`, `http_init`), so a multi-request run should reuse its TCP connection if the server keeps it alive.

```sh
# with the mock model "tooler" (two requests per run) against an HTTP/1.1 keep-alive server:
strace -f -e trace=connect,socket -o conn.txt ./qq -p tools "search for something"
grep -c 'connect(' conn.txt          # expect 1 with keep-alive, 2 without
```

Run it against the HTTP/1.0 mock and the HTTP/1.1 test server and compare. If you see two connects against a keep-alive server, that is a finding.

### T7. Tool loop latency

For `-x` (run commands) and `-r`/`-w`: measure the time from a child process exiting to the result being sent in the next request, using the T4 trace points around the `poll()` loops in `src/proc.c` and `src/tools.c`. Compare `fork`/`exec` against `posix_spawn` or `vfork`. Test with a trivial command (`true`) and with one that prints 32 KB (the `OUTPUT_MAX` cap in `tools.c`).

### T8. Large responses and memory

The response cap is 16 MB (`RESPONSE_MAX` in `src/http.c`) and `buf_reserve` doubles capacity (`src/buf.c`).

```sh
/usr/bin/time -v ./qq hello 2> time.txt    # peak RSS
valgrind --tool=massif --massif-out-file=massif.out ./qq hello && ms_print massif.out | head -60
heaptrack ./qq hello                        # allocation counts and peak
ltrace -c -e realloc+malloc+free ./qq hello # call counts (slow, use once)
```

Serve responses of 1 KB, 100 KB, 1 MB and 10 MB from the test server. Record wall time, peak RSS and number of `realloc` calls. Check whether `buf_reserve` reallocs more often than you expect for a known response size.

### T9. Where the CPU time goes

```sh
# hot spots over many runs against the test server (perf may not work on WSL2; use callgrind there)
perf record -F 999 -g -o perf.data -- sh -c 'for i in $(seq 500); do ./qq hello >/dev/null; done'
perf report --stdio | head -60
perf stat -r 200 -d ./qq hello              # cycles, instructions, cache misses, context switches, page faults

valgrind --tool=callgrind --callgrind-out-file=cg.out ./qq hello
callgrind_annotate cg.out | head -60        # deterministic instruction counts, no timing noise
```

Optionally draw a flamegraph with Brendan Gregg's FlameGraph scripts. Expect time in the loader, libcurl or OpenSSL initialization, and cJSON. If your own code shows up (config parsing, `log_escape`, JSON printing), that is worth a note.

### T10. Build and link experiments

Measure startup (T1 method) and binary size for each:

| Variant | How |
|---|---|
| Baseline | `make` (`-O2`) |
| `-O3` | `make CFLAGS=-O3` |
| `-Os` | `make CFLAGS=-Os` |
| LTO | `make CFLAGS="-O2 -flto" LDFLAGS=-flto` |
| Prelink-style | `LD_BIND_NOW` on and off (T2) |
| Fewer libraries | Build curl with only HTTP(S), or link a static libcurl (requires a static build of libcurl and its dependencies) |

Only keep a variant if the gain is larger than run-to-run noise (check with the stddev from hyperfine).

### T11. Tail latency under load (optional)

Run the T3 test while another process saturates a different core, and again while it saturates the same core. Report p99 and p99.9. This shows how qq behaves when the machine is busy, which is the part trading-firm interviewers care about.

## How to report

For each test keep a table with: build (commit, flags), machine (CPU, kernel, WSL2 or native), N, min / p50 / mean / p99 / p99.9 / stddev, and a link to the raw samples. Save the tables in `docs/PERF.md` in the repo, next to `docs/TESTING.md`.

Rules for the numbers:

- Compare like with like: same machine, same governor, same server, back to back.
- Report "before and after" only when the change is larger than the noise.
- Do not put a number on a resume that you cannot re-run.
- Say what you did not measure. That is stronger than a bare claim.

## Best single result to aim for

One before/after, for example:

- T1 and T2 say the loader is most of startup, so you try a leaner link and cut `qq -v` and a real request by X ms (p50) and Y ms (p99), or
- T3 and T4 show the client adds Z µs over raw `curl`, with a phase table saying where it goes, or
- T6 finds a connection-reuse gap and you fix it.

Then extend the resume bullet with the number, for example: "profiled startup and per-request overhead with perf, strace and callgrind against a local server; reduced ___ from ___ ms to ___ ms (p50, 1,000 runs)". Fill the blanks only with results you measured.

## Interview prep from this project

Be ready to explain, from memory: why a single `curl_easy` handle is reused, what `poll()` is doing in `proc.c` and `tools.c`, why `buf_reserve` doubles, how the 16 MB response cap and the per-stream output cap work, what the sanitizer builds caught (your TESTING.md notes one ASan-found bug), and what you would change if the tool needed streaming output. Interviewers at firms like SIG will ask about the code line by line.
