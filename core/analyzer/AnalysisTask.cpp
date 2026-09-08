#include "core/analyzer/AnalysisTask.h"

namespace videoeye {
namespace analyzer {

int AnalysisResult::VideoStreamCount() const {
    int count = 0;
    for (const auto& s : streams) {
        if (s.IsVideo()) ++count;
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

const StreamDigest* AnalysisResult::FirstVideoStream() const {
    for (const auto& s : streams) {
        if (s.IsVideo()) return &s;
    }
    return nullptr;
}

const StreamDigest* AnalysisResult::FirstAudioStream() const {
    for (const auto& s : streams) {
        if (s.IsAudio()) return &s;
    }
    return nullptr;
}

} // namespace analyzer
} // namespace videoeye
