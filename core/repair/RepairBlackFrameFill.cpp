#include "core/repair/RepairBlackFrameFill.h"

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

const char* BlackFillReasonCode(BlackFillVerdict verdict, bool fill_enabled) {
    switch (verdict) {
    case BlackFillVerdict::NoGap:
        return "black_fill_no_gap";
    case BlackFillVerdict::Fill:
        return "black_fill_inserted";
    case BlackFillVerdict::NotRequested:
        return "black_fill_not_requested";
    case BlackFillVerdict::MergeContiguous:
        return "black_fill_merge_contiguous";
    case BlackFillVerdict::GapTooLarge:
        return "black_fill_gap_too_large";
    case BlackFillVerdict::NotIntegral:
        return "black_fill_not_integral";
    case BlackFillVerdict::NoFrameRate:
        return "black_fill_no_frame_rate";
    case BlackFillVerdict::BudgetExhausted:
        return "black_fill_budget_exhausted";
    case BlackFillVerdict::EncoderRejected:
        return "black_fill_encoder_rejected";
    }
    // 理论上不可达；不返回 nullptr，免得调用方解引用崩掉。
    return fill_enabled ? "black_fill_unknown" : "black_fill_not_requested";
}

BlackFillPlan PlanBlackFill(double previous_seconds, double current_seconds, const BlackFillPolicy& policy,
                            int64_t synthetic_frames_so_far) {
    BlackFillPlan plan;
    plan.gap.source_position_seconds = previous_seconds;
    plan.verdict = BlackFillVerdict::NoGap;

    // 时间戳拿不到 / 回退：都不归这里管（回退有专门的时间轴修复链）。
    if (!std::isfinite(previous_seconds) || !std::isfinite(current_seconds) || current_seconds <= previous_seconds)
        return plan;

    const double delta = current_seconds - previous_seconds;
    if (!(policy.frame_seconds > 0.0)) {
        // 没有可信帧时长时连「是不是洞」都判不了：不能只说"没有空洞"。
        plan.verdict = BlackFillVerdict::NoFrameRate;
        plan.gap.gap_seconds = delta;
        plan.gap.action = "not_filled";
        plan.gap.reason_code = BlackFillReasonCode(plan.verdict, policy.fill_enabled);
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
        plan.verdict = BlackFillVerdict::NoFrameRate;
    } else if (std::fabs(ratio - rounded) > kIntegralTolerance) {
        plan.verdict = BlackFillVerdict::NotIntegral;
    } else if (policy.merge_contiguous) {
        plan.verdict = BlackFillVerdict::MergeContiguous;
    } else if (!policy.fill_enabled) {
        plan.verdict = BlackFillVerdict::NotRequested;
    } else if (delta > policy.max_gap_seconds) {
        plan.verdict = BlackFillVerdict::GapTooLarge;
    } else if (synthetic_frames_so_far + plan.gap.missing_frames > policy.max_total_frames) {
        plan.verdict = BlackFillVerdict::BudgetExhausted;
    } else {
        plan.verdict = BlackFillVerdict::Fill;
    }

    plan.gap.action = plan.verdict == BlackFillVerdict::Fill ? "filled" : "not_filled";
    plan.gap.reason_code = BlackFillReasonCode(plan.verdict, policy.fill_enabled);
    if (plan.verdict == BlackFillVerdict::Fill)
        plan.gap.inserted_frames = plan.gap.missing_frames;
    return plan;
}

std::string BlackFillGapText(const model::RepairSyntheticGap& gap) {
    std::ostringstream out;
    out << "空洞 @" << FormatFixed(gap.source_position_seconds, 3) << " s（第 " << gap.after_decoded_frames
        << " 个解码帧之后）：间隔 " << FormatFixed(gap.gap_seconds, 3) << " s ≈ 缺 "
        << FrameCountText(gap.missing_frames) << " → ";
    if (gap.reason_code == "black_fill_inserted") {
        out << "已插入 " << FrameCountText(gap.inserted_frames) << "合成黑帧";
        return out.str();
    }
    if (gap.reason_code == "black_fill_not_requested") {
        out << "未填补（未启用「空档补黑帧」，本次只记账不插帧）";
    } else if (gap.reason_code == "black_fill_merge_contiguous") {
        out << "未填补（时间轴策略要求删除损坏区间，空洞按策略塌缩）";
    } else if (gap.reason_code == "black_fill_gap_too_large") {
        out << "未填补（超过单处上限；大段缺失应由「未恢复区间」如实呈现，不用黑场掩盖）";
    } else if (gap.reason_code == "black_fill_not_integral") {
        out << "未填补（间隔不是标称帧时长的整数倍，源时间戳本身可疑，不猜）";
    } else if (gap.reason_code == "black_fill_no_frame_rate") {
        out << "未填补（帧率不是源侧证据，无法把秒数换算成帧数）";
    } else if (gap.reason_code == "black_fill_budget_exhausted") {
        out << "未填补（本次合成帧数已达上限）";
    } else if (gap.reason_code == "black_fill_encoder_rejected") {
        out << "只插入了 " << FrameCountText(gap.inserted_frames) << "（编码器未接受全部合成帧；后续真实帧仍照常写入）";
    } else {
        out << "未填补（" << gap.reason_code << "）";
    }
    return out.str();
}

std::string BlackFillSummary(const model::RepairSyntheticFill& fill) {
    if (!fill.inspected)
        return "未做空洞探测（本次没有跑视频解码链）";
    std::ostringstream out;
    out << "探测到 " << fill.gaps_detected << " 处空洞";
    if (fill.gaps_detected == 0)
        return out.str();
    out << "；填补 " << fill.frames_inserted << " 帧（" << FormatFixed(fill.seconds_inserted, 3) << " s 合成黑帧）";
    if (fill.gaps_refused > 0)
        out << "；" << fill.gaps_refused << " 处未填补";
    if (!fill.requested)
        out << "（未启用「空档补黑帧」）";
    return out.str();
}

std::string BlackFillBasisNote(const model::RepairSyntheticFill& fill) {
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
