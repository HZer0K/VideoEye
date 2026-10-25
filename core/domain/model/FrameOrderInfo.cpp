#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {
namespace model {

std::string FrameOrderTimeline::Summary() const {
    if (!available)
        return unavailable_reason.empty() ? std::string("帧顺序未分析") : unavailable_reason;

    // VP9 没有 POC：顺序值是"第几次显示"，而且头里不区分 P/B
    // （只有 KEY / INTER）。措辞跟着变，别让读者拿 I/P/B 去对照一个
    // 根本没有这个概念的编码。
    const bool by_display_rank = (order_source == "display_rank");

    std::string text = "共 " + std::to_string(frames_observed) + " 帧（";
    if (by_display_rank) {
        text += "关键帧 " + std::to_string(i_count) + " / 帧间帧 " + std::to_string(other_count);
    } else {
        text += "I " + std::to_string(i_count) + " / P " + std::to_string(p_count) + " / B " + std::to_string(b_count);
        if (other_count > 0)
            text += " / 其它 " + std::to_string(other_count);
    }
    text += "），";
    if (by_display_rank) {
        text += "随机接入点 " + std::to_string(idr_count) + " 个";
    } else if (codec == "hevc") {
        // HEVC 的开放 GOP 里"随机接入点"与"POC 复位点"不是一回事：
        // 插在中间的 CRA 可以跳进去，但它的 POC 不复位。两个数都给，省得
        // 出现"随机接入点 1 个 / GOP 4 段"这种看着自相矛盾的组合。
        text +=
            "随机接入点 " + std::to_string(irap_count) + " 个（其中 POC 复位 " + std::to_string(idr_count) + " 个）";
    } else {
        text += "IDR " + std::to_string(idr_count) + " 个";
    }
    if (gop_count > 0) {
        text += "，GOP " + std::to_string(gop_count) + " 段";
        text += "（最短 " + std::to_string(min_gop_frames) + " 帧 / 最长 " + std::to_string(max_gop_frames) +
                " 帧 / 平均 " + std::to_string(static_cast<long long>(avg_gop_frames + 0.5)) + " 帧）";
    }
    const std::string unit = by_display_rank ? " 帧" : " 个 POC";
    if (HasReordering()) {
        text += "；显示顺序重排 " + std::to_string(reorder_events) + " 次，最大回退 " + std::to_string(reorder_depth) +
                unit;
    } else if (b_count == 0 && !by_display_rank) {
        text += "；无 B 帧，解码顺序 = 显示顺序";
    } else if (by_display_rank) {
        text += "；显示顺序与解码顺序一致";
    }
    if (duplicate_poc_count > 0)
        text += "；" + std::to_string(duplicate_poc_count) + (by_display_rank ? " 处重复显示" : " 处 POC 重复");
    if (poc_gap_slots > 0)
        text += "；按 POC 步长推断缺 " + std::to_string(poc_gap_slots) + " 帧";
    if (truncated)
        text += "（仅统计前 " + std::to_string(frames.size()) + " 帧）";
    return text;
}

} // namespace model
} // namespace videoeye
