#!/usr/bin/env python3
"""AI 服务自测：用 FakeLLM 注入固定 JSON，验证 4 路由 + /health 的链路（无需真实模型）。"""
import json
import threading
import urllib.request
import urllib.error
from http.server import HTTPServer

import app as appmod


class FakeLLM:
    def chat(self, system, user, max_tokens=1024):
        if "拆解" in system:
            return json.dumps({"steps": [
                {"title": "整理笔记", "note": "汇总各章重点", "estimated_minutes": 120, "depends_on": []},
                {"title": "刷选择题", "note": "做近三年真题", "estimated_minutes": 90, "depends_on": [0]},
                {"title": "错题复盘", "note": "重做错题", "estimated_minutes": 60, "depends_on": [1]},
                {"title": "模拟卷", "note": "全真模拟", "estimated_minutes": 120, "depends_on": [2]},
            ]}, ensure_ascii=False)
        if "提取" in system:
            return json.dumps({"items": [
                {"title": "回复客户邮件", "note": "关于报价"},
                {"title": "提交周报", "note": "周五前"},
            ]}, ensure_ascii=False)
        if "推演" in system:
            return json.dumps({"ordered": [
                {"id": 1, "predicted_priority": 0.9, "reason": "截止最近"}
            ], "postpone": [2], "drop": [3]}, ensure_ascii=False)
        if "预判" in system:
            return json.dumps({"todos": [
                {"title": "准备会议材料", "note": "Q3 复盘", "priority": 2, "due": "2026-08-27"}
            ]}, ensure_ascii=False)
        return "{}"

    def raw_chat(self, payload):
        return {"choices": [{"message": {"content": "ok"}}]}


def call(path, method="POST", body=None):
    url = f"http://127.0.0.1:8777{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode() or "{}")


def main():
    appmod.llm = FakeLLM()
    srv = HTTPServer(("127.0.0.1", 8777), appmod.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    ok = True
    st, body = call("/health", "GET")
    print("GET  /health          ->", st, body)
    ok &= st == 200

    st, body = call("/api/decompose", body={"goal": "备考期末"})
    print("POST /api/decompose   ->", st, json.dumps(body, ensure_ascii=False)[:100])
    ok &= st == 200 and len(body.get("steps", [])) == 4

    st, body = call("/api/extract", body={
        "text": "hi 在吗？记得回复客户邮件关于报价，还有提交周报周五前"})
    print("POST /api/extract     ->", st, json.dumps(body, ensure_ascii=False)[:100])
    ok &= st == 200 and len(body.get("items", [])) >= 1

    st, body = call("/api/reprioritize", body={"tasks": [
        {"id": 1, "title": "A", "due_date": "2026-08-27", "estimated_minutes": 30},
        {"id": 2, "title": "B", "due_date": "2026-09-01", "estimated_minutes": 200},
        {"id": 3, "title": "C", "due_date": "2026-12-01", "estimated_minutes": 10},
    ]})
    print("POST /api/reprioritize->", st, json.dumps(body, ensure_ascii=False)[:100])
    ok &= st == 200 and "ordered" in body

    st, body = call("/api/predict", body={"events": [
        {"title": "Q3 复盘会", "date": "2026-08-28"}]})
    print("POST /api/predict     ->", st, json.dumps(body, ensure_ascii=False)[:100])
    ok &= st == 200 and len(body.get("todos", [])) >= 1

    srv.shutdown()
    print("\nRESULT:", "ALL PASS" if ok else "FAIL")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
