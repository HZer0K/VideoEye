#include "core/domain/model/ParameterTimeline.h"

#include <algorithm>
#include <cstdio>

namespace videoeye {
namespace model {
namespace {

// 00:12:35.240 形态。分析结果是给人看/写报告的，不用国际化的时长格式。
std::string FormatTimecode(double seconds) {
    if (seconds < 0.0)
        seconds = 0.0;
    const long long total_ms = static_cast<long long>(seconds * 1000.0 + 0.5);
    const long long ms = total_ms % 1000;
    const long long total_s = total_ms / 1000;
    const long long s = total_s % 60;
    const long long m = (total_s / 60) % 60;
    const long long h = total_s / 3600;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld", h, m, s, ms);
    return std::string(buf);
}

} // namespace

const char* ParameterSetKindCode(ParameterSetKind kind) {
    switch (kind) {
    case ParameterSetKind::Vps:
        return "vps";
    case ParameterSetKind::Sps:
        return "sps";
    case ParameterSetKind::Pps:
        return "pps";
    case ParameterSetKind::Av1SequenceHeader:
        return "av1_sequence_header";
    case ParameterSetKind::Unknown:
    default:
        return "unknown";
    }
}

const char* ParameterSetKindName(ParameterSetKind kind) {
    switch (kind) {
    case ParameterSetKind::Vps:
        return "VPS";
    case ParameterSetKind::Sps:
        return "SPS";
    case ParameterSetKind::Pps:
        return "PPS";
    case ParameterSetKind::Av1SequenceHeader:
        return "AV1 序列头";
    case ParameterSetKind::Unknown:
    default:
        return "参数集";
    }
}

std::string ParameterChangeEvent::ShortText() const {
    std::string out;
    if (has_time) {
        out += FormatTimecode(time_seconds);
        out += "  ";
    }
    out += source.empty() ? std::string(ParameterSetKindName(kind)) : source;
    if (!changes.empty()) {
        out += "  ";
        out += changes;
    } else if (!new_config.empty()) {
        out += " -> " + new_config;
    }
    return out;
}

std::string ParameterChangeEvent::ToString() const {
    std::string out = "视频流 #" + std::to_string(stream_index) + "：";
    if (has_time)
        out += FormatTimecode(time_seconds);
    else
        out += "（无 PTS）";
    out += "，";
    if (!old_config.empty()) {
        out += old_config + " -> " + new_config;
    } else {
        out += new_config;
    }
    out += "；来源：" + (source.empty() ? std::string(ParameterSetKindName(kind)) : source);
    if (packet_index >= 0)
        out += "；包号 " + std::to_string(packet_index);
    if (file_offset >= 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(file_offset));
        out += "；文件偏移 ";
        out += buf;
    }
    if (replaced_same_id)
        out += "；【同一 id 的参数集内容被替换】";
    return out;
}

std::string ParameterTimeline::ActiveSummary() const {
    // 取最后变化后的配置；没有变化事件时取第一份 SPS 的摘要。
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        if (!it->new_config.empty())
            return it->new_config;
    }
    for (const ParameterSetEntry& entry : parameter_sets) {
        if (entry.kind == ParameterSetKind::Sps && !entry.summary.empty())
            return entry.summary;
    }
    return std::string();
}

std::string ParameterTimeline::Summary() const {
    if (!analyzed)
        return "参数变化时间线：未分析";
    std::string out = "参数变化时间线：" + codec_name + "，扫描 " + std::to_string(packets_scanned) + " 包，";
    out += "参数集 " + std::to_string(parameter_sets.size()) + " 份";
    if (!changes.empty())
        out += "，配置变化 " + std::to_string(changes.size()) + " 次";
    else
        out += "，全程配置未变";
    if (missing_reference_count > 0)
        out += "，引用了 " + std::to_string(missing_reference_count) + " 次未传输的参数集";
    if (truncated)
        out += "（已到追踪上限，只覆盖前面一段）";
    return out;
}

} // namespace model
} // namespace videoeye
