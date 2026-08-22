# fetch-ffmpeg.ps1 - 自动下载并校验 FFmpeg 预编译包 (gyan.dev full shared)
# 用法: powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1
# 可选参数: -DestDir <path>  -Variant <release-full-shared|release-essentials>  -Force
#
# 默认安装到 third_party/ffmpeg-prebuilt（固定位置，与构建目录无关）
# CMake 配置时用 -DFFMPEG_ROOT 指向该目录
#
# 说明:
#   * 默认 release-full-shared 变体: gyan.dev 唯一带开发文件(include/ + lib/ 导入库)的共享构建,
#     项目需要头文件与 .lib 才能链接, 因此 essentials(仅 bin/) 不适用。
#   * 版本检测使用 stamp 文件 (.videoeye-ffmpeg.json), 不再依赖特定 avcodec-X.dll 文件名,
#     避免 gyan.dev 升级 FFmpeg 版本后旧标记仍然命中导致永不更新。
#   * 下载失败时自动回退 curl.exe (带重试)。

param(
    [string]$DestDir = "",
    [string]$Variant = "release-full-shared",
    [string]$Version = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
if (-not $DestDir) { $DestDir = Join-Path $projectRoot "third_party\ffmpeg-prebuilt" }

# 版本锁定: -Version "8.1.2" 将 Variant 从 release-full-shared 改为 8.1.2-full_build-shared (gyan.dev 版本化 URL)
# 注意: gyan.dev 版本化归档包命名含 "_build" (full_build-shared), 与 release 变体 (full-shared) 不同;
#       且 URL 路径多一层 /packages/ 子路径。
if ($Version) {
    $Variant = "$Version-full_build-shared"
    Write-Host "已指定版本锁定: FFmpeg $Version (variant=$Variant)"
}

# gyan.dev URL 结构:
#   release 变体 (latest):  /ffmpeg/builds/ffmpeg-release-full-shared.7z
#   版本化归档 (previous):  /ffmpeg/builds/packages/ffmpeg-{ver}-full_build-shared.7z
if ($Version) {
    $url = "https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-$Variant.7z"
} else {
    $url = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-$Variant.7z"
}
$shaUrl = "$url.sha256"
$archive = Join-Path $env:TEMP "videoeye-ffmpeg-$Variant.7z"

Write-Host "=== VideoEye FFmpeg 预编译包获取 ==="
Write-Host "变体: $Variant"
Write-Host "目标: $DestDir"

# 已存在则跳过（除非 -Force）— 以 stamp 文件为准，与具体 DLL 版本号解耦
$stampFile = Join-Path $DestDir ".videoeye-ffmpeg.json"
if ((Test-Path $DestDir) -and (Test-Path $stampFile) -and -not $Force) {
    $stamp = Get-Content $stampFile -Raw | ConvertFrom-Json
    if ($stamp.Variant -eq $Variant) {
        Write-Host "FFmpeg 已存在于 $DestDir (variant=$Variant, 获取于 $($stamp.FetchedAt))，跳过下载（用 -Force 强制重下）"
        if ($stamp.FFmpegVersion) { Write-Host "FFmpeg 版本: $($stamp.FFmpegVersion)" }
        Write-Host "CMake 配置: -DFFMPEG_ROOT=`"$DestDir`""
        exit 0
    } else {
        Write-Host "检测到版本变更 ($($stamp.Variant) -> $Variant)，重新获取..."
    }
} elseif ((Test-Path $DestDir) -and -not $Force) {
    # 旧布局（无 stamp，只有 avcodec-*.dll）：无法确认版本，视为过期，重新获取以确保与 Variant 一致
    Write-Host "检测到旧版 FFmpeg 安装（无版本 stamp），重新获取以确保版本可追踪..."
}

# -- 下载 (失败时自动回退 curl.exe 带重试) --
Write-Host "=== 下载 FFmpeg ==="
Write-Host "URL: $url"
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

# -- 解压 (优先 Windows 自带 bsdtar; 失败则回退 7-Zip) --
Write-Host "=== 解压到 $DestDir ==="
$extractTmp = Join-Path $env:TEMP "videoeye-ffmpeg-extract"
if (Test-Path $extractTmp) { Remove-Item $extractTmp -Recurse -Force }
if (Test-Path $DestDir) { Remove-Item $DestDir -Recurse -Force }
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
        Write-Error "解压失败且未找到 7-Zip！请安装 7-Zip (https://www.7-zip.org/) 后重试，或检查 tar 是否可用。"
        exit 1
    }
    Write-Host "解压器: $sevenZip"
    & $sevenZip x $archive "-o$extractTmp" -y | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "解压失败 (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
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

# 注意: 多余文件清理 (ffmpeg.exe/ffplay.exe/doc/) 在版本检测之后执行, 
#       因为版本检测需要运行 ffmpeg.exe

# -- 验证产物 + 写入版本 stamp --
Write-Host ""
Write-Host "=== 完成 ==="
if (Test-Path "$DestDir\include\libavcodec\avcodec.h") {
    # 提取实际 FFmpeg 版本号 (供团队协调版本一致性)
    $ffVersion = ""
    $ffExe = Join-Path $DestDir "bin\ffmpeg.exe"
    if (Test-Path $ffExe) {
        $verOutput = & $ffExe -version 2>&1 | Select-Object -First 1
        if ($verOutput -match 'ffmpeg version\s+(\S+)') {
            $ffVersion = $matches[1]
            Write-Host "FFmpeg 版本: $ffVersion"
        }
    }

    # 记录获取信息, 供后续跳过/更新判断
    $stamp = @{
        Variant       = $Variant
        FFmpegVersion = $ffVersion
        FetchedAt     = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
        Source        = $url
    }
    $stamp | ConvertTo-Json | Set-Content (Join-Path $DestDir ".videoeye-ffmpeg.json") -Encoding UTF8
    Write-Host "FFmpeg 安装成功: $DestDir"
    Write-Host "  Include: $DestDir\include"
    Write-Host "  Lib:     $DestDir\lib"
    Write-Host "  Bin:     $DestDir\bin"
    Write-Host ""
    Write-Host "CMake 配置时传递: -DFFMPEG_ROOT=`"$DestDir`""
    Write-Host "或设置环境变量: set FFMPEG_ROOT=$DestDir"
    if (-not $Version) {
        Write-Host ""
        Write-Host "提示: 当前使用 latest (release-full-shared)。团队协作时建议锁定版本:"
        Write-Host "  powershell -File scripts\fetch-ffmpeg.ps1 -Version $ffVersion"
        Write-Host "  或在 build_ninja.ps1 中设置 `$FfmpegVersion 变量"
    }

    # -- 清理运行时不需要的多余文件 (减小 ~27MB) --
    # 版本检测已完成, 删除可执行工具和文档 (项目只需要 DLL + lib + include)
    foreach ($exe in @("ffmpeg.exe", "ffplay.exe", "ffprobe.exe")) {
        $exePath = Join-Path $DestDir "bin\$exe"
        if (Test-Path $exePath) { Remove-Item $exePath -Force -ErrorAction SilentlyContinue }
    }
    foreach ($dir in @("doc", "presets")) {
        $dirPath = Join-Path $DestDir $dir
        if (Test-Path $dirPath) { Remove-Item $dirPath -Recurse -Force -ErrorAction SilentlyContinue }
    }
} else {
    Write-Error "解压产物结构异常，未找到 include/libavcodec/avcodec.h"
    exit 1
}
