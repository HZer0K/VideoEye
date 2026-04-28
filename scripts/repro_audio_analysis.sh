#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-debug}"
MEDIA_PATH="${1:-}"

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

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" -j"$(nproc)"

cat <<'EOF'
Manual verification checklist:
1. Wait for playback to start automatically.
2. Open the "分析面板" tab.
3. Verify "音频帧" keeps appending rows.
4. Verify "音频可视化" shows both waveform and spectrum charts updating.
5. Verify "统一时间轴" continues receiving "音频采样" events.
6. Optionally click the CSV export button in "音频可视化" and confirm the file contains both waveform and spectrum sections.
EOF

"$BUILD_DIR/bin/VideoEye" "$MEDIA_PATH"
