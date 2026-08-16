@echo off
REM VideoEye Windows Build Script
REM Usage: build.bat [debug|release|ninja]
REM
REM Always uses Ninja + MSVC (Visual Studio generator blocked by LOLBin policy)
REM "ninja" or no argument  -> Release (default)
REM "debug"                  -> Debug
REM "release"                -> Release

setlocal

if /i "%1"=="debug" (
    powershell -ExecutionPolicy Bypass -File "%~dp0build_ninja.ps1" -BuildType Debug
    goto :end
)

if /i "%1"=="release" (
    powershell -ExecutionPolicy Bypass -File "%~dp0build_ninja.ps1" -BuildType Release
    goto :end
)

REM Default: ninja release
powershell -ExecutionPolicy Bypass -File "%~dp0build_ninja.ps1"

:end
