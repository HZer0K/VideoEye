#include "core/model/AudioQcResult.h"

#include <cstdio>
#include <string>

namespace videoeye {
namespace model {
namespace {

std::string Fixed(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

}  // namespace

std::string AudioQcResult::ToString() const {
    if (!analyzed) return "音频 QC 未执行";
    if (!has_audio) return "无音频流";

    std::string s;
    s += "布局=" + metadata.channel_layout;
    s += " 采样率=" + std::to_string(metadata.sample_rate) + "Hz";
    s += " 时长=" + Fixed(duration_seconds, 2) + "s";
    s += " | 峰值=" + Fixed(sample_peak_dbfs, 2) + "dBFS";
    s += " 真峰值=" + Fixed(true_peak_dbtp, 2) + "dBTP";
    s += " RMS=" + Fixed(rms_dbfs, 2) + "dBFS";
    s += " DC=" + Fixed(max_dc_offset, 6);
    s += " | Integrated=" + Fixed(integrated_lufs, 2) + "LUFS";
    s += " S.max=" + Fixed(short_term_max_lufs, 2) + "LUFS";
    s += " M.max=" + Fixed(momentary_max_lufs, 2) + "LUFS";
    s += " LRA=" + Fixed(loudness_range_lu, 2) + "LU";
    s += " | 削波=" + std::to_string(clipping_sample_count) + "样本/" +
         std::to_string(clipping_event_count) + "段";
    s += " 静音=" + std::to_string(silence_ranges.size()) + "段(" +
         Fixed(silence_ratio * 100.0, 1) + "%)";
    if (correlation_available) {
        s += " 相关性 min=" + Fixed(correlation_min, 3) + " mean=" + Fixed(correlation_mean, 3);
    }
    return s;
}

}  // namespace model
}  // namespace videoeye
