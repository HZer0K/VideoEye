#pragma once

// ==========================================================================
// 「哪些 Profile / 色度 / 位深组合算 Supported」的判据（能力门禁）
//
// 为什么单独抽成 header-only：
//   这些判据决定"是否拿码流层的宽高位深去和容器层做严格一致性比对"，
//   判错的影响是双向的 —— 放太宽会把尚未验证的组合宣传成可信结论，
//   收太窄则把本来完全正确的流判成 Partial、白白错过容器比对（比如
//   HEVC 4:2:2 10bit 的贡献流，长期因为 chroma_format_idc != 1 被拦在门外）。
//   判据必须能被单测直接打到，不能只活在 BitstreamAnalyzer.cpp 的匿名命名空间里。
//
// 口径（2026-10-20 修订，见 C2）：
//   1. 色度采样：4:2:0 / 4:2:2 / 4:4:4 全部放行（chroma_format_idc ∈ {1,2,3}）。
//      原先拦它的理由是"没有样本矩阵"，但这条理由已经在 HEVC RExt / H.264
//      High 4:2:2 的真实流上失效了；而 4:2:2 / 4:4:4 恰恰是贡献流、屏幕内容
//      与专业摄影的主流 —— 卡这一条等于把这些文件整体排除在容器比对之外。
//      4:0:0（mono）继续拦：面板的"位流 vs 容器"表没有它的数据通路。
//   2. 位深：8 / 10 / 12 bit 放行。12bit 是 RExt 的标准档位（4:2:2 12bit 是
//      广电贡献流的常见组合），原先只有 8/10。
//   3. Profile：按各家实际流通的那一批放行，见下面的 gap 函数。
//
// 返回 CapabilityGap 而不是 bool：判定之外还要给 human-readable 的原因，
// UI 的 tooltip / 报告要照这个值解释"为什么是 Partial"。
// ==========================================================================

#include <string>

#include "core/domain/model/Av1BitstreamInfo.h"
#include "core/domain/model/H264BitstreamInfo.h"
#include "core/domain/model/HevcBitstreamInfo.h"

namespace videoeye {
// 具名子命名空间必须跟目录走（scripts/audit_namespace_layout.py 的 R3），
// 所以这里是 codec —— 与 core/analysis/codec 同名的那一层。
namespace codec {

// 哪一项没通过验证。None = 全部通过。
enum class CapabilityGap {
    None = 0,
    Profile,      // profile / general_profile_idc 不在验证过的清单里
    ChromaFormat, // chroma_format_idc 不在 {4:2:0, 4:2:2, 4:4:4}
    BitDepth,     // 位深不在 {8, 10, 12}
};

inline const char* GapFieldName(CapabilityGap gap) {
    switch (gap) {
    case CapabilityGap::Profile:
        return "Profile";
    case CapabilityGap::ChromaFormat:
        return "色度采样";
    case CapabilityGap::BitDepth:
        return "位深";
    case CapabilityGap::None:
        return "无";
    default:
        return "未知";
    }
}

// ---- 单维度判据 ----

// 4:2:0 / 4:2:2 / 4:4:4 放行；4:0:0（0=mono）与越界值拦下。
inline bool IsValidatedChromaFormat(int chroma_format_idc) {
    return chroma_format_idc >= 1 && chroma_format_idc <= 3;
}

inline bool IsValidatedBitDepth(int bit_depth) {
    return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
}

// H.264 profile_idc 放行清单：
//   66  Baseline / 77 Main / 88 Extended / 100 High / 110 High 10
//   122 High 4:2:2（含 Intra）/ 244 High 4:4:4 Predictive（含 Intra）
//   44  CAVLC 4:4:4 Intra
//   118 Multiview High / 128 Stereo High
// 拦下的主要是不带 Recording 场景的可伸缩/SVC 档位（83/86/118/128/138 里的
// 部分），真实文件里几乎见不到，也没有样本矩阵覆盖。
inline bool IsValidatedH264Profile(int profile_idc) {
    switch (profile_idc) {
    case 44:
    case 66:
    case 77:
    case 88:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 244:
        return true;
    default:
        return false;
    }
}

// HEVC general_profile_idc 放行清单（FFmpeg 的枚举）：
//   1=Main 2=Main10 3=Main Still Picture 4=Range Extensions（4:2:2/4:4:4 的归处）
//   9=Screen-Extended Main 10=Screen-Extended Main 10 11=Screen-Extended High Throughput
// Multiview Main / Scalable Main / 3D Main（6/7/8）没有样本，不放行。
inline bool IsValidatedHevcProfile(int general_profile_idc) {
    switch (general_profile_idc) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 9:
    case 10:
    case 11:
        return true;
    default:
        return false;
    }
}

// AV1 seq_profile 放行清单：0=Main（8/10bit 4:2:0）1=High（4:4:4）2=Professional（8/10/12bit）。
// 解析层已把 >2 直接判非法，这里再判一次只为口径统一 —— 门禁和解析是两件事，
// 哪天解析层放宽了，门禁仍然该按"有没有样本矩阵"说话。
inline bool IsValidatedAv1Profile(int seq_profile) {
    return seq_profile >= 0 && seq_profile <= 2;
}

// ---- 组合判据 ----

inline CapabilityGap H264ProfileGap(int profile_idc) {
    return IsValidatedH264Profile(profile_idc) ? CapabilityGap::None : CapabilityGap::Profile;
}

inline CapabilityGap H264SpsGap(const model::H264SpsInfo& sps) {
    if (!IsValidatedH264Profile(sps.profile_idc))
        return CapabilityGap::Profile;
    if (!IsValidatedChromaFormat(sps.chroma_format_idc))
        return CapabilityGap::ChromaFormat;
    if (!IsValidatedBitDepth(sps.BitDepthLuma()))
        return CapabilityGap::BitDepth;
    return CapabilityGap::None;
}

inline CapabilityGap HevcSpsGap(const model::HevcSpsInfo& sps) {
    if (!IsValidatedHevcProfile(sps.general_profile_idc))
        return CapabilityGap::Profile;
    if (!IsValidatedChromaFormat(sps.chroma_format_idc))
        return CapabilityGap::ChromaFormat;
    if (!IsValidatedBitDepth(sps.BitDepthLuma()))
        return CapabilityGap::BitDepth;
    return CapabilityGap::None;
}

// AV1 的门禁维度和 H.264/HEVC 不同：没有 chroma_format_idc，色度是
// subsampling_x/y 两个位，取值天然只有 4:2:0 / 4:2:2 / 4:4:4 —— 唯一要拦的
// 色度情形是 mono_chrome（4:0:0），与另两个 codec 拦 chroma_format_idc==0 同口径。
inline CapabilityGap Av1SeqGap(const model::Av1SequenceHeaderInfo& sh) {
    if (!IsValidatedAv1Profile(sh.profile))
        return CapabilityGap::Profile;
    if (sh.color_config.monochrome)
        return CapabilityGap::ChromaFormat;
    if (!IsValidatedBitDepth(sh.BitDepth()))
        return CapabilityGap::BitDepth;
    return CapabilityGap::None;
}

// 向后兼容的布尔包装（老调用点形态）
inline bool IsValidatedH264Sps(const model::H264SpsInfo& sps) {
    return H264SpsGap(sps) == CapabilityGap::None;
}

inline bool IsValidatedHevcSps(const model::HevcSpsInfo& sps) {
    return HevcSpsGap(sps) == CapabilityGap::None;
}

// ---- 原因文案 ----

// codec_label 例："H.264" / "HEVC"；field_values 由调用方给具体数值串。
inline std::string GapNote(const std::string& codec_label, CapabilityGap gap, const std::string& profile_text,
                           const std::string& chroma_text, const std::string& bit_depth_text) {
    if (gap == CapabilityGap::None)
        return std::string();
    std::string out = codec_label + " 参数集已解析，但该组合尚无完整真实样本矩阵：";
    out += GapFieldName(gap);
    out += " 不在验证过的范围内（";
    out += profile_text + " / " + chroma_text + " / " + bit_depth_text;
    out += "），字段仅供参考，不参与容器一致性比对";
    return out;
}

} // namespace codec
} // namespace videoeye
