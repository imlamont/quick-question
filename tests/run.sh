#!/usr/bin/env bash
# Integration tests for qq against a mock OpenAI-compatible server.
# usage: tests/run.sh path/to/qq
set -u

QQ=$(realpath "${1:?usage: run.sh path/to/qq}")
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
MOCK_PID=
trap '[ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null; rm -rf "$T"' EXIT

pass=0 fail=0

# q ARGS...        run qq with empty stdin; sets out, err, rc
# qin DATA ARGS... run qq with DATA piped to stdin
q()   { out=$("$QQ" "$@" 2>"$T/stderr" </dev/null); rc=$?; err=$(<"$T/stderr"); }
qin() { local data=$1; shift; out=$(printf '%s' "$data" | "$QQ" "$@" 2>"$T/stderr"); rc=$?; err=$(<"$T/stderr"); }

report()    { fail=$((fail + 1)); printf 'FAIL: %s\n  rc=%s\n  out: %.400s\n  err: %.400s\n' "$1" "$rc" "$out" "$err" >&2; }
ok()        { pass=$((pass + 1)); }
expect_rc() { [ "$rc" -eq "$2" ] && ok || report "$1: want rc $2"; }
out_has()   { [[ $out == *"$2"* ]] && ok || report "$1: stdout lacks '$2'"; }
out_lacks() { [[ $out != *"$2"* ]] && ok || report "$1: stdout has '$2'"; }
out_is()    { [ "$out" = "$2" ] && ok || report "$1: stdout is not '$2'"; }
err_has()   { [[ $err == *"$2"* ]] && ok || report "$1: stderr lacks '$2'"; }
true_that() { "${@:2}" && ok || report "$1"; }

# The mock logs every request as a JSON line.
# last PATH-SUFFIX EXPR: evaluate EXPR on the newest request to that path, e.g. last /chat/completions 'd["auth"]'
last() {
	python3 -c 'import json, sys
reqs = [json.loads(line) for line in open(sys.argv[1])]
d = [r for r in reqs if r["path"].endswith(sys.argv[2])][-1]
print(eval(sys.argv[3]))' "$T/req.log" "$1" "$2"
}
req()    { last /chat/completions "$1"; }
system() { req 'd["body"]["messages"][0]["content"]'; }
prompt() { req 'd["body"]["messages"][1]["content"]'; }

python3 "$HERE/mock_openai.py" "$T/req.log" >"$T/port" &
MOCK_PID=$!
for _ in $(seq 100); do [ -s "$T/port" ] && break; sleep 0.05; done
PORT=$(<"$T/port")

cat >"$T/config.json" <<EOF
{
  "default": "local",
  "system_prompt": "GLOBAL-STEER",
  "timeout": 30,
  "profiles": {
    "local":  { "backend": "openai", "endpoint": "http://127.0.0.1:$PORT/v1/", "model": "test-model",
                "api_key_env": "QQ_TEST_KEY", "temperature": 0.25, "max_tokens": 64,
                "system_prompt": "PROFILE-STEER" },
    "alt":    { "endpoint": "http://127.0.0.1:$PORT/v1", "model": "alt-model" },
    "down":   { "endpoint": "http://127.0.0.1:9/v1", "model": "m" },
    "tools":  { "endpoint": "http://127.0.0.1:$PORT/v1", "model": "tooler",
                "mcp_servers": ["searxng_mcp", "ghost_mcp"] },
    "lt":     { "endpoint": "http://127.0.0.1:$PORT/v1", "model": "reader" },
    "nokey":  { "endpoint": "http://127.0.0.1:$PORT/v1", "model": "test-model",
                "api_key_env": "QQ_MISSING_KEY" }
  }
}
EOF
export QQ_CONFIG="$T/config.json"
export QQ_TEST_KEY=sk-test
unset QQ_MISSING_KEY

# --- CLI -------------------------------------------------------------------
q -h;                 expect_rc "-h" 0; out_has "-h" "usage: qq"; out_has "-h" "-d profile"
out_has "-h tools" "-r, -w and -x combine"
out_has "-h usage flags" "qq [-hclrvwx]"
out_has "-h lists -l" "-l          list profile names"
q -v;                 expect_rc "-v" 0; out_is "-v" "qq 0.3.0"

q -l;                 expect_rc "-l" 0
out_is "-l marks the default" $'* local\n  alt\n  down\n  tools\n  lt\n  nokey'
q -p alt -l;          expect_rc "-p -l" 0; out_has "-l marks -p" "* alt"
out_lacks "-l marks only -p" "* local"
q -l hi;              expect_rc "-l with a prompt" 0; out_has "-l with a prompt" "* local"
q -p nokey -l;        expect_rc "-l skips profile validation" 0; out_has "-l skips profile validation" "* nokey"
q;                    expect_rc "no prompt" 2; err_has "no prompt" "usage:"
q -z hi;              expect_rc "bad flag" 2
q -t abc hi;          expect_rc "-t abc" 2; err_has "-t abc" "invalid timeout"
q -t 0 hi;            expect_rc "-t 0" 2
q -p;                 expect_rc "-p without value" 2
q -p nope hi;         expect_rc "unknown profile" 2; err_has "unknown profile" "available: local, alt, down, tools, lt, nokey"

QQ_CONFIG="$T/missing.json" "$QQ" hi </dev/null 2>"$T/stderr"; rc=$?; err=$(<"$T/stderr"); out=
expect_rc "missing config" 2; err_has "missing config" "config not found"
printf '{"profiles": {,}}' >"$T/bad.json"
QQ_CONFIG="$T/bad.json" "$QQ" hi </dev/null 2>"$T/stderr"; rc=$?; err=$(<"$T/stderr")
expect_rc "bad json" 2; err_has "bad json" "invalid JSON near line 1"
printf '{"profiles": {"x": {"backend": "gemini", "endpoint": "http://h", "model": "m"}}}' >"$T/backend.json"
QQ_CONFIG="$T/backend.json" "$QQ" -p x hi </dev/null 2>"$T/stderr"; rc=$?; err=$(<"$T/stderr")
expect_rc "other backend rejected" 2; err_has "other backend rejected" 'unknown backend "gemini" (only openai is supported)'

# --- prompt and input --------------------------------------------------------
q hello world
expect_rc "basic" 0
out_is "basic reply" "hello from test-model"
out=$(prompt);        out_is "prompt words joined" "hello world"
out=$(system)
out_has "built-in steer" "You are qq, a command-line assistant."
out_has "global then profile steer" $'\n\nGLOBAL-STEER\n\nPROFILE-STEER'
out_lacks "no context by default" "Environment:"
out_lacks "no tool note by default" "You can use tools"

q -m sonnet hi;       out_is "-m override" "hello from sonnet"
q -s "EXTRA-STEER" hi; out=$(system); out_has "-s" $'PROFILE-STEER\n\nEXTRA-STEER'
q -c hi;              out=$(system); out_has "-c" "Environment: OS="; out_has "-c cwd" "cwd=$PWD"
q how do I use ls -la; out=$(prompt); out_is "options after prompt" "how do I use ls -la"
q -- -weird prompt;   out=$(prompt); out_is "-- separator" "-weird prompt"

qin $'line one\nline two\n' summarize
expect_rc "pipe + prompt" 0
out=$(prompt);        out_is "pipe + prompt" $'summarize\n\n<stdin>\nline one\nline two\n</stdin>'
qin "only stdin";     out=$(prompt); out_is "pipe only" "only stdin"

big=$(head -c 300000 /dev/zero | tr '\0' a)
qin "$big"
expect_rc "300KB stdin" 0
out=$(req 'len(d["body"]["messages"][1]["content"])'); out_is "300KB sent intact" "300000"

q -m empty hi;        expect_rc "empty reply" 1; err_has "empty reply" "empty response"

head -c $((1024 * 1024 + 1)) /dev/zero | "$QQ" hi 2>"$T/stderr" >/dev/null; rc=$?; err=$(<"$T/stderr")
expect_rc "stdin cap" 2; err_has "stdin cap" "stdin exceeds 1 MiB"

# --- requests and errors -----------------------------------------------------
QQ_TEST_KEY=sk-test "$QQ" -p local what is up </dev/null >"$T/stdout" 2>"$T/stderr"; rc=$?
out=$(<"$T/stdout"); err=$(<"$T/stderr")
expect_rc "keyed request" 0
out_is "keyed request content" "hello from test-model"
out=$(req 'd["path"]');                         out_is "request url" "/v1/chat/completions"
out=$(req 'd["auth"]');                         out_is "bearer auth" "Bearer sk-test"
out=$(req '[m["role"] for m in d["body"]["messages"]]'); out_is "roles" "['system', 'user']"
out=$(prompt);                                  out_is "user message" "what is up"
out=$(req '(d["body"]["temperature"], d["body"]["max_tokens"], d["body"]["stream"])')
out_is "request params" "(0.25, 64, False)"
out=$(req '"tools" in d["body"]');              out_is "no tools without mcp_servers or flags" "False"

q -p nokey hi;        expect_rc "api_key_env not set" 2
err_has "api_key_env not set" 'profile "nokey": "api_key_env" names environment variable QQ_MISSING_KEY, which is not set'
QQ_MISSING_KEY= "$QQ" -p nokey hi </dev/null 2>"$T/stderr"; rc=$?; err=$(<"$T/stderr")
expect_rc "api_key_env empty" 2; err_has "api_key_env empty" "variable QQ_MISSING_KEY, which is empty"

q -p alt hi;          expect_rc "profile without backend key" 0; out_is "profile without backend key" "hello from alt-model"
out=$(req 'd["auth"]'); out_is "no auth header without api_key_env" "None"

q -m thinker hi;      out_is "think stripped" "thought answer"
q -m unauthorized hi; expect_rc "HTTP 401" 1; err_has "HTTP 401" "HTTP 401: invalid api key"
q -m garbage hi;      expect_rc "non-JSON reply" 1; err_has "non-JSON reply" "unexpected response: this is not json"
q -p down hi;         expect_rc "unreachable" 1; err_has "unreachable" "127.0.0.1:9"

start=$(date +%s%N)
q -m slow -t 1 hi
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
expect_rc "timeout" 1; err_has "timeout" "timed out"
true_that "timeout is prompt (${elapsed}ms)" [ "$elapsed" -lt 2500 ]

# --- MCP tool loop -----------------------------------------------------------
: >"$T/req.log"
q -p tools find cats
expect_rc "tool loop" 0
out_is "tool loop answer" "answer from tool: results for cats"
out=$(req 'd["body"]["tools"]')
out_is "mcp tools offered" "[{'type': 'mcp', 'server_label': 'searxng_mcp', 'server_url': 'litellm_proxy/mcp/searxng_mcp', 'require_approval': 'never'}, {'type': 'mcp', 'server_label': 'ghost_mcp', 'server_url': 'litellm_proxy/mcp/ghost_mcp', 'require_approval': 'never'}]"
out=$(last /mcp-rest/tools/call '(d["path"], d["body"])')
out_is "tool call request" "('/mcp-rest/tools/call', {'server_id': 'searxng_mcp', 'name': 'web_search', 'arguments': {'query': 'cats'}})"
out=$(req '[m["role"] for m in d["body"]["messages"]]')
out_is "tool history roles" "['system', 'user', 'assistant', 'tool']"
out=$(req 'd["body"]["messages"][2]["tool_calls"][0]["id"] == d["body"]["messages"][3]["tool_call_id"]')
out_is "tool_call_id matches" "True"
true_that "tool loop makes 3 requests" [ "$(wc -l <"$T/req.log")" -eq 3 ]

q -p tools -m twotools go
expect_rc "tool errors fed back" 0
out_is "tool errors fed back" "results for dogs | error: unknown tool \"other_mcp-lookup\" | error: Missing required argument query | error: HTTP 404: MCP server 'ghost_mcp' was not found | error: tool arguments are not a JSON object"

q -p tools -m looper go
expect_rc "tool round limit" 1; err_has "tool round limit" "model still calling tools after 20 rounds"

# --- local tools (-r, -w, -x) ----------------------------------------------
W="$T/ws"
mkdir -p "$W/sub"
printf 'alpha line\nbeta needle line\n' >"$W/notes.txt"
printf 'top secret\n' >"$T/secret.txt"
tools_offered() { req '[t["function"]["name"] for t in d["body"].get("tools", [])]'; }

# qws ARGS...          run qq inside $W detached from any terminal, so nothing can be approved
# qtty ANSWERS ARGS... run qq inside $W on a pseudo-terminal that types ANSWERS (printf %b)
qws() { out=$(cd "$W" && setsid -w "$QQ" "$@" 2>"$T/stderr" </dev/null); rc=$?; err=$(<"$T/stderr"); }
qtty() {
	local answers=$1; shift
	printf '%b' "$answers" >"$T/answers"
	(cd "$W" && script -qec "$(printf '%q ' "$QQ" "$@")" /dev/null <"$T/answers" >"$T/tty.out" 2>&1); rc=$?
	out=$(tr -d '\r' <"$T/tty.out"); err=$out
}

qws -p lt -r look
expect_rc "read_file" 0
out_is "read_file result" $'alpha line\nbeta needle line'
err_has "tool use is logged" "qq: read_file notes.txt"
out=$(tools_offered);  out_is "-r offers read tools" "['read_file', 'list_directory', 'search_files']"
out=$(system)
out_has "-r implies -c" "Environment: OS="; out_has "-r describes the tools" "You can use tools"

qws -p lt -wr -m lister look
out_is "list_directory" $'notes.txt\nsub/'
out=$(tools_offered);  out_is "-wr offers read+write tools" "['read_file', 'list_directory', 'search_files', 'write_file', 'edit_file']"
qws -p lt -rw -m lister look
out=$(tools_offered);  out_is "-rw offers the same tools" "['read_file', 'list_directory', 'search_files', 'write_file', 'edit_file']"
qws -p lt -xw -m lister look
out_has "read tool needs -r" "error: list_directory is not enabled; the user can allow it by running qq with -r"
out=$(tools_offered);  out_is "-xw offers write+exec tools" "['write_file', 'edit_file', 'run_command']"

qws -p lt -r -m searcher look
out_is "search_files" "notes.txt:2: beta needle line"

qws -p lt -r -m outsider look
out_has "outside read needs a terminal" "no terminal to ask the user"
out_lacks "outside read not done" "top secret"
qtty 'n\n' -p lt -r -m outsider look
out_has "outside read is asked" "wants to use read_file outside the current directory"
out_has "outside read denied" "the user denied this"; out_lacks "denied read not done" "top secret"
qtty 'y\n' -p lt -r -m outsider look
expect_rc "outside read approved" 0; out_has "outside read approved" "top secret"

qws -p lt -r -m writer go
out_has "write needs -w" "write_file is not enabled; the user can allow it by running qq with -w"
qws -p lt -w -m writer go
out_has "write needs a terminal" "no terminal to ask the user"
true_that "write without approval does nothing" [ ! -e "$W/out.txt" ]
qtty 'n\n' -p lt -w -m writer go
out_has "write is asked" "wants to write out.txt (17 bytes)"
out_has "write denied" "the user denied this"
true_that "denied write does nothing" [ ! -e "$W/out.txt" ]
qtty 'y\n' -p lt -w -m writer go
expect_rc "write approved" 0; out_has "write approved" "wrote 17 bytes to out.txt"
true_that "approved write happened" [ "$(cat "$W/out.txt")" = "written by model" ]

# A model that keeps asking for the same call is answered from qq's own history,
# so the user is asked once and the loop ends instead of spinning to the cap.
asked() { grep -c "Allow?" <<<"$out"; }
rm -f "$W/out.txt"
qtty 'y\ny\ny\ny\ny\n' -p lt -w -m writeloop go
expect_rc "repeated write stops" 1
err_has "repeated write stops" "kept repeating tool calls it had already made"
true_that "repeated write asked once" [ "$(asked)" -eq 1 ]
true_that "first repeated write happened" [ "$(cat "$W/out.txt")" = "written by model" ]

rm -f "$W/out.txt"
qtty 'n\nn\nn\nn\nn\n' -p lt -w -m writeloop go
expect_rc "denial holds for repeats" 1
true_that "denied repeat asked once" [ "$(asked)" -eq 1 ]
true_that "denied repeat never written" [ ! -e "$W/out.txt" ]

# A repeat alongside a fresh call still makes progress, so the loop runs to the
# cap, but the repeat is never put to the user a second time.
rm -f "$W/out.txt"
qtty 'y\ny\ny\ny\ny\n' -p lt -rw -m mixloop go
expect_rc "repeat beside new call" 1
err_has "repeat beside new call" "model still calling tools after 20 rounds"
true_that "repeat beside new call asked once" [ "$(asked)" -eq 1 ]

# Repeated reads are answered from the history too, so a model stuck on one read
# stops instead of spinning through every round.
qws -p lt -r -m readloop look
expect_rc "repeated read stops" 1
err_has "repeated read stops" "kept repeating tool calls it had already made"

# ... but a change makes a remembered read stale, so the same read after an edit
# is really done again and sees the new content.
rm -f "$W/r.txt"
qtty 'y\ny\n' -p lt -rw -m rereader go
expect_rc "read after edit" 0
out_has "read after edit is done again" "edited r.txt"
out_lacks "read after edit is not replayed" "you already called read_file"
out_has "read after edit sees the change" "two"

qws -p lt -w -m editor2 go
out_has "ambiguous edit refused before asking" "old_text occurs 2 times in notes.txt"
qtty 'y\n' -p lt -w -m editor go
out_has "edit is asked" "wants to edit notes.txt"; out_has "edit approved" "edited notes.txt"
true_that "approved edit happened" grep -q '^ALPHA line$' "$W/notes.txt"

qws -p lt -w -m runner go
out_has "command needs -x" "run_command is not enabled; the user can allow it by running qq with -x"
qtty 'n\n' -p lt -x -m runner go
out_has "command is asked" '$ echo from-shell; echo oops >&2; exit 3'
out_has "command denied" "the user denied this"; out_lacks "denied command not run" "--- stdout ---"
qtty 'y\n' -p lt -x -m runner go
expect_rc "command approved" 0
out_has "command exit status" "exit status 3"
out_has "command stdout" $'--- stdout ---\nfrom-shell'
out_has "command stderr" $'--- stderr ---\noops'

# --- -L (debug log) --------------------------------------------------------
L="$T/qq.log"
logged() { grep -c "^$(date +%Y)-.*$1" "$L"; }

rm -f "$L"
q -L "$L" hello there
expect_rc "-L runs normally" 0
out_is "-L answer unchanged" "hello from test-model"
true_that "-L writes a log" [ -s "$L" ]
out=$(<"$L")
out_has "-L logs the start" "start qq "
true_that "-L logs the command first" [ "$(head -1 "$L" | awk '{print $2}')" = "command" ]
out_has "-L logs the command" "command $QQ -L $L hello there"
out_has "-L logs the profile" "profile local model=test-model"
out_has "-L logs the request body" '"role":"user","content":"hello there"'
out_has "-L logs the response" "response 200 "
out_has "-L logs the answer" "answer hello from test-model"
out_has "-L logs the exit" "exit 0"
# Every line begins with a timestamp to the millisecond and a UTC offset.
true_that "-L stamps every line" \
	[ "$(grep -cE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}[-+][0-9]{4} ' "$L")" \
	  -eq "$(wc -l <"$L")" ]
# The key travels in a header, which is never logged.
true_that "-L never logs the api key" [ "$(grep -c "$QQ_TEST_KEY" "$L")" -eq 0 ]

# An argument a shell would have to quote is quoted in the log too.
rm -f "$L"
q -L "$L" -s "be brief" "don't explain"
out=$(<"$L")
out_has "-L quotes command arguments" "-s 'be brief' 'don'\\''t explain'"

# A second run appends rather than starting over.
before=$(wc -l <"$L")
q -L "$L" again
true_that "-L appends" [ "$(wc -l <"$L")" -gt "$before" ]

# Tool calls, approvals and results are all in there.
rm -f "$L" "$W/out.txt"
qtty 'y\n' -p lt -w -m writer -L "$L" go
expect_rc "-L with a tool" 0
out=$(<"$L")
out_has "-L logs the tool call" 'tool-call write_file {"path": "out.txt"'
out_has "-L logs the approval" "approval yes after "
out_has "-L logs the tool result" "tool-result write_file wrote 17 bytes to out.txt"
out_has "-L logs the round" "tool-round 0 calls=1"

rm -f "$L" "$W/out.txt"
qtty 'n\n' -p lt -w -m writer -L "$L" go
out=$(<"$L")
out_has "-L logs a denial" "approval no after "
out_has "-L logs the refusal result" "the user denied this"

rm -f "$L"
qws -p lt -w -m writer -L "$L" go
out=$(<"$L")
out_has "-L logs a missing terminal" "approval no-terminal"

# MCP calls name the server and the tool.
rm -f "$L"
q -p tools -L "$L" find cats
expect_rc "-L with MCP" 0
out=$(<"$L")
out_has "-L logs the mcp call" "mcp-call server=searxng_mcp tool=web_search via "
out_has "-L logs the mcp result" "mcp-result searxng_mcp-web_search results for cats"

# A repeat answered from history says so.
rm -f "$L" "$W/out.txt"
qtty 'y\ny\n' -p lt -w -m writeloop -L "$L" go
out=$(<"$L")
out_has "-L logs a repeat" "tool-repeat write_file answered from history"

# Control characters in a result cannot break a line or steer the terminal.
rm -f "$L"
qtty 'y\n' -p lt -x -m runner -L "$L" go
out=$(<"$L")
out_has "-L escapes newlines in results" "tool-result run_command exit status 3\\n"

# A log that cannot be opened is a startup error, and nothing else runs.
q -L "$T/nodir/qq.log" hi
expect_rc "unusable -L" 2
err_has "unusable -L" "cannot open log $T/nodir/qq.log"
out_is "unusable -L answers nothing" ""

q -h
out_has "-h documents -L" "-L file     append a timestamped log"
out_has "usage shows -L" "[-L file]"

# --- -d (set default) ------------------------------------------------------
cp "$T/config.json" "$T/d.json"
chmod 600 "$T/d.json"
ln -s "$T/d.json" "$T/link.json"
export QQ_CONFIG="$T/link.json"

q -d alt
expect_rc "-d alt" 0; err_has "-d alt" "default profile set to alt"
out=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["default"])' "$T/d.json")
out_is "-d rewrites default" "alt"
true_that "-d keeps symlink" [ -L "$T/link.json" ]
true_that "-d keeps mode" [ "$(stat -c %a "$T/d.json")" = 600 ]
true_that "-d leaves no temp files" [ -z "$(find "$T" -name '.qq-config.*')" ]
q hi;                 out_is "new default used" "hello from alt-model"
q -d local -l;        expect_rc "-d with -l" 0; out_has "-d -l marks the new default" "* local"
q -d alt;             expect_rc "-d alt again" 0

cp "$T/d.json" "$T/before.json"
q -d nope
expect_rc "-d unknown" 2; err_has "-d unknown" "unknown profile"
true_that "-d unknown leaves file untouched" cmp -s "$T/d.json" "$T/before.json"

q -d local -p alt hi
expect_rc "-d with prompt" 0; out_is "-p wins over new default" "hello from alt-model"
q hi;                 out_is "default persisted" "hello from test-model"

printf 'tests: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
