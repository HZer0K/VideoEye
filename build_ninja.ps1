# VideoEye Windows Build Script (PowerShell + Ninja + MSVC)
# Usage: powershell -ExecutionPolicy Bypass -File build_ninja.ps1 [-BuildType Debug|Release]
#
# 自动探测 Visual Studio 2022 / Build Tools (via vswhere)，无需硬编码路径。
# 依赖: vcpkg (vcpkg_installed/), FFmpeg (运行 scripts/fetch-ffmpeg.ps1 获取)

param(
    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = if ($BuildType -eq "Debug") { "$projectRoot\build-ninja-debug" } else { "$projectRoot\build-ninja" }

# ── FFmpeg 版本锁定 ──
# 团队统一使用 8.1.2 (gyan.dev full-shared 预编译包), 确保所有人构建环境一致。
# 8.1.1 已从 gyan.dev 下线, 8.1.2 是 8.1.x 系列当前可用版本 (ABI 兼容)。
$FfmpegVersion = "8.1.2"

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

$vcpkgTriplet = "x64-windows-release"
$vcpkgLib = "$projectRoot\vcpkg_installed\$vcpkgTriplet\lib"

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
if (-not (Test-Path "$projectRoot\vcpkg_installed\$vcpkgTriplet\include")) {
    Write-Output "WARNING: vcpkg 依赖未安装。运行:"
    Write-Output "  vcpkg install --triplet x64-windows-release --host-triplet x64-windows-release --overlay-triplets=scripts/triplets --overlay-ports=scripts/overlay-ports --x-manifest-root=. --x-install-root=vcpkg_installed"
    Write-Output "（release-only triplet，host==target 同名，省约一半磁盘/安装时间）"
}

# -- 检查/获取 FFmpeg --
$ffmpegDir = "$projectRoot\third_party\ffmpeg-prebuilt"
if (-not (Test-Path "$ffmpegDir\include\libavcodec\avcodec.h")) {
    # 回退到 build-ninja/ffmpeg_install (兼容旧布局)
    if (Test-Path "$buildDir\ffmpeg_install\include\libavcodec\avcodec.h") {
        $ffmpegDir = "$buildDir\ffmpeg_install"
    } else {
        Write-Output "FFmpeg 未找到，自动获取中..."
        if ($FfmpegVersion) {
            & powershell -ExecutionPolicy Bypass -File "$projectRoot\scripts\fetch-ffmpeg.ps1" -Version $FfmpegVersion
        } else {
            & powershell -ExecutionPolicy Bypass -File "$projectRoot\scripts\fetch-ffmpeg.ps1"
        }
        if ($LASTEXITCODE -ne 0) { Write-Error "FFmpeg 获取失败"; exit $LASTEXITCODE }
    }
}
$ffmpegArg = "-DFFMPEG_ROOT=$ffmpegDir"

# Create build directory
if (!(Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
Set-Location $buildDir

# -- Configure (总是执行; cmake 增量配置很快, 避免参数/依赖变化后陈旧缓存) --
Write-Output "=== Configuring CMake with Ninja ==="
& cmake $projectRoot -G Ninja `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
    "-DCMAKE_PREFIX_PATH=$projectRoot\vcpkg_installed\$vcpkgTriplet" `
    $ffmpegArg -DBUILD_TESTING=OFF `
    "-DVIDEOEYE_UNITY_BUILD=OFF"   # 显式关闭: MediaInfoLib/ZenLib 与 Unity Build 不兼容
if ($LASTEXITCODE -ne 0) { Write-Output "CMake configuration failed!"; exit $LASTEXITCODE }

# -- Build --
Write-Output "=== Building ==="
$jobs = [int]$env:NUMBER_OF_PROCESSORS
if ($jobs -lt 1) { $jobs = 4 }
& $ninjaExe "-j$jobs"
if ($LASTEXITCODE -ne 0) { Write-Output "Build failed!"; exit $LASTEXITCODE }

# 运行时 DLL / Qt 插件 / shaders 已由 CMake POST_BUILD 步骤 (TARGET_RUNTIME_DLLS +
# FFmpeg bin 拷贝 + windeployqt + spirv_shaders) 自动部署到 bin/, 无需手动复制。

Write-Output ""
Write-Output "=== Build complete! ==="
Write-Output "Executable: $buildDir\bin\VideoEye.exe"
