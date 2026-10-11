#!/bin/bash
# VideoEye 开发环境初始化脚本 (Linux / macOS / WSL)
#
# 只做三件事: 检查依赖、打印安装建议、调用统一的构建入口 build.sh。
# 以前这里自己 mkdird 了一个 build-linux/ 并直接跑 cmake，绕开了 preset，
# 于是"setup.sh 构建出来的"和"build.sh 构建出来的"是两套配置 —— 现在废弃那套。

set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

OS="unknown"
case "$(uname -s)" in
    Linux*)  OS="linux";;
    Darwin*) OS="macos";;
esac

SKIP_DEPS=false; BUILD_TYPE="release"
for arg in "$@"; do
    case "$arg" in
        --skip-deps) SKIP_DEPS=true ;;
        --debug) BUILD_TYPE=debug ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo "  --skip-deps    跳过依赖检查"
            echo "  --debug        Debug 构建 (默认 release)"
            exit 0 ;;
        *) echo "未知选项: $arg (用 -h 查看用法)"; exit 1 ;;
    esac
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  VideoEye 环境初始化 (${OS})${NC}"
echo -e "${GREEN}========================================${NC}"

if [ "$SKIP_DEPS" = false ]; then
    echo -e "\n${YELLOW}[1/2] 依赖检查...${NC}"
    check_cmd() { command -v "$1" >/dev/null 2>&1 && echo -e "  ${GREEN}[OK]${NC} $1" || echo -e "  ${RED}[缺]${NC} $1"; }
    check_pkg() { pkg-config --exists "$1" 2>/dev/null && echo -e "  ${GREEN}[OK]${NC} $1" || echo -e "  ${RED}[缺]${NC} $1"; }

    check_cmd cmake; check_cmd ninja; check_cmd pkg-config
    check_pkg Qt6Widgets

    # FFmpeg: Linux/macOS 默认用系统的（pkg-config），预编译包只是备选
    echo ""
    PREBUILT_DIR="$DIR/third_party/prebuilt/${OS}-$(uname -m | sed 's/x86_64/x64/;s/aarch64/arm64/')/ffmpeg"
    if check_pkg libavcodec >/dev/null 2>&1; then
        echo -e "  ${GREEN}[OK]${NC} FFmpeg (pkg-config: $(pkg-config --modversion libavcodec))"
    elif [ -d "$PREBUILT_DIR/include" ] && [ -d "$PREBUILT_DIR/lib" ]; then
        echo -e "  ${GREEN}[OK]${NC} FFmpeg (预编译包 $PREBUILT_DIR)"
    else
        echo -e "  ${RED}[缺]${NC} FFmpeg —— 系统包或预编译包都没找到"
        echo "     推荐: Debian/Ubuntu: sudo apt install -y libavcodec-dev libavformat-dev libavutil-dev \\"
        echo "                          libswscale-dev libswresample-dev"
        echo "           macOS:         brew install ffmpeg"
        echo "     备选: 把预编译包放到 $PREBUILT_DIR/{include,lib,bin}"
    fi

    echo ""
    if [ "$OS" = "linux" ]; then
        echo "  缺依赖时安装:"
        echo "    sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev \\"
        echo "      libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev"
    elif [ "$OS" = "macos" ]; then
        echo "  缺依赖时安装:"
        echo "    brew install cmake ninja qt@6 ffmpeg"
    fi
fi

# --- 编译: 交给统一入口 ---
echo -e "\n${YELLOW}[2/2] 编译 (${BUILD_TYPE})...${NC}"
./build.sh "$BUILD_TYPE"

echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}  初始化完成！${NC}"
echo -e "${GREEN}  运行: ./run.sh ${BUILD_TYPE}${NC}"
echo -e "${GREEN}========================================${NC}"
