#!/usr/bin/env bash
# 对比验证：直连 AI 服务 vs 经 cpp-todo 代理，确认代理与上游行为一致（排除 Ollama flake）
set -u
PROJ=/Users/zhangmingfeng/Projects/TODO-list
PY=/Users/zhangmingfeng/.workbuddy/binaries/python/versions/3.13.12/bin/python3

cleanup() { [ -n "${AIPID:-}" ] && kill "$AIPID" 2>/dev/null; [ -n "${CPPID:-}" ] && kill "$CPPID" 2>/dev/null; }
trap cleanup EXIT

cd "$PROJ/ai"
AI_BACKEND=ollama AI_MODEL=deepseek-r1:1.5b "$PY" app.py > /tmp/ai_svc.log 2>&1 &
AIPID=$!
cd "$PROJ"
"$PROJ/build_ai_verify/todo" serve --port 8931 --db /tmp/cpptodo_test.db > /tmp/cpp_svc.log 2>&1 &
CPPID=$!

for i in $(seq 1 30); do curl -s --noproxy '*' --max-time 2 http://127.0.0.1:8777/health >/dev/null 2>&1 && break; sleep 1; done
for i in $(seq 1 30); do curl -s --noproxy '*' --max-time 2 http://127.0.0.1:8931/ >/dev/null 2>&1 && break; sleep 1; done

BODY='{"goal":"备考期末"}'
echo "===== 直连 AI 服务 (:8777) ====="
for n in 1 2 3; do
  R=$(curl -s --noproxy '*' --max-time 60 -X POST http://127.0.0.1:8777/api/decompose -H 'Content-Type: application/json' -d "$BODY")
  echo "  尝试$n: ${R:0:160}"
  echo "$R" | grep -q '"steps"' && { echo "  -> 直连成功"; break; }
done

echo "===== 经 cpp-todo 代理 (:8931 /api/ai/decompose) ====="
for n in 1 2 3; do
  R=$(curl -s --noproxy '*' --max-time 60 -X POST http://127.0.0.1:8931/api/ai/decompose -H 'Content-Type: application/json' -d "$BODY")
  echo "  尝试$n: ${R:0:160}"
  echo "$R" | grep -q '"steps"' && { echo "  -> 代理成功"; break; }
done
echo "===== 结束 ====="
