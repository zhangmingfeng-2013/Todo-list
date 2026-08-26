#!/bin/bash
# cpp-todo.app 启动器（macOS LaunchServices 入口）
#
# 启动顺序由 macOS LaunchServices 通过 `open cpp-todo.app` 触发；
# 我们从可执行文件路径反推项目根目录，再用项目内的 .venv Python 启动 GUI。
# 失败时通过 osascript 弹窗提示用户，而不是把栈追踪写到系统日志。

set -e

# 路径反推：
#   $0                         <root>/cpp-todo.app/Contents/MacOS/cpp-todo
#   dirname($0)                <root>/cpp-todo.app/Contents/MacOS
#   dirname(dirname($0))       <root>/cpp-todo.app/Contents      (bundle 内部目录)
#   dirname(...)/..            <root>/cpp-todo.app               (bundle 根)
#   /../..  共三层上溯          <root>                            (项目根)
APP_BIN="$0"
BIN_DIR="$(cd "$(dirname "$APP_BIN")" && pwd)"
APP_DIR="$(cd "$BIN_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$APP_DIR/../.." && pwd)"

VENV_PY="$PROJECT_ROOT/.venv/bin/python"
GUI="$PROJECT_ROOT/gui/todo-gui.py"

notify_error() {
    # 通过 AppleScript 弹窗，避免 stdout 丢进系统日志后用户看不到
    osascript -e "display alert \"$1\" message \"$2\" as critical" >/dev/null 2>&1 || true
}

if [ ! -x "$VENV_PY" ]; then
    VENV_PY="$(command -v python3 || true)"
    if [ -z "$VENV_PY" ]; then
        notify_error "缺少 Python 3" "请在项目根目录执行 ./build.sh 完成构建与依赖安装。"
        exit 1
    fi
fi

if [ ! -f "$GUI" ]; then
    notify_error "未找到 GUI 启动器" "预期路径: $GUI"
    exit 1
fi

cd "$PROJECT_ROOT"
exec "$VENV_PY" "$GUI" "$@"
