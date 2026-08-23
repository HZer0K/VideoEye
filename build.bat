@echo off
REM ========================================
REM  VideoEye - 一键构建 (Windows)
REM  用法: build.bat [release|debug|clean]
REM  默认: release
REM ========================================
setlocal enabledelayedexpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
if /i "%PRESET%"=="ninja" set "PRESET=release"

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

if /i not "%PRESET%"=="release" if /i not "%PRESET%"=="debug" (
    echo [ERROR] 无效参数: %PRESET%
    echo 用法: build.bat [release^|debug^|clean]
    exit /b 1
)

REM 自动加载 MSVC 环境 (通过 vswhere + vcvars64)
echo [1/3] 检测 Visual Studio 环境...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] 未找到 vswhere。请安装 Visual Studio 2022 或 Build Tools。
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "!VSROOT!"=="" (
    echo [ERROR] 未找到带 C++ 工具链的 Visual Studio 安装。
    exit /b 1
)
echo       VS 路径: !VSROOT!

REM 通过 vcvars64.bat 导入环境到当前 cmd 进程
set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" (
    echo [ERROR] 未找到 vcvars64.bat
    exit /b 1
)
call "!VCVARS!" x64 >nul

REM 自动获取 FFmpeg (如果缺失)
echo [2/3] 检查 FFmpeg...
set "FFMPEG_DIR=%~dp0third_party\ffmpeg-prebuilt"
if not exist "!FFMPEG_DIR!\include\libavcodec\avcodec.h" (
    echo       FFmpeg 未找到，自动下载中...
    powershell -ExecutionPolicy Bypass -File "%~dp0scripts\fetch-ffmpeg.ps1"
    if errorlevel 1 (
        echo [ERROR] FFmpeg 获取失败
        exit /b 1
    )
)
echo       OK

REM CMake configure + build via presets
echo [3/3] CMake 配置 + 构建 (!PRESET!)...
echo.
cmake --preset !PRESET!
if errorlevel 1 exit /b 1

echo.
cmake --build --preset !PRESET!
if errorlevel 1 exit /b 1

echo.
echo ========================================
echo  构建成功!
echo  可执行文件: %~dp0build\!PRESET!\bin\VideoEye.exe
echo ========================================
endlocal
