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
from parseutil import extract_json, split_markdown_and_json

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


def _safe_parse(raw_text, expected_keys):
    """统一解析：先 split_markdown_and_json，再 extract_json 兜底。

    返回 (md_text, parsed_dict_or_None)。
    - md_text：去除围栏后的展示文本（可能为空字符串）
    - parsed_dict_or_None：包含期望 key 的 dict，或 None
    """
    md_text, parsed = split_markdown_and_json(raw_text)
    if isinstance(parsed, dict) and any(k in parsed for k in expected_keys):
        return md_text, parsed
    # 兜底：尝试 extract_json
    parsed2 = extract_json(raw_text)
    if isinstance(parsed2, dict) and any(k in parsed2 for k in expected_keys):
        return md_text, parsed2
    return md_text, None


def _build_success(feature, parsed, md_text_from_model, goal=None):
    """包装响应：保留原有结构化字段（steps/items/todos/ordered 等），追加 markdown 字段。"""
    md = md_text_from_model if md_text_from_model and md_text_from_model.strip() else None
    if feature == "decompose":
        steps = parsed.get("steps", []) if isinstance(parsed, dict) else []
        return 200, {"goal": goal, "steps": steps, "markdown": md or ""}
    if feature == "extract":
        items = parsed.get("items", []) if isinstance(parsed, dict) else []
        return 200, {"items": items, "markdown": md or ""}
    if feature == "predict":
        todos = parsed.get("todos", []) if isinstance(parsed, dict) else []
        return 200, {"todos": todos, "markdown": md or ""}
    # reprioritize
    base = parsed if isinstance(parsed, dict) else {}
    merged = {**base, "markdown": md or ""}
    return 200, merged


def handle_decompose(data):
    goal = (data.get("goal") or "").strip()
    if not goal:
        return 400, {"error": "missing 'goal'"}
    system, user = decompose_prompt(goal, data.get("context"))
    text = llm.chat(system, user)
    md_text, parsed = _safe_parse(text, ["steps"])
    # 即使解析失败，只要有 markdown 就返回 200（前端可展示 AI 文本）
    if parsed is None:
        return 200, {"goal": goal, "steps": [], "markdown": md_text or text, "warning": "structured_data_unavailable"}
    return _build_success("decompose", parsed, md_text, goal=goal)


def handle_extract(data):
    text = (data.get("text") or "").strip()
    if not text:
        return 400, {"error": "missing 'text'"}
    system, user = extract_prompt(text)
    out = llm.chat(system, user)
    md_text, parsed = _safe_parse(out, ["items"])
    if parsed is None:
        return 200, {"items": [], "markdown": md_text or out, "warning": "structured_data_unavailable"}
    return _build_success("extract", parsed, md_text)


def handle_reprioritize(data):
    tasks = data.get("tasks")
    if not isinstance(tasks, list):
        return 400, {"error": "missing 'tasks' list"}
    system, user = reprioritize_prompt(tasks)
    out = llm.chat(system, user)
    md_text, parsed = _safe_parse(out, ["ordered", "postpone", "drop"])
    if parsed is None:
        return 200, {"ordered": [], "postpone": [], "drop": [], "markdown": md_text or out, "warning": "structured_data_unavailable"}
    return _build_success("reprioritize", parsed, md_text)


def handle_predict(data):
    events = data.get("events")
    if not isinstance(events, list):
        return 400, {"error": "missing 'events' list"}
    system, user = predict_prompt(events)
    out = llm.chat(system, user)
    md_text, parsed = _safe_parse(out, ["todos"])
    if parsed is None:
        return 200, {"todos": [], "markdown": md_text or out, "warning": "structured_data_unavailable"}
    return _build_success("predict", parsed, md_text)


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
