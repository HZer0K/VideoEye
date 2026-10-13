#!/bin/bash
# 排序探针编译 + 运行（开发期工具）
# 用法: bash scripts/sort_probe.sh
#
# 注意：所有文件路径用 Windows 风格 (D:/...)，因为 MSYS2_ARG_CONV_EXCL='*'
# 禁掉了参数转换，Unix 风格路径 (/d/...) 会被原样传给原生 exe 而打不开。
set -e
OUT="/tmp/ve_sort_probe"
REPO_W='D:/Coding/C/videoeye/VideoEye'
VCPKG_W="$REPO_W/vcpkg_installed/x64-windows-release"
QTINC="$VCPKG_W/include"

source /d/Coding/C/videoeye/msvc_env.sh
export MSYS2_ARG_CONV_EXCL='*'
CL="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/$MSVC_VER/bin/Hostx64/x64/cl.exe"

mkdir -p "$OUT"
cd "$OUT"
"$CL" /nologo /utf-8 /EHsc /permissive- /Zc:__cplusplus /MD /std:c++17 /O2 \
  /I"$QTINC/Qt6/QtCore" /I"$QTINC/Qt6/QtGui" /I"$QTINC/Qt6/QtWidgets" /I"$QTINC/Qt6" \
  /Fe:sort_probe.exe \
  "$REPO_W/scripts/sort_probe.cpp" \
  /link /LIBPATH:"$VCPKG_W/lib" Qt6Widgets.lib Qt6Gui.lib Qt6Core.lib

QT_QPA_PLATFORM=offscreen PATH="/d/Coding/C/videoeye/VideoEye/vcpkg_installed/x64-windows-release/bin:$PATH" ./sort_probe.exe
