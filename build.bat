@echo off
chcp 936 >nul 2>&1
REM ========================================
REM  VideoEye - ??????? (Windows / MSVC)
REM  build.bat [release|debug|test|test-debug|clean]
REM  ???: release
REM  ???: ???????????? GB18030 ???? + CRLF ????,
REM        ???? cmd.exe ??????????????????
REM        "'xxx' ???????????????"??
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
    echo [ERROR] ??Ч????: %PRESET%
    echo ?÷?: build.bat [release^|debug^|test^|test-debug^|clean]
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
        echo [ERROR] δ???? VCPKG_ROOT ????????
        echo         ??? preset ????????λ vcpkg toolchain??
        echo         ???????: set VCPKG_ROOT=^<vcpkg ????^>
        echo         ??????? CMakeUserPresets.json д??????·????
        exit /b 1
    )
)

REM ??????? MSVC ???? (vswhere + vcvars64)
echo [1/3] ??? Visual Studio ????...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] δ??? vswhere.exe, ??? Visual Studio 2022 ?? Build Tools
    exit /b 1
)

set "VSROOT="
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "!VSROOT!"=="" (
    echo [ERROR] δ????? C++ ???????? Visual Studio ???
    exit /b 1
)
echo       VS ·??: !VSROOT!

set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" (
    echo [ERROR] δ??? vcvars64.bat
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

REM ?????? FFmpeg (?????)
echo [2/3] ??? FFmpeg...
set "FFMPEG_DIR=%~dp0third_party\prebuilt\windows-x64\ffmpeg"
if not exist "!FFMPEG_DIR!\include\libavcodec\avcodec.h" (
    echo       FFmpeg δ???, ?????????...
    powershell -ExecutionPolicy Bypass -File "%~dp0scripts\fetch-ffmpeg.ps1"
    if errorlevel 1 (
        echo [ERROR] FFmpeg ??????
        exit /b 1
    )
)
echo       OK

REM CMake configure + build via presets
echo [3/3] CMake ???? + ???? (!USE_PRESET!)...
echo.
cmake --preset !USE_PRESET!
if errorlevel 1 (
    echo [ERROR] CMake ???????
    exit /b 1
)

echo.
cmake --build --preset !USE_PRESET! --parallel %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo [ERROR] ???????
    exit /b 1
)

echo.
if "!ISTEST!"=="1" (
    echo ???е??????...
    ctest --preset !USE_PRESET!
    if errorlevel 1 (
        echo [ERROR] ??????????
        exit /b 1
    )
    echo.
    echo ========================================
    echo  ?????????????
    echo ========================================
    endlocal
    exit /b 0
)

echo ========================================
echo  ???????!
echo  ????????: %~dp0build\!PRESET!\bin\VideoEye.exe
echo ========================================
endlocal
