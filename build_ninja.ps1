# ============================================================
# build_ninja.ps1 — 历史入口，现在只是 build.bat 的转发器
#
# 真正的构建配置只有一份: CMakePresets.json (Windows 上由 CMakeUserPresets.json
# 覆盖本机路径)。build.bat 负责准备 MSVC 环境 + FFmpeg，然后调用 cmake --preset。
#
# 以前这个文件自己拼 -DCMAKE_PREFIX_PATH / -DFFMPEG_ROOT / triplet，和 preset 是
# 两套独立逻辑 —— 结果就是 Debug 链接 Release 的 Qt、vcpkg triplet 不一致、
# 输出目录还多出一个 build-ninja/。现在统一走 build.bat。
#
# 产物路径也变了:
#   旧: build-ninja\bin\VideoEye.exe / build-ninja-debug\bin\VideoEye.exe
#   新: build\release\bin\VideoEye.exe / build\debug\bin\VideoEye.exe
# ============================================================
param(
    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$bat = Join-Path $projectRoot "build.bat"

if (-not (Test-Path $bat)) {
    Write-Error "未找到 build.bat: $bat"
    exit 1
}

Write-Host "==> build_ninja.ps1 已改为 build.bat 的薄封装 (构建配置源: CMakePresets.json)"
Write-Host "    -BuildType $BuildType  ->  build.bat $($BuildType.ToLower())"
Write-Host ""

& cmd /c "`"$bat`" $($BuildType.ToLower())"
$code = $LASTEXITCODE

Write-Host ""
Write-Host "产物: $projectRoot\build\$($BuildType.ToLower())\bin\VideoEye.exe"
exit $code
