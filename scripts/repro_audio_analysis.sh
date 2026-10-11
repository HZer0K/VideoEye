#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MEDIA_PATH="${1:-}"

# 构建目录由 preset linux-debug 决定(build/debug)，不再让 BUILD_DIR 环境变量
# 指着别处 —— 以前这里手搓 `cmake -S . -B build-debug`，产物落在 build-debug/，
# 和 preset 约定的 build/<config>/ 两套目录并存，谁都不知道该跑哪一个。

if [[ -z "$MEDIA_PATH" ]]; then
    echo "Usage: $0 <media-file>"
    echo
    echo "Example:"
    echo "  $0 /path/to/audio_or_video_file.mp3"
    exit 1
fi

if [[ ! -f "$MEDIA_PATH" ]]; then
    echo "Media file not found: $MEDIA_PATH" >&2
    exit 1
fi

cmake --preset linux-debug
cmake --build --preset linux-debug -j"$(nproc)"

cat <<'EOF'
Manual verification checklist:
1. Wait for playback to start automatically.
2. Open the "分析面板" tab.
3. Verify "音频帧" keeps appending rows.
4. Verify "音频可视化" shows both waveform and spectrum charts updating.
5. Verify "统一时间轴" continues receiving "音频采样" events.
6. Optionally click the CSV export button in "音频可视化" and confirm the file contains both waveform and spectrum sections.
EOF

"$ROOT_DIR/build/debug/bin/VideoEye" "$MEDIA_PATH"
