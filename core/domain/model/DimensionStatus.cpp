#include "core/domain/model/DimensionStatus.h"

#include <algorithm>
#include <cstdio>

namespace videoeye {
namespace model {

const char* DimensionCode(AnalysisDimension dimension) {
    switch (dimension) {
    case AnalysisDimension::FileProbe:
        return "file_probe";
    case AnalysisDimension::ContainerStructure:
        return "container_structure";
    case AnalysisDimension::VideoBitstream:
        return "video_bitstream";
    case AnalysisDimension::FrameStructure:
        return "frame_structure";
    case AnalysisDimension::ParameterTimeline:
        return "parameter_timeline";
    case AnalysisDimension::BitrateGop:
        return "bitrate_gop";
    case AnalysisDimension::ColorHdr:
        return "color_hdr";
    case AnalysisDimension::AudioQc:
        return "audio_qc";
    case AnalysisDimension::AudioBitstream:
        return "audio_bitstream";
    case AnalysisDimension::Subtitle:
        return "subtitle";
    case AnalysisDimension::Timecode:
        return "timecode";
    case AnalysisDimension::AuxiliaryData:
        return "auxiliary_data";
    case AnalysisDimension::Mp4SampleTable:
        return "mp4_sample_table";
    case AnalysisDimension::TsTransport:
        return "ts_transport";
    case AnalysisDimension::Streaming:
        return "streaming";
    case AnalysisDimension::LegacyContainer:
        return "legacy_container";
    case AnalysisDimension::FrameOrder:
        return "frame_order";
    case AnalysisDimension::Count:
    default:
        return "unknown";
    }
}

const char* DimensionName(AnalysisDimension dimension) {
    switch (dimension) {
    case AnalysisDimension::FileProbe:
        return "文件与流表";
    case AnalysisDimension::ContainerStructure:
        return "容器结构";
    case AnalysisDimension::VideoBitstream:
        return "视频码流参数";
    case AnalysisDimension::FrameStructure:
        return "帧级结构";
    case AnalysisDimension::ParameterTimeline:
        return "参数变化时间线";
    case AnalysisDimension::BitrateGop:
        return "码率与 GOP";
    case AnalysisDimension::ColorHdr:
        return "色彩与 HDR";
    case AnalysisDimension::AudioQc:
        return "音频 QC";
    case AnalysisDimension::AudioBitstream:
        return "音频位流参数";
    case AnalysisDimension::Subtitle:
        return "字幕";
    case AnalysisDimension::Timecode:
        return "时码与章节";
    case AnalysisDimension::AuxiliaryData:
        return "辅助数据";
    case AnalysisDimension::Mp4SampleTable:
        return "MP4 样本表";
    case AnalysisDimension::TsTransport:
        return "MPEG-TS 传输层";
    case AnalysisDimension::Streaming:
        return "流媒体包";
    case AnalysisDimension::LegacyContainer:
        return "容器体检";
    case AnalysisDimension::FrameOrder:
        return "帧级显示顺序（POC）";
    case AnalysisDimension::Count:
    default:
        return "未知维度";
    }
}

void DimensionStatus::MarkConclusive() {
    // 判定口径（写死在这里，避免各调用点各写一套）：
    //   1. 必须真的跑过；
    //   2. 能力必须 Supported（Partial/Unavailable/Failed 一律不算通过）；
    //   3. 不能有失败计数与失败原因；
    //   4. 有覆盖信息时覆盖率必须 >= 99%（与音频 QC 的 completeness 口径一致）。
    if (!executed || capability != AnalysisCapability::Supported)
        conclusive = false;
    else if (failures > 0 || !failure_reason.empty())
        conclusive = false;
    else if (has_coverage && coverage_ratio < 0.99)
        conclusive = false;
    else
        conclusive = true;
}

double DimensionStatus::CoveragePercent() const {
    if (!has_coverage)
        return -1.0;
    return coverage_ratio * 100.0;
}

std::string DimensionStatus::Summary() const {
    std::string out = DimensionName(dimension);
    out += "：";
    if (!executed) {
        out += "未执行";
        if (!failure_reason.empty())
            out += "（" + failure_reason + "）";
        return out;
    }
    out += CapabilityToString(capability);
    if (!scope_unit.empty()) {
        out += "，扫描 ";
        out += std::to_string(units_scanned);
        if (units_total > 0) {
            out += "/" + std::to_string(units_total);
        }
        out += " " + scope_unit;
    }
    if (has_coverage) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "，覆盖 %.1f%%", coverage_ratio * 100.0);
        out += buf;
    }
    if (failures > 0)
        out += "，失败 " + std::to_string(failures);
    if (!failure_reason.empty())
        out += "，限制：" + failure_reason;
    if (conclusive)
        out += "，结论可用";
    return out;
}

DimensionStatus MakeSkippedDimension(AnalysisDimension dimension, const std::string& reason) {
    DimensionStatus status;
    status.dimension = dimension;
    status.executed = false;
    status.capability = AnalysisCapability::Unavailable;
    status.failure_reason = reason;
    return status;
}

DimensionStatus MakeFailedDimension(AnalysisDimension dimension, const std::string& reason, int64_t failures) {
    DimensionStatus status;
    status.dimension = dimension;
    status.executed = true;
    status.capability = AnalysisCapability::Failed;
    status.failure_reason = reason;
    status.failures = failures;
    return status;
}

} // namespace model
} // namespace videoeye
