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

// ============================ 行构造（UI / 报告共用） ============================
//
// 这两个函数以前定义在 ColorHdrAnalyzer.cpp 里、声明在 ColorHdrAnalyzer.h 里,
// 于是 UI 页面为了拿"把结果摆成表格行"这几十行展示逻辑, 必须 include 整个分析器头,
// 把 FFmpeg 侧的数据结构一起拖进自己的编译图(评审 P2: UI 不应直接依赖具体分析器)。
// 它们只吃 ColorHdrAnalysis、只吐纯 domain 类型 —— 放在这里才是它们该在的位置。

std::vector<ColorKeyValueRow> BuildColorRows(const ColorHdrAnalysis& analysis) {
    std::vector<ColorKeyValueRow> rows;
    if (!analysis.analyzed) return rows;

    const model::ColorInfo& c = analysis.color;
    auto add = [&rows](const std::string& key, const std::string& value, const std::string& note) {
        rows.push_back(ColorKeyValueRow{key, value, note});
    };

    add("像素格式", c.pixel_format.name.empty() ? "未标注" : c.pixel_format.name,
        c.pixel_format.bit_depth > 0
            ? ("每分量 " + std::to_string(c.pixel_format.bit_depth) + " 位")
            : "");

    const int depth = c.EffectiveBitDepth();
    add("位深", depth > 0 ? (std::to_string(depth) + " bit") : "未标注",
        depth >= 10 ? "高位深：HDR / 10bit 分发的最低要求"
                    : (depth == 8 ? "8 bit：SDR 常规位深，承载 PQ/HLG 会出现明显色带" : ""));

    add("色度采样", c.pixel_format.chroma_subsampling.empty() ? "RGB / 无" : c.pixel_format.chroma_subsampling,
        c.pixel_format.rgb ? "RGB 像素格式不存在色度下采样" : "");

    add("色彩原色 (Primaries)", c.primaries_name.empty() ? "未标注" : c.primaries_name,
        c.IsWideGamutPrimaries() ? "广色域：需搭配 BT.2020 矩阵与 PQ/HLG 传递函数"
                                 : "标准色域");

    add("传递函数 (Transfer)", c.transfer_name.empty() ? "未标注" : c.transfer_name,
        c.transfer == model::TransferKind::Pq  ? "SMPTE ST 2084，HDR10 的 EOTF"
        : c.transfer == model::TransferKind::Hlg ? "ARIB STD-B67，广播电视 HDR"
        : c.transfer == model::TransferKind::Sdr ? "传统 gamma 曲线（SDR）"
                                                 : "");

    add("矩阵系数 (Matrix)", c.matrix_name.empty() ? "未标注" : c.matrix_name,
        "决定 YUV ↔ RGB 转换，与 primaries 不一致会导致整体偏色");

    add("量化范围 (Range)", c.range_name.empty() ? "未标注" : c.range_name,
        (c.range == model::ColorRangeKind::Limited)
            ? "Limited：8bit 下有效值 16-235"
            : (c.range == model::ColorRangeKind::Full ? "Full：8bit 下有效值 0-255"
                                                      : "播放器通常按 Limited 猜测"));

    add("编码 Profile", c.profile_name.empty() ? "未标注" : c.profile_name,
        c.level > 0 ? ("Level " + std::to_string(c.level)) : "");

    if (c.width > 0 && c.height > 0) {
        add("分辨率", std::to_string(c.width) + " × " + std::to_string(c.height), "");
    }
    return rows;
}

std::vector<ColorKeyValueRow> BuildHdrRows(const ColorHdrAnalysis& analysis) {
    std::vector<ColorKeyValueRow> rows;
    if (!analysis.analyzed) return rows;

    const model::HdrMetadataInfo& h = analysis.hdr;
    const model::ColorInfo& c = analysis.color;
    auto add = [&rows](const std::string& key, const std::string& value, const std::string& note) {
        rows.push_back(ColorKeyValueRow{key, value, note});
    };

    add("HDR 格式", h.format_name.empty() ? model::ToString(h.format) : h.format_name,
        h.hdr ? "高动态范围内容" : "未识别到 HDR 线索");

    add("母版显示色域 (Primaries)",
        h.mastering_display.has_primaries ? h.mastering_display.PrimariesText() : "缺失",
        h.mastering_display.present ? "SMPTE ST 2086" : "未找到 SMPTE ST 2086 mastering display metadata");

    add("母版显示亮度 (Luminance)",
        h.mastering_display.has_luminance ? h.mastering_display.LuminanceText() : "缺失",
        h.mastering_display.has_luminance
            ? ("母版最低/最高亮度，播放端据此做色调映射")
            : "缺少该值时播放器通常按默认 1000 nit 映射");

    add("MaxCLL", h.content_light.present && h.content_light.max_cll > 0
                      ? (std::to_string(h.content_light.max_cll) + " cd/m²")
                      : "缺失",
        "CTA-861.3 最大内容光level");

    add("MaxFALL", h.content_light.present && h.content_light.max_fall > 0
                       ? (std::to_string(h.content_light.max_fall) + " cd/m²")
                       : "缺失",
        "CTA-861.3 最大帧平均光level");

    if (h.dolby_vision.present) {
        add("Dolby Vision Profile", h.dolby_vision.ProfileText(),
            h.dolby_vision.el_present ? "含增强层 (EL)" : "单层 (仅基础层)");
        add("Dolby Vision Level", std::to_string(h.dolby_vision.level), "");
        add("DV 兼容层", h.dolby_vision.CompatibilityText(),
            h.dolby_vision.compatibility_id == 0
                ? "非 DV 设备可能出现错误的颜色与亮度"
                : "存在向下兼容层，非 DV 设备可按 SDR/HDR10 显示");
        add("DV RPU (动态元数据)", h.dolby_vision.rpu_present ? "存在" : "未标注",
            h.dolby_vision.rpu_present ? "每帧动态 tones mapping 数据" : "");
    } else {
        add("Dolby Vision", "未检测到", "未找到 DOVI configuration record");
    }

    add("HDR10+ 动态元数据", h.has_hdr10_plus ? "存在" : "无",
        h.has_hdr10_plus ? "SMPTE ST 2094-40 逐场景/逐帧动态元数据" : "");

    if (h.has_hdr_vivid) {
        add("HDR Vivid", "存在", "CUVA 005 动态元数据");
    }

    std::string sources;
    for (size_t i = 0; i < h.sources.size(); ++i) {
        if (i > 0) sources += "、";
        sources += h.sources[i];
    }
    add("数据来源", sources.empty() ? "仅 AVCodecParameters" : sources,
        c.frame_side_data_checked ? "已尝试解码帧读取 AVFrame side data"
                                  : "未解码视频帧（动态元数据可能漏检）");

    return rows;
}

} // namespace model
} // namespace videoeye
