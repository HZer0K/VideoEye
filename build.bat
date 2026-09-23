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
if /i "!PRESET!"=="test-release" set "USE_PRESET=win-test-release"
if /i "!PRESET!"=="test-debug" set "USE_PRESET=win-test-debug"

REM ??????????¡Â??? CMakeUserPresets.json (gitignore), ????§Õ?? VS/SDK/cl.exe
REM ?????¡¤??; ??§Ú???????? CMakePresets.json ?????? preset.
if "!USE_PRESET!"=="" (
    set "USE_PRESET=!PRESET!"
    if not exist "%~dp0CMakeUserPresets.json" (
        if /i "!PRESET!"=="release" set "USE_PRESET=win-release"
        if /i "!PRESET!"=="debug" set "USE_PRESET=win-debug"
        echo       ¦Ä??? CMakeUserPresets.json, ?????? preset: !USE_PRESET!
    )
) else (
    set "ISTEST=1"
    echo       ????????????? preset: !USE_PRESET!
)
if "!USE_PRESET:~0,4!"=="win-" (
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
