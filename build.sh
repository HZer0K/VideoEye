#!/bin/bash
# ========================================
#  VideoEye - 一键构建 (Linux / macOS / WSL)
#  用法: ./build.sh [release|debug|test|clean]
#  默认: release
#  test = 带单元测试的构建 (linux-test-debug preset)，编译完自动跑 ctest
#
#  环境变量:
#    JOBS=8 ./build.sh release   # 限制编译并行数（内存小的机器用）
#
#  构建配置的唯一来源是 CMakePresets.json，本脚本只是薄封装。
#  Linux/macOS 上 FFmpeg 默认走系统开发包(由 pkg-config 查找)，
#  预编译包只是系统包不可用时的备选。
# ========================================
set -euo pipefail

PRESET="${1:-release}"
shift || true

# clean 子命令: 清理构建目录后退出
if [[ "$PRESET" == "clean" ]]; then
    PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
    cd "$PROJECT_ROOT"
    for d in build/release build/debug build/test-debug; do
        if [[ -d "$d" ]]; then
            rm -rf "$d"
            echo "已删除: $d"
        fi
    done
    echo "构建目录已清理"
    exit 0
fi

# 本脚本仅用于 Linux / macOS / WSL, 对应 CMakePresets.json 中的 linux-* preset
# CMakePresets.json 里只有 linux-test-debug（没有 linux-test-release）:
# Linux 不存在 Windows 那种"debug triplet 要重编一整套 Qt"的问题，测试直接用 Debug。
case "$PRESET" in
    release) CMAKE_PRESET="linux-release" ;;
    debug)   CMAKE_PRESET="linux-debug" ;;
    test)    CMAKE_PRESET="linux-test-debug" ;;
    *) echo "用法: $0 [release|debug|test|clean]"; exit 1 ;;
esac
IS_TEST=0
if [[ "$PRESET" == "test" ]]; then
    IS_TEST=1
    STEPS=3
else
    STEPS=2
fi

# JOBS 透传给 ninja: 内存小的机器上靠它限制并行数（QUICKSTART 里就是这么写的）
BUILD_ARGS=()
if [[ -n "${JOBS:-}" ]]; then
    BUILD_ARGS=(--parallel "$JOBS")
fi

ARCH_DIR="$(uname -m | sed 's/x86_64/x64/;s/aarch64/arm64/')"

# ============ 依赖缺失时的友好提示 ============
# 口径与 setup.sh 保持一致: 系统 FFmpeg (pkg-config) 为主, 预编译包只是备选。
print_deps_hint() {
    echo ""
    echo "--- 看起来是依赖缺失（不是代码问题）---"
    if [[ "$(uname)" == "Darwin" ]]; then
        echo "  brew install cmake ninja qt@6 ffmpeg"
    else
        echo "  sudo apt install -y build-essential cmake ninja-build pkg-config qt6-base-dev \\"
        echo "    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev"
    fi
    echo ""
    echo "FFmpeg 推荐用系统包（pkg-config 自动找到）。系统包不可用时才放预编译包:"
    echo "  third_party/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]' | sed 's/darwin/macos/')-${ARCH_DIR}/ffmpeg/{include,lib,bin}"
}

# 只在 configure 阶段、且输出确实像"找不到依赖"时才给提示。
# 编译错误（语法错、链接错、ninja 失败）必须原样抛给调用方 ——
# 否则真实的编译失败会被包装成"缺依赖"，往错误方向排查。
looks_like_missing_deps() {
    grep -qE 'Could NOT find|未找到 FFmpeg|CMAKE_(C|CXX)_COMPILER|CMAKE_MAKE_PROGRAM|Qt6|libav(codec|format|util)|pkg-config' "$1"
}

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_ROOT"

CONFIGURE_LOG="$(mktemp "${TMPDIR:-/tmp}/videoeye-configure.XXXXXX")"
trap 'rm -f "$CONFIGURE_LOG"' EXIT

echo "===================================="
echo " VideoEye Build - $PRESET"
echo "===================================="

# --- 1. Configure (via preset) ---
echo "[1/$STEPS] CMake Configure..."
if ! cmake --preset "$CMAKE_PRESET" "$@" 2>&1 | tee "$CONFIGURE_LOG"; then
    # pipefail 下这里的退出码是 cmake 的退出码
    if looks_like_missing_deps "$CONFIGURE_LOG"; then
        print_deps_hint
    else
        echo ""
        echo "--- configure 失败（不是缺依赖，上面是 CMake 原始输出）---"
    fi
    exit 1
fi

# --- 2. Build ---
echo ""
echo "[2/$STEPS] Build..."
# ${BUILD_ARGS[@]+...} 这种写法是为了兼容 bash 3.2 (macOS 自带):
# 空数组在 set -u 下直接展开会报 unbound variable。
if ! cmake --build --preset "$CMAKE_PRESET" ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}; then
    echo ""
    echo "--- 编译失败（上面是编译器的原始输出）---"
    exit 1
fi

# --- 3. 测试 preset 编译完直接跑 ctest（和 build.bat test 的行为一致）---
if [[ "$IS_TEST" == "1" ]]; then
    echo ""
    echo "[3/3] ctest..."
    if ! ctest --preset "$CMAKE_PRESET"; then
        echo ""
        echo "--- 单元测试失败 ---"
        exit 1
    fi
    echo ""
    echo "===================================="
    echo " 单元测试全部通过"
    echo "===================================="
    exit 0
fi

echo ""
echo "===================================="
echo " 构建成功: build/$PRESET/bin/VideoEye"
echo " 运行:   ./build/$PRESET/bin/VideoEye"
echo "===================================="
