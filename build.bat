@echo off
chcp 936 >nul 2>&1
REM ========================================
REM  VideoEye - 一键构建 (Windows / MSVC)
REM  用法: build.bat [release|debug|test|test-debug|clean]
REM  默认: release
REM  注意: 本文件必须保存为 GB18030 编码 + CRLF 换行,
REM        否则 cmd.exe 会把中文解析成乱码并报
REM        "'xxx' 不是内部或外部命令"。
REM ========================================
setlocal enabledelayedexpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
if /i "%PRESET%"=="ninja" set "PRESET=release"
if /i "%PRESET%"=="default" set "PRESET=release"

REM clean 子命令: 清掉 build\ 下的全部构建目录。以前这里只写死清 release 与 debug，
REM   于是各次验证手工建出来的 _recheck / _verify / cfg18 之类永远清不掉，越攒越多。
if /i "%PRESET%"=="clean" (
    echo 清理 build 下的构建目录...
    if not exist "%~dp0build" (
        echo   build 目录不存在，无需清理
        exit /b 0
    )
    for /d %%d in ("%~dp0build\*") do (
        echo   已删除: %%~nxd
        rmdir /s /q "%%d"
    )
    echo 构建目录已清理
    exit /b 0
)

set "VALID=0"
if /i "%PRESET%"=="release" set "VALID=1"
if /i "%PRESET%"=="debug" set "VALID=1"
if /i "%PRESET%"=="test" set "VALID=1"
if /i "%PRESET%"=="test-release" set "VALID=1"
if /i "%PRESET%"=="test-debug" set "VALID=1"
if "!VALID!"=="0" (
    echo [ERROR] 无效参数: %PRESET%
    echo 用法: build.bat [release^|debug^|test^|test-debug^|clean]
    exit /b 1
)

REM 测试 preset 对应 CMakePresets.json 里的 win-test-*:
REM   Windows 上带 VCPKG_MANIFEST_FEATURES=tests, 由它装 gtest;
REM   CMakeUserPresets.json 里的本机 preset 用短名(release / debug / test-release / test-debug),
REM   通用 preset 才带 win- 前缀 —— 下面统一先试短名、再回退 win-*。
set "ISTEST=0"
set "USE_PRESET="
if /i "%PRESET%"=="test" set "PRESET=test-release"

REM CMakeUserPresets.json (gitignored) pins this machine VS/SDK/cl.exe paths.
REM Its preset names are NOT fixed by convention: scripts/setup-windows-env.ps1
REM emits "win-<x>-local", a hand-written file may just use "<x>". Probe what
REM cmake actually reports instead of hard-coding one spelling.
set "HAS_LOCAL=0"
if exist "%~dp0CMakeUserPresets.json" set "HAS_LOCAL=1"
if /i "!PRESET!"=="test-release" set "ISTEST=1"
if /i "!PRESET!"=="test-debug" set "ISTEST=1"

if "!HAS_LOCAL!"=="0" (
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
REM ---- resolve the preset name that really exists (<x> > win-<x>) ----
REM CMakeUserPresets.json 里的本机 preset 用短名(release / debug / test-release / test-debug)，
REM 通用的 CMakePresets.json 只有带前缀的 win-* —— 先问 cmake 实际列出什么，再决定用哪个。
REM 以前还要试 win-<x>-local 这第三种拼写（setup-windows-env.ps1 的历史输出格式）；
REM 生成器已统一输出短名，这里就不再猜它了。老机器上若还有 win-*-local，重跑一次生成器即可。
set "USE_PRESET="
for /f "usebackq tokens=1 delims= " %%p in (`cmake --list-presets 2^>nul`) do (
    set "P=%%p"
    set "P=!P:"=!"
    if /i "!P!"=="!PRESET!" set "USE_PRESET=!PRESET!"
    if /i "!P!"=="win-!PRESET!" if "!USE_PRESET!"=="" set "USE_PRESET=win-!PRESET!"
)
if "!USE_PRESET!"=="" set "USE_PRESET=win-!PRESET!"
if "!HAS_LOCAL!"=="1" (
    echo       local preset: !USE_PRESET!
) else (
    echo       CMakeUserPresets.json not found, using base preset: !USE_PRESET!
)

REM ---- FFmpeg: 每次都调用一次获取脚本（幂等）----
REM 目录已存在且与 cmake/ffmpeg-version.json 的 url / sha256 完全对得上时，
REM 脚本只做几次文件检查就退出（不联网）。以前写的是「头文件在就跳过」，于是
REM 同版本换了构建包、或者改了锁定配置，本机都会继续用旧包，而且没有任何提示。
echo [2/3] 检查 FFmpeg...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\fetch-ffmpeg.ps1"
if errorlevel 1 (
    echo [ERROR] FFmpeg 准备失败
    exit /b 1
)

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
if "!ISTEST!"=="1" (
    echo 运行单元测试...
    ctest --preset !USE_PRESET!
    if errorlevel 1 (
        echo [ERROR] 单元测试未通过
        exit /b 1
    )
    echo.
    echo ========================================
    echo  全部测试通过
    echo ========================================
    endlocal
    exit /b 0
)

echo ========================================
echo  构建成功!
echo  可执行文件: %~dp0build\!PRESET!\bin\VideoEye.exe
echo ========================================
endlocal
