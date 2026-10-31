#include "core/analysis/codec/FrameOrderStats.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

namespace videoeye {

void ComputeFrameOrderStatistics(model::FrameOrderTimeline& timeline) {
    const std::vector<model::FrameOrderEntry>& f = timeline.frames;
    if (f.empty())
        return;

    // 汇总可能被调用多次（Finish 幂等），统计字段必须先归零再累加，
    // 否则第二次调用会把数字翻倍 —— 这种错在"只在收尾调一次"的路径上
    // 永远看不见，等有人加了"中途取一次快照"才爆出来。
    timeline.reorder_events = 0;
    timeline.reorder_depth = 0;
    timeline.duplicate_poc_count = 0;
    timeline.poc_gap_slots = 0;

    // ---- GOP 结构（先算：下面的重复 / 缺口都必须按段内统计）----
    // 边界判据：随机接入点（irap）；以及"非随机接入点的 I 帧且 POC 比前一帧更早"
    // （拼接点上一个独立编码的 I 帧）。首帧永远算第一段的开始。
    //
    // ❗ 边界必须按 irap 而不是 idr：HEVC 的开放 GOP 用 CRA 分段，它的 POC
    // 不复位（NoRaslOutputFlag == 0），因此 POC 一路递增 —— 只看 idr 的话，
    // 一份 keyint=12 的开放 GOP 文件会被报成"GOP 1 段、长 40 帧"，
    // 与容器里明摆着的 4 个同步样本直接矛盾。
    std::vector<size_t> starts;
    for (size_t i = 0; i < f.size(); ++i) {
        bool boundary = (i == 0) || f[i].irap || f[i].idr;
        if (!boundary && f[i].slice_type == 2 && f[i].poc < f[i - 1].poc)
            boundary = true;
        if (boundary)
            starts.push_back(i);
    }
    timeline.gop_count = static_cast<int>(starts.size());

    // 前导图像（HEVC 的 RADL/RASL、VVC 的 RADL/RASL）：它们按定义就该排在所属
    // 随机接入点**之前**，所以"解码顺序里 POC 变小"是语义本身，不是 B 帧重排。
    //
    // ❗ 判据取 entry.leading，由各追踪器按自己的编码填。以前这里写死
    //    `nal_type ∈ [6,9]`，那是 HEVC 的编号；VVC 的 7/8/9 恰好是 IDR/CRA，
    //    照抄会把每个随机接入点之后的重排整段排除，得到"没有重排"的假结论。
    auto is_leading_picture = [](const model::FrameOrderEntry& e) { return e.leading; };

    // ---- 显示顺序重排 ----
    // 解码顺序里 POC 变小，就说明这一帧的显示时间早于上一帧 —— 这是 B 帧存在的
    // 直接证据（不是靠 slice_type 猜）。回退幅度就是重排深度。
    //
    // 两种不算：
    //   1. 本帧是 idr —— 它在 GOP 边界把 POC 重置回 0，那不是"显示顺序往前插"，
    //      而是新的一段重新从 0 开始计。漏了这一判据，任何多 GOP 的普通文件都会
    //      报出"存在重排"，而且数量恰好等于 IDR 个数 —— 典型的一看就假的信号。
    //   2. 前一帧是随机接入点（irap），或者本帧是前导图像（RASL/RADL）——
    //      开放 GOP 里 CRA 之后解码出来的是 leading picture，它们的 POC 天生就在
    //      CRA 之前，回退幅度等于整个 GOP 的长度。那是封装语义，不是 B 帧重排；
    //      不排除的话一份普通的 open-GOP 文件会被报成"最大回退 16 个 POC"
    //      这种无从判断真假的值。
    for (size_t i = 1; i < f.size(); ++i) {
        if (f[i].idr || f[i - 1].irap || is_leading_picture(f[i]))
            continue;
        if (f[i].poc < f[i - 1].poc) {
            ++timeline.reorder_events;
            const int64_t back = f[i - 1].poc - f[i].poc;
            if (back > timeline.reorder_depth)
                timeline.reorder_depth = back;
        }
    }

    // ---- POC 重复 / 缺口（按 GOP 段内统计）----
    //
    // ❗ 不能把整条时间线的 POC 丢进一个集合里统计：POC 在每个随机接入点处重置，
    // 两个 GOP 的 POC 值域完全重叠。跨段统计会得到两个方向的假信号 ——
    // 重复数恒等于"帧数 - 不同 POC 数"（多 GOP 时必然 > 0），
    // 而缺口推断因为 min/max 只覆盖一个 GOP 的范围而永远推不出缺帧。
    for (size_t s = 0; s < starts.size(); ++s) {
        const size_t begin = starts[s];
        const size_t end = (s + 1 < starts.size()) ? starts[s + 1] : f.size();

        std::set<int64_t> uniq;
        for (size_t i = begin; i < end; ++i) {
            if (!uniq.insert(f[i].poc).second)
                ++timeline.duplicate_poc_count;
        }

        // 段内只有一个 POC 值 = 这一段根本没有显示顺序信息（例如全 I 帧、
        // 或者编码器把所有图像的 POC 都写成 0），重复/缺口都无从谈起。
        if (timeline.field_coded || uniq.size() < 2)
            continue;
        // 用观测到的最小正差当步长（逐行通常是 2，带 delta_pic_order_cnt_bottom
        // 的流可能是 1）。只用它推断"本应有多少帧"，不据此断言具体哪一帧丢了。
        int64_t step = 0;
        for (auto it = std::next(uniq.begin()); it != uniq.end(); ++it) {
            const int64_t d = *it - *std::prev(it);
            if (d > 0 && (step == 0 || d < step))
                step = d;
        }
        if (step > 0) {
            const int64_t min_poc = *uniq.begin();
            const int64_t max_poc = *uniq.rbegin();
            const int64_t expected = (max_poc - min_poc) / step + 1;
            const int64_t actual = static_cast<int64_t>(uniq.size());
            // 只有"缺"才有意义；多出来的（例如步长判断偏大）不报
            if (expected > actual)
                timeline.poc_gap_slots += expected - actual;
        }
    }
    // ---- 每段多长 ----
    //
    // ❗ 最后一段（末尾那个还没看到下一个边界的 GOP）**要**算进来。
    // 不算的话报告里会出现自相矛盾的数字：40 帧、IDR 在第 0 与第 25 帧，
    // 于是"GOP 2 段（最短 25 / 最长 25）"—— 可第二段明明只有 15 帧，
    // 而这个 15 恰恰是读者最该看到的（序列末尾被截断的 GOP 往往是拼接痕迹）。
    //
    // 唯一该排除的情况是**明细被上限截断**：那时最后一段本来就没扫完，
    // 它的长度是假的。用 timeline.truncated 区分这件事，
    // 而不是"凡是最后一段都不算"。
    std::vector<int64_t> lengths;
    lengths.reserve(starts.size());
    for (size_t s = 0; s < starts.size(); ++s) {
        const size_t begin = starts[s];
        const size_t end = (s + 1 < starts.size()) ? starts[s + 1] : f.size();
        lengths.push_back(static_cast<int64_t>(end - begin));
    }
    if (timeline.truncated && lengths.size() > 1)
        lengths.pop_back(); // 被上限截断的尾巴：长度无意义

    if (!lengths.empty()) {
        int64_t total = 0;
        for (int64_t len : lengths)
            total += len;
        timeline.min_gop_frames = *std::min_element(lengths.begin(), lengths.end());
        timeline.max_gop_frames = *std::max_element(lengths.begin(), lengths.end());
        timeline.avg_gop_frames = static_cast<double>(total) / static_cast<double>(lengths.size());
    }
}

} // namespace videoeye
