#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {
namespace model {

std::string FrameOrderTimeline::Summary() const {
    if (!available)
        return unavailable_reason.empty() ? std::string("帧顺序未分析") : unavailable_reason;

    // 顺序值来源决定措辞，别让读者拿 I/P/B 去对照一个根本没有这个概念的编码：
    //   "poc"（或空串，旧数据按 POC 处理）—— H.264 / HEVC
    //   "display_rank" —— VP9（没有 POC，顺序值 = 第几次显示）
    //   "order_hint"   —— AV1（有 order_hint，但是模 2^bits 的环形量，
    //                     解环后按显示顺序值用；同样没有 P/B 的概念）
    const bool by_poc = UsesPoc();

    std::string text = "共 " + std::to_string(frames_observed) + " 帧（";
    if (!by_poc) {
        text += "关键帧 " + std::to_string(i_count) + " / 帧间帧 " + std::to_string(other_count);
    } else {
        text += "I " + std::to_string(i_count) + " / P " + std::to_string(p_count) + " / B " + std::to_string(b_count);
        if (other_count > 0)
            text += " / 其它 " + std::to_string(other_count);
    }
    text += "），";
    // ❗ 编码帧数 ≠ 显示帧数：VP9 的 ARF 与 AV1 的 alt-ref 是**隐藏帧**，自己从不
    // 输出，只有被 show_existing 调出来时才显示。而下面的 GOP 段长是按**显示事件**
    // 算的 —— 不把这笔差额说出来，"共 65 帧 / GOP 5 段（每段 12 帧）"就叫读者
    // 自己算出 60 ≠ 65，然后怀疑统计坏了。
    // 明细被上限截断时 frames 不完整，这个差额是假的，所以只在未截断时给。
    const int64_t displayed = static_cast<int64_t>(frames.size());
    if (!truncated && displayed > 0 && frames_observed > displayed) {
        text += "其中 " + std::to_string(frames_observed - displayed) +
                " 帧是隐藏帧（ARF / alt-ref）从未显示，"
                "实际输出 " +
                std::to_string(displayed) + " 帧，";
    }
    if (!by_poc) {
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
    const std::string unit = by_poc ? " 个 POC" : " 帧";
    if (HasReordering()) {
        text += "；显示顺序重排 " + std::to_string(reorder_events) + " 次，最大回退 " + std::to_string(reorder_depth) +
                unit;
    } else if (b_count == 0 && by_poc) {
        text += "；无 B 帧，解码顺序 = 显示顺序";
    } else if (!by_poc) {
        text += "；显示顺序与解码顺序一致";
    }
    if (duplicate_poc_count > 0)
        text += "；" + std::to_string(duplicate_poc_count) + (by_poc ? " 处 POC 重复" : " 处重复显示");
    if (poc_gap_slots > 0)
        text += (by_poc ? "；按 POC 步长推断缺 " : "；按顺序值步长推断缺 ") + std::to_string(poc_gap_slots) + " 帧";
    if (truncated)
        text += "（仅统计前 " + std::to_string(frames.size()) + " 帧）";
    return text;
}

} // namespace model
} // namespace videoeye
