#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace model {

// 封面图（AV_DISPOSITION_ATTACHED_PIC）在 FFmpeg 里是一条一帧的 MJPEG 视频流。
// 它对"这个文件有没有视频画面"没有意义，所以统计口径一律排除。
int AnalysisResult::VideoStreamCount() const {
    int count = 0;
    for (const auto& s : streams) {
        if (s.IsVideo() && !s.attached_picture) ++count;
    }
    return count;
}

int AnalysisResult::AudioStreamCount() const {
    int count = 0;
    for (const auto& s : streams) {
        if (s.IsAudio()) ++count;
    }
    return count;
}

// 先认主轨（按 index 找），找不到再退回首条可播放视频流：
// streams 的顺序就是容器里的流号，而"第一条视频流"常常是封面。
const StreamDigest* AnalysisResult::FirstVideoStream() const {
    for (const auto& s : streams) {
        if (s.index == primary_video_stream_index && s.IsVideo() && !s.attached_picture) {
            return &s;
        }
    }
    for (const auto& s : streams) {
        if (s.IsVideo() && !s.attached_picture) return &s;
    }
    return nullptr;
}

const StreamDigest* AnalysisResult::FirstAudioStream() const {
    for (const auto& s : streams) {
        if (s.index == primary_audio_stream_index && s.IsAudio()) return &s;
    }
    for (const auto& s : streams) {
        if (s.IsAudio()) return &s;
    }
    return nullptr;
}

} // namespace model
} // namespace videoeye
