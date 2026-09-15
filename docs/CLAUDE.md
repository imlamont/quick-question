# CLAUDE.md

Guidance for AI coding assistants working on `qq`, a minimal C CLI that sends
a prompt to an OpenAI-compatible endpoint and prints the answer as plain text.
The README has user-facing documentation, and docs/TESTING.md has test results
recorded for an earlier release.

## Build and test

```sh
sudo apt install build-essential libcurl4-openssl-dev libcjson-dev   # once; Debian/Ubuntu example
make            # ./qq
make test       # build/unit + tests/run.sh (needs python3, util-linux script/setsid)
make debug      # same tests, ASan + UBSan, built in build/debug/
```

Before calling any change done:

- `make test` and `make debug` both pass.
- The build has **zero warnings** under `-Wall -Wextra -Wpedantic -Wshadow`.
- For memory-related changes, also run the suite under valgrind. The wrapper
  script for that is in docs/TESTING.md.

Report actual test output. Don't claim a result you didn't run.

## Scope

- `qq` has a single backend: OpenAI-compatible `/chat/completions` over
  libcurl. **Don't add a backend that shells out to a vendor's command-line
  assistant.** Such tools must not be used as a backend; their terms don't
  permit this use.
- A profile's `backend` key is optional and only `openai` is accepted. Anything
  else is a config error (exit 2).

## Installing, branches and versioning

- `make install` honors `DESTDIR`, `PREFIX` and `INSTALL_STRIP`. Set
  `INSTALL_STRIP` empty to install unstripped, for packaging tools that strip
  separately. It installs `bin/qq` and `share/man/man1/qq.1`.
- `main` is the platform-agnostic upstream branch. Keep distribution-specific
  files (packaging, distro CI) off it. Debian packaging lives on the `debian`
  branch, which merges `main`.
- When bumping the version, keep these in sync:
  - `QQ_VERSION` in `src/qq.h`
  - the `qq 0.1.0` assertion in `tests/run.sh`
  - the `.TH` line in `docs/qq.1`
- The man page `docs/qq.1` documents options, local tools, config keys and
  exit codes. Update it whenever any of those change.

## Layout

| File | Responsibility |
|---|---|
| `src/qq.c` | getopt (`"+hvcrwxd:p:m:s:t:"`), orchestration, output, exit codes |
| `src/config.c` | config path lookup, cJSON parse and validation, `config_set_default` (mkstemp, fsync, rename; follows symlinks, keeps file mode) |
| `src/prompt.c` | system-prompt layering, `-c` environment line, `<stdin>` wrapping, reply cleanup (`<think>` block, trimming) |
| `src/openai.c` | builds the chat request and runs the tool loop (at most `QQ_MAX_TOOL_ROUNDS`). Local tool calls go to `tools_call`, others to `mcp_call`. Time spent at approval prompts extends the deadline. |
| `src/http.c` | one reused libcurl handle, JSON POST, one deadline for the whole call, error-message extraction |
| `src/mcp.c` | LiteLLM MCP: `tools` array, `<server>-<tool>` name mapping, `POST /mcp-rest/tools/call`, result text |
| `src/tools.c` | local tools for `-r`/`-w`/`-x`: OpenAI function definitions, the prompt note, dispatch, `tools_inside_cwd` confinement, `/dev/tty` approval, previews with control characters scrubbed, stderr log |
| `src/proc.c` | child processes for `run_command`: pipes, `fork`/`execvp`, `poll` loop, per-stream output cap, deadline, SIGTERM then SIGKILL; `proc_now_ms` monotonic clock |
| `src/buf.c` | growable NUL-terminated buffer; out of memory is fatal |
| `src/qq.h` | version, limits, default timeout, round cap |
| `tests/unit.c` | CHECK/STREQ unit tests for buf, prompt, config, URLs, MCP and tool helpers |
| `tests/run.sh` | integration tests against the real binary |
| `tests/mock_openai.py` | chat and MCP mock; `model` picks the behavior: `empty`, `unauthorized`, `garbage`, `thinker`, `slow`, `tooler`, `twotools`, `looper`, the local-tool callers in `LOCAL`, anything else replies `hello from <model>`. Every request is appended to a JSON-lines log. |

## Code conventions

- **The owner prefers plain C17 with standard libc/POSIX calls** (getopt,
  fork/execvp, pipe/poll, stdio) because they find it easiest to read. Don't
  add clever macros, new layers of abstraction, extra dependencies or
  non-POSIX extensions. Readability beats brevity.
- Kernel/BSD style: tabs, declarations at the top of each block, braces on
  their own line for functions, short comments that explain *why*.
- Errors: functions take `char *err, size_t errlen` and return 0 or non-zero
  (or NULL). `main` prints `qq: <err>`. Cleanup goes through a single `out:`
  label.
- Exit codes are part of the interface and the tests assert them: `0` ok,
  `1` endpoint/runtime error, `2` usage or config error.
- Error message text is also asserted by the tests. If you change a message,
  update `tests/run.sh` or `tests/unit.c` too.
- `struct profile` and `struct config` strings point **into the cJSON tree**
  (or into argv for overrides). Nothing is copied, so the tree must outlive
  every use. Optional strings are `NULL` when absent or `""`.
- `buf` functions never return an allocation failure; they print and exit.
  `buf.data` is always NUL-terminated once it's non-NULL.
- Keep the binary small and startup fast.
- Run child processes through `proc_run`. Don't add another fork/poll loop.

## Behavior worth knowing before changing things

- **getopt `+`**: parsing stops at the first word that isn't an option, so
  `qq how do I use ls -la` keeps `-la` in the prompt.
- **stdin**: read whenever it isn't a TTY, capped at 1 MiB. A non-TTY stdin
  that never closes blocks `qq`. `-d NAME` with no prompt deliberately skips
  reading stdin.
- **Timeout** (`-t` or config `timeout`) is one deadline for the whole call:
  every chat request, every tool call, and `run_command`.
- **LiteLLM MCP loop**:
  - A profile's `mcp_servers` are sent as `{"type":"mcp","server_label":S,"server_url":"litellm_proxy/mcp/S","require_approval":"never"}`.
  - The proxy runs **one** round of tool calls itself, then returns the rest as `tool_calls` with `content: null`.
  - `qq` runs those via `POST <root>/mcp-rest/tools/call` with `{"server_id": S, "name": <bare tool>, "arguments": {...}}`. `server_id` is required, and the server name works as its value.
  - It appends the assistant turn with `tool_calls` and one `role: "tool"` message per call, then asks again.
  - Tool failures go back to the model as `error: ...` rather than aborting.
  - `arguments` arrives as a JSON string; `""` means `{}`.
- **Local tools** (`src/tools.c`):
  - The flags are `-r` (`read_file`, `list_directory`, `search_files`), `-w`
    (`write_file`, `edit_file`) and `-x` (`run_command`). They OR into a
    `TOOLS_*` mask and imply `-c`.
  - Approval:
    - Reads inside the working directory are automatic.
    - Reads outside it (checked with `realpath`, so symlinks count), and every
      write, edit and command, need `y`/`yes` on `/dev/tty`.
    - With no terminal the action is refused.
    - `edit_file` checks that `old_text` occurs exactly once *before* asking.
  - **Never add a way to approve without a terminal**, such as an environment
    variable, a config key or a "yes to all" flag, unless the owner explicitly
    asks. The tests answer prompts through a pseudo-terminal instead.
  - Anything model-controlled that is shown on the terminal or stderr goes
    through `append_preview`, which turns control characters into `?`. Keep it
    that way; it stops escape-sequence injection.
  - Limits:
    - `read_file`: 256 KB
    - `edit_file`: 4 MB
    - `list_directory`: 1000 entries
    - `search_files`: 200 matches, files up to 1 MB, depth 16, hidden entries
      skipped
    - `run_command`: 32 KB per stream
- **API keys** are never stored in config. `api_key_env` names an environment
  variable. Don't add keys to example files, tests or docs.

## Adding tests

- Unit: add a `test_*` function in `tests/unit.c` using `CHECK` / `STREQ`, and
  call it from `main`.
- Integration: use `q ARGS` or `qin DATA ARGS`, then `expect_rc`, `out_is`,
  `out_has`, `err_has` or `true_that`. To check what the mock received, use
  `req 'EXPR'` (last chat request), `system` / `prompt` (its system and user
  messages) or `last /mcp-rest/tools/call 'EXPR'`, where `d` is the logged
  request. New mock behavior means a new model name in `tests/mock_openai.py`.
- Local tools: run inside the scratch workspace `$W` with one of:
  - `qws ARGS`: detached with `setsid`, so no approval is possible.
  - `qtty 'y\n' ARGS`: on a pseudo-terminal via `script` that types the
    answers. Its output mixes stdout, stderr and the prompt text.

  To make a mock model call a local tool, add an entry to `LOCAL` in
  `tests/mock_openai.py`.
- Integration tests must not reach real services or touch files outside
  their temp directory. Use the mock. Real-endpoint checks are manual and
  recorded in docs/TESTING.md. Real LiteLLM runs may use up a daily upstream
  quota and return HTTP 429.
