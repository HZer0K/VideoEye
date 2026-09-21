@echo off
chcp 936 >nul 2>&1
REM ========================================
REM  VideoEye - 一键构建 (Windows / MSVC)
REM  用法: build.bat [release|debug|clean]
REM  默认: release
REM  注意: 本文件必须保存为 GBK 编码 + CRLF 换行,
REM        否则 cmd.exe 会把中文解析成乱码并报
REM        "'xxx' 不是内部或外部命令"。
REM ========================================
setlocal enabledelayedexpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
if /i "%PRESET%"=="ninja" set "PRESET=release"
if /i "%PRESET%"=="default" set "PRESET=release"

REM clean 子命令: 清理构建目录后退出
if /i "%PRESET%"=="clean" (
    if exist "%~dp0build\release" (
        rmdir /s /q "%~dp0build\release"
        echo 已删除: build\release
    )
    if exist "%~dp0build\debug" (
        rmdir /s /q "%~dp0build\debug"
        echo 已删除: build\debug
    )
    echo 构建目录已清理
    exit /b 0
)

set "VALID=0"
if /i "%PRESET%"=="release" set "VALID=1"
if /i "%PRESET%"=="debug" set "VALID=1"
if "!VALID!"=="0" (
    echo [ERROR] 无效参数: %PRESET%
    echo 用法: build.bat [release^|debug^|clean]
    exit /b 1
)

REM 本机专有配置放在 CMakeUserPresets.json (gitignore), 里面写着 VS/SDK/cl.exe
REM 的实际路径; 没有该文件时退回 CMakePresets.json 里的通用 preset.
set "USE_PRESET=!PRESET!"
if not exist "%~dp0CMakeUserPresets.json" (
    if /i "!PRESET!"=="release" set "USE_PRESET=win-release"
    if /i "!PRESET!"=="debug" set "USE_PRESET=win-debug"
    echo       未找到 CMakeUserPresets.json, 使用通用 preset: !USE_PRESET!
)
if "!USE_PRESET:~0,4!"=="win-" (
    if "!VCPKG_ROOT!"=="" (
        echo [ERROR] 未设置 VCPKG_ROOT 环境变量
        echo         通用 preset 依赖它定位 vcpkg toolchain。
        echo         请先执行: set VCPKG_ROOT=^<vcpkg 根目录^>
        echo         或复制一份 CMakeUserPresets.json 写死本机路径。
        exit /b 1
    )
)

REM 自动加载 MSVC 环境 (vswhere + vcvars64)
echo [1/3] 检测 Visual Studio 环境...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] 未找到 vswhere.exe, 请安装 Visual Studio 2022 或 Build Tools
    exit /b 1
)

set "VSROOT="
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "!VSROOT!"=="" (
    echo [ERROR] 未找到带 C++ 工具链的 Visual Studio 安装
    exit /b 1
)
echo       VS 路径: !VSROOT!

set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" (
    echo [ERROR] 未找到 vcvars64.bat
    exit /b 1
)
call "!VCVARS!" x64 >nul

REM 自动获取 FFmpeg (如果缺失)
echo [2/3] 检查 FFmpeg...
set "FFMPEG_DIR=%~dp0third_party\prebuilt\windows-x64\ffmpeg"
if not exist "!FFMPEG_DIR!\include\libavcodec\avcodec.h" (
    echo       FFmpeg 未找到, 自动下载中...
    powershell -ExecutionPolicy Bypass -File "%~dp0scripts\fetch-ffmpeg.ps1"
    if errorlevel 1 (
        echo [ERROR] FFmpeg 获取失败
        exit /b 1
    )
)
echo       OK

REM CMake configure + build via presets
echo [3/3] CMake 配置 + 构建 (!USE_PRESET!)...
echo.
cmake --preset !USE_PRESET!
if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

echo.
cmake --build --preset !USE_PRESET! --parallel %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo [ERROR] 编译失败
    exit /b 1
)

echo.
echo ========================================
echo  构建成功!
echo  可执行文件: %~dp0build\!PRESET!\bin\VideoEye.exe
echo ========================================
endlocal
