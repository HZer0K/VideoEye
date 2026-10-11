#!/bin/bash
# ========================================
#  VideoEye - 统一构建入口
#  支持: Linux / macOS / WSL / Windows 的 Git Bash / MSYS2
#
#  用法: ./build.sh [release|debug|test|clean]
#  默认: release
#    release  Release 构建
#    debug    Debug 构建
#    test     构建 + 编译完自动跑 ctest
#    clean    清理 build/ 下全部构建目录
#
#  第一个参数之后的内容会原样透传给 cmake --preset（需要额外 -D 时用）。
#
#  环境变量:
#    JOBS=8 ./build.sh release   # 限制编译并行数（内存小的机器用）
#
#  构建配置的唯一来源是 CMakePresets.json / CMakeUserPresets.json，本脚本只是薄封装。
#  Linux/macOS 上 FFmpeg 默认走系统开发包(由 pkg-config 查找)，预编译包只是备选。
#
#  Windows 上有两个入口，按"谁负责注入 MSVC 环境"分工:
#    build.bat  —— cmd.exe / CI / 还没有 CMakeUserPresets.json 的机器，
#                  它会用 vswhere + vcvars 自己把 cl.exe / ninja 铺进 PATH。
#    build.sh   —— Git Bash / MSYS2，依赖 CMakeUserPresets.json 里钉死的
#                  INCLUDE / LIB / PATH / cl.exe 绝对路径，不需要 vcvars，
#                  因此在跑不了 cmd.exe 的环境里也能构建。
# ========================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_ROOT"

# ============ 平台判定 ============
case "$(uname -s)" in
    Linux*)                          PLATFORM="linux" ;;
    Darwin*)                         PLATFORM="macos" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT*) PLATFORM="windows" ;;
    *) echo "[ERROR] 不支持的平台: $(uname -s)"; exit 1 ;;
esac

# CMakePresets.json 里通用 preset 都带前缀(win-* / linux-*)，
# CMakeUserPresets.json(本机，gitignore) 用的是短名 —— 两个名字都要会拼。
case "$PLATFORM" in
    windows) PFX="win" ;;
    *)       PFX="linux" ;;
esac

# ============ clean ============
if [[ "${1:-release}" == "clean" ]]; then
    echo "清理 build/ 下的构建目录..."
    if [[ ! -d "$PROJECT_ROOT/build" ]]; then
        echo "  build/ 不存在，无需清理"
        exit 0
    fi
    found=0
    # 只删 build/ 的一级子目录。
    for d in "$PROJECT_ROOT"/build/*/; do
        [[ -d "$d" ]] || continue
        name="$(basename "$d")"
        size="$(du -sh "$d" 2>/dev/null | cut -f1)"
        rm -rf "$d"
        echo "  已删除: build/$name ($size)"
        found=1
    done
    if [[ "$found" == "0" ]]; then
        echo "  build/ 下没有子目录"
    fi
    echo "构建目录已清理"
    exit 0
fi

PRESET_KIND="${1:-release}"
shift || true

case "$PRESET_KIND" in
    release) GENERIC="$PFX-release"; SHORT="release" ;;
    debug)   GENERIC="$PFX-debug";   SHORT="debug" ;;
    # Windows 用 test-release: win-test-debug 要用 debug triplet 重编一整套 Qt(约 1 小时)。
    # Linux 没有这个问题，测试直接用 Debug。
    test)
        if [[ "$PLATFORM" == "windows" ]]; then
            GENERIC="win-test-release"; SHORT="test-release"
        else
            GENERIC="linux-test-debug"; SHORT="test-debug"
        fi ;;
    *) echo "用法: $0 [release|debug|test|clean]"; exit 1 ;;
esac

IS_TEST=0
if [[ "$PRESET_KIND" == "test" ]]; then IS_TEST=1; fi

# JOBS 透传给 ninja: 内存小的机器上靠它限制并行数
BUILD_ARGS=()
if [[ -n "${JOBS:-}" ]]; then
    BUILD_ARGS=(--parallel "$JOBS")
fi

# Windows: 系统里的 VCPKG_ROOT 可能指向 Visual Studio 自带的 vcpkg，
# 会覆盖 CMakeUserPresets.json 里配置好的那个 —— 必须让 preset 说话。
if [[ "$PLATFORM" == "windows" ]]; then
    unset VCPKG_ROOT
fi

# ============ preset 名解析 ============
# 通用 preset(win-* / linux-*) 一定存在；短名只在本机有 CMakeUserPresets.json 时才有。
# 先问 cmake 实际列出什么，再决定用哪个 —— 不靠猜。
list_presets() {
    cmake --list-presets 2>/dev/null | tr -d '\r' |
        awk 'NF && $1 !~ /^Available/ {gsub(/"/, "", $1); print $1}'
}

resolve_preset() {
    if [[ -f "$PROJECT_ROOT/CMakeUserPresets.json" ]] && list_presets | grep -qx -- "$SHORT"; then
        echo "$SHORT"
    else
        echo "$GENERIC"
    fi
}

# ============ Windows: MSVC 日志转码 ============
# cl.exe 的输出是本机代码页(中文 Windows = GBK)，有时是 UTF-16 —— 直接打到终端会乱码，
# 所以落盘后按 UTF-16 / UTF-8 / GBK 顺序转码再打印。
# 顺序很重要: utf-16-le 几乎永远"解码成功"(任意偶数长度字节都能解出字符)，
# 放在第一位会把 ASCII+GBK 的内容变成一堆汉字乱码。
dump_log() {
    local log="$1"
    if command -v python >/dev/null 2>&1; then
        local log_arg="$log"
        # LOG 是 MSYS 风格路径 (/d/...)，Windows 版 python 认不出来
        if command -v cygpath >/dev/null 2>&1; then log_arg="$(cygpath -w "$log")"; fi
        python - "$log_arg" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
    print(data.decode('utf-16'))
else:
    for enc in ('utf-8', 'gbk'):
        try:
            print(data.decode(enc))
            break
        except Exception:
            continue
PY
    else
        cat "$log"
    fi
}

run_logged() {
    # $1 = 阶段名, 其余 = 命令
    local stage="$1"; shift
    mkdir -p "$PROJECT_ROOT/.workbuddy"
    local log="$PROJECT_ROOT/.workbuddy/build-$stage.log"
    set +e
    "$@" > "$log" 2>&1
    local code=$?
    set -e
    dump_log "$log"
    return $code
}

# ============ 依赖缺失提示(仅 Linux/macOS) ============
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
    echo "  third_party/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]' | sed 's/darwin/macos/')-$(uname -m | sed 's/x86_64/x64/;s/aarch64/arm64/')/ffmpeg/{include,lib,bin}"
}

# 只在 configure 阶段、且输出确实像"找不到依赖"时才给提示。
# 编译错误（语法错、链接错、ninja 失败）必须原样抛给调用方 ——
# 否则真实的编译失败会被包装成"缺依赖"，往错误方向排查。
looks_like_missing_deps() {
    grep -qE 'Could NOT find|未找到 FFmpeg|CMAKE_(C|CXX)_COMPILER|CMAKE_MAKE_PROGRAM|Qt6|libav(codec|format|util)|pkg-config' "$1"
}

# ============ 主流程 ============
PRESET="$(resolve_preset)"
STEPS=2
if [[ "$IS_TEST" == "1" ]]; then STEPS=3; fi

echo "===================================="
echo " VideoEye Build - $PRESET_KIND ($PLATFORM)"
echo " preset: $PRESET"
echo "===================================="

# --- 1. Configure ---
echo "[1/$STEPS] CMake Configure..."
if [[ "$PLATFORM" == "windows" ]]; then
    # 已经有缓存时 cmake 是幂等的，每次都 configure 的代价可以忽略，
    # 而且能自动重建被删掉的 rules.ninja。
    if ! run_logged configure cmake --preset "$PRESET" "$@"; then
        echo ""
        echo "--- configure 失败（上面是 CMake 原始输出）---"
        exit 1
    fi
else
    CONFIGURE_LOG="$(mktemp "${TMPDIR:-/tmp}/videoeye-configure.XXXXXX")"
    trap 'rm -f "$CONFIGURE_LOG"' EXIT
    if ! cmake --preset "$PRESET" "$@" 2>&1 | tee "$CONFIGURE_LOG"; then
        if looks_like_missing_deps "$CONFIGURE_LOG"; then
            print_deps_hint
        else
            echo ""
            echo "--- configure 失败（不是缺依赖，上面是 CMake 原始输出）---"
        fi
        exit 1
    fi
fi

# --- 2. Build ---
echo ""
echo "[2/$STEPS] Build..."
# ${BUILD_ARGS[@]+...} 这种写法是为了兼容 bash 3.2 (macOS 自带):
# 空数组在 set -u 下直接展开会报 unbound variable。
if [[ "$PLATFORM" == "windows" ]]; then
    if ! run_logged build cmake --build --preset "$PRESET" ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}; then
        echo ""
        echo "--- 编译失败（上面是编译器的原始输出）---"
        exit 1
    fi
else
    if ! cmake --build --preset "$PRESET" ${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}; then
        echo ""
        echo "--- 编译失败（上面是编译器的原始输出）---"
        exit 1
    fi
fi

# --- 3. ctest（test 子命令）---
if [[ "$IS_TEST" == "1" ]]; then
    echo ""
    echo "[3/3] ctest..."
    if ! ctest --preset "$PRESET"; then
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

# Windows 上 release / debug / test 三个 preset 的 binaryDir 都是 build/<config>，
# 与 Linux 一致（win-release → build/release），所以这里不需要按 preset 名再映射一次。
BIN_DIR="build/$PRESET_KIND"
EXE="$BIN_DIR/bin/VideoEye"
if [[ "$PLATFORM" == "windows" ]]; then EXE="$EXE.exe"; fi
echo ""
echo "===================================="
echo " 构建成功: $EXE"
echo "===================================="
