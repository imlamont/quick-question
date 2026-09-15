# qq — quick question

`qq` is a tiny C command-line tool that sends a question to an AI model and
prints the answer as plain text.

```
$ qq what is the capital of france
Paris

$ echo 'int main(void){ return 0; }' | qq -c explain this in one sentence
This is a minimal C program that defines `main`, takes no arguments, and exits successfully with return code 0.

$ qq -c how do I list files by size
ls -lS
```

It talks to any **OpenAI-compatible** `/chat/completions` endpoint, such as
LiteLLM, Ollama, vLLM or OpenAI itself.

With `-r`, `-w` and `-x` the model can **read files, edit files and run
commands** on your machine. `qq` asks you before anything changes. Through a
LiteLLM proxy it can also answer with **MCP tools** such as web search.

It is written in plain C17 on POSIX: getopt, fork/exec, poll, libcurl and
cJSON. The stripped binary is about 40 KB and starts in 5–10 ms, so nearly
all the waiting is the model.

## Install

`qq` isn't in any distribution's package repositories yet. For now, either
download a prebuilt package or build it from source.

### Download a release (Debian and Ubuntu, amd64)

Prebuilt Debian packages are published on the project's
[GitHub Releases](https://github.com/imlamont/quick-question/releases) page as
`quick-question_<version>_amd64.deb`. They are built on Ubuntu 24.04 and
install on Ubuntu 24.04 or newer and comparable Debian releases. `apt`
installs the libcurl and cJSON libraries they need.

```sh
sudo apt install ./quick-question_0.1.0-1_amd64.deb
qq -v
```

The package installs `qq` in `/usr/bin`, the `qq(1)` man page, and the example
configuration in `/usr/share/doc/quick-question/examples/`.

### Build from source

On any other platform or architecture, follow [Build](#build) below, then run
`sudo make install`.

Either way, then set up a configuration file as described in
[Configuration](#configuration).

## Build

`qq` targets Linux and other POSIX systems; it has been tested on Linux. You
need a C17 compiler, make, libcurl and cJSON with their development headers.

```sh
# for example, on Debian or Ubuntu
sudo apt install build-essential libcurl4-openssl-dev libcjson-dev

make                 # builds ./qq
sudo make install    # installs bin/qq and share/man/man1/qq.1 under /usr/local
```

`make install` accepts three variables:
- `PREFIX` sets the install prefix (default `/usr/local`).
- `DESTDIR` installs into a staging directory.
- `INSTALL_STRIP=` installs the binary unstripped, for packaging tools that
  strip it themselves.

## Usage

```
qq [-hcrvwx] [-d profile] [-p profile] [-m model] [-s text] [-t secs] [--] prompt...
```

| Option | Meaning |
|---|---|
| `-h` | Show help |
| `-v` | Show version |
| `-c` | Add shell context (OS, shell, working directory) to the prompt |
| `-r` | Let the model read files (see [Local tools](#local-tools--r--w--x)) |
| `-w` | Let the model create and edit files |
| `-x` | Let the model run shell commands |
| `-p profile` | Use this profile for this call |
| `-d profile` | Save the profile as the default in the config, then run the prompt if one is given |
| `-m model` | Override the profile's model |
| `-s text` | Add extra steering text to the system prompt |
| `-t secs` | Timeout for the whole call, tool rounds included (default: config `timeout`, else 120) |

- **Prompt words.** Option parsing stops at the first word that isn't an option, so
  `qq how do I use ls -la` keeps `-la` in the prompt. Use `--` before a prompt that
  starts with `-`.
- **Piped input.** When stdin isn't a terminal, `qq` reads it and appends it to the
  prompt inside `<stdin>` tags. If stdin isn't a terminal but never closes, as in
  some scripts or editors, redirect it with `</dev/null`.
- **Exit codes.**
  - `0`: success.
  - `1`: the endpoint or the network failed.
  - `2`: a usage or config error.

## Local tools: `-r`, `-w`, `-x`

These flags let the model work with the files and shell on your machine. They
combine in any order (`-rw`, `-wr`, `-xw`, `-rwx`), and any of them also turns
on `-c` so the model knows where it is. `qq` defines the tools and runs them
itself.

| Flag | Tools |
|---|---|
| `-r` | `read_file`, `list_directory`, `search_files` |
| `-w` | `write_file`, `edit_file` (replaces text that occurs exactly once) |
| `-x` | `run_command` (`/bin/sh -c`) |

```sh
qq -r what does the debug target in the Makefile do
qq -rw add a clean target to the Makefile
qq -x how much space is left on this disk
```

### Approval

- **Reads** inside the current directory run straight away. A read outside it
  asks first; a symlink that leads outside counts as outside.
- **Writes, edits and commands always ask** on your terminal, showing exactly
  what will happen:
  ```
  qq: the model wants to run a command:
  $ df -h .
  Allow? [y/N]
  ```
- Only `y` or `yes` approves. With no terminal to ask, as in a script or cron
  job, the action is refused.
- A refusal goes back to the model as the tool's result, telling it not to
  retry.
- Each tool that runs is logged on stderr, for example `qq: read_file Makefile`.
- Time spent waiting at a prompt doesn't count against `-t`.
- If the model calls a tool whose flag you didn't give, it's told which flag
  would allow it.
- Limits:
  - `read_file` returns at most 256 KB.
  - `edit_file` refuses files over 4 MB.
  - `search_files` stops at 200 matches and skips hidden and binary files.
  - `run_command` keeps 32 KB of each output stream and is stopped at the
    timeout.

## Configuration

`qq` looks for its config in this order:
1. `$QQ_CONFIG`
2. `$XDG_CONFIG_HOME/qq/config.json`
3. `~/.config/qq/config.json`

Start from the example:

```sh
mkdir -p ~/.config/qq
cp config.example.json ~/.config/qq/config.json
```

```json
{
  "default": "local",
  "system_prompt": "Answer concisely in plain text.",
  "timeout": 120,
  "profiles": {
    "local": {
      "backend": "openai",
      "endpoint": "http://localhost:11434/v1",
      "model": "qwen3:8b",
      "api_key_env": "OPENAI_API_KEY",
      "temperature": 0.2,
      "max_tokens": 1024
    },
    "litellm": {
      "backend": "openai",
      "endpoint": "http://localhost:4000",
      "model": "nemotron-3.5-lightning",
      "api_key_env": "LITELLM_API_KEY",
      "mcp_servers": ["searxng_mcp"]
    }
  }
}
```

### Top level

| Key | Meaning |
|---|---|
| `default` | Profile used when `-p` isn't given (set it with `qq -d NAME`) |
| `system_prompt` | Extra steering added to every request |
| `timeout` | Seconds allowed per call (default 120) |
| `profiles` | Named endpoint settings (required) |

### Profiles

| Key | Meaning |
|---|---|
| `endpoint` | Base URL; `/chat/completions` is added unless it's already there. **Required.** |
| `model` | Model name. **Required.** |
| `api_key_env` | Name of the environment variable that holds the key, sent as `Authorization: Bearer`. Keys never go in the file. |
| `temperature`, `max_tokens` | Passed through when set |
| `system_prompt` | Steering for this profile |
| `mcp_servers` | LiteLLM MCP server names to offer as tools (see below) |
| `backend` | Optional; `openai` is the only value accepted |

The model must support OpenAI-style tool calling for `-r`, `-w`, `-x` and
`mcp_servers` to work.

## How the prompt is steered

The system prompt is built from these parts, in order, separated by blank
lines. Empty parts are skipped.

1. `qq`'s built-in instruction: answer directly, in plain text, with no markdown or preamble.
2. The top-level `system_prompt`.
3. The profile's `system_prompt`.
4. `-s text`.
5. The environment line, when `-c` (or `-r`, `-w`, `-x`) is given.
6. With `-r`, `-w` or `-x`, a note that the model has tools and how approval
   works.

Replies are trimmed. A leading `<think>…</think>` block, which some reasoning
models emit, is removed.

## MCP tools through LiteLLM

When a profile lists `mcp_servers`, each server is offered to the model like this:

```json
{"type": "mcp", "server_label": "searxng_mcp",
 "server_url": "litellm_proxy/mcp/searxng_mcp", "require_approval": "never"}
```

LiteLLM runs the first round of tool calls itself, but it hands any further
calls back to the client. `qq` handles those:

1. It runs each call through the proxy's `POST /mcp-rest/tools/call`. A call
   named `searxng_mcp-web_search` goes to server `searxng_mcp`, tool `web_search`.
2. It appends the results as `tool` messages.
3. It asks again, until the model replies with text.

If a tool fails (unknown tool, bad arguments, a tool error or a missing server),
the error goes back to the model as the tool's result so it can recover.

MCP tools and local tools can be used together. The loop is capped at 20
rounds, and `-t` limits the whole exchange. One HTTP connection is reused for
every request.

## Development

- [docs/TESTING.md](docs/TESTING.md): test results recorded for an earlier
  release (unit, integration, sanitizers, valgrind, end-to-end runs) and how
  to reproduce them.
- [docs/CLAUDE.md](docs/CLAUDE.md): project guidance for AI coding assistants.

```sh
make test     # unit tests + integration tests (needs python3 and util-linux)
make debug    # the same tests built with AddressSanitizer + UBSan
make clean
```

- `tests/unit.c` covers the buffer, prompt, config, MCP and local-tool helpers,
  including the checks that keep automatic reads inside the current
  directory.
- `tests/run.sh` drives the real binary against `tests/mock_openai.py`, a mock
  chat and MCP tool server. It checks:
  - flags, stdin handling and prompt steering
  - timeouts, error messages and exit codes
  - the MCP loop
  - every local tool, with and without its flag
  - the `-d` config rewrite

  Approval prompts are answered on a pseudo-terminal (`script`), and the
  no-terminal case runs under `setsid`.
- To run the suite under valgrind, pass `tests/run.sh` a small wrapper script
  that runs `valgrind --error-exitcode=99 ./qq "$@"`.

Source layout:

```
src/qq.c       options, orchestration, output
src/config.c   config lookup, validation, atomic default rewrite
src/prompt.c   system prompt, shell context, stdin wrapping, reply cleanup
src/openai.c   chat completions + tool loop
src/http.c     shared libcurl handle, JSON POST, deadline
src/mcp.c      LiteLLM MCP tools: offering, name mapping, calls
src/tools.c    local tools for -r/-w/-x: definitions, approval, confinement
src/proc.c     child processes for run_command, with poll(), output caps and timeouts
src/buf.c      growable string buffer
```

## License

MIT; see [LICENSE](LICENSE).
