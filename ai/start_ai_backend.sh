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
# 后端运行时选择（环境变量 AI_RUNTIME）：
#   mlx      (默认，仅 macOS Apple Silicon)
#            使用 ai/train/fused 自训融合权重，需 ai/train/venv
#   ollama   (跨平台：Linux/Windows/macOS)
#            使用本地 Ollama，需先 `ollama pull qwen2.5:3b`
#            模型由 AI_MODEL 指定（默认 qwen2.5:3b，省内存可改 1.5b）
#   llamacpp (跨平台：Linux/Windows/macOS)
#            使用 llama.cpp 的 llama-server，需 AI_GGUF 指向 GGUF 模型路径
#
# 相关环境变量：
#   AI_RUNTIME      mlx|ollama|llamacpp （默认 mlx）
#   AI_MODEL        Ollama 模型名（默认 qwen2.5:3b）
#   AI_GGUF         GGUF 模型绝对路径（llamacpp 必填）
#   AI_MODEL_PORT   模型服务端口（默认 8080）
#   AI_PYTHON       AI 服务使用的 python 解释器（默认沿用脚本内 PY 变量）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TRAIN="$ROOT/ai/train"
AI="$ROOT/ai"
PIDD="$AI/run"
LOG="$TRAIN"

# 默认 python：优先用项目训练 venv（macOS），否则用 PATH 中的 python3
PY="${AI_PYTHON:-}"
if [[ -z "$PY" ]]; then
  if [[ -x "$TRAIN/venv/bin/python" ]]; then
    PY="$TRAIN/venv/bin/python"
  else
    PY="$(command -v python3 || command -v python)"
  fi
fi

# 运行时配置
RUNTIME="${AI_RUNTIME:-mlx}"
MODEL_PORT="${AI_MODEL_PORT:-8080}"
AI_PORT="8777"
OLLAMA_MODEL="${AI_MODEL:-qwen2.5:3b}"
LLAMACPP_BIN="${AI_LLAMACPP_BIN:-llama-server}"
LLAMACPP_GGUF="${AI_GGUF:-}"

mkdir -p "$PIDD"

pidfile() { echo "$PIDD/$1.pid"; }
alive()   { local pf; pf="$(pidfile "$1")"; [[ -f "$pf" ]] && kill -0 "$(cat "$pf")" 2>/dev/null; }

stop_one() {
  local name="$1" pf; pf="$(pidfile "$name")"
  if [[ -f "$pf" ]]; then
    local pid; pid="$(cat "$pf")"
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      for _ in 1 2 3 4 5; do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pf"
  fi
  # 按运行时清理残留进程
  case "$name:$RUNTIME" in
    mlx:mlx)      pkill -f "mlx_lm server" 2>/dev/null || true ;;
    mlx:ollama)   pkill -f "ollama serve" 2>/dev/null || true ;;
    mlx:llamacpp) pkill -f "llama-server" 2>/dev/null || true ;;
    ai:*)         pkill -f "ai/app.py" 2>/dev/null || true ;;
  esac
}

start_one_bg() {
  # 用 nohup + disown 让子进程脱离当前 shell；
  # 若端口已被外部进程占用，则认领其 PID（幂等）。
  local name="$1"; shift
  local pf; pf="$(pidfile "$name")"
  local lf="$LOG/${name}.log"
  local port; case "$name" in mlx) port="$MODEL_PORT" ;; ai) port="$AI_PORT" ;; *) port=0 ;; esac

  if alive "$name"; then echo "[$name] 已在运行 (PID $(cat "$pf"))"; return 0; fi

  if [[ $port -gt 0 ]] && command -v lsof >/dev/null 2>&1 && lsof -iTCP:"$port" -sTCP:LISTEN -nP >/dev/null 2>&1; then
    local ext_pid
    ext_pid=$(lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null | head -1)
    if [[ -n "$ext_pid" ]]; then
      echo "$ext_pid" > "$pf"
      echo "[$name] 端口 $port 已被外部进程占用 (PID $ext_pid)，已认领"
      return 0
    fi
  fi

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

# 构造模型服务启动命令（按 RUNTIME 分发）
build_model_cmd() {
  case "$RUNTIME" in
    mlx)
      if [[ ! -x "$TRAIN/venv/bin/python" ]]; then
        echo "[mlx] 错误：$TRAIN/venv 不存在（仅 macOS 可用）。改用 AI_RUNTIME=ollama 或 llamacpp" >&2
        return 1
      fi
      if [[ ! -d "$TRAIN/fused" ]]; then
        echo "[mlx] 错误：$TRAIN/fused 融合权重不存在。" >&2
        return 1
      fi
      echo "$TRAIN/venv/bin/python" -m mlx_lm server --model "$TRAIN/fused" --host 127.0.0.1 --port "$MODEL_PORT"
      ;;
    ollama)
      if ! command -v ollama >/dev/null 2>&1; then
        echo "[ollama] 错误：未找到 ollama 可执行文件。安装见 https://ollama.com" >&2
        return 1
      fi
      echo ollama serve
      ;;
    llamacpp)
      # 注意：bash 3.2（macOS 默认）在函数内动态赋值变量+[[ ]] 组合下有 bug，
      # 变量绑定会在 [[ ]] 后丢失。这里直接引用全局变量，避免函数内赋值。
      if ! command -v "${LLAMACPP_BIN:-llama-server}" >/dev/null 2>&1 \
        && [[ ! -x "${LLAMACPP_BIN:-llama-server}" ]]; then
        echo "[llamacpp] 错误：未找到 ${LLAMACPP_BIN:-llama-server}。设 AI_LLAMACPP_BIN 或加入 PATH" >&2
        return 1
      fi
      if [[ -z "${LLAMACPP_GGUF:-}" || ! -f "${LLAMACPP_GGUF:-}" ]]; then
        echo "[llamacpp] 错误：AI_GGUF 未指定或文件不存在（需指向 .gguf 权重）" >&2
        return 1
      fi
      echo "${LLAMACPP_BIN:-llama-server}" --model "${LLAMACPP_GGUF:-}" --host 127.0.0.1 --port "$MODEL_PORT"
      ;;
    *)
      echo "[runtime] 未知 AI_RUNTIME=${RUNTIME:-}（支持 mlx|ollama|llamacpp）" >&2
      return 1
      ;;
  esac
}

# Ollama 模型预热（仅 ollama 后端）
ensure_ollama_model() {
  if ! command -v ollama >/dev/null 2>&1; then return 0; fi
  if ! ollama list 2>/dev/null | awk '{print $1}' | grep -qx "$OLLAMA_MODEL"; then
    echo "[ollama] 模型 $OLLAMA_MODEL 未拉取，执行 ollama pull（首次较慢）..."
    ollama pull "$OLLAMA_MODEL" || { echo "[ollama] pull 失败，请手动执行 ollama pull $OLLAMA_MODEL" >&2; return 1; }
  fi
}

ACTION="${1:-start}"
case "$ACTION" in
  status)
    echo "=== AI 后端栈状态 (runtime=$RUNTIME) ==="
    for n in mlx ai; do
      if alive "$n"; then echo "  [$n] 运行中 (PID $(cat "$(pidfile $n)"))"; else echo "  [$n] 未运行"; fi
    done
    command -v lsof >/dev/null 2>&1 && {
      lsof -iTCP:"$MODEL_PORT" -sTCP:LISTEN >/dev/null 2>&1 && echo "  [port $MODEL_PORT] 监听中" || echo "  [port $MODEL_PORT] 未监听"
      lsof -iTCP:"$AI_PORT" -sTCP:LISTEN >/dev/null 2>&1 && echo "  [port $AI_PORT] 监听中" || echo "  [port $AI_PORT] 未监听"
    } || echo "  (lsof 不可用，跳过端口检查)"
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

# 构造并启动模型服务
echo "[start] 模型服务 (runtime=$RUNTIME, port=$MODEL_PORT) ..."
MODEL_CMD=$(build_model_cmd) || exit 1
if [[ "$RUNTIME" == "ollama" ]]; then
  ensure_ollama_model || exit 1
fi
# shellcheck disable=SC2086
start_one_bg mlx $MODEL_CMD

echo "[start] AI 服务 (app.py :$AI_PORT) ..."
# 清除沙箱 HTTP_PROXY 防止 urllib 走代理；注入后端类型便于 config.py 默认值
start_one_bg ai env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy -u ALL_PROXY -u all_proxy \
  AI_BACKEND="$RUNTIME" \
  "$PY" "$AI/app.py"

echo "[start] 等待就绪 ..."
mlx_ok=0; ai_ok=0
if wait_ready "http://127.0.0.1:$MODEL_PORT/v1/models" 40 2; then
  echo "  [mlx] 模型服务就绪 (PID $(cat "$(pidfile mlx)"))"
  mlx_ok=1
else
  echo "  [mlx] ⚠ 未就绪（查看 $LOG/mlx.log）"; tail -15 "$LOG/mlx.log" 2>/dev/null
fi
if wait_ready "http://127.0.0.1:$AI_PORT/health" 20 1; then
  echo "  [ai]  AI 服务就绪 (PID $(cat "$(pidfile ai)"))"
  ai_ok=1
else
  echo "  [ai]  ⚠ 未就绪（查看 $LOG/ai.log）"; tail -15 "$LOG/ai.log" 2>/dev/null
fi

echo
if [[ $mlx_ok -eq 1 && $ai_ok -eq 1 ]]; then
  echo "✅ AI 后端栈已启动 (runtime=$RUNTIME)"
  echo "   模型服务 → http://127.0.0.1:$MODEL_PORT  ($RUNTIME)"
  if [[ "$RUNTIME" == "mlx" ]]; then
    echo "                              自训融合模型 $TRAIN/fused"
  elif [[ "$RUNTIME" == "ollama" ]]; then
    echo "                              Ollama 模型 $OLLAMA_MODEL"
  else
    echo "                              llama.cpp + $LLAMACPP_GGUF"
  fi
  echo "   AI 服务   → http://127.0.0.1:$AI_PORT  (Python 标准库 HTTP, 4 特征路由)"
  echo "   日志      → $LOG/mlx.log 与 $LOG/ai.log"
  echo
  echo "下一步：./build/todo serve   # 启动 cpp-todo 主服务（:8931，AI 入口在侧栏 AI 助手）"
  echo "停止：  $0 stop"
  exit 0
else
  echo "⚠ AI 后端栈未完全启动（mlx=$mlx_ok ai=$ai_ok），请查看上方日志"
  exit 1
fi
