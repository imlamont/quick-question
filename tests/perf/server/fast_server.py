#!/usr/bin/env python3
"""Minimal HTTP/1.1 keep-alive test server for qq perf tests.

Unlike tests/mock_openai.py (ThreadingHTTPServer, HTTP/1.0, per-request
logging), this does no logging and no per-request file I/O, and speaks
HTTP/1.1 with keep-alive so connection reuse across tool rounds (T6) and
per-request overhead (T3) can be measured without that noise.

Usage: fast_server.py PORT [RESPONSE_BYTES]

Serves a fixed chat-completion JSON on POST /v1/chat/completions. If
RESPONSE_BYTES is given, the "content" field is padded to roughly that size
(for T8's response-size sweep).

For T6 (connection reuse across tool rounds), a request whose "model" is
"lister" gets a two-round exchange: the first reply asks qq to run its local
list_directory tool, the second (once qq posts the tool result back) returns
a plain answer. Both go out over the same keep-alive connection if qq reuses
its curl handle correctly.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def make_body(pad_bytes):
    content = "ok" + ("x" * pad_bytes if pad_bytes else "")
    return json.dumps({
        "id": "bench-1",
        "object": "chat.completion",
        "choices": [{
            "index": 0,
            "message": {"role": "assistant", "content": content},
            "finish_reason": "stop",
        }],
        "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
    }).encode()


def make_tool_call_body():
    return json.dumps({
        "id": "bench-tool-1",
        "object": "chat.completion",
        "choices": [{
            "index": 0,
            "message": {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "call-list_directory",
                    "type": "function",
                    "function": {"name": "list_directory", "arguments": "{}"},
                }],
            },
            "finish_reason": "tool_calls",
        }],
    }).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    lister_rounds = 0

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        model = ""
        try:
            model = json.loads(raw).get("model", "") if raw else ""
        except json.JSONDecodeError:
            pass

        if model == "lister":
            Handler.lister_rounds += 1
            body = make_tool_call_body() if Handler.lister_rounds == 1 else make_body(0)
        else:
            body = make_body(PAD_BYTES)

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    port = int(sys.argv[1])
    PAD_BYTES = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
