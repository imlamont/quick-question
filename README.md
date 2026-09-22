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
sudo apt install ./quick-question_0.4.0-1_amd64.deb
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
qq [-hclrvwxy] [-d model] [-p model] [-s text] [-t secs] [-L file]
   [--] prompt...
```

| Option | Meaning |
|---|---|
| `-h` | Show help |
| `-v` | Show version |
| `-l` | List every model with what it may do, marking with `*` the one that would be used |
| `-c` | Add shell context (OS, shell, working directory) to the prompt |
| `-r` | Let the model read files (see [Local tools](#local-tools--r--w--x)) |
| `-w` | Let the model create and edit files |
| `-x` | Let the model run shell commands |
| `-p model` | Use this model for this call: `gateway/model`, or just `model` when only one gateway has it. A gateway name alone means that gateway's default model (see [Choosing a model](#choosing-a-model)) |
| `-d model` | Save the model as the default in the config (as `gateway/model`), then run the prompt if one is given |
| `-s text` | Add extra steering text to the system prompt |
| `-t secs` | Timeout for the whole call, tool rounds included (default: config `timeout`, else 120) |
| `-L file` | Append a timestamped log of the whole exchange to `file` (see [Debug log](#debug-log--l)) |
| `-y` | **Dangerous.** Do every tool call without asking (see [Skipping approval](#skipping-approval--y)) |

### Skipping approval (`-y`)

`-y` answers every approval with yes. Writes, edits, commands and reads outside
the working directory all happen as soon as the model asks for them, and no
terminal is needed, so `qq` will do them in a script or a cron job where it
would otherwise refuse for want of one.

Because that removes the only thing standing between a model and your files,
the flag is not enough on its own. The gateway has to have opted in:

```json
"agent": {
  "endpoint": "http://localhost:4000",
  "api_key_env": "LITELLM_API_KEY",
  "tools": ["read", "write", "exec"],
  "allow_danger": true,
  "models": { "nemotron": { "id": "nemotron-3.5-lightning" } }
}
```

`allow_danger` is off unless a gateway says `true`, and `-y` on any other
gateway's models is an error (exit 2) that does nothing. A model can turn it
back off with `"allow_danger": false`, but can never turn it on for itself.
Keep it on a gateway you made for the purpose, not the one you use day to day.

Each action is still printed on stderr before it happens, so the run leaves a
record of what was done on your behalf:

```
qq: the model wants to write out.txt (17 bytes):
written by model
Allowed by -y.
qq: write_file out.txt
```

`-y` has nothing to do with MCP calls: those never ask for approval in the
first place, since they run on the server rather than on your machine.

### Debug log (`-L`)

`-L file` appends a line per event, so you can see what `qq` and the model
actually said to each other. It answers "what did it send?", "what came back?"
and "which tool ran?" without a proxy in the way.

```
$ qq -w -L /tmp/qq.log write the current weather to test.txt
$ cut -c1-90 /tmp/qq.log
2026-09-16T00:04:22.911-0400 command qq -w -L /tmp/qq.log 'write the current weather to test.txt'
2026-09-16T00:04:22.911-0400 start qq 0.3.0 pid=14987
2026-09-16T00:04:22.911-0400 model ollama/hermes3 id=hermes3:latest endpoint=http://localhost:11434/v1 tools=w allowed=rwx mcp=0
2026-09-16T00:04:22.911-0400 timeout 300s
2026-09-16T00:04:22.914-0400 request http://localhost:11434/v1/chat/completions {"model":"herm
2026-09-16T00:04:29.980-0400 response 200 7066ms {"id":"chatcmpl-177","object":"chat.completio
2026-09-16T00:04:29.980-0400 tool-round 0 calls=1
2026-09-16T00:04:29.980-0400 tool-call write_file {"path":"test.txt","content":"The current we
2026-09-16T00:04:29.980-0400 approval yes after 1531ms
2026-09-16T00:04:29.980-0400 tool-result write_file wrote 56 bytes to test.txt
2026-09-16T00:04:29.981-0400 request http://localhost:11434/v1/chat/completions {"model":"herm
2026-09-16T00:04:30.593-0400 response 200 612ms {"id":"chatcmpl-971","object":"chat.completion"
2026-09-16T00:04:30.593-0400 answer The current weather has been successfully written to test.t
2026-09-16T00:04:30.593-0400 exit 0
```

Every line starts with a local timestamp to the millisecond and its UTC offset,
then the event:

| Event | What it records |
| --- | --- |
| `command` | the whole command line as invoked, quoted the way a shell would need it |
| `start` | version and process id |
| `model` | `gateway/model`, the id sent to the API, the endpoint, the tool flags in force, the tools the model is allowed (`allowed=`) and how many MCP servers it has |
| `timeout` | the limit the whole call has to finish in |
| `request` | the URL and the **entire** request body, chat and MCP alike |
| `response` | status, how long it took, and the whole body |
| `request-failed` | a connection or timeout failure instead of a reply |
| `tool-round` | which round it is and how many calls the model made |
| `tool-call` | the tool's name and its arguments as the model sent them |
| `approval` | `yes`, `no`, `timeout` or `no-terminal`, and how long you took |
| `tool-result` | what went back to the model |
| `tool-repeat` | a call answered from history rather than done again |
| `mcp-call` | the MCP server, the tool, and the gateway URL |
| `mcp-result` | what the server returned |
| `answer` | the final text, after `<think>` stripping |
| `failed` | the error `qq` exits with |
| `exit` | the exit status |

- Entries are flushed as they happen, so the log still explains a run you
  interrupt with `^C` or one that times out.
- The file is appended to, so runs accumulate. Delete it when it gets long.
- Newlines and other control characters in bodies and results are escaped
  (`\n`, `\x1b`), so one event is always one line and reading the log can't
  move your terminal around.
- **The log is as private as the conversation**: prompts, any file the model
  read, and any command output are all in it verbatim. Your API key is not,
  since that travels in a header `qq` doesn't log.
- A file that can't be opened is a startup error (exit 2) and nothing is sent.

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
  job, the action is refused. An unanswered prompt is refused after 120
  seconds, so a run you walk away from ends instead of waiting forever.
- A refusal goes back to the model as the tool's result, telling it not to
  retry, and it sticks: if the model asks for that exact call again, `qq`
  repeats the refusal itself rather than putting it to you a second time.
- The same holds for calls that were allowed. A model that loops on one call --
  some local models do -- is answered from `qq`'s own record of what it already
  did, so nothing is written, read or run twice and you are asked only once.
  When a whole round is nothing but repeats, `qq` stops with an error instead of
  spinning to the round limit. A write, edit or command that runs clears the
  remembered reads, so re-reading a file after changing it really does read it
  again.
- MCP calls are recorded the same way, so a model stuck on one search doesn't
  put the identical query to the server round after round.
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
  "default": "ollama/hermes3",
  "system_prompt": "Answer concisely in plain text.",
  "timeout": 120,
  "gateways": {
    "ollama": {
      "endpoint": "http://localhost:11434/v1",
      "models": {
        "hermes3": { "id": "hermes3:latest", "temperature": 0.2 }
      }
    },
    "litellm": {
      "endpoint": "http://localhost:4000",
      "api_key_env": "LITELLM_API_KEY",
      "default_model": "nemotron",
      "tools": ["read", "write"],
      "mcp_servers": ["searxng_mcp", "files_mcp"],
      "models": {
        "nemotron": {
          "id": "nemotron-3.5-lightning",
          "mcp_servers": ["searxng_mcp"]
        },
        "readonly": {
          "id": "nemotron-3.5-lightning",
          "tools": ["read"],
          "mcp_servers": []
        }
      }
    }
  }
}
```

A **gateway** is one endpoint. It lists the **models** you can use on it and sets
the ceiling for what they may do: which local tools (`-r`, `-w`, `-x`) and which
MCP servers they can reach. A model can only narrow that ceiling, never widen
it.

### Ollama

Ollama's OpenAI-compatible API needs no key, so leave `api_key_env` out. Pull
the model first (`ollama pull hermes3`), or set the model's `id` to something
from `ollama list`; otherwise the first call returns `HTTP 404: model ... not
found`. Give large models a longer `timeout` (top level, or on the gateway or model), since the first call loads the
model. Tools (`-r`, `-w`, `-x`) need a model that supports tool calling;
hermes3 does.

Running `qq` in WSL with Ollama on Windows is a special case, because WSL's
default NAT networking means `localhost` doesn't reach the Windows host.
Either switch WSL to mirrored networking, by putting `networkingMode=mirrored`
under `[wsl2]` in `%UserProfile%\.wslconfig` and running `wsl --shutdown`, or
point `endpoint` at the host's gateway address
(`ip route show default | awk '{print $3}'`, typically `172.x.x.1`). That
address can change when Windows reboots, so mirrored networking is the more
durable option.

### Top level

| Key | Meaning |
|---|---|
| `default` | The model used when `-p` isn't given, as `gateway/model` (set it with `qq -d NAME`) |
| `system_prompt` | Extra steering added to every request |
| `timeout` | Seconds allowed per call (default 120) |
| `gateways` | The endpoints and their models (required) |

The old `profiles` format is no longer read; `qq` says so and stops. To convert
one, move its `endpoint` and `api_key_env` into a gateway and its `model` into
that gateway's `models`, as the model's `id` (its name in `models` can be the
same string).

### Gateways

| Key | Meaning |
|---|---|
| `endpoint` | Base URL; `/chat/completions` is added unless it's already there. **Required.** |
| `models` | The models on this gateway, an object keyed by the name you type. **Required**, and needs at least one. |
| `default_model` | The model used when only the gateway is named (`-p litellm`); the first one listed if omitted |
| `api_key_env` | Name of the environment variable that holds the key, sent as `Authorization: Bearer`. Keys never go in the file, and naming a variable that is unset or empty is an error (exit 2) when a model on the gateway is used. Omit it for an endpoint that needs no auth. |
| `tools` | Which of `-r`, `-w` and `-x` its models may be used with: any of `"read"`, `"write"`, `"exec"`. **A gateway grants nothing it doesn't list.** |
| `mcp_servers` | LiteLLM MCP server names its models can offer (see below). Nothing is offered if omitted. |
| `allow_danger` | `true` lets `-y` skip every approval for its models; off unless set (see [Skipping approval](#skipping-approval--y)) |
| `system_prompt`, `temperature`, `max_tokens`, `timeout` | Defaults for its models |

### Models

A model is an entry in a gateway's `models`. Its key is the name you type with
`-p`. All of its members are optional, so `"hermes3": {}` is a model.

| Key | Meaning |
|---|---|
| `id` | What is sent to the API as the model name. Defaults to the key, so use `id` for a name you'd rather not type (`hermes3:latest`) or to list one model twice with different limits. |
| `system_prompt`, `temperature`, `max_tokens`, `timeout` | Override the gateway's value for this model |
| `tools` | A subset of the gateway's `tools`. Leave it out to inherit all of them; `[]` means none. |
| `mcp_servers` | A subset of the gateway's `mcp_servers`. Leave it out to inherit all of them; `[]` means none. |
| `allow_danger` | May only be `false`, to opt out of a gateway that allows `-y` |

A model that lists something its gateway doesn't grant is a configuration error
(exit 2), not something quietly ignored. So is a misspelt key, a retired one
(`mcp`, `backend`, or a boolean `tools`), a repeated model or gateway name, a
gateway with no models, and a `default_model` that isn't one of its models. The
whole file is checked on every run, so a mistake in a gateway you aren't using is
still reported. Only the selected gateway's `api_key_env` has to be set.

`tools` is a ceiling, not a switch: it says what `-r`, `-w` and `-x` are
*allowed* to do with the model, and you still have to pass the flag to get the
tool. `-w` on a model whose `tools` lack `"write"` is refused with an error
(exit 2) rather than quietly ignored, since you asked for the flag on purpose.
Local tools and MCP servers are independent: a model with `"mcp_servers": []`
can still use `-r`.

The model must support OpenAI-style tool calling for `-r`, `-w`, `-x` and
`mcp_servers` to work.

### Choosing a model

`-p` (and `default`, and `-d`) take one of:

- `gateway/model`, such as `litellm/nemotron`. This is always unambiguous.
- `model`, when exactly one gateway has a model of that name. If two do, `qq`
  refuses and lists them (`"reader" is ambiguous; use gateway/model: lt/reader,
  danger/reader`) rather than guess which endpoint you meant.
- `gateway`, for that gateway's `default_model`.

Model names may contain `/` (`meta/llama-3`). The first `/` only separates a
gateway when what comes before it is a gateway's name; otherwise the whole thing
is a model name. If you have a gateway called `meta` as well as a model called
`meta/llama-3` on some other gateway `x`, write `x/meta/llama-3`.

`qq -l` shows the result of all this, one line per model:

```
$ qq -l
* ollama/hermes3    tools=none  mcp=none  danger=no
  litellm/nemotron  tools=read,write  mcp=searxng_mcp  danger=no
  litellm/readonly  tools=read  mcp=none  danger=no
```

## How the prompt is steered

The system prompt is built from these parts, in order, separated by blank
lines. Empty parts are skipped.

1. `qq`'s built-in instruction: answer directly, in plain text, with no markdown or preamble.
2. The top-level `system_prompt`.
3. The gateway's `system_prompt`.
4. The model's `system_prompt`.
5. `-s text`.
6. The environment line, when `-c` (or `-r`, `-w`, `-x`) is given.
7. With `-r`, `-w` or `-x`, a note that the model has tools and how approval
   works.

Replies are trimmed. A leading `<think>…</think>` block, which some reasoning
models emit, is removed.

## MCP tools through LiteLLM

When a model has `mcp_servers` (its own list, or its gateway's), each server is offered to the model like this:

```json
{"type": "mcp", "server_label": "searxng_mcp",
 "server_url": "litellm_proxy/mcp/searxng_mcp", "require_approval": "always"}
```

`require_approval` is `always` so the gateway hands every tool call back to the
client instead of running any itself. With `never` it ran them, and that
included `qq`'s own `-r`/`-w`/`-x` functions, which it has no way to run: the
model was told `Error executing tool: 'write_file'` for a call `qq` then carried
out perfectly well, and reasoned from that phantom failure — retrying with
different file content, or hunting for a file that was there all along.

`qq` handles every MCP call itself:

1. It runs each call through the proxy's `POST /mcp-rest/tools/call`. A call
   named `searxng_mcp-web_search` goes to server `searxng_mcp`, tool `web_search`.
2. It appends the results as `tool` messages.
3. It asks again, until the model replies with text.

If a tool fails (unknown tool, bad arguments, a tool error or a missing server),
the error goes back to the model as the tool's result so it can recover.

An MCP call repeated with the same arguments isn't sent again: the earlier
result goes back instead, and a round of nothing but repeats ends the run. A
model that latches onto one query can otherwise search for it a dozen times in
a row -- qwen3 asked the same thing eight times in one run here -- which costs
time and hits the server for nothing, since the answer cannot change in the
seconds between two identical queries.

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
