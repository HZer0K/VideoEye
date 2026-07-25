# fetch-ffmpeg.ps1 - 自动下载并校验 FFmpeg 预编译包 (gyan.dev full shared)
# 用法: powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1
# 可选参数: -DestDir <path>  -Variant <release-full-shared|release-essentials>  -Force
#
# 默认安装到 third_party/ffmpeg-prebuilt（固定位置，与构建目录无关）
# CMake 配置时用 -DFFMPEG_ROOT 指向该目录

param(
    [string]$DestDir = "",
    [string]$Variant = "release-full-shared",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
if (-not $DestDir) { $DestDir = Join-Path $projectRoot "third_party\ffmpeg-prebuilt" }

$url = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-$Variant.7z"
$shaUrl = "$url.sha256"
$archive = Join-Path $env:TEMP "videoeye-ffmpeg-$Variant.7z"

Write-Host "=== VideoEye FFmpeg 预编译包获取 ==="
Write-Host "变体: $Variant"
Write-Host "目标: $DestDir"

# 已存在则跳过（除非 -Force）
$markerDll = Join-Path $DestDir "bin\avcodec-62.dll"
if ((Test-Path $DestDir) -and (Test-Path $markerDll) -and -not $Force) {
    Write-Host "FFmpeg 已存在于 $DestDir，跳过下载（用 -Force 强制重下）"
    Write-Host "CMake 配置: -DFFMPEG_ROOT=`"$DestDir`""
    exit 0
}

# -- 下载 --
Write-Host "=== 下载 FFmpeg ==="
Write-Host "URL: $url"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing

if (-not (Test-Path $archive)) {
    Write-Error "下载失败: $url"
    exit 1
}
$sizeMB = [math]::Round((Get-Item $archive).Length / 1MB, 1)
Write-Host "下载完成: $sizeMB MB"

# -- SHA256 校验 --
Write-Host "=== 校验 SHA256 ==="
$actualHash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
Write-Host "实际 SHA256: $actualHash"

$verified = $false
try {
    Invoke-WebRequest -Uri $shaUrl -OutFile "$archive.sha256" -UseBasicParsing
    $expectedRaw = (Get-Content "$archive.sha256" -Raw).Trim()
    # 兼容 "<hash>  <filename>" 或纯 hash 两种格式
    $expectedHash = ($expectedRaw -split '\s+')[0].ToLower()
    if ($actualHash -eq $expectedHash) {
        Write-Host "SHA256 校验通过"
        $verified = $true
    } else {
        Write-Warning "SHA256 不匹配! 期望=$expectedHash 实际=$actualHash"
        Write-Warning "校验文件格式可能不同，请人工到 gyan.dev 核对后继续"
    }
} catch {
    Write-Warning "无法下载校验文件 ($shaUrl)，已跳过自动校验"
    Write-Warning "请到 https://www.gyan.dev/ffmpeg/builds/ 手动核对上述 SHA256"
}

# -- 定位 7z 解压器 --
$sevenZip = $null
$candidates = @(
    "7z",
    "$env:VCPKG_ROOT\downloads\tools\7zip\19.00\7z.exe",
    "$env:VCPKG_ROOT\downloads\tools\7zip\*\7z.exe",
    "C:\Program Files\7-Zip\7z.exe",
    "C:\Program Files (x86)\7-Zip\7z.exe"
)
foreach ($cand in $candidates) {
    $resolved = Get-Command $cand -ErrorAction SilentlyContinue
    if ($resolved) { $sevenZip = $resolved.Source; break }
    $matches = Get-ChildItem $cand -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($matches) { $sevenZip = $matches.FullName; break }
}
if (-not $sevenZip) {
    Write-Error "未找到 7z！请安装 7-Zip (https://www.7-zip.org/) 或设置 VCPKG_ROOT"
    exit 1
}
Write-Host "解压器: $sevenZip"

# -- 解压 --
Write-Host "=== 解压到 $DestDir ==="
$extractTmp = Join-Path $env:TEMP "videoeye-ffmpeg-extract"
if (Test-Path $extractTmp) { Remove-Item $extractTmp -Recurse -Force }
if (Test-Path $DestDir) { Remove-Item $DestDir -Recurse -Force }

& $sevenZip x $archive "-o$extractTmp" -y | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Error "解压失败 (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

# gyan 包解压后顶层有一个 ffmpeg-xxxx 目录，提取其内容到 DestDir
$topDir = Get-ChildItem $extractTmp -Directory | Select-Object -First 1
if ($topDir -and (Test-Path "$($topDir.FullName)\bin")) {
    Move-Item $topDir.FullName $DestDir
} else {
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
    Copy-Item "$extractTmp\*" $DestDir -Recurse -Force
}

# -- 清理临时文件 --
Remove-Item $archive -Force -ErrorAction SilentlyContinue
Remove-Item "$archive.sha256" -Force -ErrorAction SilentlyContinue
Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue

# -- 验证产物 --
Write-Host ""
Write-Host "=== 完成 ==="
if (Test-Path "$DestDir\include\libavcodec\avcodec.h") {
    Write-Host "FFmpeg 安装成功: $DestDir"
    Write-Host "  Include: $DestDir\include"
    Write-Host "  Lib:     $DestDir\lib"
    Write-Host "  Bin:     $DestDir\bin"
    Write-Host ""
    Write-Host "CMake 配置时传递: -DFFMPEG_ROOT=`"$DestDir`""
    Write-Host "或设置环境变量: set FFMPEG_ROOT=$DestDir"
} else {
    Write-Error "解压产物结构异常，未找到 include/libavcodec/avcodec.h"
    exit 1
}
