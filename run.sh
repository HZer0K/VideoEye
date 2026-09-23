#!/bin/bash
# ========================================
#  VideoEye - 快速启动 (Linux / macOS / WSL)
#  用法: ./run.sh [release|debug]
#  默认: release
#
#  构建配置统一放在 CMakePresets.json，这里只负责"没构建过就先构建"。
# ========================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${1:-release}"

case "$BUILD_TYPE" in
    release|debug) ;;
    *) echo "用法: $0 [release|debug]"; exit 1 ;;
esac

BUILD_DIR="${SCRIPT_DIR}/build/${BUILD_TYPE}"
EXECUTABLE="${BUILD_DIR}/bin/VideoEye"

echo "====================================="
echo "VideoEye 2.0 启动器"
echo "====================================="

if [ ! -f "$EXECUTABLE" ]; then
    echo "未找到可执行文件: $EXECUTABLE"
    echo "先构建 ($BUILD_TYPE)..."
    "$SCRIPT_DIR/build.sh" "$BUILD_TYPE"
fi

echo "可执行文件: $EXECUTABLE"
echo "启动 VideoEye..."
echo "====================================="

# Linux 桌面环境下强制 xcb 插件（Wayland 会话里 Qt 有时会挑错平台插件）
if [ "$(uname)" = "Linux" ]; then
    export QT_QPA_PLATFORM=xcb
fi

"$EXECUTABLE" &
PID=$!
echo "VideoEye 已启动 (PID: $PID)"
echo "====================================="
