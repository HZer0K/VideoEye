#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <string>
#include <vector>

namespace videoeye {
namespace diagnostics {
namespace detail {

// 色彩与 HDR 规则（video.color.*）。调用方（CheckRule 前缀路由）保证 rule.id
// 属于本类，故此处不再重复罗列各 id。
std::vector<model::DiagnosticIssue> CheckColorHdrRules(const model::QcRule& rule,
                                                       const model::AnalysisResult& result,
                                                       const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- 色彩与 HDR ----------
    const model::ColorHdrAnalysis& analysis = result.color_hdr;
    if (!analysis.analyzed) return issues;

    const model::ColorInfo& color = analysis.color;
    const model::HdrMetadataInfo& hdr = analysis.hdr;
    const int stream = color.stream_index;

    if (rule.id == "video.color.hdr_missing_mastering") {
        // HLG 不依赖静态元数据（广播电视场景本就没有），只对 PQ 要求 SMPTE ST 2086
        if (color.IsPqTransfer() && !hdr.mastering_display.Complete() &&
            !hdr.dolby_vision.present) {
            std::string missing = "母版显示信息";
            missing += hdr.mastering_display.present ? "不完整" : "缺失";
            issues.push_back(make_issue(1.0,
                "传递函数为 PQ，但 SMPTE ST 2086 " + missing +
                "，播放器只能按默认色彩体量做色调映射。",
                model::TimeRange::Global(), stream));
        }
    } else if (rule.id == "video.color.hdr_missing_light_level") {
        if (color.IsPqTransfer() && !hdr.content_light.Complete() &&
            !hdr.dolby_vision.present) {
            issues.push_back(make_issue(1.0,
                "传递函数为 PQ，但 MaxCLL/MaxFALL " +
                (hdr.content_light.present ? std::string("不完整（只有其中一项）")
                                           : std::string("缺失")) +
                "，接收端无法预判内容亮度。",
                model::TimeRange::Global(), stream));
        }
    } else if (rule.id == "video.color.hdr_low_bitdepth") {
        const int depth = color.EffectiveBitDepth();
        if (color.IsHdrTransfer() && depth > 0 && depth < 10) {
            issues.push_back(make_issue(static_cast<double>(depth),
                "传递函数为 " + color.transfer_name + "，但像素格式 " +
                (color.pixel_format.name.empty() ? std::string("未知")
                                                 : color.pixel_format.name) +
                " 只有 " + std::to_string(depth) + " bit，HDR 需要 ≥ 10 bit。",
                model::TimeRange::Global(), stream));
        }
    } else if (rule.id == "video.color.wide_gamut_sdr_transfer") {
        if (color.IsWideGamutPrimaries() && color.transfer == model::TransferKind::Sdr) {
            issues.push_back(make_issue(1.0,
                "色彩原色为 " + color.primaries_name + "，但传递函数为 " +
                color.transfer_name +
                "：广色域 + SDR gamma 的组合容易被播放端误判，"
                "要么补 HDR 元数据并按 HDR 分发，要么回到 BT.709 SDR。",
                model::TimeRange::Global(), stream));
        }
    } else if (rule.id == "video.color.matrix_mismatch") {
        // 同代标准组合才是常态：709+709 / 2020+2020NCL / SDR 601+601
        bool mismatch = false;
        std::string detail;
        if (color.primaries == model::ColorPrimariesKind::Bt2020 &&
            color.matrix == model::MatrixKind::Bt709) {
            mismatch = true;
            detail = "BT.2020 原色配 BT.709 矩阵（应为 BT.2020 NCL）";
        } else if (color.primaries == model::ColorPrimariesKind::Bt709 &&
                   (color.matrix == model::MatrixKind::Bt2020Ncl ||
                    color.matrix == model::MatrixKind::Bt2020Cl)) {
            mismatch = true;
            detail = "BT.709 原色配 BT.2020 矩阵（应为 BT.709）";
        } else if (color.primaries == model::ColorPrimariesKind::Bt709 &&
                   color.matrix == model::MatrixKind::Bt601) {
            mismatch = true;
            detail = "BT.709 原色配 BT.601 矩阵（常见于 SD/HD 转换漏标）";
        } else if (color.primaries == model::ColorPrimariesKind::Bt601 &&
                   color.matrix == model::MatrixKind::Bt709) {
            mismatch = true;
            detail = "BT.601 原色配 BT.709 矩阵（SD 素材按 HD 矩阵还原会偏色）";
        }
        if (mismatch) {
            issues.push_back(make_issue(1.0,
                detail + "：YUV→RGB 按错误矩阵还原，画面整体偏色。",
                model::TimeRange::Global(), stream));
        }
    } else if (rule.id == "video.color.range_conflict") {
        std::string detail;
        if (color.pixel_format.ImpliesFullRange() &&
            color.range == model::ColorRangeKind::Limited) {
            detail = "像素格式 " + color.pixel_format.name +
                     " 隐含 full range，色彩范围却被标注为 Limited";
        } else if (color.pixel_format.rgb && color.range == model::ColorRangeKind::Limited) {
            detail = "RGB 数据不存在 limited range，色彩范围却被标注为 Limited";
        } else if (color.pixel_format.rgb && !color.pixel_format.chroma_subsampling.empty()) {
            detail = "RGB 像素格式却标注了色度下采样 " + color.pixel_format.chroma_subsampling;
        }
        if (!detail.empty()) {
            issues.push_back(make_issue(1.0, detail + "：灰阶会被整体抬高或压低。",
                                        model::TimeRange::Global(), stream));
        }
    } else if (rule.id == "video.color.unspecified") {
        std::vector<std::string> missing_items;
        if (!color.PrimariesSpecified()) missing_items.push_back("primaries");
        if (!color.TransferSpecified()) missing_items.push_back("transfer");
        if (!color.MatrixSpecified()) missing_items.push_back("matrix");
        if (!color.RangeSpecified()) missing_items.push_back("range");
        if (!missing_items.empty()) {
            std::string list;
            for (size_t i = 0; i < missing_items.size(); ++i) {
                if (i > 0) list += " / ";
                list += missing_items[i];
            }
            issues.push_back(make_issue(static_cast<double>(missing_items.size()),
                "未标注的色彩项: " + list + "。缺失时播放器按默认值猜测，"
                "广色域/HDR 内容几乎必然颜色错误。",
                model::TimeRange::Global(), stream,
                static_cast<int>(missing_items.size())));
        }
    } else if (rule.id == "video.color.dv_no_compatibility") {
        if (hdr.dolby_vision.present && hdr.dolby_vision.compatibility_id == 0) {
            issues.push_back(make_issue(1.0,
                "Dolby Vision profile " + std::to_string(hdr.dolby_vision.profile) +
                " 的 bl_signal_compatibility_id 为 0（" + hdr.dolby_vision.CompatibilityText() +
                "），非 DV 设备回放颜色会明显错误。",
                model::TimeRange::Global(), stream));
        }
    }
    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye