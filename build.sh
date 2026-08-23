#!/bin/bash
# ========================================
#  VideoEye - 一键构建 (Linux / macOS / WSL)
#  用法: ./build.sh [release|debug|clean]
#  默认: release
# ========================================
set -euo pipefail

PRESET="${1:-release}"
shift || true

# clean 子命令: 清理构建目录后退出
if [[ "$PRESET" == "clean" ]]; then
    PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
    cd "$PROJECT_ROOT"
    for d in build/release build/debug; do
        if [[ -d "$d" ]]; then
            rm -rf "$d"
            echo "已删除: $d"
        fi
    done
    echo "构建目录已清理"
    exit 0
fi

# 本脚本仅用于 Linux / macOS / WSL, 对应 CMakePresets.json 中的 linux-* preset
case "$PRESET" in
    release) CMAKE_PRESET="linux-release" ;;
    debug)   CMAKE_PRESET="linux-debug" ;;
    *) echo "用法: $0 [release|debug|clean]"; exit 1 ;;
esac

# ============ 依赖缺失时的友好提示 ============
handle_deps_hint() {
    echo ""
    echo "--- 依赖安装提示 ---"
    if [[ "$(uname)" == "Darwin" ]]; then
        echo "macOS (Homebrew):"
        echo "  brew install cmake ninja qt@6 sdl2 ffmpeg zlib glslang vulkan-headers"
    else
        echo "Debian/Ubuntu:"
        echo "  sudo apt install -y cmake ninja-build pkg-config \\"
        echo "    qt6-base-dev qt6-charts-dev libsdl2-dev zlib1g-dev \\"
        echo "    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \\"
        echo "    libvulkan-dev glslc"
    fi
    exit 1
}
trap handle_deps_hint ERR

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_ROOT"

echo "===================================="
echo " VideoEye Build - $PRESET"
echo "===================================="

# --- 1. Configure (via preset) ---
echo "[1/2] CMake Configure..."
cmake --preset "$CMAKE_PRESET" "$@"

# --- 2. Build ---
echo ""
echo "[2/2] Build..."
cmake --build --preset "$CMAKE_PRESET"

echo ""
echo "===================================="
echo " 构建成功: build/$PRESET/bin/VideoEye"
echo " 运行:   ./build/$PRESET/bin/VideoEye"
echo "===================================="
