#include "FrameData.h"
#include <cstring>
#include <sstream>

namespace videoeye {
namespace model {

std::string StreamInfo::ToString() const {
    std::ostringstream oss;
    oss << "Stream Info:\n"
        << "  File: " << filename << "\n"
        << "  Format: " << format_name << "\n"
        << "  Duration: " << (duration_ms / 1000) << "s\n"
        << "  Video: " << video_width << "x" << video_height 
        << " @ " << video_fps << "fps, " << codec_name_video << "\n"
        << "  Audio: " << audio_sample_rate << "Hz, " << audio_channels 
        << "ch, " << codec_name_audio;
    return oss.str();
}

FrameData::~FrameData() {
    for (int i = 0; i < 8; ++i) {
        if (data[i]) {
            delete[] data[i];
            data[i] = nullptr;
        }
    }
}

void FrameData::CopyFrom(const FrameData& other) {
    width = other.width;
    height = other.height;
    format = other.format;
    pts = other.pts;
    timestamp = other.timestamp;
    
    for (int i = 0; i < 8; ++i) {
        linesize[i] = other.linesize[i];
        if (other.data[i] && other.linesize[i] > 0) {
            if (!data[i]) {
                data[i] = new uint8_t[linesize[i] * height];
            }
            std::memcpy(data[i], other.data[i], linesize[i] * height);
        }
    }
}

} // namespace model
} // namespace videoeye
