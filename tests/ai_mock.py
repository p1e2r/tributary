#!/usr/bin/env python3
"""Mock Anthropic / OpenAI / Gemini / Ollama streaming endpoints for testing Tributary's assistant.

Each conversation is scripted: read src/util.hpp -> edit it -> final answer. Every request body is
appended to the log so tests can check what the client sent (tool results, headers, auth)."""
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8780
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/aimock-log.jsonl"
OLD = "inline int add(int a, int b) {\n    return a + b;\n}"
NEW = "constexpr int add(int a, int b) noexcept {\n    return a + b;\n}"
FINAL = ("I made `add` in **src/util.hpp** `constexpr` and `noexcept`, so it can be used in constant expressions:\n\n"
         "```cpp\nconstexpr int add(int a, int b) noexcept {\n    return a + b;\n}\n```\n\nThe change is in the Changes tab, ready for your pull request.")


def step(body: str) -> int:
    # how many tool results came back so far
    return max(body.count('"tool_result"'), body.count('"role":"tool"'), body.count('"functionResponse"'))


def plan(n):
    if n == 0: return ("Let me look at the helper first.", "read_file", {"path": "src/util.hpp"})
    if n == 1: return ("", "edit_file", {"path": "src/util.hpp", "old_string": OLD, "new_string": NEW})
    return (FINAL, None, None)


def chunks(s, n=12):
    return [s[i:i + n] for i in range(0, len(s), n)] or [""]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def start_stream(self, ctype="text/event-stream"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def chunk(self, data: str):
        b = data.encode()
        self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
        self.wfile.flush()
        time.sleep(0.03)

    def end_stream(self):
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def sse(self, obj, event=None):
        self.chunk((f"event: {event}\n" if event else "") + "data: " + (obj if isinstance(obj, str) else json.dumps(obj)) + "\n\n")

    def do_GET(self):
        p = self.path
        with open(LOG, "a") as f:
            f.write(json.dumps({"method": "GET", "path": p, "headers": dict(self.headers)}) + "\n")
        if p.startswith("/v1/models"):
            return self.send_json({"data": [{"id": "claude-haiku-4-5", "type": "model"}, {"id": "claude-sonnet-4-5", "type": "model"}]})
        if p.startswith("/models"):
            return self.send_json({"data": [{"id": "gpt-4o-mini"}, {"id": "gpt-5"}, {"id": "text-embedding-3-small"}]})
        if p.startswith("/v1beta/models"):
            return self.send_json({"models": [{"name": "models/gemini-2.5-flash", "supportedGenerationMethods": ["generateContent"]},
                                              {"name": "models/gemini-2.5-pro", "supportedGenerationMethods": ["generateContent"]},
                                              {"name": "models/embedding-001", "supportedGenerationMethods": ["embedContent"]}]})
        if p.startswith("/api/tags"):
            return self.send_json({"models": [{"name": "qwen3:8b"}, {"name": "llama3.2:latest"}]})
        self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode()
        with open(LOG, "a") as f:
            f.write(json.dumps({"method": "POST", "path": self.path, "headers": dict(self.headers), "body": json.loads(body)}) + "\n")
        compact = json.dumps(json.loads(body), separators=(",", ":"))
        text, tool, args = plan(step(compact))
        if '"tools"' not in compact and '"functionDeclarations"' not in compact:  # plain chat: a commit message
            text, tool, args = ("Show relative fetch times in the toolbar\n\nReplace the absolute timestamp with \"2 hours ago\" style text, "
                                "which is easier to read at a glance.", None, None)
        p = self.path
        if p.startswith("/v1/messages"):
            self.start_stream()
            self.sse({"type": "message_start", "message": {"id": "msg_1", "role": "assistant", "content": []}}, "message_start")
            i = 0
            if text:
                self.sse({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}, "content_block_start")
                for c in chunks(text):
                    self.sse({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": c}}, "content_block_delta")
                self.sse({"type": "content_block_stop", "index": 0}, "content_block_stop")
                i = 1
            if tool:
                self.sse({"type": "content_block_start", "index": i, "content_block": {"type": "tool_use", "id": f"toolu_{step(compact)}", "name": tool, "input": {}}}, "content_block_start")
                for c in chunks(json.dumps(args), 20):
                    self.sse({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": c}}, "content_block_delta")
                self.sse({"type": "content_block_stop", "index": i}, "content_block_stop")
            self.sse({"type": "message_delta", "delta": {"stop_reason": "tool_use" if tool else "end_turn"}}, "message_delta")
            self.sse({"type": "message_stop"}, "message_stop")
            return self.end_stream()
        if p.startswith("/chat/completions"):
            self.start_stream()
            for c in chunks(text):
                self.sse({"choices": [{"index": 0, "delta": {"content": c}}]})
            if tool:
                a = json.dumps(args)
                self.sse({"choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_1", "type": "function", "function": {"name": tool, "arguments": ""}}]}}]})
                for c in chunks(a, 20):
                    self.sse({"choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "function": {"arguments": c}}]}}]})
            self.sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls" if tool else "stop"}]})
            self.sse("[DONE]")
            return self.end_stream()
        if ":streamGenerateContent" in p:
            self.start_stream()
            for c in chunks(text, 30):
                self.sse({"candidates": [{"content": {"role": "model", "parts": [{"text": c}]}}]})
            if tool:
                self.sse({"candidates": [{"content": {"role": "model", "parts": [{"functionCall": {"name": tool, "args": args}, "thoughtSignature": "c2lnbmF0dXJl"}]}, "finishReason": "STOP"}]})
            else:
                self.sse({"candidates": [{"content": {"role": "model", "parts": [{"text": ""}]}, "finishReason": "STOP"}]})
            return self.end_stream()
        if p.startswith("/api/chat"):
            self.start_stream("application/x-ndjson")
            for c in chunks(text):
                self.chunk(json.dumps({"message": {"role": "assistant", "content": c}, "done": False}) + "\n")
            if tool:
                self.chunk(json.dumps({"message": {"role": "assistant", "content": "", "tool_calls": [{"function": {"name": tool, "arguments": args}}]}, "done": False}) + "\n")
            self.chunk(json.dumps({"message": {"role": "assistant", "content": ""}, "done": True, "done_reason": "stop"}) + "\n")
            return self.end_stream()
        self.send_json({"error": {"message": "unknown endpoint " + p}}, 404)


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
