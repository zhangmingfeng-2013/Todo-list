#!/usr/bin/env bash
# 验证 cpp-todo 的 /api/ai/* 代理：cpp-todo(:8931) -> AiGateway(libcurl) -> AI 服务(:8777, Ollama deepseek)
set -u
PROJ=/Users/zhangmingfeng/Projects/TODO-list
PY=/Users/zhangmingfeng/.workbuddy/binaries/python/versions/3.13.12/bin/python3

cleanup() { [ -n "${AIPID:-}" ] && kill "$AIPID" 2>/dev/null; [ -n "${CPPID:-}" ] && kill "$CPPID" 2>/dev/null; }
trap cleanup EXIT

# 1) AI 服务（Ollama 后端）:8777
cd "$PROJ/ai"
AI_BACKEND=ollama AI_MODEL=deepseek-r1:1.5b "$PY" app.py > /tmp/ai_svc.log 2>&1 &
AIPID=$!
cd "$PROJ"

# 2) cpp-todo（刚构建）:8931
"$PROJ/build_ai_verify/todo" serve --port 8931 --db /tmp/cpptodo_test.db > /tmp/cpp_svc.log 2>&1 &
CPPID=$!

# 等待就绪
for i in $(seq 1 30); do
  curl -s --noproxy '*' --max-time 2 http://127.0.0.1:8777/health >/dev/null 2>&1 && break
  sleep 1
done
for i in $(seq 1 30); do
  curl -s --noproxy '*' --max-time 2 http://127.0.0.1:8931/ >/dev/null 2>&1 && break
  sleep 1
done

echo "===== C++ 代理端到端验证 ====="
echo "--- GET  /api/ai/health (经 cpp-todo :8931 代理到 AI 服务 :8777) ---"
curl -s --noproxy '*' --max-time 10 http://127.0.0.1:8931/api/ai/health; echo
echo "--- POST /api/ai/decompose (经 cpp-todo :8931 代理) ---"
curl -s --noproxy '*' --max-time 60 -X POST http://127.0.0.1:8931/api/ai/decompose \
  -H 'Content-Type: application/json' -d '{"goal":"备考期末"}'; echo
echo "===== 结束 ====="
