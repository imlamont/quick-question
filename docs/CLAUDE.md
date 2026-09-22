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
- There is no `backend` config key any more; OpenAI-compatible is the only
  kind, so nothing selects it. Do not resurrect a `backend` key without being
  asked.

## Installing, branches and versioning

- `make install` honors `DESTDIR`, `PREFIX` and `INSTALL_STRIP`. Set
  `INSTALL_STRIP` empty to install unstripped, for packaging tools that strip
  separately. It installs `bin/qq` and `share/man/man1/qq.1`.
- `main` is the platform-agnostic upstream branch. Keep distribution-specific
  files (packaging, distro CI) off it. Debian packaging lives on the `debian`
  branch, which merges `main`.
- When bumping the version, keep these in sync:
  - `QQ_VERSION` in `src/qq.h`
  - the version assertion in `tests/run.sh`
  - the `.TH` line in `docs/qq.1`
  - the `.deb` filename in the README's download example
- The man page `docs/qq.1` documents options, local tools, config keys and
  exit codes. Update it whenever any of those change.

## Layout

| File | Responsibility |
|---|---|
| `src/qq.c` | getopt (`"+hlvcrwxyd:p:s:t:L:"`), orchestration, output, exit codes |
| `src/config.c` | config path lookup, cJSON parse and validation of gateways and their models, the tools/mcp_servers/allow_danger narrowing rules, `config_profile` (selection), `config_list` (`-l`), `config_set_default` (mkstemp, fsync, rename; follows symlinks, keeps file mode; saves the qualified `gateway/model`) |
| `src/prompt.c` | system-prompt layering, `-c` environment line, `<stdin>` wrapping, reply cleanup (`<think>` block, trimming) |
| `src/openai.c` | builds the chat request and runs the tool loop (at most `QQ_MAX_TOOL_ROUNDS`). Local tool calls go to `tools_call`, others to `mcp_call`. Time spent at approval prompts extends the deadline. Keeps a `struct history` of local calls already answered, so repeats are replayed instead of redone. |
| `src/http.c` | one reused libcurl handle, JSON POST, one deadline for the whole call, error-message extraction |
| `src/mcp.c` | LiteLLM MCP: `tools` array, `<server>-<tool>` name mapping, `POST /mcp-rest/tools/call`, result text |
| `src/tools.c` | local tools for `-r`/`-w`/`-x`: OpenAI function definitions, the prompt note, dispatch, `tools_inside_cwd` confinement, `/dev/tty` approval, previews with control characters scrubbed, stderr log |
| `src/proc.c` | child processes for `run_command`: pipes, `fork`/`execvp`, `poll` loop, per-stream output cap, deadline, SIGTERM then SIGKILL; `proc_now_ms` monotonic clock |
| `src/log.c` | the `-L` debug log: one timestamped line per event, escaping, flushed per entry. A no-op until `log_open` succeeds, which is what lets the logging calls sit anywhere without a guard. |
| `src/buf.c` | growable NUL-terminated buffer; out of memory is fatal |
| `src/qq.h` | version, limits, default timeout, round cap |
| `tests/unit.c` | CHECK/STREQ unit tests for buf, prompt, config, URLs, MCP, tool and log helpers |
| `tests/run.sh` | integration tests against the real binary |
| `tests/mock_openai.py` | chat and MCP mock; `model` (the id qq sends, not a config key) picks the behavior: `empty`, `unauthorized`, `garbage`, `thinker`, `slow`, `tooler`, `twotools`, `looper`, `writeloop`, `readloop`, `mixloop`, `rereader`, the local-tool callers in `LOCAL`, anything else replies `hello from <model>`. Every request is appended to a JSON-lines log. |

- **Debug log** (`src/log.c`, `-L`):
  - `log_open` appends, and every other entry point does nothing until it has
    succeeded, so call sites need no `if (logging)` guard. `log_on()` exists only
    to skip work the log alone would need, such as escaping a large body.
  - `log_command` opens the log with the command line as invoked, each argument
    shell-quoted only when it needs it, so a log read back later starts with
    what was asked for.
  - One event per line: `stamp()` writes a local timestamp to the millisecond
    plus the UTC offset, then the event name and its details. Keep it that way --
    the format's whole value is that `grep` and `cut` work on it.
  - Anything that came from the model, a file or a command goes through
    `log_escape`, which escapes backslashes, newlines and control characters.
    That keeps an entry on one line and stops escape sequences reaching the
    terminal the log is later read on, the same reason `tools.c` has
    `append_preview`.
  - Entries are flushed as they are written, so an interrupted run still has a
    usable log.
  - **Never log a request header.** The body is logged in full, and the API key
    is only ever a header, which is what keeps it out of the file. `http.c` logs
    `data` (the serialized body) and nothing else about the request.
  - The log is otherwise as sensitive as the conversation: prompts, file contents
    and command output are all in it verbatim. Say so wherever it is documented.

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
  every use. Optional strings are `NULL` when absent or `""`. `struct profile`
  is now the result of resolving one model on one gateway with everything it
  inherits already applied -- there is no other struct for "the config entry
  as written"; `parse_gateway`/`parse_model` in `config.c` do the resolving.
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
- **Timeout** (`-t`, else the model's, else its gateway's, else the top-level
  `timeout`) is one deadline for the whole call: every chat request, every
  tool call, and `run_command`.
- **Gateways and models** (`config.c`): the config's `gateways` object holds
  named gateways, each with a `models` object of its own. `struct gateway` in
  `config.c` is the checked, in-memory form of one gateway (endpoint, key,
  tuning defaults, and what it grants); it is not part of the public header,
  only `struct profile` is, since only a fully-resolved model is ever handed
  to the rest of the program.
  - A gateway grants **nothing** it doesn't list: `tools` (an array of
    `"read"`, `"write"`, `"exec"`) and `mcp_servers` both default to none, and
    `allow_danger` defaults to false. This is the opposite of the old
    `profiles` format, where omitting `tools`/`mcp` meant "on".
  - A model may **narrow** what its gateway grants -- a subset of `tools`, a
    subset of `mcp_servers`, `allow_danger: false` to opt out -- but never
    widen it. Listing something the gateway doesn't grant is a config error
    (exit 2) caught by `parse_model`, not something silently dropped or
    silently ignored.
  - Validation is eager for structure: every gateway and every model is
    checked at load (`validate()` in `config.c`), so a mistake in a gateway
    that isn't selected is still reported. Only the *selected* model's
    `api_key_env` has to actually be set; other gateways' keys don't need to
    exist in the environment.
  - Selection (`-p`, `-d`, and the config's `default`) takes
    `[gateway/]model`. `select_model()` tries a qualified name first (splitting
    on the first `/` only when what precedes it is a real gateway name, so a
    model id like `meta/llama-3` still works bare), then a bare model name
    (unique across gateways, or it's an error naming the candidates) or a bare
    gateway name (its `default_model`, or the first model listed).
- **LiteLLM MCP loop**:
  - A model's effective `mcp_servers` (its own list if it set one, else its
    gateway's) are sent as
    `{"type":"mcp","server_label":S,"server_url":"litellm_proxy/mcp/S","require_approval":"always"}`.
  - **`require_approval` must stay `always`.** With `never` the gateway executes
    tool calls itself, and it does not stop at MCP tools: it also tried to run
    `write_file`, `read_file` and `run_command`, failed, and fed the model
    `Error executing tool: 'write_file'` for calls `qq` then carried out. The
    model reasoned from that phantom failure and changed its behaviour -- in one
    logged run it wrote different file content than it had first chosen. With
    `always` the gateway returns every call and `qq` runs it.
  - The gateway therefore runs no tool calls at all; every MCP call goes through
    `mcp_call` and `POST /mcp-rest/tools/call`, and through the repeat history
    on the way.
  - `qq` runs those via `POST <root>/mcp-rest/tools/call` with `{"server_id": S, "name": <bare tool>, "arguments": {...}}`. `server_id` is required, and the server name works as its value.
  - It appends the assistant turn with `tool_calls` and one `role: "tool"` message per call, then asks again.
  - Tool failures go back to the model as `error: ...` rather than aborting.
  - `arguments` arrives as a JSON string; `""` means `{}`.

- **Local tools** (`src/tools.c`):
  - The flags are `-r` (`read_file`, `list_directory`, `search_files`), `-w`
    (`write_file`, `edit_file`) and `-x` (`run_command`). They OR into a
    `TOOLS_*` mask and imply `-c`.
  - `qq.c` refuses a flag whose category isn't in the resolved model's
    effective `tools` (exit 2, before anything is sent), because the flag was
    typed on purpose. Keep that asymmetry with MCP: a model with no
    `mcp_servers` just gets none offered, but an explicit `-r`/`-w`/`-x` the
    model isn't allowed is a surprise worth erroring on.
  - Approval:
    - Reads inside the working directory are automatic.
    - Reads outside it (checked with `realpath`, so symlinks count), and every
      write, edit and command, need `y`/`yes` on `/dev/tty`.
    - With no terminal the action is refused, and so is a prompt left
      unanswered for `APPROVAL_TIMEOUT_MS` (120 s). Waiting doesn't count
      against `-t`, so without that limit an unattended run would hang.
    - `edit_file` checks that `old_text` occurs exactly once *before* asking.
    - An empty `"path"` is treated as no path at all, so `list_directory` and
      `search_files` fall back to `.` as their descriptions promise. Models do
      send `{"path":""}`, and it used to reach `opendir("")` as `cannot list :`.
    - `tools_call` sets `*refused` when a prompt was answered no, timed out or
      could not be shown, which is what `openai.c` keys its history on.
  - Repeated calls (`struct history` in `src/openai.c`): every call, local or
    MCP, is recorded under `name\x1farguments`, and an identical later call is
    answered from that record -- not run, not put to the user again -- so a
    looping model cannot write, read or run the same thing twice, cannot put the
    same query to an MCP server twice, and a refusal holds for the whole run. Entries for reads that succeeded are flagged, and
    `history_forget_reads` drops them as soon as a write, edit or command is
    carried out, so a read that follows a change is really redone; a refusal is
    never flagged, so it always sticks, and neither is an MCP result, which runs
    on the server and is not ours to invalidate from here. `run_tools` returns how many calls it
    really carried out, and a round of nothing but repeats ends the run with an
    error rather than spinning to `QQ_MAX_TOOL_ROUNDS`.
  - **Never add a way to approve without a terminal**, such as an environment
    variable, a config key or a "yes to all" flag, unless the owner explicitly
    asks. The tests answer prompts through a pseudo-terminal instead. The owner
    asked for exactly one, `-y`, and it is the only one: do not add a second, and
    do not loosen this one.
  - `-y` (`tools_skip_approval`) makes `confirm()` return yes without opening
    `/dev/tty`, which is also what lets a `-y` run work with no terminal at all.
    Two things guard it and both must stay: `qq.c` refuses the flag unless the
    resolved model's effective `allow_danger` is true (exit 2, before anything
    is sent -- a gateway grants it, and a model may only turn it back off, never
    on for itself), and the question is still printed to stderr so the run
    records what was done. `allow_danger`, `tools` and `mcp_servers` all default
    to **off/none** at the gateway; a model can only take them away, never add
    them.
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
  variable, set on a gateway. Don't add keys to example files, tests or docs.

## Adding tests

- Unit: add a `test_*` function in `tests/unit.c` using `CHECK` / `STREQ`, and
  call it from `main`.
- Integration: use `q ARGS` or `qin DATA ARGS`, then `expect_rc`, `out_is`,
  `out_has`, `err_has` or `true_that`. To check what the mock received, use
  `req 'EXPR'` (last chat request), `system` / `prompt` (its system and user
  messages) or `last /mcp-rest/tools/call 'EXPR'`, where `d` is the logged
  request. New mock behavior means a new model id in `tests/mock_openai.py`,
  and a config entry under some gateway's `models` giving that id (as `id`,
  or as the model's key when they're the same string) so a test can select it.
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
