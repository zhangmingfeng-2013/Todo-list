#!/usr/bin/env bash
# cpp-todo AI 后端栈一键管理：模型服务 (:8080) + AI 服务 (:8777)。
# cpp-todo 主服务 (:8931) 仍由 ./build/todo serve 单独启动。
#
# 用法：
#   ai/start_ai_backend.sh start     # 启动（默认；幂等，已运行则跳过）
#   ai/start_ai_backend.sh stop      # 停止
#   ai/start_ai_backend.sh restart   # 重启
#   ai/start_ai_backend.sh status    # 查看状态
#
# 模型目录：ai/train/fused/（来自 train_lora.py 的 mlx_lm fuse 产物）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TRAIN="$ROOT/ai/train"
AI="$ROOT/ai"
PY="/Users/zhangmingfeng/.workbuddy/binaries/python/versions/3.13.12/bin/python3"
PIDD="$AI/run"
LOG="$TRAIN"

mkdir -p "$PIDD"

pidfile() { echo "$PIDD/$1.pid"; }
alive()   { local pf; pf="$(pidfile "$1")"; [[ -f "$pf" ]] && kill -0 "$(cat "$pf")" 2>/dev/null; }

stop_one() {
  local name="$1" pf; pf="$(pidfile "$name")"
  if [[ -f "$pf" ]]; then
    local pid; pid="$(cat "$pf")"
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      # 给点时间优雅退出
      for _ in 1 2 3 4 5; do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pf"
  fi
  # 保险起见也用 pkill 清掉残留
  case "$name" in
    mlx) pkill -f "mlx_lm server" 2>/dev/null || true ;;
    ai)  pkill -f "ai/app.py" 2>/dev/null || true ;;
  esac
}

start_one_bg() {
  # 用 nohup + disown 让子进程脱离当前 shell（macOS/Linux 通用，避免跨 shell 被清理）；
  # 若端口已被外部进程占用，则认领其 PID（幂等）。
  local name="$1"; shift
  local pf; pf="$(pidfile "$name")"
  local lf="$LOG/${name}.log"
  local port; case "$name" in mlx) port=8080 ;; ai) port=8777 ;; *) port=0 ;; esac

  # 1) PID 文件有效 → 已运行
  if alive "$name"; then echo "[$name] 已在运行 (PID $(cat "$pf"))"; return 0; fi

  # 2) 端口已被外部进程占用（PID 文件陈旧或不存在）→ 认领监听进程
  if [[ $port -gt 0 ]] && lsof -iTCP:"$port" -sTCP:LISTEN -nP >/dev/null 2>&1; then
    local ext_pid
    ext_pid=$(lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null | head -1)
    if [[ -n "$ext_pid" ]]; then
      echo "$ext_pid" > "$pf"
      echo "[$name] 端口 $port 已被外部进程占用 (PID $ext_pid)，已认领"
      return 0
    fi
  fi

  # 3) 清理陈旧 PID 文件后启动
  rm -f "$pf"
  nohup "$@" > "$lf" 2>&1 < /dev/null &
  local new_pid=$!
  echo "$new_pid" > "$pf"
  disown 2>/dev/null || true
  sleep 1
  if ! kill -0 "$new_pid" 2>/dev/null; then
    echo "[$name] ⚠ 启动后立即退出，查看 $lf"
    tail -10 "$lf" 2>/dev/null
    return 1
  fi
  return 0
}

wait_ready() {
  local url="$1" max="${2:-40}" delay="${3:-2}"
  for ((i=1; i<=max; i++)); do
    if curl -s --noproxy '*' --max-time 3 "$url" >/dev/null 2>&1; then return 0; fi
    sleep "$delay"
  done
  return 1
}

ACTION="${1:-start}"
case "$ACTION" in
  status)
    echo "=== AI 后端栈状态 ==="
    for n in mlx ai; do
      if alive "$n"; then echo "  [$n] 运行中 (PID $(cat "$(pidfile $n)"))"; else echo "  [$n] 未运行"; fi
    done
    lsof -iTCP:8080 -sTCP:LISTEN >/dev/null 2>&1 && echo "  [port 8080] 监听中" || echo "  [port 8080] 未监听"
    lsof -iTCP:8777 -sTCP:LISTEN >/dev/null 2>&1 && echo "  [port 8777] 监听中" || echo "  [port 8777] 未监听"
    exit 0 ;;
esac

if [[ "$ACTION" == "stop" ]]; then
  echo "[stop] AI 服务 ..."
  stop_one ai
  echo "[stop] 模型服务 ..."
  stop_one mlx
  echo "[stop] 完成"
  exit 0
fi

# start / restart 路径
if [[ "$ACTION" == "restart" ]]; then
  "$0" stop >/dev/null 2>&1 || true
  sleep 1
elif [[ "$ACTION" != "start" && "$ACTION" != "" ]]; then
  echo "用法: $0 [start|stop|restart|status]"; exit 2
fi

echo "[start] 模型服务 (mlx_lm.server :8080) ..."
start_one_bg mlx "$TRAIN/venv/bin/python" -m mlx_lm server --model "$TRAIN/fused" --host 127.0.0.1 --port 8080

echo "[start] AI 服务 (app.py :8777) ..."
# 清除沙箱 HTTP_PROXY 防止 urllib 走代理
start_one_bg ai env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy -u ALL_PROXY -u all_proxy \
  "$PY" "$AI/app.py"

echo "[start] 等待就绪 ..."
mlx_ok=0; ai_ok=0
if wait_ready "http://127.0.0.1:8080/v1/models" 40 2; then
  echo "  [mlx] 模型服务就绪 (PID $(cat "$(pidfile mlx)"))"
  mlx_ok=1
else
  echo "  [mlx] ⚠ 未就绪（查看 $LOG/mlx.log）"; tail -15 "$LOG/mlx.log" 2>/dev/null
fi
if wait_ready "http://127.0.0.1:8777/health" 20 1; then
  echo "  [ai]  AI 服务就绪 (PID $(cat "$(pidfile ai)"))"
  ai_ok=1
else
  echo "  [ai]  ⚠ 未就绪（查看 $LOG/ai.log）"; tail -15 "$LOG/ai.log" 2>/dev/null
fi

echo
if [[ $mlx_ok -eq 1 && $ai_ok -eq 1 ]]; then
  echo "✅ AI 后端栈已启动"
  echo "   模型服务 → http://127.0.0.1:8080  (mlx_lm.server, 自训融合模型)"
  echo "   AI 服务   → http://127.0.0.1:8777  (Python 标准库 HTTP, 4 特征路由)"
  echo "   日志      → $LOG/mlx.log 与 $LOG/ai.log"
  echo
  echo "下一步：./build/todo serve   # 启动 cpp-todo 主服务（:8931，AI 入口在侧栏 AI 助手）"
  echo "停止：  $0 stop"
  exit 0
else
  echo "⚠ AI 后端栈未完全启动（mlx=$mlx_ok ai=$ai_ok），请查看上方日志"
  exit 1
fi