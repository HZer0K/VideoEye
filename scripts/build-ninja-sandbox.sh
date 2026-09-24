#!/usr/bin/env bash
# 沙箱内的 Windows 构建封装（build.bat 依赖 cmd /c，会被沙箱拦截）
#
# 用法: bash scripts/build-ninja-sandbox.sh [preset] [-- <cmake --build 的额外参数>]
#   preset 默认 release。所有本机路径（VS 版本 / Windows SDK / cl.exe / ninja /
#   vcpkg / FFmpeg）都写在 CMakeUserPresets.json 里（已 gitignore），本脚本一个都不重复。
# 例:
#   bash scripts/build-ninja-sandbox.sh
#   bash scripts/build-ninja-sandbox.sh test-release -- -j8
#
# 为什么不用 -D 参数直接喂 cmake:
#   CMakePresets.json / CMakeUserPresets.json 是构建配置的唯一真实来源。
#   在脚本里再抄一份 -DCMAKE_PREFIX_PATH / -DFFMPEG_ROOT，两边迟早漂移。
#   build preset 默认 inheritConfigureEnvironment=true，所以 configure preset
#   里配好的 PATH / INCLUDE / LIB（含 rc.exe、mt.exe 所在的 WinKits/bin）
#   在 cmake --build 阶段同样生效。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-release}"
if [ "$#" -gt 0 ]; then shift; fi

# 系统变量指向 VS 自带的错误 vcpkg 路径，会覆盖 preset 里配置好的 VCPKG_ROOT
unset VCPKG_ROOT

cd "$ROOT" || exit 1
mkdir -p "$ROOT/.workbuddy"

echo "[sandbox-build] preset: $PRESET (配置来源: CMakeUserPresets.json)"

# ---- 1. Configure ----
# 删掉 build.ninja 后只跑 ninja 无法自举(rules.ninja 丢失)，所以每次都先 configure。
# 已经有缓存时 cmake 是幂等的，代价可以忽略。
if ! cmake --preset "$PRESET"; then
    echo "[sandbox-build] configure 失败"
    exit 1
fi

# ---- 2. Build ----
# MSVC 输出是本机代码页(中文 Windows = GBK)，直接打到终端会乱码 —— 落盘后转码再打印。
LOG="$ROOT/.workbuddy/build-ninja.log"
cmake --build --preset "$PRESET" "$@" > "$LOG" 2>&1
code=$?

# LOG 是 MSYS 风格路径 (/d/...)，Windows 版 python 认不出来，
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
