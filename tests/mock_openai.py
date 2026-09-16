#!/usr/bin/env python3
"""Minimal OpenAI-compatible chat server plus LiteLLM MCP tool endpoint, for tests.

Prints the port it listens on, then appends every request as a JSON line to
the log path given as argv[1]. The requested model picks the chat behavior.
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = sys.argv[1]


def reply(content=None, tool_calls=None):
    message = {"role": "assistant", "content": content}
    if tool_calls:
        message["tool_calls"] = tool_calls
    finish = "tool_calls" if tool_calls else "stop"
    return 200, json.dumps({"choices": [{"finish_reason": finish, "message": message}]})


def call(call_id, name, arguments):
    return {"id": call_id, "type": "function", "function": {"name": name, "arguments": arguments}}


def local(name, **arguments):
    return call("call-" + name, name, json.dumps(arguments))


# Models that call qq's local tools once, then answer with the tool results
# joined by " | ".
LOCAL = {
    "reader": [local("read_file", path="notes.txt")],
    "lister": [local("list_directory")],
    "searcher": [local("search_files", pattern="needle")],
    "outsider": [local("read_file", path="../secret.txt")],
    "writer": [local("write_file", path="out.txt", content="written by model\n")],
    "editor": [local("edit_file", path="notes.txt", old_text="alpha", new_text="ALPHA")],
    "editor2": [local("edit_file", path="notes.txt", old_text="line", new_text="LINE")],
    "runner": [local("run_command", command="echo from-shell; echo oops >&2; exit 3")],
    # Models send an empty path where they mean the current directory.
    "emptylister": [local("list_directory", path="")],
    "emptysearcher": [local("search_files", pattern="needle", path="")],
    "emptyreader": [local("read_file", path="")],
}


def chat(body):
    model = body.get("model")
    messages = body["messages"]
    tool_results = [m["content"] for m in messages if m["role"] == "tool"]

    if model in LOCAL:
        if tool_results:
            return reply(" | ".join(tool_results))
        return reply(None, LOCAL[model])
    if model == "empty":
        return reply("")
    if model == "unauthorized":
        return 401, '{"error": {"message": "invalid api key"}}'
    if model == "garbage":
        return 200, "this is not json"
    if model == "thinker":
        return reply("<think>reasoning</think>\n\nthought answer")
    if model == "slow":
        time.sleep(3)
        return reply("too late")
    if model == "tooler":
        if messages[-1]["role"] == "tool":
            return reply("answer from tool: " + messages[-1]["content"])
        return reply(None, [call("call-1", "searxng_mcp-web_search", '{"query": "cats"}')])
    if model == "twotools":
        if tool_results:
            return reply(" | ".join(tool_results))
        return reply(None, [
            call("call-a", "searxng_mcp-web_search", '{"query": "dogs"}'),
            call("call-b", "other_mcp-lookup", "{}"),
            call("call-c", "searxng_mcp-web_search", ""),
            call("call-d", "ghost_mcp-anything", '{"x": 1}'),
            call("call-e", "searxng_mcp-web_search", "not json"),
        ])
    if model == "looper":
        return reply(None, [call("call-%d" % len(tool_results), "searxng_mcp-web_search", '{"query": "again"}')])
    # Models that never stop asking for the same local tool call, whatever the
    # tool results say. qq must answer the repeats itself instead of asking again.
    if model == "writeloop":
        return reply(None, [call("call-%d" % len(tool_results), "write_file",
                                 json.dumps({"path": "out.txt", "content": "written by model\n"}))])
    # A call it repeats forever next to one that is new every round, so the loop
    # keeps making progress while the repeat must still be asked about only once.
    if model == "mixloop":
        n = len(tool_results)
        return reply(None, [call("call-w%d" % n, "write_file",
                                 json.dumps({"path": "out.txt", "content": "written by model\n"})),
                            call("call-s%d" % n, "search_files",
                                 json.dumps({"pattern": "p%d" % n}))])
    if model == "readloop":
        return reply(None, [call("call-%d" % len(tool_results), "read_file",
                                 json.dumps({"path": "notes.txt"}))])
    # write, read, edit, then the same read again: the second read must really
    # happen, because the edit made the remembered one stale.
    if model == "rereader":
        steps = [local("write_file", path="r.txt", content="one\n"),
                 local("read_file", path="r.txt"),
                 local("edit_file", path="r.txt", old_text="one", new_text="two"),
                 local("read_file", path="r.txt")]
        n = len(tool_results)
        return reply(" | ".join(tool_results)) if n >= len(steps) else reply(None, [steps[n]])
    return reply(f"hello from {model}")


def tool(body):
    server = body.get("server_id")
    if server != "searxng_mcp":
        return 404, json.dumps({"detail": {"error": "server_not_found",
                                           "message": f"MCP server '{server}' was not found"}})
    query = (body.get("arguments") or {}).get("query")
    if query is None:
        return 200, json.dumps({"content": [{"type": "text", "text": "Missing required argument query"}],
                                "isError": True})
    return 200, json.dumps({"content": [{"type": "text", "text": f"results for {query}"}], "isError": False})


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        with open(LOG, "a") as f:
            f.write(json.dumps({"path": self.path, "auth": self.headers.get("Authorization"), "body": body}) + "\n")

        code, resp = tool(body) if self.path.endswith("/mcp-rest/tools/call") else chat(body)
        data = resp.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
print(server.server_address[1], flush=True)
server.serve_forever()
