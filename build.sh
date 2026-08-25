#!/bin/bash
# cpp-todo 一键构建脚本
set -e
cd "$(dirname "$0")"

echo "==> 配置 CMake (Release)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

echo "==> 编译"
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

echo ""
echo "==> 构建完成"
echo "    可执行文件: build/todo"
echo "    用法示例:"
echo "      ./build/todo serve --open                                            # 启动本地服务并打开浏览器"
echo "      ./build/todo add \"写周报\" --due 2026-08-28 --prio high --tag 工作"
echo "      ./build/todo list --today"
echo "      ./build/todo import 滴答导出.json --format ticktick"

echo "./build/todo serve                                                         # 仅启动服务，访问 http://127.0.0.1:8931/"
echo "./build/todo serve --port 9000 --db ~/mytodo.db                            # 仅启动服务，访问 http://127.0.0.1:9000/"
