#include "core/repair/RepairDisplayOrder.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "core/analysis/codec/FrameOrderTracker.h"
#include "core/repair/RepairBoxWriter.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairSampleScan.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace videoeye {
namespace repair {
namespace {

// 单个访问单元的内存上限。H.264 的关键帧通常在几十 KB 量级，8 MiB 已经远超
// 正常值；超过就说明边界推断错了 —— 宁可判"不可用"，也不把几百 MB 读进内存。
constexpr uint32_t kMaxAuBytes = 8u * 1024u * 1024u;

// 逐帧明细上限：200k 帧 ≈ 2 小时 25 fps。超过后追踪器会置 truncated，
// 我们据此把未扫完的末段整段丢掉（见 MeasureDisplayOrder）。
constexpr std::size_t kMaxTrackedFrames = 200000;

int CodecIdFromName(const std::string& codec) {
    if (codec == "h264")
        return AV_CODEC_ID_H264;
    if (codec == "hevc")
        return AV_CODEC_ID_HEVC;
    return AV_CODEC_ID_NONE;
}

std::string JoinSegments(uint64_t segments) {
    return std::to_string(segments) + " 个随机接入点段";
}

} // namespace

RepairDisplayOrderPlan DeriveDisplayOrder(std::vector<RepairDisplayFrame> frames,
                                          const RepairFrameDurationEvidence& duration) {
    RepairDisplayOrderPlan plan;
    plan.duration = duration;
    plan.frames_observed = frames.size();

    if (frames.empty()) {
        plan.unavailable_reason = "没有逐帧 POC 实测结果，无法推导显示顺序";
        plan.confidence = "unknown";
        return plan;
    }
    for (const auto& frame : frames) {
        if (frame.leading) {
            plan.unavailable_reason = "含开放 GOP 的前导图像（RASL/RADL），其 POC 按定义小于所属随机接入点，"
                                      "当前不做显示顺序推导";
            plan.confidence = "unknown";
            return plan;
        }
    }

    // 分段：非首帧的每个随机接入点开一段。H.264 的 irap 即 IDR，HEVC 含 CRA ——
    // 两种编码的 POC 都在这里归零，跨段比较 POC 没有意义。
    std::vector<std::size_t> segment_start{0};
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].irap)
            segment_start.push_back(i);
    }
    plan.rap_segments = segment_start.size();

    const bool starts_at_rap = frames.front().irap;
    std::size_t duplicate_poc = 0;
    uint64_t display = 0;
    std::vector<std::size_t> order;
    for (std::size_t segment = 0; segment < segment_start.size(); ++segment) {
        const std::size_t begin = segment_start[segment];
        const std::size_t end = segment + 1 < segment_start.size() ? segment_start[segment + 1] : frames.size();
        order.clear();
        order.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i)
            order.push_back(i);
        // 稳定排序 + POC 相同按解码序：重复 POC 的样本顺序不能靠排序算法实现细节决定。
        std::stable_sort(order.begin(), order.end(), [&frames](std::size_t lhs, std::size_t rhs) {
            if (frames[lhs].poc != frames[rhs].poc)
                return frames[lhs].poc < frames[rhs].poc;
            return frames[lhs].decode_index < frames[rhs].decode_index;
        });
        for (std::size_t rank = 0; rank < order.size(); ++rank) {
            RepairDisplayFrame& frame = frames[order[rank]];
            frame.display_index = display++;
            frame.segment = static_cast<uint32_t>(segment);
            if (rank > 0 && frame.poc == frames[order[rank - 1]].poc)
                ++duplicate_poc;
        }
    }

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const int64_t shift =
            static_cast<int64_t>(frames[i].display_index) - static_cast<int64_t>(frames[i].decode_index);
        const uint64_t magnitude = static_cast<uint64_t>(shift < 0 ? -shift : shift);
        if (frames[i].display_index != frames[i].decode_index)
            ++plan.reordered_frames;
        if (magnitude > plan.max_reorder)
            plan.max_reorder = magnitude;
    }

    // 合成时间偏移：只有恒定样本时长才成立。ctts 偏移 = (显示序 − 解码序) × 样本时长；
    // 负值靠 ctts version 1 的有符号字段表达，不做"整体平移 + elst 裁掉开头"
    // （那会把媒体时长改长，且要求调用方额外写编辑列表）。
    if (duration.constant && duration.sample_delta > 0 && duration.timescale > 0) {
        bool fits = true;
        plan.composition_offsets.reserve(frames.size());
        for (const auto& frame : frames) {
            const int64_t shift = static_cast<int64_t>(frame.display_index) - static_cast<int64_t>(frame.decode_index);
            const int64_t offset = shift * static_cast<int64_t>(duration.sample_delta);
            if (offset > std::numeric_limits<int32_t>::max() || offset < std::numeric_limits<int32_t>::min()) {
                fits = false;
                break;
            }
            plan.composition_offsets.push_back(offset);
            if (offset != 0)
                plan.ctts_required = true;
        }
        if (!fits) {
            plan.composition_offsets.clear();
            plan.ctts_required = false;
            plan.limitations.push_back("显示序偏移超出 ctts 有符号 32 位范围，未生成合成时间偏移");
        }
    } else {
        plan.limitations.push_back("没有恒定样本时长证据（" + duration.source + "），不生成 ctts");
    }

    plan.available = true;
    plan.confidence = (starts_at_rap && duplicate_poc == 0 && !duration.source.empty()) ? "verified" : "derived";
    if (!starts_at_rap) {
        plan.limitations.push_back("码流未从随机接入点开始，首段显示顺序取段内 POC 相对序");
    }
    if (duplicate_poc > 0) {
        plan.limitations.push_back("段内有 " + std::to_string(duplicate_poc) +
                                   " 处 POC 重复，显示顺序按解码序稳定排列");
    }
    if (!plan.ctts_required)
        plan.limitations.push_back("显示序与解码序一致，产物不需要 ctts");

    plan.basis =
        "码流 POC 逐帧实测；按 " + JoinSegments(plan.rap_segments) + " 分段后在段内按 POC 升序定显示序，跨段不比较 POC";
    plan.frames = std::move(frames);
    return plan;
}

RepairDisplayOrderPlan MeasureDisplayOrder(SampleByteSource& source, const std::vector<RepairAuRange>& access_units,
                                           const std::vector<uint8_t>& parameter_sets, const std::string& codec,
                                           const RepairFrameDurationEvidence& duration, const std::atomic<bool>* cancel,
                                           const RepairRunBudget* budget) {
    RepairDisplayOrderPlan plan;
    plan.codec = codec;
    plan.duration = duration;

    if (access_units.empty()) {
        plan.available = false;
        plan.unavailable_reason = "没有可分析的访问单元";
        return plan;
    }
    const int codec_id = CodecIdFromName(codec);
    if (codec_id == AV_CODEC_ID_NONE) {
        plan.available = false;
        plan.unavailable_reason = "编码 \"" + codec + "\" 没有帧级 POC 追踪实现";
        return plan;
    }

    FrameOrderTracker tracker;
    FrameOrderOptions options;
    options.max_frames = kMaxTrackedFrames;
    tracker.Reset(codec_id, 0, options);
    tracker.SetFraming(NalFraming::LengthPrefixed);
    tracker.SetLengthPrefixBytes(4);

    std::vector<std::string> notes;
    if (!parameter_sets.empty() && !tracker.FeedExtradata(parameter_sets.data(), parameter_sets.size())) {
        notes.push_back("参数集未被识别，POC 可能不可用");
    }

    std::vector<uint8_t> buffer;
    for (std::size_t i = 0; i < access_units.size(); ++i) {
        if (budget != nullptr && budget->Checkpoint()) {
            plan.available = false;
            plan.unavailable_reason = BudgetStopMessage(budget, "显示顺序分析");
            return plan;
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            plan.available = false;
            plan.unavailable_reason = "显示顺序分析被取消";
            return plan;
        }
        const RepairAuRange& au = access_units[i];
        if (au.size == 0)
            continue;
        if (au.size > kMaxAuBytes) {
            plan.available = false;
            plan.unavailable_reason = "第 " + std::to_string(i) + " 个访问单元 " + std::to_string(au.size) +
                                      " 字节超过单项上限，边界推断不可信，未逐帧实测 POC";
            return plan;
        }
        buffer.resize(au.size);
        if (!source.Read(au.relative_offset, buffer.data(), buffer.size())) {
            plan.available = false;
            plan.unavailable_reason = source.Error().empty() ? "读取访问单元失败" : source.Error();
            return plan;
        }
        tracker.Feed(buffer.data(), buffer.size(), false, 0.0, static_cast<int64_t>(i));
    }

    const model::FrameOrderTimeline& timeline = tracker.Finish();
    plan.frames_observed = static_cast<uint64_t>(timeline.frames_observed);
    plan.order_source = timeline.order_source;
    if (!timeline.available) {
        plan.available = false;
        plan.unavailable_reason =
            timeline.unavailable_reason.empty() ? "帧级 POC 追踪不可用" : timeline.unavailable_reason;
        plan.limitations = std::move(notes);
        return plan;
    }

    // 明细被上限截断时，末段一定没扫完 —— 用它排显示序会把最后几帧的序号算错，
    // 所以整段丢弃，只保留完整段。
    std::size_t usable = timeline.frames.size();
    plan.truncated = timeline.truncated;
    if (timeline.truncated) {
        std::size_t last_rap = 0;
        for (std::size_t i = 1; i < usable; ++i) {
            if (timeline.frames[i].irap)
                last_rap = i;
        }
        usable = last_rap;
        notes.push_back("逐帧明细达到上限，末段未扫完，已整段排除");
    }
    if (usable == 0) {
        plan.available = false;
        plan.unavailable_reason = "可用的完整随机接入点段为空，无法推导显示顺序";
        plan.limitations = std::move(notes);
        return plan;
    }

    std::vector<RepairDisplayFrame> frames;
    frames.reserve(usable);
    for (std::size_t i = 0; i < usable; ++i) {
        const model::FrameOrderEntry& entry = timeline.frames[i];
        RepairDisplayFrame frame;
        frame.decode_index = i;
        frame.poc = entry.poc;
        frame.irap = entry.irap;
        frame.idr = entry.idr;
        frame.leading = entry.leading;
        frames.push_back(frame);
    }

    RepairDisplayOrderPlan derived = DeriveDisplayOrder(std::move(frames), duration);
    derived.codec = codec;
    derived.order_source = plan.order_source;
    derived.frames_observed = plan.frames_observed;
    derived.truncated = plan.truncated;
    derived.limitations.insert(derived.limitations.begin(), notes.begin(), notes.end());
    if (timeline.poc_type >= 0) {
        derived.basis += "；pic_order_cnt_type = " + std::to_string(timeline.poc_type);
    }
    if (derived.available && !timeline.frames.empty()) {
        // 追踪器自己算出的重排次数是交叉证据：与我们按显示序重排得到的结论必须同向。
        if (timeline.reorder_events > 0 && derived.reordered_frames == 0) {
            derived.confidence = "derived";
            derived.limitations.push_back("追踪器报告存在 POC 回退，但推导出的显示序与解码序一致，"
                                          "结论按「推定」处理");
        }
    }
    return derived;
}

std::vector<uint8_t> BuildCttsBox(const RepairDisplayOrderPlan& plan) {
    if (!plan.available || !plan.ctts_required)
        return {};
    if (plan.composition_offsets.empty() || plan.composition_offsets.size() != plan.frames.size())
        return {};

    struct Run {
        uint32_t count = 0;
        int64_t offset = 0;
    };
    std::vector<Run> runs;
    for (int64_t offset : plan.composition_offsets) {
        if (!runs.empty() && runs.back().offset == offset) {
            ++runs.back().count;
            continue;
        }
        runs.push_back(Run{1, offset});
    }

    RepairBoxWriter writer;
    const std::size_t box = writer.BeginFullBox("ctts", 1, 0); // version 1 = 有符号偏移
    writer.U32(static_cast<uint32_t>(runs.size()));
    for (const Run& run : runs) {
        writer.U32(run.count);
        // 有符号值按补码写进 32 位字段：bit pattern 相同，读侧按 version 1 解释。
        writer.U32(static_cast<uint32_t>(static_cast<int32_t>(run.offset)));
    }
    writer.EndBox(box);
    return writer.TakeBytes();
}

model::RepairDisplayOrderSummary SummarizeDisplayOrder(const RepairDisplayOrderPlan& plan, bool ctts_written) {
    model::RepairDisplayOrderSummary summary;
    summary.analyzed = true;
    summary.available = plan.available;
    summary.unavailable_reason = plan.unavailable_reason;
    summary.codec = plan.codec;
    summary.order_source = plan.order_source;
    summary.basis = plan.basis;
    summary.confidence = plan.confidence;
    summary.duration_source = plan.duration.source;
    summary.timescale = plan.duration.timescale;
    summary.sample_delta = plan.duration.sample_delta;
    summary.frames = plan.frames.size();
    summary.frames_observed = plan.frames_observed;
    summary.truncated = plan.truncated;
    summary.rap_segments = plan.rap_segments;
    summary.reordered_frames = plan.reordered_frames;
    summary.max_reorder = plan.max_reorder;
    summary.ctts_required = plan.ctts_required;
    // ❗ 只有真的写进去才算 true：调用方传错也要在这里被拦住。
    summary.ctts_written =
        ctts_written && plan.available && plan.ctts_required && plan.composition_offsets.size() == plan.frames.size();
    summary.limitations = plan.limitations;
    return summary;
}

} // namespace repair
} // namespace videoeye
