#!/usr/bin/env bash
# cpp-todo 构建脚本：cmake 配置 + 编译，输出单二进制 + 内嵌 SQLite + AI 网关(libcurl)。
#
# 用法：
#   ./build.sh                  # 默认 Release 模式
#   ./build.sh debug            # Debug 模式
#   BUILD_TYPE=Debug ./build.sh # 通过环境变量指定
#   JOBS=8 ./build.sh           # 指定并行作业数（默认自动探测 CPU 核数）
#
# 产物：./build/todo（可执行二进制），./build/web/（前端静态资源，由 cmake 拷贝）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT/build"

# 模式与并行度
case "${1:-${BUILD_TYPE:-Release}}" in
  debug|Debug|DEBUG)   BUILD_TYPE=Debug ;;
  release|Release|RELEASE|"") BUILD_TYPE=Release ;;
  *) echo "[build] 未知模式: $1（支持 debug/release）"; exit 2 ;;
esac
JOBS="${JOBS:-}"
if [[ -z "$JOBS" ]]; then
  if command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  elif command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS=4
  fi
fi

echo "[build] ROOT      = $ROOT"
echo "[build] BUILD_DIR = $BUILD_DIR"
echo "[build] TYPE      = $BUILD_TYPE"
echo "[build] JOBS      = $JOBS"

# 配置（增量：已存在 build/ 时 cmake 自动刷新以纳入新增源/依赖，如 ai_gateway + CURL）
echo "[build] cmake configure ..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" > "$BUILD_DIR/.cmake-configure.log" 2>&1 \
  || { echo "[build] cmake configure 失败，详见 $BUILD_DIR/.cmake-configure.log"; tail -30 "$BUILD_DIR/.cmake-configure.log"; exit 1; }

# 编译
echo "[build] compile ..."
cmake --build "$BUILD_DIR" -j "$JOBS" > "$BUILD_DIR/.cmake-build.log" 2>&1 \
  || { echo "[build] compile 失败，详见 $BUILD_DIR/.cmake-build.log"; tail -40 "$BUILD_DIR/.cmake-build.log"; exit 1; }

BIN="$BUILD_DIR/todo"
if [[ -x "$BIN" ]]; then
  SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN" 2>/dev/null || echo "?")
  echo "[build] OK -> $BIN (${SIZE} bytes)"
  echo "[build] 运行： $BIN serve   # 启动 HTTP 服务（默认 127.0.0.1:8931）"
else
  echo "[build] 警告：未找到产物 $BIN（编译可能未产生 todo 目标）"
  exit 1
fi