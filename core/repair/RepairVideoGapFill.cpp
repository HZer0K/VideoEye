#include "core/repair/RepairVideoGapFill.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace videoeye {
namespace repair {
namespace {

// 帧间隔的容差：正常 VFR 抖动与时间基取整都落在 ±0.5 帧以内，超过 1.5 帧
// 才当作「有洞」。用 1.5 而不是 1.0 是为了不把 29.97 这类分数帧率的取整误差
// 当成空洞（实测 30000/1001 在毫秒粒度的容器里会抖到 1.03 帧）。
constexpr double kGapRatioThreshold = 1.5;

// 「整帧缺失」的判据：间隔必须接近帧时长的整数倍。0.12 帧 ≈ 5 ms @25fps，
// 足以容纳容器时间基的取整，又拦得住「3.7 帧」这种只能是时间戳坏了的跳变。
constexpr double kIntegralTolerance = 0.12;

std::string FormatFixed(double value, int digits) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

std::string FrameCountText(int64_t frames) {
    return std::to_string(frames) + " 帧";
}

} // namespace

const char* VideoGapFillReasonCode(VideoGapFillVerdict verdict, bool fill_enabled) {
    switch (verdict) {
    case VideoGapFillVerdict::NoGap:
        return "video_gap_fill_no_gap";
    case VideoGapFillVerdict::Fill:
        return "video_gap_fill_inserted";
    case VideoGapFillVerdict::FillWithBlackFallback:
        return "video_gap_fill_fallback_black";
    case VideoGapFillVerdict::NotRequested:
        return "video_gap_fill_not_requested";
    case VideoGapFillVerdict::MergeContiguous:
        return "video_gap_fill_merge_contiguous";
    case VideoGapFillVerdict::GapTooLarge:
        return "video_gap_fill_gap_too_large";
    case VideoGapFillVerdict::NotIntegral:
        return "video_gap_fill_not_integral";
    case VideoGapFillVerdict::NoFrameRate:
        return "video_gap_fill_no_frame_rate";
    case VideoGapFillVerdict::BudgetExhausted:
        return "video_gap_fill_budget_exhausted";
    case VideoGapFillVerdict::EncoderRejected:
        return "video_gap_fill_encoder_rejected";
    }
    // 理论上不可达；不返回 nullptr，免得调用方解引用崩掉。
    return fill_enabled ? "video_gap_fill_unknown" : "video_gap_fill_not_requested";
}

const char* VideoGapFillKind(VideoGapFillVerdict verdict, model::VideoGapFillContent requested) {
    switch (verdict) {
    case VideoGapFillVerdict::Fill:
    case VideoGapFillVerdict::EncoderRejected:
        // Fill 按请求的内容；EncoderRejected 是「按请求插了但没插满」，实际送进去的
        // 仍是请求的那一种，所以内容类型也跟请求走。
        return requested == model::VideoGapFillContent::PreviousFrame ? "repeated_frame" : "black_frame";
    case VideoGapFillVerdict::FillWithBlackFallback:
        // 请求的是重复画面，但该处没有可重复的帧 —— 实际填的是黑帧，必须这么记。
        return "black_frame";
    default:
        return "";
    }
}

VideoGapFillPlan PlanVideoGapFill(double previous_seconds, double current_seconds, const VideoGapFillPolicy& policy,
                                  int64_t synthetic_frames_so_far) {
    VideoGapFillPlan plan;
    plan.gap.source_position_seconds = previous_seconds;
    plan.verdict = VideoGapFillVerdict::NoGap;

    // 时间戳拿不到 / 回退：都不归这里管（回退有专门的时间轴修复链）。
    if (!std::isfinite(previous_seconds) || !std::isfinite(current_seconds) || current_seconds <= previous_seconds)
        return plan;

    const double delta = current_seconds - previous_seconds;
    if (!(policy.frame_seconds > 0.0)) {
        // 没有可信帧时长时连「是不是洞」都判不了：不能只说"没有空洞"。
        plan.verdict = VideoGapFillVerdict::NoFrameRate;
        plan.gap.gap_seconds = delta;
        plan.gap.action = "not_filled";
        plan.gap.reason_code = VideoGapFillReasonCode(plan.verdict, policy.fill_enabled);
        plan.gap.text = "空洞 @" + FormatFixed(previous_seconds, 3) + " s：帧时长不可用，无法判定";
        return plan;
    }

    const double ratio = delta / policy.frame_seconds;
    if (ratio < kGapRatioThreshold)
        return plan;

    plan.gap.gap_seconds = delta;
    // 先按「整帧缺失」算出帧数，后面无论填不填都用它记账。
    const double rounded = std::round(ratio);
    const int64_t missing = static_cast<int64_t>(rounded) - 1;
    plan.gap.missing_frames = missing > 0 ? missing : 1;

    if (!policy.frame_rate_from_source) {
        plan.verdict = VideoGapFillVerdict::NoFrameRate;
    } else if (std::fabs(ratio - rounded) > kIntegralTolerance) {
        plan.verdict = VideoGapFillVerdict::NotIntegral;
    } else if (policy.merge_contiguous) {
        plan.verdict = VideoGapFillVerdict::MergeContiguous;
    } else if (!policy.fill_enabled) {
        plan.verdict = VideoGapFillVerdict::NotRequested;
    } else if (delta > policy.max_gap_seconds) {
        plan.verdict = VideoGapFillVerdict::GapTooLarge;
    } else if (synthetic_frames_so_far + plan.gap.missing_frames > policy.max_total_frames) {
        plan.verdict = VideoGapFillVerdict::BudgetExhausted;
    } else if (policy.content == model::VideoGapFillContent::PreviousFrame && !policy.previous_frame_available) {
        // 要重复画面但手里没有 —— 这里**不是**「不填」：位置照样要占住，否则产物
        // 时间轴重新错位。改用黑帧，并让 reason_code 说明「退回了」。
        plan.verdict = VideoGapFillVerdict::FillWithBlackFallback;
    } else {
        plan.verdict = VideoGapFillVerdict::Fill;
    }

    const bool fills =
        plan.verdict == VideoGapFillVerdict::Fill || plan.verdict == VideoGapFillVerdict::FillWithBlackFallback;
    plan.gap.action = fills ? "filled" : "not_filled";
    plan.gap.reason_code = VideoGapFillReasonCode(plan.verdict, policy.fill_enabled);
    plan.gap.synthetic_kind = VideoGapFillKind(plan.verdict, policy.content);
    if (fills)
        plan.gap.inserted_frames = plan.gap.missing_frames;
    return plan;
}

std::string VideoGapFillGapText(const model::RepairSyntheticGap& gap) {
    std::ostringstream out;
    out << "空洞 @" << FormatFixed(gap.source_position_seconds, 3) << " s（第 " << gap.after_decoded_frames
        << " 个解码帧之后）：间隔 " << FormatFixed(gap.gap_seconds, 3) << " s ≈ 缺 "
        << FrameCountText(gap.missing_frames) << " → ";
    if (gap.reason_code == "video_gap_fill_inserted") {
        out << (gap.synthetic_kind == "repeated_frame"
                    ? "已插入 " + FrameCountText(gap.inserted_frames) + "重复画面（冻结上一帧，不是源里的新画面）"
                    : "已插入 " + FrameCountText(gap.inserted_frames) + "合成黑帧");
        return out.str();
    }
    if (gap.reason_code == "video_gap_fill_fallback_black") {
        out << "已插入 " << FrameCountText(gap.inserted_frames)
            << "合成黑帧（该处没有可用的上一帧画面，无法重复，已退回黑帧）";
        return out.str();
    }
    if (gap.reason_code == "video_gap_fill_not_requested") {
        out << "未填补（未启用「空档补帧」，本次只记账不插帧）";
    } else if (gap.reason_code == "video_gap_fill_merge_contiguous") {
        out << "未填补（时间轴策略要求删除损坏区间，空洞按策略塌缩）";
    } else if (gap.reason_code == "video_gap_fill_gap_too_large") {
        out << "未填补（超过单处上限；大段缺失应由「未恢复区间」如实呈现，不用填充掩盖）";
    } else if (gap.reason_code == "video_gap_fill_not_integral") {
        out << "未填补（间隔不是标称帧时长的整数倍，源时间戳本身可疑，不猜）";
    } else if (gap.reason_code == "video_gap_fill_no_frame_rate") {
        out << "未填补（帧率不是源侧证据，无法把秒数换算成帧数）";
    } else if (gap.reason_code == "video_gap_fill_budget_exhausted") {
        out << "未填补（本次合成帧数已达上限）";
    } else if (gap.reason_code == "video_gap_fill_encoder_rejected") {
        out << "只插入了 " << FrameCountText(gap.inserted_frames) << "（编码器未接受全部合成帧；后续真实帧仍照常写入）";
    } else {
        out << "未填补（" << gap.reason_code << "）";
    }
    return out.str();
}

const char* VideoGapFillContentCode(model::VideoGapFillContent content) {
    return content == model::VideoGapFillContent::PreviousFrame ? "repeated_frame" : "black_frame";
}

std::string VideoGapFillContentName(const std::string& code) {
    // 故意短：这个串会被嵌进别的句子（timeline_note / 汇总行 / 限制项），带括号
    // 说明会套出「…（重复画面（冻结上一帧））（合成帧没有…）」这种双括号。
    // 「冻结上一帧」这件事由逐处空洞文案说清，那里有足够的上下文。
    if (code == "repeated_frame")
        return "重复画面";
    if (code == "black_frame")
        return "合成黑帧";
    return "合成内容";
}

std::string VideoGapFillSummary(const model::RepairSyntheticFill& fill) {
    if (!fill.inspected)
        return "未做空洞探测（本次没有跑视频解码链）";
    std::ostringstream out;
    out << "探测到 " << fill.gaps_detected << " 处空洞";
    if (fill.gaps_detected == 0)
        return out.str();

    // 汇总里的内容标签必须反映「实际填进去的是什么」：用户要重复画面，但有几处
    // 退回了黑帧时，笼统写「合成黑帧」或笼统写「重复画面」都会误导。
    const int64_t inserted = fill.frames_inserted;
    const int64_t fallback = std::max<int64_t>(0, std::min<int64_t>(fill.fallback_black_frames, inserted));
    std::string label;
    if (inserted > 0) {
        if (fallback == 0)
            label = VideoGapFillContentName(fill.content);
        else if (fallback == inserted)
            label = "合成黑帧（全部退回）";
        else
            label = "合成内容：重复画面 + 黑帧";
    }
    out << "；填补 " << inserted << " 帧（" << FormatFixed(fill.seconds_inserted, 3) << " s " << label << "）";
    if (fallback > 0 && fallback < inserted)
        out << "；其中 " << fallback << " 帧因该处无可用上一帧而改用黑帧";
    if (fill.gaps_refused > 0)
        out << "；" << fill.gaps_refused << " 处未填补";
    if (!fill.requested)
        out << "（未启用「空档补帧」）";
    return out.str();
}

std::string VideoGapFillBasisNote(const model::RepairSyntheticFill& fill) {
    if (!fill.inspected)
        return "本次没有跑视频解码链，空洞探测未执行；这不等于「没有空洞」。";
    std::string note = fill.basis.empty() ? std::string("判据：源解码帧 PTS 间隔 vs 标称帧率") : fill.basis;
    if (fill.frames_without_source_pts > 0) {
        note += "；其中 " + std::to_string(fill.frames_without_source_pts) +
                " 帧没有源时间戳，空洞探测不完整（缺时间戳的区段无法判断是否有洞）";
    }
    return note;
}

} // namespace repair
} // namespace videoeye
