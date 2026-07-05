#!/bin/bash
# VideoEye 开发环境初始化脚本
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "=== VideoEye 环境初始化 ==="

# 1. Git 子模块
echo "[1/4] Git 子模块..."
git submodule update --init --recursive && echo "  完成" || echo "  失败"

# 2. FFmpeg 源码
echo "[2/4] FFmpeg 源码..."
if [ ! -f "$DIR/third_party/FFmpeg/configure" ]; then
    echo "  下载 n8.1 (~40MB)..."
    git clone --depth 1 --branch n8.1 \
        https://git.ffmpeg.org/ffmpeg.git "$DIR/third_party/FFmpeg" 2>/dev/null || \
    git clone --depth 1 --branch n8.1 \
        https://github.com/FFmpeg/FFmpeg.git "$DIR/third_party/FFmpeg" || \
    echo "  失败! 请手动克隆"
else
    echo "  已存在"
fi

# 3. 系统依赖
echo "[3/4] 系统依赖..."
for cmd in cmake gcc g++ make; do
    command -v "$cmd" >/dev/null 2>&1 && echo "  [OK] $cmd" || echo "  [缺] $cmd"
done
pkg-config --exists sdl2 2>/dev/null && echo "  [OK] sdl2" || echo "  [缺] sdl2"
command -v glslc >/dev/null 2>&1 && echo "  [OK] glslc" || echo "  [可选] glslc"
pkg-config --exists vulkan 2>/dev/null && echo "  [OK] vulkan" || echo "  [可选] vulkan"

# 4. 编译
echo "[4/4] 编译..."
BUILD_DIR="$DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
cmake --build . -j"$(nproc 2>/dev/null || echo 4)"
echo ""
echo "=== 完成 ==="
echo "运行: $BUILD_DIR/bin/VideoEye"
echo "测试: cd $BUILD_DIR && ctest"
