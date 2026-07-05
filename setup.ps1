# VideoEye 开发环境初始化脚本 (Windows PowerShell)
# 用法: .\setup.ps1 [-SkipDeps] [-BuildOnly] [-Debug]
param(
    [switch]$SkipDeps,
    [switch]$BuildOnly,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"
$DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $DIR

$BUILD_TYPE = if ($Debug) { "Debug" } else { "Release" }

Write-Host "========================================" -ForegroundColor Green
Write-Host "  VideoEye 环境初始化 (Windows)" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green

if (-not $BuildOnly) {
    # 1. Git 子模块
    Write-Host "`n[1/4] Git 子模块..." -ForegroundColor Yellow
    git submodule update --init --recursive
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  完成" -ForegroundColor Green
    } else {
        Write-Host "  失败" -ForegroundColor Red
    }

    # 2. FFmpeg 源码
    Write-Host "`n[2/4] FFmpeg 源码 (n8.1)..." -ForegroundColor Yellow
    if (Test-Path "$DIR/third_party/FFmpeg/configure") {
        Write-Host "  已存在" -ForegroundColor Green
    } else {
        Write-Host "  下载中 (~40MB)..."
        git clone --depth 1 --branch n8.1 https://github.com/FFmpeg/FFmpeg.git "$DIR/third_party/FFmpeg"
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  完成" -ForegroundColor Green
        } else {
            git clone --depth 1 --branch n8.1 https://git.ffmpeg.org/ffmpeg.git "$DIR/third_party/FFmpeg"
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  失败! 请手动克隆" -ForegroundColor Red
            }
        }
    }

    # 3. 系统依赖
    if (-not $SkipDeps) {
        Write-Host "`n[3/4] 系统依赖..." -ForegroundColor Yellow

        $cmds = @("cmake", "gcc", "g++")
        foreach ($cmd in $cmds) {
            if (Get-Command $cmd -ErrorAction SilentlyContinue) {
                Write-Host "  [OK] $cmd" -ForegroundColor Green
            } else {
                Write-Host "  [缺] $cmd" -ForegroundColor Red
            }
        }

        # MSBuild / Visual Studio
        if (Get-Command msbuild -ErrorAction SilentlyContinue) {
            Write-Host "  [OK] msbuild" -ForegroundColor Green
        } elseif (Get-Command ninja -ErrorAction SilentlyContinue) {
            Write-Host "  [OK] ninja" -ForegroundColor Green
        } else {
            Write-Host "  [缺] 构建工具 (Visual Studio 或 Ninja)" -ForegroundColor Yellow
        }

        # Vulkan SDK
        $vkSdk = $env:VULKAN_SDK
        if ($vkSdk) {
            Write-Host "  [OK] Vulkan SDK ($vkSdk)" -ForegroundColor Green
        } else {
            Write-Host "  [可选] Vulkan SDK" -ForegroundColor Yellow
            Write-Host "    下载: https://vulkan.lunarg.com/" -ForegroundColor Yellow
        }

        # glslc
        if (Get-Command glslc -ErrorAction SilentlyContinue) {
            Write-Host "  [OK] glslc" -ForegroundColor Green
        } else {
            Write-Host "  [可选] glslc (Vulkan SDK 自带)" -ForegroundColor Yellow
        }

        # vcpkg
        if (Get-Command vcpkg -ErrorAction SilentlyContinue) {
            Write-Host "  [OK] vcpkg" -ForegroundColor Green
        } else {
            Write-Host "  [建议] vcpkg - 推荐用于管理 Qt/OpenCV/SDL 依赖" -ForegroundColor Yellow
            Write-Host "    安装: git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg" -ForegroundColor Yellow
            Write-Host "    然后: C:\vcpkg\bootstrap-vcpkg.bat" -ForegroundColor Yellow
        }

        Write-Host ""
        Write-Host "  依赖安装建议 (使用 vcpkg):" -ForegroundColor Yellow
        Write-Host "    vcpkg install opencv4 sdl2 qt6-base qt6-multimedia qt6-charts" -ForegroundColor White
        Write-Host "  或手动安装 Qt6: https://www.qt.io/download" -ForegroundColor White
    }
}

# 4. 编译
Write-Host "`n[4/4] 编译 ($BUILD_TYPE)..." -ForegroundColor Yellow
$BUILD_DIR = "$DIR/build"
New-Item -ItemType Directory -Force -Path $BUILD_DIR | Out-Null
Set-Location $BUILD_DIR

# CMake 配置
$cmakeArgs = @("..", "-DCMAKE_BUILD_TYPE=$BUILD_TYPE")

# 如果配置了 vcpkg toolchain
$vcpkgRoot = $null
if (Test-Path env:VCPKG_ROOT) {
    $vcpkgRoot = $env:VCPKG_ROOT
} elseif (Test-Path "C:\vcpkg") {
    $vcpkgRoot = "C:\vcpkg"
}

if ($vcpkgRoot) {
    $toolchain = "$vcpkgRoot\scripts\buildsystems\vcpkg.cmake"
    if (Test-Path $toolchain) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
        Write-Host "  使用 vcpkg: $vcpkgRoot"
    }
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

# 编译
$jobs = [Environment]::ProcessorCount
cmake --build . -j $jobs --config $BUILD_TYPE

Write-Host "`n========================================" -ForegroundColor Green
Write-Host "  初始化完成！" -ForegroundColor Green
Write-Host "  运行: $BUILD_DIR\bin\$BUILD_TYPE\VideoEye.exe" -ForegroundColor Green
Write-Host "  测试: cd $BUILD_DIR; ctest -C $BUILD_TYPE" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
