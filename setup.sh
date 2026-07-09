#!/bin/bash
# VideoEye 开发环境初始化脚本 (Linux / macOS / WSL)
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# 检测操作系统
OS="unknown"
case "$(uname -s)" in
    Linux*)  OS="linux";;
    Darwin*) OS="macos";;
esac

SKIP_DEPS=false; BUILD_ONLY=false; BUILD_TYPE="Release"
for arg in "$@"; do
    case "$arg" in
        --skip-deps) SKIP_DEPS=true ;;
        --build-only) BUILD_ONLY=true ;;
        --debug) BUILD_TYPE=Debug ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo "  --skip-deps    跳过依赖检查"
            echo "  --build-only   仅编译 (需已完成初始化)"
            echo "  --debug        Debug 构建"
            exit 0 ;;
    esac
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  VideoEye 环境初始化 (${OS})${NC}"
echo -e "${GREEN}========================================${NC}"

if [ "$BUILD_ONLY" = false ]; then
    # 1. Git 子模块
    echo -e "\n${YELLOW}[1/3] Git 子模块...${NC}"
    git submodule update --init --recursive && echo -e "${GREEN}  完成${NC}" || echo -e "${RED}  失败${NC}"

    # 2. 系统依赖
    if [ "$SKIP_DEPS" = false ]; then
        echo -e "\n${YELLOW}[2/3] 系统依赖...${NC}"

        check_cmd() { command -v "$1" >/dev/null 2>&1 && echo -e "  ${GREEN}[OK]${NC} $1" || echo -e "  ${RED}[缺]${NC} $1"; }
        check_pkg() { pkg-config --exists "$1" 2>/dev/null && echo -e "  ${GREEN}[OK]${NC} $1" || echo -e "  ${RED}[缺]${NC} $1"; }

        check_cmd cmake; check_cmd gcc; check_cmd g++; check_cmd make; check_cmd pkg-config
        check_pkg Qt6Widgets; check_pkg opencv4; check_pkg sdl2; check_pkg zlib
        check_pkg libavcodec; check_pkg libavformat; check_pkg libavutil
        check_pkg libswscale; check_pkg libswresample

        command -v glslc >/dev/null 2>&1 && echo -e "  ${GREEN}[OK]${NC} glslc" || echo -e "  ${YELLOW}[可选]${NC} glslc"
        check_pkg vulkan

        # 打印安装建议
        echo ""
        if [ "$OS" = "linux" ]; then
            echo "  Debian/Ubuntu 安装命令:"
            echo "    sudo apt install -y build-essential cmake pkg-config \\"
            echo "      qt6-base-dev qt6-multimedia-dev qt6-charts-dev \\"
            echo "      libopencv-dev libsdl2-dev zlib1g-dev \\"
            echo "      libvulkan-dev glslc \\"
            echo "      libavcodec-dev libavformat-dev libavutil-dev \\"
            echo "      libswscale-dev libswresample-dev"
        elif [ "$OS" = "macos" ]; then
            echo "  brew install cmake qt@6 opencv sdl2 zlib"
            echo "  brew install ffmpeg" 
            echo "  Vulkan (可选): brew install vulkan-sdk"
        fi
    fi
fi

# 3. 编译
echo -e "\n${YELLOW}[3/3] 编译 (${BUILD_TYPE})...${NC}"
BUILD_DIR="$DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=OFF

if [ "$OS" = "macos" ]; then
    JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
else
    JOBS=$(nproc 2>/dev/null || echo 4)
fi
cmake --build . -j"$JOBS"

echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}  初始化完成！${NC}"
echo -e "${GREEN}  运行: $BUILD_DIR/bin/VideoEye${NC}"
echo -e "${GREEN}========================================${NC}"
