#!/bin/bash

# VideoEye 构建脚本
# 用法: ./build.sh [debug|release]

set -e

BUILD_TYPE=${1:-release}
BUILD_DIR="build-${BUILD_TYPE}"

echo "====================================="
echo "VideoEye 构建脚本"
echo "====================================="
echo "构建类型: ${BUILD_TYPE}"
echo "构建目录: ${BUILD_DIR}"
echo "====================================="

# 创建构建目录
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}

# 配置
echo "配置项目..."
cmake .. \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE^} \
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
