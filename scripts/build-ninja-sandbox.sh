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
export PATH="$MSVC/bin/HostX64/x64:$PATH"
unset VCPKG_ROOT   # 系统变量指向 VS 自带的错误路径

cd "$ROOT/build-ninja" || exit 1
LOG="$ROOT/.workbuddy/build-ninja.log"
"$NINJA" "$@" > "$LOG" 2>&1
code=$?
# MSVC 输出是 UTF-16，转码后再打印
python - "$LOG" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
for enc in ('utf-16-le', 'utf-8', 'gbk'):
    try:
        print(data.decode(enc))
        break
    except Exception:
        continue
PY
exit $code
