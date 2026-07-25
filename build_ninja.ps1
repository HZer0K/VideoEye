# VideoEye Windows Build Script (PowerShell + Ninja + MSVC)
# Usage: powershell -ExecutionPolicy Bypass -File build_ninja.ps1
#
# 自动探测 Visual Studio 2022 / Build Tools (via vswhere)，无需硬编码路径。
# 依赖: vcpkg (vcpkg_installed/), FFmpeg (运行 scripts/fetch-ffmpeg.ps1 获取)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = "$projectRoot\build-ninja"

# -- 用 vswhere 自动探测 Visual Studio 安装路径 --
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) {
    Write-Error "未找到 vswhere！请安装 Visual Studio 2022 或 Build Tools。"
    exit 1
}
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) {
    Write-Error "vswhere 未找到带 C++ 工具链的 VS 安装。请在 VS Installer 中勾选 '使用 C++ 的桌面开发' 工作负载。"
    exit 1
}
Write-Output "VS 安装路径: $vsRoot"

# -- 通过 vcvars64.bat 导入完整 MSVC 环境 (PATH/INCLUDE/LIB) --
$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "未找到 vcvars64.bat: $vcvars"
    exit 1
}
Write-Output "导入 MSVC 环境..."
cmd /c "`"$vcvars`" x64 && set" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2].TrimEnd()
    }
}

# -- 定位 ninja --
$ninjaExe = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $ninjaExe)) {
    $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninjaCmd) { $ninjaExe = $ninjaCmd.Source }
    else {
        Write-Error "未找到 ninja.exe！请在 VS Installer 中勾选 'C++ CMake tools for Windows'，或将 ninja 加入 PATH。"
        exit 1
    }
}

$vcpkgBin = "$projectRoot\vcpkg_installed\x64-windows\bin"
$vcpkgLib = "$projectRoot\vcpkg_installed\x64-windows\lib"
$vcpkgQtPlugins = "$projectRoot\vcpkg_installed\x64-windows\Qt6\plugins"

# vcpkg lib 必须加入 LIB，否则链接器找不到 SDL2.lib 等裸名引用
if (Test-Path $vcpkgLib) {
    $env:LIB = "$vcpkgLib;$env:LIB"
}

Write-Output "=== VideoEye Ninja Build (Windows) ==="
Write-Output "Project: $projectRoot"
$clPath = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($clPath) { Write-Output "Compiler: $($clPath.Source)" } else { Write-Error "cl.exe not found!"; exit 1 }
Write-Output "Ninja: $ninjaExe"

# -- 检查 vcpkg 依赖 --
if (-not (Test-Path "$projectRoot\vcpkg_installed\x64-windows\include")) {
    Write-Output "WARNING: vcpkg 依赖未安装。运行: vcpkg install --triplet x64-windows"
}

# -- 检查/获取 FFmpeg --
$ffmpegDir = "$projectRoot\third_party\ffmpeg-prebuilt"
if (-not (Test-Path "$ffmpegDir\include\libavcodec\avcodec.h")) {
    # 回退到 build-ninja/ffmpeg_install (兼容旧布局)
    if (Test-Path "$buildDir\ffmpeg_install\include\libavcodec\avcodec.h") {
        $ffmpegDir = "$buildDir\ffmpeg_install"
    } else {
        Write-Output "FFmpeg 未找到，自动获取中..."
        & powershell -ExecutionPolicy Bypass -File "$projectRoot\scripts\fetch-ffmpeg.ps1"
        if ($LASTEXITCODE -ne 0) { Write-Error "FFmpeg 获取失败"; exit $LASTEXITCODE }
    }
}
$ffmpegArg = "-DFFMPEG_ROOT=$ffmpegDir"

# Create build directory
if (!(Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
Set-Location $buildDir

# -- Configure (only if build.ninja doesn't exist) --
if (!(Test-Path "$buildDir\build.ninja")) {
    Write-Output "=== Configuring CMake with Ninja ==="
    & cmake $projectRoot -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
        "-DCMAKE_PREFIX_PATH=$projectRoot\vcpkg_installed\x64-windows" `
        $ffmpegArg -DBUILD_TESTING=OFF
    if ($LASTEXITCODE -ne 0) { Write-Output "CMake configuration failed!"; exit $LASTEXITCODE }
}

# -- Build --
Write-Output "=== Building ==="
$jobs = [int]$env:NUMBER_OF_PROCESSORS
if ($jobs -lt 1) { $jobs = 4 }
& $ninjaExe "-j$jobs"
if ($LASTEXITCODE -ne 0) { Write-Output "Build failed!"; exit $LASTEXITCODE }

# -- Copy runtime DLLs --
Write-Output "=== Copying runtime DLLs ==="
$binDir = "$buildDir\bin"

# FFmpeg DLLs
$ffmpegBin = "$ffmpegDir\bin"
if (Test-Path $ffmpegBin) { Copy-Item "$ffmpegBin\*.dll" $binDir -Force }
# vcpkg DLLs (Qt6, SDL2, OpenCV, zlib, icu, harfbuzz, etc. - copy all)
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

# Copy SPIR-V shaders if they exist
$shaderDir = "$buildDir\shaders"
if (Test-Path $shaderDir) {
    $dstShader = "$binDir\shaders"
    if (!(Test-Path $dstShader)) { New-Item -ItemType Directory -Path $dstShader | Out-Null }
    Copy-Item "$shaderDir\*.spv" $dstShader -Force
}

Write-Output ""
Write-Output "=== Build complete! ==="
Write-Output "Executable: $binDir\VideoEye.exe"
