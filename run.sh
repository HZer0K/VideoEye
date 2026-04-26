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
    echo "正在编译项目..."
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    
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

if [ -n "$VIDEOEYE_FFMPEG_LD_PRELOAD" ]; then
    export LD_PRELOAD="$VIDEOEYE_FFMPEG_LD_PRELOAD"
else
    unset LD_PRELOAD
fi

# 运行程序
cd "$BUILD_DIR"
./bin/VideoEye &

echo "🎬 VideoEye 已启动 (PID: $!)"
echo "====================================="
