#!/usr/bin/env bash
# 端到端验证：自训模型服务 + AI 服务 4 路由是否输出合法 JSON（离线/隐私闭环）。
# 用法：bash verify_closed_loop.sh
set -u
TRAIN_DIR="$(cd "$(dirname "$0")" && pwd)"
AI_DIR="$(dirname "$TRAIN_DIR")"

cleanup() {
  [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
  [ -n "${AISRV:-}" ] && kill "$AISRV" 2>/dev/null
}
trap cleanup EXIT

cd "$TRAIN_DIR"
echo "[verify] 启动自训模型服务 (mlx_lm.server :8080) ..."
./venv/bin/python -m mlx_lm server --model ./fused --host 127.0.0.1 --port 8080 > serve_verify.log 2>&1 &
SRV=$!

# 等待模型加载（轮询 /v1/models）
for i in $(seq 1 40); do
  if curl -s --noproxy '*' --max-time 3 http://127.0.0.1:8080/v1/models >/dev/null 2>&1; then
    echo "[verify] 模型服务就绪 (${i}x2s)"; break
  fi
  sleep 2
done

cd "$AI_DIR"
echo "[verify] 启动 AI 服务 (:8777) 指向 8080 ..."
env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy -u ALL_PROXY -u all_proxy \
  AI_BASE_URL=http://127.0.0.1:8080/v1 \
  /Users/zhangmingfeng/.workbuddy/binaries/python/versions/3.13.12/bin/python3 app.py > ai_verify.log 2>&1 &
AISRV=$!
for i in $(seq 1 20); do
  if curl -s --noproxy '*' --max-time 3 http://127.0.0.1:8777/health >/dev/null 2>&1; then
    echo "[verify] AI 服务就绪 (${i}x1s)"; break
  fi
  sleep 1
done

echo
echo "================ 4 路由端到端验证 ================"
call() {
  local path="$1"; local body="$2"; local label="$3"
  echo "--- $label ($path) ---"
  curl -s --noproxy '*' --max-time 60 -X POST "http://127.0.0.1:8777$path" \
    -H 'Content-Type: application/json' -d "$body" | head -c 800
  echo; echo
}

call /api/decompose '{"goal":"完成项目报告"}' "任务拆解"
call /api/extract '{"text":"记得回复客户邮件关于报价，周五前提交周报，顺便把发票搞定"}' "上下文抽取"
call /api/reprioritize '{"tasks":[{"id":1,"title":"A","due_date":"2026-08-27","estimated_minutes":30},{"id":2,"title":"B","due_date":"2026-09-01","estimated_minutes":200}]}' "动态重排"
call /api/predict '{"events":[{"title":"Q3 复盘会","date":"2026-08-28"}]}' "任务预判"
echo "=================================================="
