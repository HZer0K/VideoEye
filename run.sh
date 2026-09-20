#!/bin/bash

# VideoEye 快速启动脚本

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
EXECUTABLE="${BUILD_DIR}/bin/VideoEye"

echo "====================================="
echo "VideoEye 2.0 启动器"
echo "====================================="

# 检查可执行文件是否存在
if [ ! -f "$EXECUTABLE" ]; then
    echo "❌ 未找到可执行文件"
    echo "正在编译项目 (cmake --preset linux-release)..."

    cd "$SCRIPT_DIR"
    cmake --preset linux-release
    if [ $? -ne 0 ]; then
        echo "❌ CMake 配置失败! FFmpeg 需放在 third_party/prebuilt/linux-x64/ffmpeg/"
        exit 1
    fi
    cmake --build --preset linux-release
    if [ $? -ne 0 ]; then
        echo "❌ 编译失败!"
        exit 1
    fi
fi

echo "✅ 找到可执行文件: $EXECUTABLE"
echo "启动 VideoEye..."
echo "====================================="

# 设置 Qt 平台
export QT_QPA_PLATFORM=xcb

# 运行程序
cd "$BUILD_DIR"
./bin/VideoEye &

echo "🎬 VideoEye 已启动 (PID: $!)"
echo "====================================="
