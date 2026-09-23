# fetch-ffmpeg.ps1 - 下载并强校验 FFmpeg 预编译包 (gyan.dev full shared)
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -Version 8.1.2
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -DestDir <path>
#
# 行为约定:
#   * 版本/URL/SHA256 的唯一来源是 cmake/ffmpeg-version.json —— 所有入口必须一致,
#     否则不同开发者会构建出 ABI 不同的产物。
#   * SHA256 是硬性要求: 拿不到校验值或校验不过一律失败退出, 不留"警告继续"。
#     只有在人工核对过镜像的场景才用 -SkipChecksum 显式关闭。
#   * 先解压到临时目录并验证结构完整, 再替换已有目录 —— 中途失败不会把已经有
#     的、可用的依赖弄丢。
#
# 为什么固定用 release-full-shared 系列: gyan.dev 唯一带 include/ 头文件与 lib/
# 导入库的共享构建。essentials 变体只有 bin/, 没法拿来链接。

param(
    [string]$DestDir = "",
    [string]$Version = "",
    [switch]$Force,
    [switch]$SkipChecksum
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
if (-not $DestDir) { $DestDir = Join-Path $projectRoot "third_party\prebuilt\windows-x64\ffmpeg" }

# ── 版本: 唯一来源 cmake/ffmpeg-version.json ──
$versionFile = Join-Path $projectRoot "cmake\ffmpeg-version.json"
if (-not (Test-Path $versionFile)) {
    Write-Error "缺少版本配置文件: $versionFile（本脚本不允许再使用 latest 滚动版本）"
    exit 1
}
$cfg = Get-Content $versionFile -Raw | ConvertFrom-Json
$lockedVersion = $cfg.version
if ([string]::IsNullOrWhiteSpace($lockedVersion)) {
    Write-Error "$versionFile 里没有 version 字段"
    exit 1
}
if ([string]::IsNullOrWhiteSpace($Version)) { $Version = $lockedVersion }

$expectedSha = ""
if ($Version -eq $lockedVersion -and $cfg.windows.url -and $cfg.windows.sha256) {
    # 锁定版本的完整 Shopify 路径: URL + SHA256 都在仓库里, 完全离线可验证
    $url = $cfg.windows.url
    $expectedSha = $cfg.windows.sha256
    Write-Host "版本锁定: FFmpeg $Version (cmake/ffmpeg-version.json)"
} else {
    if (-not $cfg.windows.urlTemplate) {
        Write-Error "$versionFile 缺少 windows.urlTemplate, 无法拼出 $Version 的下载地址"
        exit 1
    }
    # gyan.dev 版本化 URL 路径多一层 /packages/, 文件名是 full_build-shared
    $url = ($cfg.windows.urlTemplate -replace '\{version\}', $Version)
    Write-Host "版本: FFmpeg $Version (未锁定校验值, 将从 $url.sha256 在线获取)"
    if ($Version -ne $lockedVersion) {
        Write-Warning "注意: 项目锁定的版本是 $lockedVersion, 你正在拉 $Version, 产物 ABI 可能与其他人不一致。"
    }
}
$archive = Join-Path $env:TEMP "videoeye-ffmpeg-$Version.7z"
$variant = "$Version-full_build-shared"

Write-Host "=== VideoEye FFmpeg 预编译包获取 ==="
Write-Host "版本:   $Version"
Write-Host "地址:   $url"
Write-Host "目标:   $DestDir"

# ── 已存在则跳过（以 stamp 为准, 与具体 DLL 文件名解耦）──
$stampFile = Join-Path $DestDir ".videoeye-ffmpeg.json"
if ((Test-Path $DestDir) -and (Test-Path $stampFile) -and -not $Force) {
    $stamp = Get-Content $stampFile -Raw | ConvertFrom-Json
    if ($stamp.Variant -eq $variant) {
        Write-Host "已存在且版本一致 (variant=$variant, 获取于 $($stamp.FetchedAt))，跳过下载。用 -Force 强制重下。"
        exit 0
    } else {
        Write-Host "检测到版本变更 ($($stamp.Variant) -> $variant)，重新获取..."
    }
} elseif ((Test-Path $DestDir) -and -not $Force) {
    Write-Host "检测到旧版 FFmpeg（无版本 stamp），重新获取以保证版本可追踪..."
}

# ── 下载 ──
Write-Host ""
Write-Host "=== 下载 ==="
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
$downloaded = $false
try {
    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -TimeoutSec 600
    $downloaded = $true
} catch {
    Write-Warning "Invoke-WebRequest 失败: $($_.Exception.Message)"
    Write-Warning "回退 curl.exe..."
}
if (-not $downloaded) {
    & curl.exe -L --fail --retry 5 --retry-all-errors --retry-delay 3 -o $archive $url
    if ($LASTEXITCODE -ne 0) {
        Write-Error "下载失败 (curl exit $LASTEXITCODE): $url"
        exit 1
    }
}
if (-not (Test-Path $archive)) {
    Write-Error "下载失败: $url"
    exit 1
}
$sizeMB = [math]::Round((Get-Item $archive).Length / 1MB, 1)
Write-Host "下载完成: $sizeMB MB"

# ── SHA256 校验 (硬性) ──
Write-Host ""
Write-Host "=== 校验 SHA256 ==="
$actualHash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
Write-Host "实际 SHA256: $actualHash"

if ($SkipChecksum) {
    Write-Warning "已通过 -SkipChecksum 跳过校验 —— 请确保 you trust 该下载源。"
} else {
    if (-not $expectedSha) {
        # 版本没被仓库锁住时, 只能在线取 sidecar; 取不到就失败, 不能"算了继续"
        try {
            Invoke-WebRequest -Uri "$url.sha256" -OutFile "$archive.sha256" -UseBasicParsing -TimeoutSec 60
            $expectedSha = (($((Get-Content "$archive.sha256" -Raw).Trim()) -split '\s+')[0]).ToLower()
        } catch {
            Remove-Item $archive -Force -ErrorAction SilentlyContinue
            Write-Error "无法获取校验文件 $url.sha256 ($($_.Exception.Message))。"
            Write-Error "构建依赖不允许在校验缺失的情况下继续。请把该版本的 sha256 写进 cmake/ffmpeg-version.json 后重试。"
            exit 1
        }
    }
    Write-Host "期望 SHA256: $($expectedSha.ToLower())"
    if ($actualHash -ne $expectedSha.ToLower()) {
        Remove-Item $archive -Force -ErrorAction SilentlyContinue
        Write-Error "SHA256 校验失败！期望=$($expectedSha.ToLower()) 实际=$actualHash"
        Write-Error "归档可能已损坏或被替换, 已中止。请核对 cmake/ffmpeg-version.json 里的 sha256 后再试。"
        exit 1
    }
    Write-Host "SHA256 校验通过"
}

# ── 解压到临时目录 ──
Write-Host ""
Write-Host "=== 解压 ==="
$extractTmp = Join-Path $env:TEMP "videoeye-ffmpeg-extract-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $extractTmp -Force | Out-Null

$tarOk = $false
& tar -xf $archive -C $extractTmp 2>$null
if ($LASTEXITCODE -eq 0) { $tarOk = $true }

if (-not $tarOk) {
    Write-Host "bsdtar 解压失败，回退 7-Zip..."
    $sevenZip = $null
    $candidates = @(
        "7z",
        "$env:VCPKG_ROOT\downloads\tools\7zip\19.00\7z.exe",
        "C:\Program Files\7-Zip\7z.exe",
        "C:\Program Files (x86)\7-Zip\7z.exe"
    )
    foreach ($cand in $candidates) {
        $resolved = Get-Command $cand -ErrorAction SilentlyContinue
        if ($resolved) { $sevenZip = $resolved.Source; break }
        $hit = Get-ChildItem $cand -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $sevenZip = $hit.FullName; break }
    }
    if (-not $sevenZip) {
        Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue
        Write-Error "解压失败且未找到 7-Zip。请安装 7-Zip (https://www.7-zip.org/) 或确保 tar 可用。"
        exit 1
    }
    Write-Host "解压器: $sevenZip"
    & $sevenZip x $archive "-o$extractTmp" -y | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue
        Write-Error "解压失败 (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

# gyan 包解压后顶层是一个 ffmpeg-8.1.2-full_build-www.gyan.dev 目录
function Test-FfmpegLayout([string]$dir) {
    return (Test-Path (Join-Path $dir "include\libavcodec\avcodec.h")) -and
           (Test-Path (Join-Path $dir "lib\avcodec.lib")) -and
           ((Get-ChildItem (Join-Path $dir "bin") -Filter "avcodec-*.dll" -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0)
}

$payload = $null
if (Test-FfmpegLayout $extractTmp) {
    $payload = $extractTmp
} else {
    foreach ($sub in (Get-ChildItem $extractTmp -Directory)) {
        if (Test-FfmpegLayout $sub.FullName) { $payload = $sub.FullName; break }
    }
}
if (-not $payload) {
    Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue
    Write-Error "解压产物结构异常: 未找到完整的 {include,lib,bin}。请确认下载的是 full_build-shared 而非 essentials。"
    exit 1
}
Write-Host "结构校验通过: include/ + lib/ + bin/ 齐全"

# ── 原子替换: 先备份旧目录, 成功后再删除 ──
# 顺序很关键 —— 直接在解压前 Remove-Item 目标目录, 一旦下载/解压中途失败,
# 原本能用的依赖也没了, 只剩一个"需要联网重下"的坑。
$backup = ""
if (Test-Path $DestDir) {
    $backup = "$DestDir.old"
    if (Test-Path $backup) { Remove-Item $backup -Recurse -Force }
    Move-Item $DestDir $backup
}

try {
    Write-Host ""
    Write-Host "=== 安装到 $DestDir ==="
    Copy-Item $payload $DestDir -Recurse -Force
    if (-not (Test-FfmpegLayout $DestDir)) {
        throw "安装后的目录不完整: $DestDir"
    }
} catch {
    Write-Error "安装失败: $($_.Exception.Message)"
    if ($backup -and (Test-Path $backup)) {
        Write-Warning "正在恢复原有 FFmpeg: $DestDir"
        if (Test-Path $DestDir) { Remove-Item $DestDir -Recurse -Force }
        Move-Item $backup $DestDir
    }
    Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

if ($backup -and (Test-Path $backup)) { Remove-Item $backup -Recurse -Force }
Remove-Item $extractTmp -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $archive -Force -ErrorAction SilentlyContinue
Remove-Item "$archive.sha256" -Force -ErrorAction SilentlyContinue

# ── 记录版本 stamp (含 sha256, 便于追溯) ──
Write-Host ""
Write-Host "=== 完成 ==="
$ffVersion = ""
$ffExe = Join-Path $DestDir "bin\ffmpeg.exe"
if (Test-Path $ffExe) {
    $verOutput = & $ffExe -version 2>&1 | Select-Object -First 1
    if ($verOutput -match 'ffmpeg version\s+(\S+)') { $ffVersion = $matches[1] }
}

$stamp = @{
    Variant       = $variant
    FFmpegVersion = $ffVersion
    RequestedTag  = $Version
    Sha256        = $actualHash
    Verified      = -not $SkipChecksum
    FetchedAt     = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
    Source        = $url
}
$stamp | ConvertTo-Json | Set-Content (Join-Path $DestDir ".videoeye-ffmpeg.json") -Encoding UTF8

Write-Host "FFmpeg 安装成功: $DestDir"
if ($ffVersion) { Write-Host "  ffmpeg.exe 自报版本: $ffVersion" }
Write-Host "  Include: $DestDir\include"
Write-Host "  Lib:     $DestDir\lib"
Write-Host "  Bin:     $DestDir\bin"

# ── 清理运行时不需要的东西 (约 -27MB): ffmpeg.exe/ffplay.exe/doc/ ──
# 放在版本探测之后, 因为版本号是从 ffmpeg.exe -version 读出来的
foreach ($exe in @("ffmpeg.exe", "ffplay.exe", "ffprobe.exe")) {
    $exePath = Join-Path $DestDir "bin\$exe"
    if (Test-Path $exePath) { Remove-Item $exePath -Force -ErrorAction SilentlyContinue }
}
foreach ($dir in @("doc", "presets")) {
    $dirPath = Join-Path $DestDir $dir
    if (Test-Path $dirPath) { Remove-Item $dirPath -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "CMake 侧会自动找到该目录; 如需覆盖: -DFFMPEG_ROOT=`"$DestDir`""
exit 0
