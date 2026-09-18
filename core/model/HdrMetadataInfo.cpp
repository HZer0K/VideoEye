#include "core/model/HdrMetadataInfo.h"

#include <cstdio>
#include <string>

namespace videoeye {
namespace model {
namespace {

std::string Fixed(double value, int decimals) {
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

}  // namespace

const char* ToString(HdrFormat format) {
    switch (format) {
        case HdrFormat::Unknown:    return "未知";
        case HdrFormat::Sdr:        return "SDR";
        case HdrFormat::Hlg:        return "HLG";
        case HdrFormat::Hdr10:      return "HDR10";
        case HdrFormat::Hdr10Basic: return "HDR10（缺静态元数据）";
        case HdrFormat::Hdr10Plus:  return "HDR10+";
        case HdrFormat::DolbyVision: return "Dolby Vision";
        case HdrFormat::HdrVivid:   return "HDR Vivid";
    }
    return "未知";
}

bool MasteringDisplayMetadata::Plausible() const {
    if (!has_primaries && !has_luminance) return false;
    auto in_unit = [](double v) { return v >= 0.0 && v <= 1.0; };
    if (has_primaries) {
        const bool ok = in_unit(red_x) && in_unit(red_y) && in_unit(green_x) && in_unit(green_y) &&
                        in_unit(blue_x) && in_unit(blue_y) && in_unit(white_x) && in_unit(white_y) &&
                        (red_x + red_y + green_x + green_y + blue_x + blue_y + white_x + white_y) > 0.0;
        if (!ok) return false;
    }
    if (has_luminance) {
        if (max_luminance <= 0.0 || min_luminance < 0.0) return false;
        if (max_luminance < min_luminance) return false;
    }
    return true;
}

std::string MasteringDisplayMetadata::PrimariesText() const {
    if (!has_primaries) return {};
    return "R(" + Fixed(red_x, 4) + "," + Fixed(red_y, 4) + ") " +
           "G(" + Fixed(green_x, 4) + "," + Fixed(green_y, 4) + ") " +
           "B(" + Fixed(blue_x, 4) + "," + Fixed(blue_y, 4) + ") " +
           "WP(" + Fixed(white_x, 4) + "," + Fixed(white_y, 4) + ")";
}

std::string MasteringDisplayMetadata::LuminanceText() const {
    if (!has_luminance) return {};
    return Fixed(min_luminance, 4) + " - " + Fixed(max_luminance, 4) + " cd/m²";
}

std::string DolbyVisionMetadata::CompatibilityText() const {
    switch (compatibility_id) {
        case 0:  return "无兼容层（非 DV 设备无法正确显示）";
        case 1:  return "兼容 SDR (BT.709)";
        case 2:  return "兼容 HDR10 (PQ)";
        case 3:  return "兼容 SDR (BT.601)";
        case 4:  return "兼容 HLG (ARIB STD-B67)";
        case 5:  return "兼容 UHD SDR (BT.2020 + BT.1886)";
        case 6:  return "保留：HDR10 兼容（未实现）";
        default: return "保留/未知 (" + std::to_string(compatibility_id) + ")";
    }
}

std::string DolbyVisionMetadata::ProfileText() const {
    std::string text = std::to_string(profile);
    if (compatibility_id > 0) text += "." + std::to_string(compatibility_id);
    return text;
}

std::string HdrMetadataInfo::ToString() const {
    std::string out = format_name.empty() ? model::ToString(format) : format_name;
    if (mastering_display.Complete()) out += " ｜ MaxCLL/MaxFALL 见详表";
    if (dolby_vision.present) out += " ｜ DV " + dolby_vision.ProfileText();
    return out;
}

HdrFormat ClassifyHdrFormat(const ColorInfo& color, const HdrMetadataInfo& hdr) {
    if (hdr.dolby_vision.present) return HdrFormat::DolbyVision;
    if (hdr.has_hdr_vivid) return HdrFormat::HdrVivid;
    if (!color.TransferSpecified() && !hdr.mastering_display.present && !hdr.has_hdr10_plus &&
        !color.IsWideGamutPrimaries()) {
        return HdrFormat::Unknown;
    }
    if (color.transfer == TransferKind::Hlg) {
        return hdr.has_hdr10_plus ? HdrFormat::Hdr10Plus : HdrFormat::Hlg;
    }
    if (color.transfer == TransferKind::Pq) {
        if (hdr.has_hdr10_plus) return HdrFormat::Hdr10Plus;
        // HDR10 要求静态元数据齐备（SMPTE ST 2086 + MaxCLL/MaxFALL）；
        // 缺一项就只能算“裸 PQ”，多数播放器会按默认 1000nit 映射，亮度与色阶可能偏差。
        const bool static_meta = hdr.mastering_display.Complete() && hdr.content_light.present;
        return static_meta ? HdrFormat::Hdr10 : HdrFormat::Hdr10Basic;
    }
    // 没有 HDR 传递函数，但存在 HDR 线索（DV 已在上面处理；HDR10+ / Vivid 元数据
    // 通常伴随 PQ），SDR 传递函数 + HDR 元数据属于标注冲突，仍按 SDR 归类，
    // 由 video.color.* 规则给出告警。
    return hdr.has_hdr10_plus ? HdrFormat::Hdr10Plus : HdrFormat::Sdr;
}

}  // namespace model
}  // namespace videoeye
