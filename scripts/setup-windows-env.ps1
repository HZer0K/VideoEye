<#
.SYNOPSIS
    VideoEye Windows 构建环境一键配置脚本（面向开发机，非本沙箱）。

.DESCRIPTION
    在 Windows 10/11 + 管理员 PowerShell 上运行，自动完成：
      1. 安装 Visual Studio 2022 构建工具（C++ 桌面开发 + Windows SDK）
      2. 安装 CMake (>=3.23) 与 Ninja
      3. Clone + bootstrap vcpkg（设置机器级 VCPKG_ROOT）
      4. 拉取 FFmpeg 预编译包（scripts/fetch-ffmpeg.ps1，版本锁定 8.1.2）
      5. 用 vcpkg 编译并安装 Qt6（qtbase，首次约 30~60 分钟）
      6. 用 vswhere 探测本机 MSVC / Windows SDK / Ninja 路径，生成 CMakeUserPresets.json

    设计原则：
      - 本脚本只负责「装环境 + 写配置」，不重复 CMakePresets.json 里的 -D 参数
        （构建配置的唯一来源仍是 CMakePresets.json / CMakeUserPresets.json）。
      - 幂等：已存在的组件会被跳过。
      - 与项目自带 scripts/run-vcpkg-install.ps1、scripts/fetch-ffmpeg.ps1 复用同一套逻辑。

.PARAMETER VcpkgRoot
    vcpkg 安装根目录，默认 C:\vcpkg。会写入机器级环境变量 VCPKG_ROOT。

.PARAMETER SkipVS
    跳过 VS2022 安装（本机已装好带 C++ 工具集的 VS 时使用）。

.PARAMETER SkipQt
    跳过 vcpkg install qtbase（首次编译 Qt6 很慢，可稍后手动补）。

.PARAMETER WithTests
    一并安装单元测试依赖（gtest，vcpkg "tests" feature）。

.PARAMETER ProxyUrl
    若处于企业代理后（如本沙箱的 127.0.0.1:11783 MITM 代理），传入该地址以让
    git / vcpkg / 下载走代理；同时会自动关闭 git/native TLS 校验以避免自签 CA 报错。
    普通开发机无需传此参数。

.PARAMETER NoPause
    执行结束后不暂停（便于 CI / 自动化调用）。

.EXAMPLE
    # 标准一键安装（需管理员）
    powershell -ExecutionPolicy Bypass -File scripts/setup-windows-env.ps1

.EXAMPLE
    # 已装好 VS、只想补 Qt6 与配置
    powershell -ExecutionPolicy Bypass -File scripts/setup-windows-env.ps1 -SkipVS

.EXAMPLE
    # 处于代理后
    powershell -ExecutionPolicy Bypass -File scripts/setup-windows-env.ps1 -ProxyUrl http://127.0.0.1:11783
#>

param(
    [string]$VcpkgRoot = "C:\vcpkg",
    [switch]$SkipVS,
    [switch]$SkipQt,
    [switch]$WithTests,
    [string]$ProxyUrl = "",
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

function Write-Step($n, $msg) { Write-Host "`n[$n] $msg" -ForegroundColor Cyan }
function Test-Command($name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }

# ──────────────────────────────────────────────
# 0. 环境预检 + 代理处理
# ──────────────────────────────────────────────
Write-Step "0/7" "环境预检"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "未以管理员运行：VS2022 构建工具与机器级环境变量需要管理员权限。建议右键 PowerShell 选『以管理员身份运行』后重试。"
}

if ($ProxyUrl) {
    Write-Host "检测到代理参数，配置 HTTP(S)_PROXY 并关闭 git / 原生 TLS 校验..."
    $env:HTTP_PROXY = $ProxyUrl; $env:HTTPS_PROXY = $ProxyUrl
    $env:http_proxy = $ProxyUrl; $env:https_proxy = $ProxyUrl
    git config --global http.sslVerify false 2>$null
    # 让 .NET (Invoke-WebRequest) 与 curl 接受自签 CA
    [System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
    $env:CURL_CA_BUNDLE = ""   # 清空，配合 -k
    $env:GIT_SSL_NO_VERIFY = "1"
}

# 刷新 PATH（winget 新装的包对当前会话不可见时需要）
function Refresh-Path {
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("Path", "User")
}
Refresh-Path

# ──────────────────────────────────────────────
# 1. Visual Studio 2022 构建工具
# ──────────────────────────────────────────────
if (-not $SkipVS) {
    Write-Step "1/7" "安装 Visual Studio 2022 构建工具 (C++ 桌面 + Windows SDK)"
    if (Test-Command "winget") {
        $vsArgs = @(
            '--add Microsoft.VisualStudio.Workload.VCTools',
            '--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '--add Microsoft.VisualStudio.Component.Windows11SDK.22621',
            '--add Microsoft.VisualStudio.Component.VC.CMake.Project',
            '--quiet --wait --norestart'
        ) -join ' '
        # 用 winget --override 把参数透传给 VS 安装器
        winget install --id Microsoft.VisualStudio.2022.BuildTools --override $vsArgs `
            -e --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) {
            Write-Error "VS2022 构建工具安装失败（winget exit $LASTEXITCODE）。可手动安装『使用 C++ 的桌面开发』工作负载 + Windows 11 SDK 后加 -SkipVS 重试。"
        }
    } else {
        Write-Error "未找到 winget。请手动安装 Visual Studio 2022 构建工具（https://visualstudio.microsoft.com/zh-hans/downloads/），勾选『使用 C++ 的桌面开发』与『Windows 11 SDK』，然后加 -SkipVS 重新运行本脚本。"
    }
} else {
    Write-Step "1/7" "跳过 VS2022 安装（SkipVS）"
}

# ──────────────────────────────────────────────
# 2. CMake + Ninja
# ──────────────────────────────────────────────
Write-Step "2/7" "安装 CMake 与 Ninja"
if (-not (Test-Command "cmake")) {
    if (Test-Command "winget") {
        winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements
    } else { Write-Error "未找到 cmake 且 winget 不可用，请手动安装 CMake >= 3.23。" }
} else { Write-Host "  cmake 已存在: $(cmake --version | Select-Object -First 1)" }
if (-not (Test-Command "ninja")) {
    if (Test-Command "winget") {
        winget install --id Ninja-build.Ninja -e --accept-package-agreements --accept-source-agreements
    } else { Write-Error "未找到 ninja 且 winget 不可用，请手动安装 Ninja。" }
} else { Write-Host "  ninja 已存在: $(ninja --version)" }
Refresh-Path

# ──────────────────────────────────────────────
# 3. vcpkg
# ──────────────────────────────────────────────
Write-Step "3/7" "部署 vcpkg -> $VcpkgRoot"
if (Test-Path "$VcpkgRoot\vcpkg.exe") {
    Write-Host "  vcpkg 已存在，跳过 clone/bootstrap。"
} else {
    if (-not (Test-Path $VcpkgRoot)) { New-Item -ItemType Directory -Path $VcpkgRoot -Force | Out-Null }
    if (-not (Test-Path "$VcpkgRoot\.git")) {
        Write-Host "  git clone vcpkg ..."
        git clone https://github.com/microsoft/vcpkg "$VcpkgRoot" 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { Write-Error "vcpkg clone 失败。" }
    }
    Write-Host "  bootstrap-vcpkg ..."
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
    if ($LASTEXITCODE -ne 0) { Write-Error "vcpkg bootstrap 失败。" }
}
# 写入机器级 VCPKG_ROOT
[System.Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "Machine")
$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "  已设置 VCPKG_ROOT=$VcpkgRoot (Machine)"

# ──────────────────────────────────────────────
# 4. FFmpeg 预编译包
# ──────────────────────────────────────────────
Write-Step "4/7" "获取 FFmpeg 预编译包 (锁定 8.1.2)"
$ffDest = Join-Path $ProjectRoot "third_party\prebuilt\windows-x64\ffmpeg"
if ((Test-Path "$ffDest\include\libavcodec\avcodec.h") -and (Test-Path "$ffDest\lib\avcodec.lib")) {
    Write-Host "  FFmpeg 已存在，跳过。"
} else {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $ScriptDir "fetch-ffmpeg.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Error "FFmpeg 获取失败，请检查网络或手动放置到 $ffDest。" }
}

# ──────────────────────────────────────────────
# 5. Qt6 (vcpkg)
# ──────────────────────────────────────────────
if (-not $SkipQt) {
    Write-Step "5/7" "vcpkg 安装 Qt6 (qtbase) — 首次约 30~60 分钟"
    # run-vcpkg-install.ps1 的 -Feature 只接受 feature 名（tests），不含 Config。
    # 注意：PowerShell 的 1..0 这种「start>end」区间会退化成 @(1,0)，
    # 把第 0 个元素也带进来，所以这里显式构造 feature 数组，避免误传 'release'。
    $featureArgs = if ($WithTests) { @("tests") } else { @() }
    & powershell -ExecutionPolicy Bypass -File (Join-Path $ScriptDir "run-vcpkg-install.ps1") `
        -Config release -Feature $featureArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "vcpkg install 失败（exit $LASTEXITCODE）。可单独排查 scripts/run-vcpkg-install.ps1。" }
} else {
    Write-Step "5/7" "跳过 Qt6 安装（SkipQt）。稍后可运行: scripts/run-vcpkg-install.ps1 -Config release"
}

# ──────────────────────────────────────────────
# 6. 生成 CMakeUserPresets.json（探测本机工具链路径）
# ──────────────────────────────────────────────
Write-Step "6/7" "用 vswhere 探测路径并生成 CMakeUserPresets.json"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" }
if (-not (Test-Path $vswhere)) { Write-Error "找不到 vswhere.exe，无法生成 CMakeUserPresets.json 的 MSVC 路径。" }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { Write-Error "vswhere 未找到带 C++ 工具集的 VS。" }

$msvcRoot = (Get-ChildItem "$vsPath\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name | Select-Object -Last 1).FullName
if (-not $msvcRoot) { Write-Error "未找到 MSVC 工具集目录 ($vsPath\VC\Tools\MSVC)。" }

$sdkBase = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVer = (Get-ChildItem "$sdkBase\Include" -Directory -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
           Sort-Object Name | Select-Object -Last 1).Name
if (-not $sdkVer) { Write-Error "未找到 Windows SDK ($sdkBase\Include)。" }

$msvcBin   = "$msvcRoot\bin\Hostx64\x64"
$msvcLib   = "$msvcRoot\lib\x64"
$msvcInc   = "$msvcRoot\include"
$sdkBin    = "$sdkBase\bin\$sdkVer\x64"
$sdkIncUm  = "$sdkBase\Include\$sdkVer\um"
$sdkIncSh  = "$sdkBase\Include\$sdkVer\shared"
$sdkIncWr  = "$sdkBase\Include\$sdkVer\winrt"
$sdkIncCw  = "$sdkBase\Include\$sdkVer\cppwinrt"
$sdkLibUm  = "$sdkBase\Lib\$sdkVer\um\x64"
$sdkLibUc  = "$sdkBase\Lib\$sdkVer\ucrt\x64"

$ninjaPath = (Get-Command ninja -ErrorAction SilentlyContinue).Source
$vcpkgBin  = "$VcpkgRoot"

# 合并进 PATH（保留系统 PATH，前置工具链目录）
# 末尾必须是字面量 $penv{PATH}（= 本 preset 生效前的父环境 PATH），让 CMake 在 configure 时展开。
# ⚠️ 不能用 $env{PATH}：在同一个 preset 的 environment 里，$env{} 指的是“本 preset 生效后”的环境，
#    而 PATH 恰在本 preset 里被设置 → 自引用/循环，CMake 会直接报
#    `Invalid preset / Invalid macro expansion`，导致整个 CMakeUserPresets.json 失效。
$extraPath = @($msvcBin, $sdkBin, $vcpkgBin, ($ninjaPath | Split-Path -Parent)) -join ';'
$newPath   = "$extraPath;" + '$penv{PATH}'

$incList = @($msvcInc, $sdkIncUm, $sdkIncSh, $sdkIncWr, $sdkIncCw) -join ';'
$libList = @($msvcLib, $sdkLibUm, $sdkLibUc) -join ';'

$presetJson = @"
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 23, "patch": 0 },
  "vendor": {
    "videoeye": {
      "note": "本文件由 scripts/setup-windows-env.ps1 自动生成（gitignore）。重新运行该脚本可刷新路径。不要手工改这里的绝对路径——改了就和 vswhere 探测脱节。",
      "generated": "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
      "why-vcpkg-keep-env-vars": "vcpkg 为每个 port 构建派生的是白名单『干净环境』，会丢弃继承的 PATH，之后再靠 vcvarsall.bat 回填工具链环境。若 vcvarsall 拿不到 Windows SDK（例如 vcvarsqueryregistry.bat 的 reg.exe 被安全策略拦截），CMake 会得到 CMAKE_RC_COMPILER=rc / CMAKE_MT-NOTFOUND，报 'rc ... no such file or directory'。把 PATH;INCLUDE;LIB 交给 VCPKG_KEEP_ENV_VARS 可让 port 构建直接复用本 preset 已验证的 PATH/INCLUDE/LIB。",
      "path-macro": "追加系统 PATH 必须用 `$penv{PATH}（父环境）；写成 `$env{PATH} 会在同一 preset 内自引用，CMake 报 Invalid macro expansion 并使整个文件失效。"
    }
  },
  "configurePresets": [
    {
      "name": "win-release-local",
      "inherits": "win-release",
      "environment": {
        "VCPKG_ROOT": "$VcpkgRoot",
        "VCPKG_KEEP_ENV_VARS": "PATH;INCLUDE;LIB",
        "PATH": "$newPath",
        "INCLUDE": "$incList",
        "LIB": "$libList",
        "FFMPEG_ROOT": "`${sourceDir}/third_party/prebuilt/windows-x64/ffmpeg"
      },
      "cacheVariables": {
        "CMAKE_C_COMPILER": "cl.exe",
        "CMAKE_CXX_COMPILER": "cl.exe",
        "CMAKE_MAKE_PROGRAM": "ninja.exe"
      }
    },
    {
      "name": "win-debug-local",
      "inherits": "win-debug",
      "environment": {
        "VCPKG_ROOT": "$VcpkgRoot",
        "VCPKG_KEEP_ENV_VARS": "PATH;INCLUDE;LIB",
        "PATH": "$newPath",
        "INCLUDE": "$incList",
        "LIB": "$libList",
        "FFMPEG_ROOT": "`${sourceDir}/third_party/prebuilt/windows-x64/ffmpeg"
      },
      "cacheVariables": {
        "CMAKE_C_COMPILER": "cl.exe",
        "CMAKE_CXX_COMPILER": "cl.exe",
        "CMAKE_MAKE_PROGRAM": "ninja.exe"
      }
    },
    {
      "name": "win-test-release-local",
      "inherits": "win-test-release",
      "environment": {
        "VCPKG_ROOT": "$VcpkgRoot",
        "VCPKG_KEEP_ENV_VARS": "PATH;INCLUDE;LIB",
        "PATH": "$newPath",
        "INCLUDE": "$incList",
        "LIB": "$libList",
        "FFMPEG_ROOT": "`${sourceDir}/third_party/prebuilt/windows-x64/ffmpeg"
      },
      "cacheVariables": {
        "CMAKE_C_COMPILER": "cl.exe",
        "CMAKE_CXX_COMPILER": "cl.exe",
        "CMAKE_MAKE_PROGRAM": "ninja.exe"
      }
    },
    {
      "name": "win-test-debug-local",
      "inherits": "win-test-debug",
      "environment": {
        "VCPKG_ROOT": "$VcpkgRoot",
        "VCPKG_KEEP_ENV_VARS": "PATH;INCLUDE;LIB",
        "PATH": "$newPath",
        "INCLUDE": "$incList",
        "LIB": "$libList",
        "FFMPEG_ROOT": "`${sourceDir}/third_party/prebuilt/windows-x64/ffmpeg"
      },
      "cacheVariables": {
        "CMAKE_C_COMPILER": "cl.exe",
        "CMAKE_CXX_COMPILER": "cl.exe",
        "CMAKE_MAKE_PROGRAM": "ninja.exe"
      }
    }
  ]
}
"@

$presetPath = Join-Path $ProjectRoot "CMakeUserPresets.json"
$presetJson | Set-Content -Path $presetPath -Encoding UTF8
Write-Host "  已生成 $presetPath"
Write-Host "    MSVC : $msvcRoot"
Write-Host "    SDK  : $sdkVer"
Write-Host "    Ninja: $ninjaPath"

# ──────────────────────────────────────────────
# 7. 完成 + 下一步
# ──────────────────────────────────────────────
Write-Step "7/7" "完成"
Write-Host @"

============================================================
 VideoEye 构建环境已配置完成
============================================================
下一步（在『x64 Native Tools Command Prompt for VS 2022』或普通
PowerShell 均可，因为 CMakeUserPresets.json 已钉死工具链路径）：

  构建 Release:
      cmake --preset win-release-local
      cmake --build --preset win-release-local --parallel %NUMBER_OF_PROCESSORS%

  构建 + 跑单元测试:
      cmake --preset win-test-release-local
      cmake --build --preset win-test-release-local
      ctest --preset win-test-release-local

  或直接用项目脚本:
      build.bat release        (需 VCPKG_ROOT 已设置，已设)
      build.bat test           (Release + 单元测试)

运行:
      build\release\bin\VideoEye.exe

说明:
  - 本机专有路径已写入 gitignore 的 CMakeUserPresets.json，不会进仓库。
  - 若重装/升级了 VS 或 Windows SDK，重新运行本脚本即可刷新。
  - 当前沙箱环境的 linker 可能被拦截、无法跑完整 ctest；
    以上构建请在你的真机（普通 Windows + VS2022）上执行。
============================================================
"@

if (-not $NoPause) {
    Write-Host "`n按任意键继续..." -NoNewline
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
}
