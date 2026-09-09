#include "core/model/GopInfo.h"

#include <cstdio>

namespace videoeye {
namespace model {

double GopInfo::DurationSeconds() const {
    const double d = end_seconds - start_seconds;
    return d > 0.0 ? d : 0.0;
}

double GopInfo::AverageBitrateKbps() const {
    const double d = DurationSeconds();
    if (d <= 0.0 || byte_count <= 0) return 0.0;
    return static_cast<double>(byte_count) * 8.0 / 1000.0 / d;
}

double GopInfo::AverageFrameBytes() const {
    if (frame_count <= 0) return 0.0;
    return static_cast<double>(byte_count) / static_cast<double>(frame_count);
}

double GopInfo::AverageFps() const {
    const double d = DurationSeconds();
    if (d <= 0.0) return 0.0;
    return static_cast<double>(frame_count) / d;
}

std::string GopInfo::FrameTypeSummary() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%dI / %dP / %dB%s", i_count, p_count, b_count,
                  unknown_count > 0 ? (" / " + std::to_string(unknown_count) + "?").c_str() : "");
    return std::string(buf);
}

std::string GopInfo::ToString() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "GOP#%d [%.3f - %.3f] %.3fs %d帧 %.1fKB 平均%.0fkbps 最大帧%.1fKB %s%s",
                  index, start_seconds, end_seconds, DurationSeconds(), frame_count,
                  static_cast<double>(byte_count) / 1024.0, AverageBitrateKbps(),
                  static_cast<double>(max_frame_bytes) / 1024.0,
                  closed_gop ? "closed" : "open",
                  complete ? "" : " (未收尾)");
    return std::string(buf);
}

}  // namespace model
}  // namespace videoeye
