@echo off
chcp 936 >nul 2>&1
REM ========================================
REM  VideoEye - ??????? (Windows / MSVC)
REM  ?¡Â?: build.bat [release|debug|clean]
REM  ???: release
REM  ???: ???????????? GBK ???? + CRLF ????,
REM        ???? cmd.exe ??????????????????
REM        "'xxx' ???????????????"??
REM ========================================
setlocal enabledelayedexpansion

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
if /i "%PRESET%"=="ninja" set "PRESET=release"
if /i "%PRESET%"=="default" set "PRESET=release"

REM clean ??????: ??????????????
if /i "%PRESET%"=="clean" (
    if exist "%~dp0build\release" (
        rmdir /s /q "%~dp0build\release"
        echo ?????: build\release
    )
    if exist "%~dp0build\debug" (
        rmdir /s /q "%~dp0build\debug"
        echo ?????: build\debug
    )
    echo ????????????
    exit /b 0
)

set "VALID=0"
if /i "%PRESET%"=="release" set "VALID=1"
if /i "%PRESET%"=="debug" set "VALID=1"
if /i "%PRESET%"=="test" set "VALID=1"
if /i "%PRESET%"=="test-release" set "VALID=1"
if /i "%PRESET%"=="test-debug" set "VALID=1"
if "!VALID!"=="0" (
    echo [ERROR] ??§¹????: %PRESET%
    echo ?¡Â?: build.bat [release^|debug^|test^|test-debug^|clean]
    exit /b 1
)

REM ???? preset ????? CMakePresets.json ??? win-test-*:
REM   ??????? VCPKG_MANIFEST_FEATURES=tests, ????? gtest ???;
REM   CMakeUserPresets.json ????? preset ???????????þŸ
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
        echo [ERROR] ¦Ä???? VCPKG_ROOT ????????
        echo         ??? preset ????????¦Ë vcpkg toolchain??
        echo         ???????: set VCPKG_ROOT=^<vcpkg ????^>
        echo         ??????? CMakeUserPresets.json §Õ??????¡¤????
        exit /b 1
    )
)

REM ??????? MSVC ???? (vswhere + vcvars64)
echo [1/3] ??? Visual Studio ????...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] ¦Ä??? vswhere.exe, ??? Visual Studio 2022 ?? Build Tools
    exit /b 1
)

set "VSROOT="
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "!VSROOT!"=="" (
    echo [ERROR] ¦Ä????? C++ ???????? Visual Studio ???
    exit /b 1
)
echo       VS ¡¤??: !VSROOT!

set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
if not exist "!VCVARS!" (
    echo [ERROR] ¦Ä??? vcvars64.bat
    exit /b 1
)
call "!VCVARS!" x64 >nul
REM ---- resolve the preset name that really exists (win-<x>-local > <x> > win-<x>) ----
set "USE_PRESET="
for /f "usebackq tokens=1 delims= " %%p in (`cmake --list-presets 2^>nul`) do (
    set "P=%%p"
    set "P=!P:"=!"
    if /i "!P!"=="win-!PRESET!-local" set "USE_PRESET=win-!PRESET!-local"
    if /i "!P!"=="!PRESET!" if "!USE_PRESET!"=="" set "USE_PRESET=!PRESET!"
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
    echo       FFmpeg ¦Ä???, ?????????...
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
    echo ???§Ö??????...
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
