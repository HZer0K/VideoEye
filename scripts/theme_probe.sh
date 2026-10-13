#!/bin/bash
# 主题探针编译 + 运行 + 抓图（开发期工具）
# 用法: bash scripts/theme_probe.sh <输出目录>
#
# 注意：所有文件路径用 Windows 风格 (D:/...)，因为 MSYS2_ARG_CONV_EXCL='*'
# 禁掉了参数转换，Unix 风格路径 (/d/...) 会被原样传给原生 exe 而打不开。
set -e
OUT="${1:-/tmp/ve_theme_probe}"
REPO_W='D:/Coding/C/videoeye/VideoEye'
VCPKG_W="$REPO_W/vcpkg_installed/x64-windows-release"
QTINC="$VCPKG_W/include"

source /d/Coding/C/videoeye/msvc_env.sh
# Git Bash 的 MSYS 会把 /nologo、/I... 这类参数当成 Unix 路径改写成 Windows 路径
# (cl 报 D9024)，必须禁掉参数转换；文件路径因此一律用 D:/ 形式。
export MSYS2_ARG_CONV_EXCL='*'
CL="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/$MSVC_VER/bin/Hostx64/x64/cl.exe"
RCC="$VCPKG_W/tools/Qt6/bin/rcc.exe"

mkdir -p "$OUT"
cd "$OUT"
# qrc -> cpp（勾选图标资源）
"$RCC" "$REPO_W/ui/theme/theme.qrc" -o qrc_theme.cpp

"$CL" /nologo /utf-8 /EHsc /permissive- /Zc:__cplusplus /MD /std:c++17 /O2 \
  /I"$REPO_W" /I"$QTINC/Qt6/QtCore" /I"$QTINC/Qt6/QtGui" /I"$QTINC/Qt6/QtWidgets" /I"$QTINC/Qt6" \
  /Fe:theme_probe.exe \
  "$REPO_W/scripts/theme_probe.cpp" "$REPO_W/ui/theme/AppTheme.cpp" qrc_theme.cpp \
  /link /LIBPATH:"$VCPKG_W/lib" Qt6Widgets.lib Qt6Gui.lib Qt6Core.lib

# 运行（offscreen），DLL 从 vcpkg bin 找。
# 注意：PATH 前缀用 Unix 风格 (/d/...) —— MSYS 生成子进程时会把 PATH 自动转成
# Windows 形式；直接写 D:/ 正斜杠会让原生加载器找不到 Qt6Core.dll。
QT_QPA_PLATFORM=offscreen PATH="/d/Coding/C/videoeye/VideoEye/vcpkg_installed/x64-windows-release/bin:$PATH" ./theme_probe.exe
echo "--- 抓图输出 ---"
ls -la "$OUT"/*.png
