# fetch-ffmpeg.ps1 - 下载并强校验 FFmpeg 预编译包 (BtbN win64-lgpl-shared, LGPLv3)
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -DestDir <path>
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -Force          # 强制重下
#   powershell -ExecutionPolicy Bypass -File scripts/fetch-ffmpeg.ps1 -SkipChecksum   # 仅在人工核对过着镜像时用
#
# 幂等: 每次都调用它是安全的 —— 目录已存在且与 cmake/ffmpeg-version.json 完全对得上时,
#       只做几次文件检查就退出（不联网、不写盘）。
#
# 行为约定:
#   * 版本/URL/SHA256 的唯一来源是 cmake/ffmpeg-version.json —— 所有入口必须一致,
#     否则不同开发者会构建出 ABI 不同的产物。
#   * SHA256 是硬性要求: 拿不到校验值或校验不过一律失败退出, 不留"警告继续"。
#     只有在人工核对过镜像的场景才用 -SkipChecksum 显式关闭。
#   * 先解压到临时目录并验证结构完整, 再替换已有目录 —— 中途失败不会把已经有
#     的、可用的依赖弄丢。
#
# 为什么固定用 win64-lgpl-shared:
#   BtbN/FFmpeg-Builds 唯一同时满足"带 include/ + lib/*.lib 可用于 MSVC 链接"、
#   "带 bin/*.dll 可随包分发"、"LGPLv3 不带 libx264/libx265"的变体。
#   gyan.dev 的 full_build-shared 是 GPLv3（含 GPL-only 组件），会让 Windows 分发物
#   整体落到 GPL 之下 —— 那是本项目明确要避开的（见 docs/FFMPEG_COMMAND_WORKBENCH.md
#   第 8 节）。
#
# ⚠️ 许可证（决定脚本默认怎么处理 ffmpeg.exe、以及为什么必须留下 LICENSE.txt）:
#   锁定包是 **LGPLv3**。LGPL 要求: 随附许可证文本 + 提供对应源码的获取方式 +
#   不限制使用者替换这份库。所以:
#     · 清理 doc/ presets/ 时**必须保留 LICENSE.txt** —— 它是安装树里
#       third-party 许可证清单的素材（CMake 会把它装进 doc/licenses/）;
#     · ffmpeg.exe 仍默认删除, 但理由不再是许可证, 而是体积（约 30MB）与
#       "只服务于 FFmpeg 命令工作台这一个辅助页面"; 要随包就显式开
#       -KeepFfmpegExe 并配合 -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON。

param(
    [string]$DestDir = "",
    # 保留 ffmpeg.exe（默认删除，见文件头的许可证说明）。
    # 打包者确定要随包分发 ffmpeg CLI、或本机想直接调试「FFmpeg 命令工作台」时打开。
    [switch]$KeepFfmpegExe,
    # 忽略已有目录里的 stamp，重新下载并安装。
    # ⚠️ 下面「已存在则跳过」的判断与完成时写 stamp 的两处都引用了它，但以前没有在
    # param 里声明 —— 于是脚本自己打印的「用 -Force 强制重下」照着敲会直接报
    # "找不到与参数名称 Force 匹配的参数"，那条提示等于骗人。
    [switch]$Force,
    # 跳过 SHA256 校验。只给「人工核对过镜像」的场景用，正常构建不要碰。
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
$cfg = Get-Content $versionFile -Raw -Encoding UTF8 | ConvertFrom-Json
$lockedVersion = $cfg.version
if ([string]::IsNullOrWhiteSpace($lockedVersion)) {
    Write-Error "$versionFile 里没有 version 字段"
    exit 1
}
if ([string]::IsNullOrWhiteSpace($Version)) { $Version = $lockedVersion }

# BtbN 的资产名里带 git commit（ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1.zip），
# 没法靠"版本号 → URL"拼出来；想换版本只能人工挑一个 autobuild tag，把 url/sha256
# 一起改掉再提交。所以这里不再提供 -Version 走模板的捷径 —— 留着它只会让人拿到
# 一个拼不出来、或拼出来指向别的东西的 URL。
if ($Version -ne $lockedVersion) {
    Write-Error "-Version 已废弃：cmake/ffmpeg-version.json 锁定的版本是 $lockedVersion。"
    Write-Error "要换版本请直接改 cmake/ffmpeg-version.json 的 windows.url / windows.sha256"
    Write-Error "（sha256 从同 release 的 checksums.sha256 取，别手抄），再重新运行本脚本。"
    exit 1
}

$url = $cfg.windows.url
$expectedSha = $cfg.windows.sha256
if (-not $url -or -not $expectedSha) {
    Write-Error "$versionFile 的 windows.url / windows.sha256 不完整，无法继续。"
    exit 1
}
Write-Host "版本锁定: FFmpeg $Version ($($cfg.windows.variant), cmake/ffmpeg-version.json)"

# 归档扩展名跟着 URL 走（BtbN 是 .zip，不排除以后换成别的容器）
$archiveExt = [System.IO.Path]::GetExtension($url)
if (-not $archiveExt) { $archiveExt = ".zip" }
$archive = Join-Path $env:TEMP "videoeye-ffmpeg-$Version$archiveExt"
$variant = "$Version-$($cfg.windows.variant)"

Write-Host "=== VideoEye FFmpeg 预编译包获取 ==="
Write-Host "版本:   $Version"
Write-Host "地址:   $url"
Write-Host "目标:   $DestDir"

# BtbN / gyan 的包解压后顶层都是一个 ffmpeg-<build>-<variant> 目录，这里不认名字，
# 只认结构 —— 换供应商时不用改这段。
#
# ⚠️ 定义必须放在下面「已存在则跳过」判断之前：PowerShell 的函数要等定义那一行执行过
# 才存在，写在文件后半段的话，前面的调用会直接报 "无法识别 Test-FfmpegLayout"。
function Test-FfmpegLayout([string]$dir) {
    return (Test-Path (Join-Path $dir "include\libavcodec\avcodec.h")) -and
           (Test-Path (Join-Path $dir "lib\avcodec.lib")) -and
           ((Get-ChildItem (Join-Path $dir "bin") -Filter "avcodec-*.dll" -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0)
}

# ── 已存在则跳过？三个判据缺一不可 ──
#   1) stamp.Variant 与锁定配置一致（版本号 + 变体）
#   2) stamp.Sha256 与 cmake/ffmpeg-version.json 的 windows.sha256 一致
#   3) stamp.Source 与 windows.url 一致，且目录结构仍然完整
#
# 只比 Variant 是不够的（原来的实现）: 同一个版本号可以对应不同的归档 ——
# BtbN 的 autobuild 每天重打一次，"版本号不变、url 与 sha256 变了"是常态。
# 此时旧包会被**无提示地继续使用**，本机编出来的 ABI 与 CI / 别人机器不一致，
# 而本机看起来一切正常，症状只以"某台机器上莫名崩溃"的形式出现。
$stampFile = Join-Path $DestDir ".videoeye-ffmpeg.json"
$skipReason = ""
if ($Force) {
    Write-Host "-Force: 忽略已有目录，强制重新获取"
} elseif (-not (Test-Path $DestDir)) {
    $skipReason = "目标目录不存在"
} elseif (-not (Test-Path $stampFile)) {
    $skipReason = "缺少版本 stamp（无法确认这份就是锁定包）"
} else {
    $stamp = $null
    try { $stamp = Get-Content $stampFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $stamp = $null }
    if (-not $stamp) {
        $skipReason = "stamp 无法解析"
    } elseif ("$($stamp.Variant)" -ne $variant) {
        $skipReason = "variant 变更（$($stamp.Variant) -> $variant）"
    } elseif ("$($stamp.Sha256)".ToLower() -ne $expectedSha.ToLower()) {
        $skipReason = "归档 SHA256 变更（$($stamp.Sha256) -> $($expectedSha.ToLower())）"
    } elseif ("$($stamp.Source)" -ne $url) {
        $skipReason = "下载地址变更"
    } elseif (-not (Test-FfmpegLayout $DestDir)) {
        $skipReason = "目录结构不完整（include/ lib/ bin/ 有缺件）"
    }
}

if (-not $skipReason) {
    Write-Host "已存在且与锁定配置一致 (variant=$variant, sha256=$($expectedSha.ToLower().Substring(0,12))…, 获取于 $($stamp.FetchedAt))，跳过下载。用 -Force 强制重下。"
    exit 0
}
Write-Host "已有目录不可复用：$skipReason —— 重新获取…"

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
        # 正常情况下走不到这里（上面已经强制要求 json 里带 sha256），留着是给
        # "有人手工把 sha256 清空了" 的场景一个明确出口，而不是静默跳过校验。
        Remove-Item $archive -Force -ErrorAction SilentlyContinue
        Write-Error "cmake/ffmpeg-version.json 里没有 windows.sha256。"
        Write-Error "构建依赖不允许在校验缺失的情况下继续。sha256 请从同一 release 的"
        Write-Error "checksums.sha256 取：$($cfg.windows.checksumsUrl)"
        exit 1
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
    Write-Error "解压产物结构异常: 未找到完整的 {include,lib,bin}。请确认下载的是带开发文件的 shared 包。"
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

# ── 许可证文件必须在清理之前先定位 ──
# LGPL/GPL 都要求随附许可证文本；CMake 会把这份文件装进安装树的 doc/licenses/，
# 并写进 THIRD_PARTY_NOTICES。下面删 doc/ 时如果把它一起删了，安装树里就会缺
# 一份必备文件，而构建本身还"一切正常" —— 这种缺口只有在合规审查时才暴露。
$licenseFile = $null
$licenseName = ""
foreach ($cand in @("LICENSE.txt", "LICENSE", "COPYING.txt", "COPYING")) {
    $p = Join-Path $DestDir $cand
    if (Test-Path $p) { $licenseFile = $p; $licenseName = $cand; break }
}
if (-not $licenseFile) {
    Write-Error "预编译包里找不到许可证文件（LICENSE.txt / LICENSE / COPYING*）。"
    Write-Error "分发 FFmpeg 二进制必须随附其许可证文本，缺了它不能继续。请换一个带许可证文件的包。"
    exit 1
}
Write-Host "许可证文件: $licenseName"

$stamp = @{
    Variant       = $variant
    FFmpegVersion = $ffVersion
    RequestedTag  = $Version
    Sha256        = $actualHash
    Verified      = -not $SkipChecksum
    FetchedAt     = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
    Source        = $url
    ReleaseTag    = $cfg.windows.releaseTag
    License       = $cfg.license.spdx
    LicenseFile   = $licenseName
}
$stamp | ConvertTo-Json | Set-Content (Join-Path $DestDir ".videoeye-ffmpeg.json") -Encoding UTF8

Write-Host "FFmpeg 安装成功: $DestDir"
if ($ffVersion) { Write-Host "  ffmpeg.exe 自报版本: $ffVersion" }
Write-Host "  Include: $DestDir\include"
Write-Host "  Lib:     $DestDir\lib"
Write-Host "  Bin:     $DestDir\bin"
# ── 清理运行时不需要的东西: ffmpeg.exe/ffplay.exe/ffprobe.exe/doc/presets ──
# 放在版本探测之后, 因为版本号是从 ffmpeg.exe -version 读出来的。
#
# ⚠️ ffmpeg.exe **默认删除**（约 30MB，只服务于「FFmpeg 命令工作台」这一个辅助
# 页面；换成 LGPL 包后这不再是许可证问题，纯粹是体积与用途的取舍）。
# 留着它的唯一理由是本机调试这一页，或打包者确实要随包分发 ——
# 那就加 -KeepFfmpegExe，并配合 -DVIDEOEYE_BUNDLE_FFMPEG_TOOL=ON。
# 删掉之后工作台页会显示"未找到 ffmpeg"并引导用户安装，不是构建坏了。
$cleanupExes = @("ffplay.exe", "ffprobe.exe")
if (-not $KeepFfmpegExe) { $cleanupExes += "ffmpeg.exe" }
foreach ($exe in $cleanupExes) {
    $exePath = Join-Path $DestDir "bin\$exe"
    if (Test-Path $exePath) { Remove-Item $exePath -Force -ErrorAction SilentlyContinue }
}

if (Test-Path $ffExe) {
    Write-Host "  CLI:     ffmpeg.exe 已保留 (-KeepFfmpegExe) —— 仅供本机调试, 默认不会进安装包"
} else {
    Write-Host "  CLI:     ffmpeg.exe 已删除 (体积/用途取舍; 需要本机调试请加 -KeepFfmpegExe)"
}
foreach ($dir in @("doc", "presets")) {
    $dirPath = Join-Path $DestDir $dir
    if (Test-Path $dirPath) { Remove-Item $dirPath -Recurse -Force -ErrorAction SilentlyContinue }
}

# 清理完之后再确认一次许可证文件还在（有的包把 LICENSE 放在 doc/ 里）
if (-not (Test-Path $licenseFile)) {
    Write-Error "许可证文件 $licenseName 在清理过程中被删除了（可能原本位于 doc/ 下）。"
    Write-Error "分发 FFmpeg 二进制必须随附其许可证文本。请手工把它放回 $DestDir\$licenseName。"
    exit 1
}

Write-Host ""
Write-Host "CMake 侧会自动找到该目录; 如需覆盖: -DFFMPEG_ROOT=`"$DestDir`""
exit 0
