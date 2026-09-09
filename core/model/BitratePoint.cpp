#include "core/model/BitratePoint.h"

namespace videoeye {
namespace model {

const char* ToString(FrameType type) {
    switch (type) {
        case FrameType::Unknown: return "未知";
        case FrameType::I:       return "I";
        case FrameType::P:       return "P";
        case FrameType::B:       return "B";
        case FrameType::S:       return "S";
        case FrameType::SI:      return "SI";
        case FrameType::SP:      return "SP";
        case FrameType::BI:      return "BI";
    }
    return "未知";
}

bool IsIntraFrame(FrameType type) {
    return type == FrameType::I || type == FrameType::SI;
}

bool IsReferenceFrame(FrameType type) {
    return type == FrameType::I || type == FrameType::P || type == FrameType::SP ||
           type == FrameType::SI;
}

double BytesToKbps(int64_t bytes, double window_seconds) {
    if (window_seconds <= 0.0 || bytes <= 0) return 0.0;
    return static_cast<double>(bytes) * 8.0 / 1000.0 / window_seconds;
}

}  // namespace model
}  // namespace videoeye
