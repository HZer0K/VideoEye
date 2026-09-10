#!/usr/bin/env bash
# 在沙箱里直接编译并运行 AudioQc 单测（不经过 CMake，gtest 只提供 gtest.lib，用 shim 补 main）
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSVC="${MSVC:-C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207}"
WINKIT="${WINKIT:-C:/Program Files (x86)/Windows Kits/10}"
SDKVER="${SDKVER:-10.0.22621.0}"

export INCLUDE="$MSVC/include;$WINKIT/Include/$SDKVER/ucrt;$WINKIT/Include/$SDKVER/um;$WINKIT/Include/$SDKVER/shared;$ROOT/vcpkg_installed/x64-windows-release/include"
export LIB="$MSVC/lib/x64;$WINKIT/Lib/$SDKVER/ucrt/x64;$WINKIT/Lib/$SDKVER/um/x64"
export PATH="$MSVC/bin/HostX64/x64:$PATH"

OUT_REL=".workbuddy/tmp"
OUT="$ROOT/$OUT_REL"
ROOTW="$(cygpath -w "$ROOT")"
mkdir -p "$OUT"
GTLIB="$ROOT/vcpkg_installed/x64-windows-release/lib/gtest.lib"

cd "$ROOT" || exit 1
CL="${CL:-$MSVC/bin/HostX64/x64/cl.exe}"
INC="-I$ROOTW -I$ROOTW\\core -I$ROOTW\\vcpkg_installed\\x64-windows-release\\include"
LINK="/link $(cygpath -w "$GTLIB")"
CFLAGS="/nologo /std:c++17 /EHsc /O2 /MD /utf-8 /permissive-"

run_one() {  # $1=test src  $2=exe name  $3+=extra cpp
  local src="$1"; local exe="$2"; shift 2
  "$CL" $CFLAGS $INC "$src" "$@" "$OUT_REL/gtest_main_shim.cpp" \
     /Fe:"$OUT_REL/$exe.exe" /Fo:"$OUT_REL/" /Fd:"$OUT_REL/" $LINK > "$OUT/compile_$exe.log" 2>&1
  local code=$?
  python - "$(cygpath -w "$OUT/compile_$exe.log")" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
enc = 'utf-16-le' if b'\x00' in data[:4096] else 'utf-8'
print(data.decode(enc, 'replace'))
PY
  [ $code -ne 0 ] && exit $code
  "$OUT/$exe.exe"
}

echo "===== AudioQcAnalyzer 测试 ====="
run_one "tests/unit/test_audio_qc_analyzer.cpp" test_audio_qc_analyzer \
   "core/analyzer/AudioQcAnalyzer.cpp" "core/model/AudioQcResult.cpp"

echo
echo "===== AudioQc 规则测试 ====="
run_one "tests/unit/test_audio_qc_rules.cpp" test_audio_qc_rules \
   "core/analyzer/AudioQcAnalyzer.cpp" "core/analyzer/QcRuleEngine.cpp" \
   "core/analyzer/AnalysisTask.cpp" "core/model/AudioQcResult.cpp" \
   "core/model/QcModels.cpp" "core/model/QcReport.cpp" \
   "core/analyzer/BitrateGopAnalyzer.cpp" "core/model/BitratePoint.cpp" \
   "core/model/GopInfo.cpp" "core/model/TimelineDiagnostic.cpp"
