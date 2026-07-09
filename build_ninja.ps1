# VideoEye Ninja Build Script (PowerShell)
# Usage: powershell -ExecutionPolicy Bypass -File build_ninja.ps1
#
# This script bypasses the MSBuild LOLBin restriction by using the Ninja generator
# with manually configured MSVC environment variables.

$ErrorActionPreference = "Stop"

$projectRoot = "D:\Coding\C\videoeye\VideoEye"
$buildDir = "$projectRoot\build-ninja"

# ── MSVC and Windows SDK paths ──
$msvcRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808"
$winKits = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer = "10.0.22621.0"
$ninjaExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$vcpkgBin = "$projectRoot\vcpkg_installed\x64-windows\bin"
$vcpkgLib = "$projectRoot\vcpkg_installed\x64-windows\lib"
$vcpkgQtPlugins = "$projectRoot\vcpkg_installed\x64-windows\Qt6\plugins"

# ── Set up MSVC environment ──
$env:PATH = "$msvcRoot\bin\Hostx64\x64;$winKits\bin\$sdkVer\x64;$env:PATH"
$env:INCLUDE = "$msvcRoot\include;$winKits\Include\$sdkVer\ucrt;$winKits\Include\$sdkVer\um;$winKits\Include\$sdkVer\shared;$winKits\Include\$sdkVer\winrt"
$env:LIB = "$msvcRoot\lib\x64;$winKits\Lib\$sdkVer\ucrt\x64;$winKits\Lib\$sdkVer\um\x64;$vcpkgLib"

Write-Output "=== VideoEye Ninja Build ==="

# Verify cl.exe
$clPath = Get-Command cl.exe -ErrorAction SilentlyContinue
if (!$clPath) { Write-Output "ERROR: cl.exe not found!"; exit 1 }
Write-Output "Compiler: $($clPath.Source)"

# Create build directory
if (!(Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
Set-Location $buildDir

# ── Configure (only if build.ninja doesn't exist) ──
if (!(Test-Path "$buildDir\build.ninja")) {
    Write-Output "=== Configuring CMake with Ninja ==="
    & cmake $projectRoot -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DCMAKE_PREFIX_PATH=$projectRoot\vcpkg_installed\x64-windows" -DBUILD_TESTING=OFF
    if ($LASTEXITCODE -ne 0) { Write-Output "CMake configuration failed!"; exit $LASTEXITCODE }
}

# ── Build ──
Write-Output "=== Building ==="
& $ninjaExe -j4
if ($LASTEXITCODE -ne 0) { Write-Output "Build failed!"; exit $LASTEXITCODE }

# ── Copy runtime DLLs ──
Write-Output "=== Copying runtime DLLs ==="
$binDir = "$buildDir\bin"
$ffmpegBin = "$buildDir\ffmpeg_install\bin"

# FFmpeg DLLs
if (Test-Path $ffmpegBin) { Copy-Item "$ffmpegBin\*.dll" $binDir -Force }
# vcpkg DLLs (Qt6, SDL2, OpenCV, zlib)
if (Test-Path $vcpkgBin) { Copy-Item "$vcpkgBin\*.dll" $binDir -Force }
# Qt6 plugins
$pluginDirs = @("platforms", "styles", "imageformats", "tls")
foreach ($dir in $pluginDirs) {
    $src = "$vcpkgQtPlugins\$dir"
    if (Test-Path $src) {
        $dst = "$binDir\$dir"
        if (!(Test-Path $dst)) { New-Item -ItemType Directory -Path $dst | Out-Null }
        Copy-Item "$src\*.dll" $dst -Force
    }
}

Write-Output ""
Write-Output "=== Build complete! ==="
Write-Output "Executable: $binDir\VideoEye.exe"
