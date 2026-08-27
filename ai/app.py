#!/usr/bin/env python3
"""cpp-todo AI 服务（P0 骨架）。

路由：
  GET  /health
  POST /api/decompose      {goal, context?}
  POST /api/extract        {text}
  POST /api/reprioritize   {tasks:[...]}
  POST /api/predict        {events:[...]}
  POST /v1/chat/completions  (透传至所配置的后端 LLM)

运行：  python ai/app.py
配置：  环境变量 AI_BASE_URL / AI_API_KEY / AI_MODEL / AI_HOST / AI_PORT
        或 ~/.cpp-todo.conf 的 [ai] 段。
"""
import json
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

from config import load_config
from llm import LLMClient
from prompts import (
    decompose_prompt,
    extract_prompt,
    reprioritize_prompt,
    predict_prompt,
)
from parseutil import extract_json

cfg = load_config()
llm = LLMClient(
    cfg["base_url"], cfg["api_key"], cfg["model"], float(cfg.get("temperature", 0.3))
)


def _reply(handler, status, obj):
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def _body_json(handler):
    length = int(handler.headers.get("Content-Length", 0) or 0)
    raw = handler.rfile.read(length) if length else b"{}"
    try:
        return json.loads(raw.decode("utf-8") or "{}")
    except Exception:
        return {}


def handle_decompose(data):
    goal = (data.get("goal") or "").strip()
    if not goal:
        return 400, {"error": "missing 'goal'"}
    system, user = decompose_prompt(goal, data.get("context"))
    text = llm.chat(system, user)
    parsed = extract_json(text)
    if not parsed or "steps" not in parsed:
        return 502, {"error": "model returned no parseable steps", "raw": text}
    return 200, {"goal": goal, "steps": parsed["steps"]}


def handle_extract(data):
    text = (data.get("text") or "").strip()
    if not text:
        return 400, {"error": "missing 'text'"}
    system, user = extract_prompt(text)
    out = llm.chat(system, user)
    parsed = extract_json(out)
    if not parsed or "items" not in parsed:
        return 502, {"error": "model returned no parseable items", "raw": out}
    return 200, {"items": parsed["items"]}


def handle_reprioritize(data):
    tasks = data.get("tasks")
    if not isinstance(tasks, list):
        return 400, {"error": "missing 'tasks' list"}
    system, user = reprioritize_prompt(tasks)
    out = llm.chat(system, user)
    parsed = extract_json(out)
    if not parsed:
        return 502, {"error": "model returned no parseable plan", "raw": out}
    return 200, parsed


def handle_predict(data):
    events = data.get("events")
    if not isinstance(events, list):
        return 400, {"error": "missing 'events' list"}
    system, user = predict_prompt(events)
    out = llm.chat(system, user)
    parsed = extract_json(out)
    if not parsed or "todos" not in parsed:
        return 502, {"error": "model returned no parseable todos", "raw": out}
    return 200, {"todos": parsed["todos"]}


ROUTES = {
    ("POST", "/api/decompose"): handle_decompose,
    ("POST", "/api/extract"): handle_extract,
    ("POST", "/api/reprioritize"): handle_reprioritize,
    ("POST", "/api/predict"): handle_predict,
}


class Handler(BaseHTTPRequestHandler):
    def _dispatch(self):
        parsed = urlparse(self.path)
        path = parsed.path
        if self.command == "GET" and path == "/health":
            _reply(self, 200, {
                "status": "ok",
                "model": llm.resolved_model(),
                "base_url": cfg["base_url"],
                "configured_model": cfg["model"],
            })
            return
        if self.command == "POST" and path == "/v1/chat/completions":
            data = _body_json(self)
            try:
                result = llm.raw_chat(data)
            except Exception as e:
                _reply(self, 502, {"error": str(e)})
                return
            _reply(self, 200, result)
            return
        key = (self.command, path)
        if key in ROUTES:
            try:
                status, obj = ROUTES[key](_body_json(self))
            except Exception as e:
                _reply(self, 500, {"error": str(e)})
                return
            _reply(self, status, obj)
            return
        _reply(self, 404, {"error": "not found", "path": path})

    def do_GET(self):
        self._dispatch()

    def do_POST(self):
        self._dispatch()

    def log_message(self, fmt, *args):
        print(f"[ai-http] {fmt % args}")


def main():
    host = cfg.get("host", "127.0.0.1")
    port = int(cfg.get("port", 8777))
    print(f"[ai-service] http://{host}:{port}  model={cfg['model']}  llm={cfg['base_url']}")
    ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == "__main__":
    main()
