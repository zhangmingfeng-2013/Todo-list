#!/usr/bin/env bash
# cpp-todo 桌面 GUI 一键启动
# 首次运行会自动创建 .venv 并安装 pywebview（仅需一次，之后直接启动）
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x .venv/bin/python ]; then
  echo "==> 首次运行：创建 Python 虚拟环境并安装 pywebview（约 1 分钟）…"
  python3 -m venv .venv
  .venv/bin/pip install --quiet --upgrade pip
  .venv/bin/pip install --quiet pywebview
fi

exec .venv/bin/python gui/todo-gui.py "$@"
