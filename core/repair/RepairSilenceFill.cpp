#include "core/repair/RepairSilenceFill.h"

#include <iomanip>
#include <sstream>

namespace videoeye {
namespace repair {
namespace {

// 「缺失」的容差：半个编码块。容器时基取整（TS 的 1/90000 落在 44100 上会抖
// ±1 采样）远小于这个值；而真的丢了一个编码块（aac 1024）刚好是它的两倍。
// 定成半个块而不是 0，是为了不把逐帧定位的取整误差当成空洞往产物里灌静音。
constexpr double kGapToleranceFractionOfBlock = 0.5;

// 编码器不声明块长时的兜底容差：20 ms。
constexpr double kFallbackToleranceSeconds = 0.02;

std::string FormatFixed(double value, int digits) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

std::string SampleCountText(int64_t samples) {
    return std::to_string(samples) + " 采样";
}

int64_t ToleranceSamples(const SilenceFillPolicy& policy) {
    if (policy.block_samples > 0)
        return static_cast<int64_t>(policy.block_samples * kGapToleranceFractionOfBlock);
    if (policy.sample_rate > 0)
        return static_cast<int64_t>(policy.sample_rate * kFallbackToleranceSeconds);
    return 0;
}

} // namespace

const char* SilenceFillReasonCode(SilenceFillVerdict verdict, bool fill_enabled) {
    switch (verdict) {
    case SilenceFillVerdict::NoGap:
        return "audio_silence_no_gap";
    case SilenceFillVerdict::Fill:
        return "audio_silence_inserted";
    case SilenceFillVerdict::NotRequested:
        return "audio_silence_not_requested";
    case SilenceFillVerdict::MergeContiguous:
        return "audio_silence_merge_contiguous";
    case SilenceFillVerdict::CopyTrackCannotSynthesize:
        return "audio_silence_track_copied";
    case SilenceFillVerdict::GapTooLarge:
        return "audio_silence_gap_too_large";
    case SilenceFillVerdict::BudgetExhausted:
        return "audio_silence_budget_exhausted";
    case SilenceFillVerdict::NoSampleRate:
        return "audio_silence_no_sample_rate";
    case SilenceFillVerdict::EncoderRejected:
        return "audio_silence_encoder_rejected";
    }
    // 理论上不可达；不返回 nullptr，免得调用方解引用崩掉。
    return fill_enabled ? "audio_silence_unknown" : "audio_silence_not_requested";
}

SilenceFillPlan PlanSilenceFill(int64_t expected_samples, int64_t actual_samples, const SilenceFillPolicy& policy,
                                int64_t inserted_samples_so_far) {
    SilenceFillPlan plan;
    plan.verdict = SilenceFillVerdict::NoGap;
    if (policy.sample_rate > 0)
        plan.gap.source_position_seconds = static_cast<double>(expected_samples) / policy.sample_rate;

    const int64_t deficit = actual_samples - expected_samples;
    // 时间戳回退 / 相等都不归这里管（回退有专门的时间轴修复链）。
    if (deficit <= 0)
        return plan;
    if (deficit <= ToleranceSamples(policy))
        return plan;

    plan.gap.missing_samples = deficit;
    if (policy.sample_rate > 0)
        plan.gap.gap_seconds = static_cast<double>(deficit) / policy.sample_rate;

    if (policy.sample_rate <= 0) {
        // 换算不出来就不能补：补多少采样都只能靠猜。
        plan.verdict = SilenceFillVerdict::NoSampleRate;
    } else if (policy.merge_contiguous) {
        plan.verdict = SilenceFillVerdict::MergeContiguous;
    } else if (!policy.fill_enabled) {
        plan.verdict = SilenceFillVerdict::NotRequested;
    } else if (!policy.reencode_available) {
        plan.verdict = SilenceFillVerdict::CopyTrackCannotSynthesize;
    } else if (plan.gap.gap_seconds > policy.max_gap_seconds) {
        plan.verdict = SilenceFillVerdict::GapTooLarge;
    } else {
        const double used_seconds = static_cast<double>(inserted_samples_so_far) / policy.sample_rate;
        if (used_seconds + plan.gap.gap_seconds > policy.max_total_seconds)
            plan.verdict = SilenceFillVerdict::BudgetExhausted;
        else
            plan.verdict = SilenceFillVerdict::Fill;
    }

    plan.gap.action = plan.verdict == SilenceFillVerdict::Fill ? "filled" : "not_filled";
    plan.gap.reason_code = SilenceFillReasonCode(plan.verdict, policy.fill_enabled);
    if (plan.verdict == SilenceFillVerdict::Fill)
        plan.gap.inserted_samples = plan.gap.missing_samples;
    return plan;
}

std::string SilenceFillGapText(const model::RepairSyntheticAudioGap& gap) {
    std::ostringstream out;
    out << "空洞 @" << FormatFixed(gap.source_position_seconds, 3) << " s（第 " << gap.after_audio_frames
        << " 个音频帧之后）：间隔 " << FormatFixed(gap.gap_seconds, 3) << " s ≈ 缺 "
        << SampleCountText(gap.missing_samples) << " → ";
    if (gap.reason_code == "audio_silence_inserted") {
        out << "已补入 " << SampleCountText(gap.inserted_samples) << "静音";
        return out.str();
    }
    if (gap.reason_code == "audio_silence_not_requested") {
        out << "未填补（未启用「空档补静音」，本次只记账不补）";
    } else if (gap.reason_code == "audio_silence_merge_contiguous") {
        out << "未填补（时间轴策略要求删除损坏区间，空洞按策略塌缩）";
    } else if (gap.reason_code == "audio_silence_track_copied") {
        out << "未填补（该轨为码流复制：不重编码就没有编码器，造不出这段采样；"
               "空洞在产物里原样保留）";
    } else if (gap.reason_code == "audio_silence_gap_too_large") {
        out << "未填补（超过单处上限；大段缺失应由「未恢复区间」如实呈现，不用静音掩盖）";
    } else if (gap.reason_code == "audio_silence_budget_exhausted") {
        out << "未填补（本次合成静音时长已达上限）";
    } else if (gap.reason_code == "audio_silence_no_sample_rate") {
        out << "未填补（采样率不可用，无法把秒数换算成采样数）";
    } else if (gap.reason_code == "audio_silence_encoder_rejected") {
        out << "只补入了 " << SampleCountText(gap.inserted_samples) << "（编码器未接受全部静音采样）";
    } else {
        out << "未填补（" << gap.reason_code << "）";
    }
    return out.str();
}

std::string SilenceFillSummary(const model::RepairSyntheticAudioFill& fill) {
    std::string prefix;
    if (fill.input_stream_index >= 0)
        prefix = "音频流 #" + std::to_string(fill.input_stream_index) + "：";
    if (!fill.inspected)
        return prefix + "未做音频空洞探测（本次没有扫描这条轨）";
    std::ostringstream out;
    out << prefix << "探测到 " << fill.gaps_detected << " 处空洞";
    if (fill.samples_inserted > 0)
        out << "；补入静音 " << SampleCountText(fill.samples_inserted) << "（" << FormatFixed(fill.seconds_inserted, 3)
            << " s 合成静音）";
    if (fill.gaps_refused > 0)
        out << "；" << fill.gaps_refused << " 处未填补";
    if (!fill.requested)
        out << "（未启用「空档补静音」）";
    else if (fill.coding_mode == "copy")
        out << "（该轨为码流复制，无法合成静音）";
    return out.str();
}

std::string SilenceFillBasisNote(const model::RepairSyntheticAudioFill& fill) {
    if (!fill.inspected)
        return "本次没有扫描这条音频轨，空洞探测未执行；这不等于「没有空洞」。";
    std::string note = fill.basis.empty() ? std::string("判据：相邻音频帧 PTS 间隔 vs 编码块长度") : fill.basis;
    if (fill.packets_without_duration > 0) {
        note += "；其中 " + std::to_string(fill.packets_without_duration) +
                " 个包没有可用时长，空洞判定不完整（缺时长的区段无法判断是否有洞）";
    }
    if (fill.coding_mode == "copy")
        note += "；该轨按码流复制输出，补静音需要重编码成容器能收的编码，本次没有走那条路";
    return note;
}

} // namespace repair
} // namespace videoeye
