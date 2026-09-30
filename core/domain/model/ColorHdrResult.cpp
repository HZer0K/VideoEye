#include "core/domain/model/ColorHdrResult.h"

#include <string>

namespace videoeye {
namespace model {

bool ColorHdrAnalysis::StaticMetadataComplete() const {
    return hdr.mastering_display.Complete() && hdr.content_light.Complete();
}

std::string ColorHdrAnalysis::ToString() const {
    if (!analyzed) return "未执行色彩/HDR 分析";
    std::string out = hdr.format_name;
    out += " ｜ " + color.ToString();
    if (hdr.mastering_display.Complete()) {
        out += " ｜ MaxCLL " + std::to_string(hdr.content_light.max_cll) + " / MaxFALL " +
               std::to_string(hdr.content_light.max_fall);
    }
    return out;
}

} // namespace model
} // namespace videoeye
