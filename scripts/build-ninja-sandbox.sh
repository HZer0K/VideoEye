#!/usr/bin/env bash
# 沙箱内可用的 ninja 构建封装（build_ninja.ps1 依赖 cmd /c，会被沙箱拦截）
#
# 用法: bash scripts/build-ninja-sandbox.sh [ninja 参数...]
# 例:   bash scripts/build-ninja-sandbox.sh -j8
#
# 说明: 手动拼 MSVC / Windows SDK 的 INCLUDE、LIB 与 PATH，绕开 vcvars（reg.exe 被拦截时必需）。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSVC="${MSVC:-C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207}"
WINKIT="${WINKIT:-C:/Program Files (x86)/Windows Kits/10}"
SDKVER="${SDKVER:-10.0.22621.0}"
NINJA="${NINJA:-/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe}"

export INCLUDE="$MSVC/include;$WINKIT/Include/$SDKVER/ucrt;$WINKIT/Include/$SDKVER/um;$WINKIT/Include/$SDKVER/shared;$WINKIT/Include/$SDKVER/winrt;$WINKIT/Include/$SDKVER/cppwinrt"
export LIB="$MSVC/lib/x64;$WINKIT/Lib/$SDKVER/ucrt/x64;$WINKIT/Lib/$SDKVER/um/x64"
# WinKits/bin 也必须进 PATH: 只钉 cl.exe 不够, 链接期 vs_link_exe 会去调
# rc.exe / mt.exe, 找不到时表现为 "--mt=CMAKE_MT-NOTFOUND" + "RC Pass 1 failed"。
# （有 CMakeCache 时不会重新探测, 所以只在全新 configure 时才暴露）
export PATH="$MSVC/bin/HostX64/x64:$WINKIT/bin/$SDKVER/x64:$PATH"
unset VCPKG_ROOT   # 系统变量指向 VS 自带的错误路径

# ---- 需要时先 configure ----
# 只跑 ninja 的话, 删掉 CMakeCache/CMakeFiles 后 build.ninja 无法自举
# (rules.ninja 丢失), 必须先走一遍 cmake。
CMAKE_EXE="${CMAKE:-cmake}"
if [ ! -f "$ROOT/build-ninja/CMakeCache.txt" ] || [ ! -f "$ROOT/build-ninja/build.ninja" ]; then
    echo "[build-ninja-sandbox] 构建目录未配置, 先运行 CMake configure..."
    mkdir -p "$ROOT/build-ninja"
    # 传给 cmake(Windows 程序) 的路径必须是 C:/... 形式:
    #   * /c/... 会被当成 POSIX 路径, 表现为 ninja --version "系统找不到指定的文件";
    #   * C:\... 也不行 —— cmake 会把路径原样写进 CMakeCCompiler.cmake 的 set(),
    #     反斜杠被当转义符, 下次读回时 "Syntax error in cmake code"。
    #   cygpath -m 输出的 "C:/..." 两者都避开了。
    to_win() { cygpath -m "$1" 2>/dev/null || echo "$1"; }
    "$CMAKE_EXE" -S "$(to_win "$ROOT")" -B "$(to_win "$ROOT/build-ninja")" -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
        -DCMAKE_MAKE_PROGRAM="$(to_win "$NINJA")" \
        -DCMAKE_PREFIX_PATH="$(to_win "$ROOT/vcpkg_installed/x64-windows-release")" \
        -DFFMPEG_ROOT="$(to_win "$ROOT/third_party/prebuilt/windows-x64/ffmpeg")" \
        -DBUILD_TESTING=OFF \
        -DCMAKE_C_COMPILER="$(to_win "$MSVC/bin/HostX64/x64/cl.exe")" \
        -DCMAKE_CXX_COMPILER="$(to_win "$MSVC/bin/HostX64/x64/cl.exe")" \
        -DCMAKE_RC_COMPILER="$(to_win "$WINKIT/bin/$SDKVER/x64/rc.exe")" \
        -DCMAKE_MT="$(to_win "$WINKIT/bin/$SDKVER/x64/mt.exe")" || exit 1
fi

cd "$ROOT/build-ninja" || exit 1
LOG="$ROOT/.workbuddy/build-ninja.log"
"$NINJA" "$@" > "$LOG" 2>&1
code=$?
# MSVC 输出是 UTF-16，转码后再打印
# 注意: LOG 是 MSYS 风格路径 (/d/...)，Windows 版 python 认不出来，
# 必须先用 cygpath 转成 D:\... 再传给 python，否则报 FileNotFoundError。
LOG_WIN="$(cygpath -w "$LOG" 2>/dev/null || echo "$LOG")"
python - "$LOG_WIN" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
# 顺序很重要: cl 的输出是本机代码页(中文 Windows = GBK)。
# utf-16-le 几乎永远"解码成功"(任意偶数长度字节都能解出字符)，
# 放在第一位会把 ASCII+GBK 的内容变成一堆汉字乱码。
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
exit $code
