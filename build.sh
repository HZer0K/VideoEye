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

# 检查 FFmpeg 源码是否存在
if [ ! -f "third_party/FFmpeg/configure" ]; then
    echo "FFmpeg 源码未找到，正在克隆 FFmpeg n8.1..."
    git clone --depth 1 --branch n8.1 https://git.ffmpeg.org/ffmpeg.git third_party/FFmpeg
    if [ $? -ne 0 ]; then
        echo "❌ FFmpeg 克隆失败!"
        exit 1
    fi
    echo "✅ FFmpeg n8.1 克隆完成"
fi

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

# 检查库依赖
for pkg in Qt6Widgets opencv4 sdl2 zlib vulkan; do
    if ! pkg-config --exists "$pkg" 2>/dev/null; then
        MISSING_DEPS="$MISSING_DEPS $pkg(dev)"
    fi
done

if [ -n "$MISSING_DEPS" ]; then
    echo "❌ 缺少依赖: $MISSING_DEPS"
    echo ""
    echo "Debian/Ubuntu 安装命令:"
    echo "  sudo apt install -y cmake g++ make pkg-config \\"
    echo "    qt6-base-dev qt6-multimedia-dev qt6-charts-dev \\"
    echo "    libopencv-dev libsdl2-dev zlib1g-dev \\"
    echo "    libvulkan-dev libglslc-dev glslc"
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
