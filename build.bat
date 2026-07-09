@echo off
REM VideoEye Windows Build Script
REM Usage: build.bat [debug|release|ninja]
REM
REM By default uses Ninja + MSVC (bypasses MSBuild LOLBin restriction)
REM Use "ninja" or no argument for Ninja build

setlocal

if "%1"=="ninja" goto :ninja
if "%1"=="" goto :ninja

if /i "%1"=="debug" (
    set BUILD_TYPE=Debug
    goto :vs
) else (
    if /i "%1"=="release" (
        set BUILD_TYPE=Release
        goto :vs
    )
)

:ninja
echo =====================================
echo VideoEye Build (Ninja + MSVC)
echo =====================================
powershell -ExecutionPolicy Bypass -File "%~dp0build_ninja.ps1"
goto :end

:vs
echo =====================================
echo VideoEye Build (Visual Studio Generator)
echo Note: This may fail if MSBuild is blocked
echo =====================================
set BUILD_DIR=build-%BUILD_TYPE%
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
set "PROJECT_ROOT=%CD%"
cd %BUILD_DIR%

set VCPKG_ARGS=
if exist "%PROJECT_ROOT%\vcpkg_installed\x64-windows\share\opencv4" (
    set "VCPKG_ARGS=-DCMAKE_PREFIX_PATH=%PROJECT_ROOT%/vcpkg_installed/x64-windows"
)

cmake .. -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -G "Visual Studio 17 2022" -A x64 %VCPKG_ARGS% -DBUILD_TESTING=OFF %*
if %errorlevel% neq 0 (
    echo Configure failed!
    pause
    exit /b %errorlevel%
)

cmake --build . --config %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo Build failed! Try: build.bat ninja
    pause
    exit /b %errorlevel%
)

echo =====================================
echo Build complete: %BUILD_DIR%\bin\VideoEye.exe
echo =====================================

:end
