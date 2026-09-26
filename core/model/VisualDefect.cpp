#include "core/model/VisualDefect.h"

namespace videoeye {
namespace model {

const char* ToString(VisualDefectType type) {
    switch (type) {
        case VisualDefectType::BlackFrame:       return "黑场";
        case VisualDefectType::FreezeFrame:      return "冻结帧";
        case VisualDefectType::Blockiness:       return "花屏/马赛克";
        case VisualDefectType::Blur:             return "模糊";
        case VisualDefectType::Flicker:          return "闪烁";
        case VisualDefectType::OverExposure:     return "过曝";
        case VisualDefectType::UnderExposure:    return "欠曝";
        case VisualDefectType::ColorCast:        return "色偏";
        case VisualDefectType::InterlaceCombing: return "隔行梳齿";
        case VisualDefectType::Letterbox:        return "上下黑边";
        case VisualDefectType::Pillarbox:        return "左右黑边";
    }
    return "未知";
}

const char* ToString(VisualDefectSeverity severity) {
    switch (severity) {
        case VisualDefectSeverity::Info:     return "提示";
        case VisualDefectSeverity::Warning:  return "警告";
        case VisualDefectSeverity::Error:    return "错误";
        case VisualDefectSeverity::Critical: return "严重";
    }
    return "提示";
}

const char* DefectTypeCode(VisualDefectType type) {
    switch (type) {
        case VisualDefectType::BlackFrame:       return "black_frame";
        case VisualDefectType::FreezeFrame:      return "freeze_frame";
        case VisualDefectType::Blockiness:       return "blockiness";
        case VisualDefectType::Blur:             return "blur";
        case VisualDefectType::Flicker:          return "flicker";
        case VisualDefectType::OverExposure:     return "over_exposure";
        case VisualDefectType::UnderExposure:    return "under_exposure";
        case VisualDefectType::ColorCast:        return "color_cast";
        case VisualDefectType::InterlaceCombing: return "interlace_combing";
        case VisualDefectType::Letterbox:        return "letterbox";
        case VisualDefectType::Pillarbox:        return "pillarbox";
    }
    return "unknown";
}

int VisualDefectReport::CountByType(VisualDefectType type) const {
    int count = 0;
    for (const auto& d : defects) {
        if (d.type == type) ++count;
    }
    return count;
}

int VisualDefectReport::CountBySeverity(VisualDefectSeverity severity) const {
    int count = 0;
    for (const auto& d : defects) {
        if (d.severity == severity) ++count;
    }
    return count;
}

}  // namespace model
}  // namespace videoeye
