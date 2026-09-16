# Test results

Results for commit `0fda403` (the first commit), recorded on 2026-09-15.

"Fresh" results come from a clean rerun at that commit. "Earlier" results come
from runs earlier the same day, against the same source code; after those runs
only the README, LICENSE and .gitignore were added.

That commit also contained a second backend that has since been removed. Its
results and test descriptions are left out below; the test counts are the
totals recorded at the time.

## Environment

| | |
|---|---|
| OS | Ubuntu 26.04.1 LTS on WSL2 (Linux 5.15.167.4-microsoft-standard-WSL2) |
| Compiler | gcc 15.2.0, GNU Make 4.4.1 |
| Libraries | libcurl 8.18.0 (OpenSSL build), cJSON 1.7.19 |
| Tools | valgrind 3.26.0, gdb 17.1, Python 3.14.4 (test mock only) |

## Summary

| Check | Result | When |
|---|---|---|
| Clean build (`make`, `-Wall -Wextra -Wpedantic -Wshadow`) | 0 warnings, 0 errors | fresh |
| Unit tests (`build/unit`) | 90 / 90 checks pass | fresh |
| Integration tests (`tests/run.sh`) | 95 / 95 pass | fresh |
| ASan + UBSan build (`make debug`), unit + integration | 90 / 90 and 95 / 95 pass, no sanitizer reports | fresh |
| Unit tests under valgrind | 0 errors, all heap blocks freed | fresh |
| Integration suite with every `qq` call under valgrind | 95 / 95 pass, 56 valgrind runs, 0 error logs | fresh |
| Real LiteLLM endpoint, including the MCP tool loop | all runs correct | earlier |
| Real LiteLLM endpoint, rerun | upstream HTTP 429 (daily free-model quota used up); `qq` reported it and exited 1 | fresh |

## Build

```
cc -std=c17 -Wall -Wextra -Wpedantic -Wshadow -D_XOPEN_SOURCE=700 -O2 ...
build rc=0 warnings=0 errors=0
```

| | |
|---|---|
| Binary size, unstripped | 46,464 bytes |
| Binary size, stripped (`make install` strips) | 39,120 bytes (38.2 KB) |
| Startup, `qq -v` | 5.1 ms per run, averaged over 100 runs (an earlier measurement gave 10–11 ms; WSL timing varies) |
| Linked libraries | `libcurl.so.4`, `libcjson.so.1` (plus libc) |

## Unit tests — `tests/unit.c`

```
unit: 90 checks, 0 failed
```

| Area | What is checked |
|---|---|
| `buf` | growth to 10,000 bytes, `appendf`, `steal`, `str_trim`; a buffer that is reserved but never written must still hold an empty string (this is the regression test for the bug ASan found) |
| Prompt | system-prompt ordering and skipping of empty parts, `<stdin>` wrapping with and without a trailing newline, removal of a leading `<think>` block while keeping one that is unterminated or mid-text |
| URLs | chat URL joining with trailing slashes or an existing `/chat/completions`; MCP call URL derived by dropping `/chat/completions` and `/v1` |
| MCP | `<server>-<tool>` mapping where the longest server name wins and near-misses are rejected; result text from joined text items or `structuredContent`; the `tools` array JSON |
| Config | valid parse and defaults; unknown profile (lists the available names); no profile selected; invalid JSON with its line number; wrong types; unknown backend; missing endpoint or model; bad `max_tokens`; every `mcp_servers` validation case |

## Integration tests — `tests/run.sh`

```
tests: 95 passed, 0 failed
```

These tests run the real `qq` binary against `tests/mock_openai.py`, an
OpenAI-compatible chat server with a LiteLLM `/mcp-rest/tools/call` endpoint;
the requested model picks its behavior.

| Area | Cases |
|---|---|
| CLI | `-h`, `-v`, no prompt (exit 2), bad flag, bad `-t`, `-p` without a value, unknown profile, missing config, invalid JSON |
| stdin | pipe plus prompt, pipe only, 300 KB round trip, more than 1 MiB rejected (exit 2) |
| Requests | request path, Bearer header present or absent, roles and content, `temperature` / `max_tokens` / `stream`, no `tools` key without `mcp_servers`, `<think>` stripped, HTTP 401 message shown, non-JSON body, connection refused, request timeout |
| `-L` debug log | a normal run logged end to end (`command` first, then `start`, `profile`, request body, `response`, `answer`, `exit`); every line timestamped to the millisecond; the API key absent; a second run appending; `tool-round`, `tool-call`, `approval yes/no/no-terminal`, `tool-result`, `tool-repeat`, `mcp-call` and `mcp-result`; control characters escaped; an unopenable log failing at startup (exit 2); an argument a shell would quote being quoted in the `command` line; `-h` and the usage line mentioning it |
| `-y` | refused (exit 2) on a profile without `allow_danger`, doing nothing; with it, a write, a command and an outside read all carried out detached from any terminal (`setsid`); the question still shown on stderr with "Allowed by -y."; the same profile still refusing without `-y`; both log lines |
| profile switches | `"tools": false` refusing `-r`, `-w`, `-x` and `-rwx` (exit 2) while the profile still answers a plain prompt with no `tools` in the body; `"mcp": false` hiding the profile's `mcp_servers` while `-r` still works; the same profile without the switch still offering MCP |
| empty tool paths | `{"path":""}` for `list_directory` and `search_files` falling back to the current directory, and `read_file` reporting a missing path |
| repeated tool calls | a model looping on one `write_file`: asked once, the first write happens, the run ends with "kept repeating tool calls"; a denial holds so nothing is written and nothing is asked twice; a repeat beside a fresh call still reaches the round cap but is asked once; a looping `read_file` stops the same way; a read repeated after an edit is really redone and sees the new content |
| MCP tool loop | `tools` array sent; the tool-call request body (`server_id`, bare `name`, parsed `arguments`); history roles and matching `tool_call_id`; exactly 3 requests for one tool round; tool errors fed back to the model (unknown tool, `isError`, HTTP 404 from the server, arguments that aren't JSON); an 8-round limit (exit 1) |
| `-d` | rewrites `default`, follows a symlink, keeps file mode 600, leaves no temp files; the new default is used; an unknown profile leaves the file byte-for-byte identical; `-p` wins over a default just set in the same call; the default persists |

## Sanitizers — `make debug`

```
build/debug/unit          unit: 90 checks, 0 failed
tests/run.sh build/debug/qq   tests: 95 passed, 0 failed
```

This build uses `-fsanitize=address,undefined`. LeakSanitizer was confirmed
active in this environment: a deliberately leaking test program was reported.

ASan found one real bug during development. `buf_reserve` did not
NUL-terminate a new allocation, so a child process that wrote nothing gave
back uninitialized memory. The fix is in `src/buf.c`, and the unit test above
covers it.

## Valgrind

**Unit tests (fresh):**

```
unit: 90 checks, 0 failed
in use at exit: 0 bytes in 0 blocks
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts
```

**Integration suite with each `qq` call under valgrind (fresh):** 95 / 95
tests pass across 56 valgrind runs, with 0 non-empty error logs (33 s). Each
call ran with
`--leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99`.
Any memory error or real leak would change the exit code and fail the test.

**Real endpoint (earlier, same source):**

| Run | Errors | Definitely / indirectly / possibly lost | Still reachable |
|---|---|---|---|
| LiteLLM plain call (`say ok` gave `ok`) | 0 | 0 / 0 / 0 | 56 bytes |
| LiteLLM MCP tool loop (answered `7.2.6`) | 0 | 0 / 0 / 0 | 56 bytes |
| LiteLLM call hitting HTTP 429 (fresh) | 0 | 0 / 0 / 0 | 56 bytes |

The 56 reachable bytes belong to OpenSSL, not `qq`. They are a lock that
OpenSSL creates once (`CRYPTO_THREAD_lock_new` inside `pthread_once`) during
`curl_global_init()` and never frees by design.

## End to end: LiteLLM proxy (OpenAI-compatible)

The proxy ran at `http://localhost:4000`, serving the models
`nemotron-3.5-lightning`, `nemotron-3-super` and `nemotron-3-ultra`, with the
`searxng_mcp` MCP server.

**Earlier (same source):**

| Command | Output | Exit | Time |
|---|---|---|---|
| `qq what is the capital of france` | `Paris` | 0 | 20.2 s (model latency; the same prompt through plain `curl` took 3.7 s and 8.4 s) |
| `printf 'int main(void){ return 0; }\n' \| qq -c explain this in one sentence` | ``This is a minimal C program that defines `main`, takes no arguments, and exits successfully with return code 0.`` | 0 | — |
| `qq -m nemotron-3-super what is 17 times 23` | `391` | 0 | 1.8 s |
| `qq -m nemotron-3-ultra what is 17 times 23` | `391` | 0 | 25.1 s |
| wrong API key | `qq: HTTP 400: No connected db.` (the proxy's own message) | 1 | — |
| tool loop: `search the web for today's top news headline…` | one-sentence headline | 0 | 12.0 s |

**Tool loop traced with gdb (earlier).** Breakpoints on `http_post_json` and
`mcp_call` printed a line each time they were hit (see "Reproducing" below).
The prompt asked for two separate web searches (latest Linux kernel, latest
Rust). Two runs gave identical traces:

```
[gdb] http_post_json      # chat request; the proxy runs the first tool round itself
[gdb] mcp_call            # the proxy hands back a second tool call; qq runs it
[gdb] http_post_json      #   POST /mcp-rest/tools/call
[gdb] http_post_json      # chat request with the tool result -> final answer
```

The answers were `7.2` / `1.98.1` and `7.2.6` / `1.98.1`. The Rust version
matches the installed toolchain.

**Fresh rerun:** every LiteLLM request returned an upstream rate-limit error:

```
qq: HTTP 429: litellm.RateLimitError: RateLimitError: OpenrouterException - {"error":{"message":"Rate limit exceeded: free-models-per-day. ...
```

The OpenRouter free-model quota for the day (50 requests) had been used up by
testing. `qq` reported the error and exited 1 within about 5 s; valgrind was
still clean. The successful LiteLLM results above can be re-recorded once the
quota resets.

## Reproducing

```sh
make test                      # unit + integration
make debug                     # the same with ASan + UBSan

# unit tests under valgrind
valgrind --leak-check=full --error-exitcode=99 build/unit

# integration suite with every qq call under valgrind
cat > /tmp/qq-vg <<'EOF'
#!/bin/sh
exec valgrind -q --leak-check=full --errors-for-leak-kinds=definite,indirect \
  --error-exitcode=99 --log-file=/tmp/qq-vg.%p.log "$PWD/qq" "$@"
EOF
chmod +x /tmp/qq-vg && tests/run.sh /tmp/qq-vg

# watch the tool loop's requests with gdb (needs an mcp_servers profile)
gdb -q -batch -ex 'set startup-with-shell off' \
  -ex 'dprintf http_post_json,"POST\n"' -ex 'dprintf mcp_call,"tool call\n"' \
  -ex run --args ./qq "do two separate web searches: ..." </dev/null
```

Note that `$PWD` is expanded when the wrapper script is written, so create it
from the repository root.

## Not covered

- A real **HTTPS** endpoint. The TLS path is libcurl's own; only plain HTTP to
  localhost was exercised.
- MCP gateways other than LiteLLM with `searxng_mcp`.
- Non-Linux platforms. The code uses POSIX calls plus GNU `getopt`'s `+` prefix.
