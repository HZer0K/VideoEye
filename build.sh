#!/bin/bash
# VideoEye 构建脚本 (Linux / macOS / WSL)
# 用法: ./build.sh [debug|release] [cmake-args...]

set -e

BUILD_TYPE=${1:-release}
shift || true
BUILD_DIR="build-${BUILD_TYPE}"
PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"

echo "====================================="
echo "VideoEye 构建脚本 (Linux)"
echo "====================================="
echo "构建类型: ${BUILD_TYPE}"
echo "构建目录: ${BUILD_DIR}"
echo "====================================="

# 检查构建依赖
echo "检查构建依赖..."
MISSING_DEPS=""

if ! command -v cmake &>/dev/null; then
    MISSING_DEPS="$MISSING_DEPS cmake"
fi
if ! command -v gcc &>/dev/null && ! command -v g++ &>/dev/null; then
    MISSING_DEPS="$MISSING_DEPS g++"
fi
if ! command -v make &>/dev/null; then
    MISSING_DEPS="$MISSING_DEPS make"
fi
if ! command -v pkg-config &>/dev/null; then
    MISSING_DEPS="$MISSING_DEPS pkg-config"
fi

# 检查库依赖 (FFmpeg 通过 apt 安装, 不再需要源码编译)
for pkg in Qt6Widgets opencv4 sdl2 zlib vulkan libavcodec libavformat libavutil libswscale libswresample; do
    if ! pkg-config --exists "$pkg" 2>/dev/null; then
        MISSING_DEPS="$MISSING_DEPS $pkg(dev)"
    fi
done

if [ -n "$MISSING_DEPS" ]; then
    echo "❌ 缺少依赖: $MISSING_DEPS"
    echo ""
    echo "Debian/Ubuntu 安装命令:"
    echo "  sudo apt install -y cmake g++ make pkg-config \\"
    echo "    qt6-base-dev qt6-charts-dev \\"
    echo "    libopencv-dev libsdl2-dev zlib1g-dev \\"
    echo "    libvulkan-dev libglslc-dev glslc \\"
    echo "    libavcodec-dev libavformat-dev libavutil-dev \\"
    echo "    libswscale-dev libswresample-dev"
    exit 1
fi

echo "✅ 依赖检查通过"

# 创建构建目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# 配置
echo "配置项目..."
cmake .. \
    -DCMAKE_BUILD_TYPE="$(echo ${BUILD_TYPE} | sed 's/^./\U&/')" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "$@"

# 编译
echo "开始编译..."
CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
echo "使用 ${CPU_COUNT} 个并行进程"
cmake --build . -j ${CPU_COUNT}

echo "====================================="
echo "构建完成!"
echo "可执行文件位置: ${BUILD_DIR}/bin/VideoEye"
echo "====================================="
echo "运行: ./${BUILD_DIR}/bin/VideoEye"
