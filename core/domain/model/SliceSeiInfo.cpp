#include "core/domain/model/SliceSeiInfo.h"

#include <cmath>

namespace videoeye {
namespace model {
namespace {

std::string Fixed(double v, int digits) {
    if (!(v == v)) return "n/a";  // NaN
    char buf[32] = {0};
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf);
}

}  // namespace

std::string SeiSummary::Summary() const {
    if (!analyzed) return "未分析";
    std::string out = "SEI 消息 " + std::to_string(total_messages) + " 条";
    if (has_mastering_display) {
        out += " / MasteringDisplay(" + Fixed(max_display_mastering_luminance, 1) + " nits, min " +
               Fixed(min_display_mastering_luminance, 4) + ")";
    }
    if (has_content_light_level) {
        out += " / MaxCLL=" + std::to_string(max_content_light_level) +
               " MaxFALL=" + std::to_string(max_pic_average_light_level);
    }
    if (has_hdr10plus) {
        out += " / HDR10+ (" + std::to_string(hdr10plus_count) + " 条动态元数据)";
    }
    if (has_dolby_vision_rpu) {
        out += " / Dolby Vision RPU (" + std::to_string(dolby_vision_rpu_count) + " 个)";
    }
    if (has_film_grain) {
        out += " / FilmGrain(" + std::to_string(film_grain_count) +
               (film_grain_cancel_flag ? ", cancel)" : ")");
    }
    if (has_pic_timing) {
        out += " / pic_timing(" + std::to_string(pic_timing_count) + ")";
    }
    if (out.find('/') == std::string::npos) {
        out = "未检出 HDR 相关的 SEI（共 " + std::to_string(total_messages) + " 条消息）";
    }
    return out;
}

std::string SliceSummary::Summary() const {
    if (!analyzed) return "未分析";
    if (!supported) {
        return std::string("不支持：") + unsupported_reason;
    }
    std::string out = "slice " + std::to_string(slice_count) + " 个 (I=" + std::to_string(type_i) +
                      " P=" + std::to_string(type_p) + " B=" + std::to_string(type_b);
    if (type_other > 0) out += " other=" + std::to_string(type_other);
    out += ")";
    if (qp_valid) {
        out += " / QP=[" + std::to_string(qp_min) + ", " + std::to_string(qp_max) +
               "] avg=" + Fixed(qp_avg, 1);
    } else {
        out += " / QP 不可用（缺少 PPS 的 pic_init_qp_minus26 基准）";
    }
    return out;
}

std::string SliceSeiSummary::Summary() const {
    if (!analyzed) return "未分析";
    std::string out = slices.Summary() + " ; " + sei.Summary();
    if (truncated) {
        out += " [窗口截断，仅扫描前 " + std::to_string(packets_scanned) + " 个包]";
    }
    return out;
}

}  // namespace model
}  // namespace videoeye
