param(
    [ValidateSet("release", "debug")]
    [string]$Config = "release",
    # 单元测试需要的额外 feature (vcpkg.json 里的 "tests" -> gtest)
    [string[]]$Feature = @(),
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$TargetTriplet = "x64-windows-$Config"
# host triplet 恒定为 release: host 工具是在构建机上跑的（vcpkg 自身的工具链、
# 代码生成器等），跟被测程序用什么配置无关。以前跟着 $Config 变，Debug 时会
# 把 host 也切成 debug —— 于是还得再编一份 debug 的 host 工具，纯属浪费；
# 更糟的是 target/host 混用还可能让 manifest install 走交叉编译分支。
$HostTriplet = "x64-windows-release"

$logFile = Join-Path $ProjectRoot "vcpkg-install.log"

# 干掉注入的死代理，否则 vcpkg 下载会卡住
foreach ($v in @("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy")) {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}

# 通过 vcvars64.bat 加载完整 MSVC 环境（与 build_ninja.ps1 同一套做法）
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) { Write-Error "未找到 vswhere.exe"; exit 1 }

$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) { Write-Error "vswhere: 没找到带 C++ 工具链的 VS"; exit 1 }

$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { Write-Error "vcvars64.bat not found: $vcvars"; exit 1 }

# cmd 的 set 输出里同一个变量可能有多个大小写变体且值不同（Path/PATH/path），
# 按顺序全部 Set-Item 会被最后一条覆盖掉 MSVC 路径 —— 按大小写不敏感去重后只取第一个。
$envSeen = @{}
cmd /c "`"$vcvars`" x64 && set" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        $name = $matches[1]
        if ($envSeen.ContainsKey($name.ToLowerInvariant())) { return }
        $envSeen[$name.ToLowerInvariant()] = $true
        Set-Item -Path "env:$name" -Value $matches[2].TrimEnd()
    }
}

$msbuild = Get-Command msbuild -ErrorAction SilentlyContinue
Write-Output "MSVC 环境已加载. msbuild: $(if ($msbuild) { $msbuild.Source } else { 'NOT FOUND' })"

if (-not $env:VCPKG_ROOT) { Write-Error "VCPKG_ROOT is not set"; exit 1 }

Set-Location $ProjectRoot
Write-Output "Running vcpkg install..."
Write-Output "  target triplet: $TargetTriplet"
Write-Output "  host triplet:   $HostTriplet (恒定 release)"
if ($Feature.Count -gt 0) { Write-Output "  features:       $($Feature -join ', ')" }

$vcpkgArgs = @(
    "install",
    "--triplet", $TargetTriplet,
    "--host-triplet", $HostTriplet,
    "--overlay-triplets=scripts/triplets",
    "--overlay-ports=scripts/overlay-ports",
    "--x-manifest-root=.",
    "--x-install-root=vcpkg_installed"
)
foreach ($f in $Feature) { $vcpkgArgs += "--x-feature=$f" }

& "$env:VCPKG_ROOT\vcpkg.exe" @vcpkgArgs 2>&1 | Tee-Object -FilePath $logFile

Write-Output "vcpkg exit code: $LASTEXITCODE"
exit $LASTEXITCODE
