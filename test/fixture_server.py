#!/usr/bin/env python3
"""Local OpenAI-compatible fixture server for test/sql/open_prompt_fixture.test.

  POST /v1/chat/completions      200; the reply text is a JSON summary of the request
                                 (model, messages, response_format, temperature, max_tokens, auth header)
  POST /status/<code>            <code> with an OpenAI-style {"error":{"message":...}} body
  POST /ollama-error             404 with an Ollama-style {"error":"..."} body
  POST /ratelimit/<key>/<n>      429 (Retry-After: 0) for <n> requests per <key>, then once like /v1/...
  POST /always-429               429 (Retry-After: 0)
  POST /garbage                  200 with a body that is not JSON
  POST /no-choices               200 with {"choices":[]}
  POST /refusal                  200 with a refusal instead of content

Usage:
  python3 test/fixture_server.py [port]   (default 8766)
  OPEN_PROMPT_FIXTURE_URL=http://127.0.0.1:8766 ./build/release/test/unittest test/sql/open_prompt_fixture.test
"""
import http.server
import json
import sys
import threading

COUNTERS = {}
LOCK = threading.Lock()


def completion(text):
    return {"choices": [{"index": 0, "message": {"role": "assistant", "content": text}, "finish_reason": "stop"}]}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, status, body, headers=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def echo(self, request):
        summary = {
            "model": request.get("model"),
            "messages": request.get("messages"),
            "response_format": request.get("response_format"),
            "temperature": request.get("temperature"),
            "max_tokens": request.get("max_tokens"),
            "auth": self.headers.get("Authorization"),
        }
        summary = {k: v for k, v in summary.items() if v is not None}
        self.reply(200, completion(json.dumps(summary, sort_keys=True, separators=(",", ":"), ensure_ascii=False)))

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            request = json.loads(raw)
        except ValueError:
            self.reply(400, {"error": {"message": "request body is not JSON"}})
            return
        parts = self.path.strip("/").split("/")
        if self.path == "/v1/chat/completions":
            self.echo(request)
        elif parts[0] == "status":
            self.reply(int(parts[1]), {"error": {"message": "fixture error %s" % parts[1], "type": "test"}})
        elif parts[0] == "ollama-error":
            self.reply(404, {"error": "model '%s' not found" % request.get("model")})
        elif parts[0] == "ratelimit":
            with LOCK:
                seen = COUNTERS.get(parts[1], 0)
                # start over after a success, so the tests can run again against the same server
                COUNTERS[parts[1]] = seen + 1 if seen < int(parts[2]) else 0
            if seen < int(parts[2]):
                self.reply(429, {"error": {"message": "slow down"}}, {"Retry-After": "0"})
            else:
                self.echo(request)
        elif parts[0] == "always-429":
            self.reply(429, {"error": {"message": "slow down"}}, {"Retry-After": "0"})
        elif parts[0] == "garbage":
            self.reply(200, b"<html>not json</html>")
        elif parts[0] == "no-choices":
            self.reply(200, {"choices": []})
        elif parts[0] == "refusal":
            self.reply(200, {"choices": [{"message": {"role": "assistant", "content": None, "refusal": "I can't"}}]})
        else:
            self.reply(404, {"error": {"message": "no such fixture path"}})

    def log_message(self, format, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
