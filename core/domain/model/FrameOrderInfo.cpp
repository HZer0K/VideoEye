#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {
namespace model {

std::string FrameOrderTimeline::Summary() const {
    if (!available)
        return unavailable_reason.empty() ? std::string("帧顺序未分析") : unavailable_reason;

    std::string text = "共 " + std::to_string(frames_observed) + " 帧（I " + std::to_string(i_count) + " / P " +
                       std::to_string(p_count) + " / B " + std::to_string(b_count);
    if (other_count > 0)
        text += " / 其它 " + std::to_string(other_count);
    text += "），IDR " + std::to_string(idr_count) + " 个";
    if (gop_count > 0) {
        text += "，GOP " + std::to_string(gop_count) + " 段";
        text += "（最短 " + std::to_string(min_gop_frames) + " 帧 / 最长 " + std::to_string(max_gop_frames) +
                " 帧 / 平均 " + std::to_string(static_cast<long long>(avg_gop_frames + 0.5)) + " 帧）";
    }
    if (HasReordering()) {
        text += "；显示顺序重排 " + std::to_string(reorder_events) + " 次，最大回退 " + std::to_string(reorder_depth) +
                " 个 POC";
    } else if (b_count == 0) {
        text += "；无 B 帧，解码顺序 = 显示顺序";
    }
    if (duplicate_poc_count > 0)
        text += "；" + std::to_string(duplicate_poc_count) + " 处 POC 重复";
    if (poc_gap_slots > 0)
        text += "；按 POC 步长推断缺 " + std::to_string(poc_gap_slots) + " 帧";
    if (truncated)
        text += "（仅统计前 " + std::to_string(frames.size()) + " 帧）";
    return text;
}

} // namespace model
} // namespace videoeye
