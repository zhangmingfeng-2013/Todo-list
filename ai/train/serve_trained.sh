#!/usr/bin/env bash
# 启动自训融合模型（离线闭环后端）。
# mlx_lm.server 提供 OpenAI 兼容 /v1/chat/completions，默认 127.0.0.1:8080。
# AI 服务 (ai/app.py) 的 base_url 默认即指向 8080，故启动本服务即完成「替换后端」。
set -e
cd "$(dirname "$0")"
exec ./venv/bin/python -m mlx_lm server --model ./fused --host 127.0.0.1 --port 8080
