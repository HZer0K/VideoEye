#!/usr/bin/env bash
# ============================================================
# fetch-ffmpeg.sh — 确保 Linux 上有一份满足最低版本要求的 FFmpeg
#
# 用法:
#   bash scripts/fetch-ffmpeg.sh                 # 自动判断
#   bash scripts/fetch-ffmpeg.sh --dest <dir>    # 指定兜底包落点
#   bash scripts/fetch-ffmpeg.sh --force         # 忽略已有目录，重新下载
#   bash scripts/fetch-ffmpeg.sh --print-only    # 只判断，不下载
#
# 输出约定（CI 靠这个接）:
#   * 系统 FFmpeg 够新  -> stdout 为空，退出 0（用系统那份，不下载任何东西）
#   * 系统不够 / 没有    -> stdout 只有一行：兜底包的绝对路径，交给 -DFFMPEG_ROOT
#   * 所有人类可读的信息一律走 stderr，免得污染 stdout
#
# 为什么需要这个脚本:
#   VideoEye 编译期要求 FFmpeg >= 7.1（libavcodec >= 61.19），具体由
#   cmake/deps/FfmpegApiProbe.cmake 用符号探测在 configure 阶段强制执行。
#   但 Linux 的 FFmpeg 版本由发行版决定 —— Ubuntu 24.04 是 6.1、Debian 12 是 5.1，
#   项目的 configure 会直接失败。与其让每个开发者/CI 自己去找一份新版 FFmpeg，
#   不如把"不够就换锁定包"这件事脚本化。
#
#   兜底包的 URL 与 SHA256 同样锁在 cmake/ffmpeg-version.json 里（linux.fallback），
#   而且与 Windows 锁定包是**同一次上游构建**（同一个 buildId），保证三平台行为一致。
#
# 本地开发一般不需要它：装个够新的系统 FFmpeg 就行。它主要服务 CI。
# ============================================================

set -euo pipefail

# 与 cmake/deps/FfmpegApiProbe.cmake、CONTRIBUTING.md「FFmpeg 支持矩阵」保持一致
REQUIRED_VERSION="7.1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERSION_FILE="${REPO_ROOT}/cmake/ffmpeg-version.json"
DEFAULT_DEST="${REPO_ROOT}/third_party/prebuilt/linux-x64/ffmpeg"

DEST="${DEFAULT_DEST}"
FORCE=0
PRINT_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dest)        DEST="$2"; shift 2 ;;
        --force)       FORCE=1; shift ;;
        --print-only)  PRINT_ONLY=1; shift ;;
        -h|--help)     sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数: $1（用 --help 看用法）" >&2; exit 2 ;;
    esac
done

log() { echo "[fetch-ffmpeg] $*" >&2; }

# ---- 1) 系统 FFmpeg 够不够 ----
system_version=""
if command -v pkg-config >/dev/null 2>&1; then
    system_version="$(pkg-config --modversion libavcodec 2>/dev/null || true)"
fi

if [[ -n "${system_version}" ]]; then
    # sort -V 做版本比较：min 排在前面说明 system >= min
    newest_first="$(printf '%s\n%s\n' "${REQUIRED_VERSION}" "${system_version}" | sort -V | head -n1)"
    if [[ "${newest_first}" == "${REQUIRED_VERSION}" ]]; then
        log "系统 FFmpeg ${system_version} >= ${REQUIRED_VERSION}，直接用它（不下载）"
        exit 0
    fi
    log "系统 FFmpeg ${system_version} < ${REQUIRED_VERSION}，需要兜底包"
else
    log "pkg-config 找不到 libavcodec，需要兜底包"
fi

if [[ "${PRINT_ONLY}" -eq 1 ]]; then
    log "--print-only：不下载，退出"
    exit 1
fi

# ---- 2) 读锁定包信息 ----
if [[ ! -f "${VERSION_FILE}" ]]; then
    echo "找不到 ${VERSION_FILE}" >&2
    exit 1
fi

read_json() {
    python3 - "$1" "${VERSION_FILE}" <<'PY'
import json, sys
key = sys.argv[1].split('.')
try:
    with open(sys.argv[2], encoding="utf-8") as f:
        d = json.load(f)
    cur = d
    for k in key:
        cur = cur[k]
    print(cur)
except Exception:
    sys.exit(1)
PY
}

URL="$(read_json linux.fallback.url || true)"
SHA256="$(read_json linux.fallback.sha256 || true)"
BUILD_ID="$(read_json linux.fallback.buildId || true)"

if [[ -z "${URL}" || -z "${SHA256}" ]]; then
    echo "cmake/ffmpeg-version.json 的 linux.fallback 缺少 url / sha256。" >&2
    echo "系统 FFmpeg 版本不够，又没有可用的兜底包，无法继续。" >&2
    exit 1
fi

# ---- 3) 已经装过就直接用 ----
# 判据三条，缺一不可（与 Windows 侧 fetch-ffmpeg.ps1 同口径）:
#   · stamp 里记的是同一个 buildId
#   · stamp 里的 sha256 与锁定配置一致  ← 同一版本号可能对应重新打包的归档
#   · stamp 里的下载地址一致            ← 版本号不变但换了 URL / 供应商
# 只比 buildId 时，"同版本换了包"会被静默放过，本机与 CI 的 ABI 就此分叉。
STAMP="${DEST}/.videoeye-ffmpeg.json"
if [[ "${FORCE}" -eq 0 && -f "${DEST}/include/libavcodec/avcodec.h" && -d "${DEST}/lib" ]]; then
    if [[ -f "${STAMP}" ]] \
       && grep -qF "${BUILD_ID}" "${STAMP}" 2>/dev/null \
       && grep -qF "${SHA256}" "${STAMP}" 2>/dev/null \
       && grep -qF "${URL}" "${STAMP}" 2>/dev/null; then
        log "已有兜底包（${BUILD_ID}, sha256 ${SHA256:0:12}…）：${DEST}"
        echo "${DEST}"
        exit 0
    fi
    log "${DEST} 已存在但 stamp 与 cmake/ffmpeg-version.json 不一致（buildId / sha256 / url），重新下载"
fi

# ---- 4) 下载 + 校验 ----
mkdir -p "${DEST}"
TMP_XZ="$(mktemp -t videoeye-ffmpeg-XXXXXX.tar.xz)"
trap 'rm -f "${TMP_XZ}"' EXIT

log "下载 ${URL}"
curl -sS -L --retry 3 --max-time 900 -o "${TMP_XZ}" "${URL}"

actual="$(sha256sum "${TMP_XZ}" | awk '{print $1}')"
if [[ "${actual}" != "${SHA256}" ]]; then
    echo "SHA256 校验失败：期望 ${SHA256}，实际 ${actual}。" >&2
    echo "不带着可疑的归档继续 —— 请核对 cmake/ffmpeg-version.json 的 linux.fallback。" >&2
    exit 1
fi
log "SHA256 校验通过（${actual}）"

rm -rf "${DEST}"
mkdir -p "${DEST}"
tar -xJf "${TMP_XZ}" -C "${DEST}" --strip-components=1

if [[ ! -f "${DEST}/include/libavcodec/avcodec.h" || ! -d "${DEST}/lib" ]]; then
    echo "解包产物结构异常：缺 include/libavcodec/avcodec.h 或 lib/" >&2
    exit 1
fi

# 与 Windows 侧同构的 stamp（CMake 的许可声明会读它）
cat > "${STAMP}" <<EOF
{
  "Variant": "linux64-lgpl-shared",
  "FFmpegVersion": "${BUILD_ID}",
  "RequestedTag": "8.1.3",
  "Sha256": "${actual}",
  "Verified": true,
  "Source": "${URL}"
}
EOF

log "兜底包就绪：${DEST}"
echo "${DEST}"
