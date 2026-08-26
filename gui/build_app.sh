#!/bin/bash
# build_app.sh —— 构建/重建 cpp-todo.app 桌面 Bundle
#
# 用法:
#     ./gui/build_app.sh                  # 默认在项目根生成 cpp-todo.app
#     ./gui/build_app.sh --no-sign         # 跳过 ad-hoc 签名（极少用，仅 CI 时）
#
# 行为:
#   1. 在项目根生成 cpp-todo.app/ 目录结构
#   2. 复制 Info.plist 与启动器
#   3. 符号链接图标资源（避免重复 411 KB 二进制）
#   4. 对 .app 做 ad-hoc 签名——避免双击启动时的 Gatekeeper "无法验证开发者" 弹窗
#   5. 清理 Dock / Finder 缓存，刷新图标显示

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMPLATE="$ROOT/gui/macos-bundle"
APP="$ROOT/cpp-todo.app"
ICON_SRC="$ROOT/gui/icons/icon.icns"
INFO_PLIST_SRC="$TEMPLATE/Info.plist"
LAUNCHER_SRC="$TEMPLATE/launcher.sh"
DO_SIGN=1

for arg in "$@"; do
    case "$arg" in
        --no-sign) DO_SIGN=0 ;;
        -h|--help)
            echo "用法: $0 [--no-sign]"
            echo "  --no-sign  跳过 ad-hoc 签名"
            exit 0
            ;;
        *)
            echo "[build_app] 未知参数: $arg" >&2
            exit 2
            ;;
    esac
done

# 前置检查
[ -f "$INFO_PLIST_SRC" ] || { echo "[build_app] 缺少 $INFO_PLIST_SRC" >&2; exit 1; }
[ -f "$LAUNCHER_SRC"   ] || { echo "[build_app] 缺少 $LAUNCHER_SRC"   >&2; exit 1; }
[ -f "$ICON_SRC"       ] || { echo "[build_app] 缺少 $ICON_SRC（先执行 python gui/make_icon.py）" >&2; exit 1; }

echo "[build_app] 重建 $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# 元数据
cp "$INFO_PLIST_SRC" "$APP/Contents/Info.plist"
cp "$LAUNCHER_SRC"   "$APP/Contents/MacOS/cpp-todo"
chmod +x "$APP/Contents/MacOS/cpp-todo"

# 图标资源——符号链接，避免重复二进制
ln -s "$ICON_SRC" "$APP/Contents/Resources/icon.icns"

# PkgInfo（macOS legacy 标识；不影响功能但保持规范）
printf 'APPL????' > "$APP/Contents/PkgInfo"

# Ad-hoc 签名（关键：避免 Gatekeeper 弹窗）
if [ "$DO_SIGN" = "1" ]; then
    codesign --force --deep --sign - "$APP" 2>/dev/null || {
        echo "[build_app] 警告: ad-hoc 签名失败（无 codesign / 受限环境），仍可使用" >&2
    }
    # 若 .app 之前被隔离过，去掉 quarantine 属性
    xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
fi

# 刷新 Finder / Dock 缓存
touch "$APP"
killall Dock 2>/dev/null || true

echo "[build_app] 完成 ✓"
echo ""
echo "  启动:        open $APP"
echo "  或:           ./cpp-todo"
echo "  拖到 Dock 即可像原生应用一样使用"
