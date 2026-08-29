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
import os
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
from parseutil import extract_json, split_markdown_and_json, build_fallback_markdown

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


def _normalize_reprioritize_ids(parsed, tasks):
    """修复模型常见错误：把 ordered/postpone/drop 中的 id 从数组下标映射回真实任务 id。

    模型训练初期可能把 id 写成 0/1/2（数组位置）而非输入的真实 id。
    启发式：
      1) 收集 JSON 中所有出现的 id 值；
      2) 若全部是 0..n-1 范围的整数，且「与真实 id 集合完全不重合」或「集合大小等于 n 且完全覆盖 0..n-1」，
         则判定为下标风格，做整体映射；
      3) 否则保持原样（真实 id 风格）。
    """
    if not isinstance(parsed, dict) or not isinstance(tasks, list) or not tasks:
        return parsed
    n = len(tasks)
    real_ids = [t.get("id") for t in tasks]
    real_id_set = set(tid for tid in real_ids if tid is not None)

    # 收集 JSON 中出现的所有 id
    json_ids = set()
    ordered = parsed.get("ordered") or []
    if isinstance(ordered, list):
        for o in ordered:
            if isinstance(o, dict) and "id" in o and isinstance(o["id"], int):
                json_ids.add(o["id"])
    for key in ("postpone", "drop"):
        lst = parsed.get(key) or []
        if isinstance(lst, list):
            for x in lst:
                if isinstance(x, int):
                    json_ids.add(x)

    # 判定：是否下标风格
    is_index_style = bool(json_ids) and all(0 <= x < n for x in json_ids)
    if is_index_style:
        # 若与真实 id 有交集但覆盖了 0..n-1 全集（长度为 n 的全等排列），仍判下标
        overlap = json_ids & real_id_set
        if overlap and len(json_ids) != n:
            is_index_style = False

    def _convert(x):
        if is_index_style and isinstance(x, int) and 0 <= x < n:
            return real_ids[x]
        return x

    fixed = dict(parsed)
    ordered = fixed.get("ordered") or []
    if isinstance(ordered, list):
        for o in ordered:
            if isinstance(o, dict) and "id" in o:
                o["id"] = _convert(o["id"])
    postpone = fixed.get("postpone") or []
    if isinstance(postpone, list):
        fixed["postpone"] = [_convert(x) for x in postpone]
    drop = fixed.get("drop") or []
    if isinstance(drop, list):
        fixed["drop"] = [_convert(x) for x in drop]
    return fixed


def _inject_reprioritize_titles(md_text, parsed, tasks):
    """把 Markdown 中的「任务 #id」占位符替换为真实任务标题，同时清理模板文字。"""
    if not isinstance(md_text, str) or not md_text:
        return md_text
    title_map = {}
    if isinstance(tasks, list):
        for t in tasks:
            if "id" in t and "title" in t:
                title_map[t["id"]] = t["title"]
    ordered = parsed.get("ordered") if isinstance(parsed, dict) else []
    if isinstance(ordered, list):
        for o in ordered:
            if isinstance(o, dict) and "id" in o and "title" in o:
                title_map.setdefault(o["id"], o["title"])

    def _sub(m):
        tid = m.group(1)
        try:
            key = int(tid)
        except ValueError:
            key = tid
        if key in title_map:
            return title_map[key]
        if str(key) in title_map:
            return title_map[str(key)]
        return m.group(0)

    import re as _re
    result = _re.sub(r"任务 #(\S+)", _sub, md_text)
    # 清理【】 / ### Markdown/JSON 标题 等各类模板文字
    result = _re.sub(
        r"^\s*[#\s]*第一部分\s*[：:：—\-]*\s*(Markdown|展示|展示文本)[^\n]*\n?", "", result,
        flags=_re.IGNORECASE | _re.MULTILINE,
    )
    result = _re.sub(
        r"^\s*[#\s]*\【第一部分\s*[：:：—\-]?\s*Markdown\s*展示文本\s*\】[^\n]*\n?", "", result,
        flags=_re.IGNORECASE | _re.MULTILINE,
    )
    result = _re.sub(
        r"^\s*#{1,6}\s*Markdown\s*展示\s*文本\s*$", "", result,
        flags=_re.IGNORECASE | _re.MULTILINE,
    )
    result = _re.sub(
        r"^\s*#{1,6}\s*Markdown\s*展示\s*$", "", result,
        flags=_re.IGNORECASE | _re.MULTILINE,
    )
    # 裁剪【第二部分 / JSON 结构化数据】之后的所有内容
    result = _re.sub(
        r"\n\s*[#\s]*\【第二部分\s*[：:：—\-]?\s*JSON\s*结构化数据\s*\】[\s\S]*$", "", result,
        flags=_re.IGNORECASE,
    )
    result = _re.sub(
        r"\n\s*[#\s]*第二部分\s*[：:：—\-]\s*(JSON|结构化)[^\n]*[\s\S]*$", "", result,
        flags=_re.IGNORECASE,
    )
    result = _re.sub(
        r"\n\s*#{1,6}\s*JSON\s*结构化(数据)?\s*$[\s\S]*", "", result,
        flags=_re.IGNORECASE,
    )
    # 删除尾部 ```json ... ``` 围栏
    result = _re.sub(r"```(?:json)?[\s\S]*?```\s*$", "", result, flags=_re.IGNORECASE)
    # 清理常见重复的"当前任务池无需调整。"（当已有建议顺序时）
    if "建议顺序" in result:
        result = _re.sub(r"\n\s*当前任务池无需调整。\s*$", "", result)
    # 去除连续 3 行以上的空白行
    result = _re.sub(r"\n{3,}", "\n\n", result)
    return result.strip()


def _is_reprioritize_md_template(md_fixed):
    """判定 Markdown 是否仍是模型复述的提示词模板，没有真实内容。

    典型模板特征：
    - 包含「任务名1 / 任务名2 / 任务名3」
    - 理由写着「理由」两字（而非具体文字）
    - 包含「任务 #0 任务 #1 任务 #2」这类占位
    """
    if not isinstance(md_fixed, str) or not md_fixed.strip():
        return True
    bad_markers = ("任务名1", "任务名2", "任务名3", "任务名 1", "任务名 2", "任务名 3")
    if any(m in md_fixed for m in bad_markers):
        return True
    import re as _re
    if _re.search(r"[—\-–]\s*理由\s*$", md_fixed, flags=_re.MULTILINE):
        return True
    if _re.search(r"任务\s*#[012](?:\D|$)", md_fixed):
        # 必须确认为占位符，而不是真实任务名里含有"任务 #"
        placeholder_count = len(_re.findall(r"任务\s*#\d+\b", md_fixed))
        if placeholder_count >= 2:
            return True
    return False


def handle_reprioritize(data):
    tasks = data.get("tasks")
    if not isinstance(tasks, list):
        return 400, {"error": "missing 'tasks' list"}
    if not tasks:
        return 400, {"error": "任务列表为空，无法推演。请先创建任务，或在其他视图查看任务后再回到 AI 助手"}
    system, user = reprioritize_prompt(tasks)
    out = llm.chat(system, user)
    md_text, parsed = _safe_parse(out, ["ordered", "postpone", "drop"])
    if parsed is None:
        md_fixed = _inject_reprioritize_titles(md_text or out, {}, tasks)
        return 200, {"ordered": [], "postpone": [], "drop": [], "markdown": md_fixed, "warning": "structured_data_unavailable"}
    parsed_fixed = _normalize_reprioritize_ids(parsed, tasks)
    # 去重：ordered 中重复的 id 只保留首个
    seen_ids = set()
    dedup_ordered = []
    for o in parsed_fixed.get("ordered") or []:
        if isinstance(o, dict) and "id" in o:
            if o["id"] in seen_ids:
                continue
            seen_ids.add(o["id"])
        dedup_ordered.append(o)
    parsed_fixed["ordered"] = dedup_ordered
    # 若 ordered 没覆盖所有任务 → 按 tasks 原始顺序补齐（不会丢任何一个任务）
    covered = {o["id"] for o in dedup_ordered if isinstance(o, dict) and "id" in o}
    for t in tasks:
        tid = t.get("id") if isinstance(t, dict) else None
        if tid is None or tid in covered:
            continue
        dedup_ordered.append({
            "id": tid,
            "title": t.get("title", ""),
            "predicted_priority": 0.0,
            "reason": "（未出现在模型输出，默认归入末尾）",
        })
        covered.add(tid)
    # 后端回退 Markdown：若模型输出的 md 为空、未含"建议顺序"，或仍是模板复述
    md_fixed = _inject_reprioritize_titles(md_text, parsed_fixed, tasks)
    if not md_fixed or "建议顺序" not in md_fixed or _is_reprioritize_md_template(md_fixed):
        # _titles 注入让 fallback 用真实标题
        payload = dict(parsed_fixed)
        payload["_titles"] = {str(t["id"]): t["title"] for t in tasks if "id" in t and "title" in t}
        md_fixed = build_fallback_markdown("reprioritize", payload)
    return _build_success("reprioritize", parsed_fixed, md_fixed)


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
