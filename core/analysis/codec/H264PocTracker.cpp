#include "core/analysis/codec/H264PocTracker.h"

#include <algorithm>
#include <set>

#include "core/analysis/codec/H264BitstreamParser.h"
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataParser.h"

namespace videoeye {

namespace {

// first_mb_in_slice 的合理上界：4K 的宏块数约 8K×4K/256 ≈ 13 万，1M 有近十倍余量。
// 用 Clamped 而不是 Bounded：这个字段读错位只影响"是不是帧首"这一判断，
// 不该因此把整条流判成非法（与参数集解析的严格口径不同）。
constexpr uint32_t kMaxFirstMbInSlice = 1u << 20;

} // namespace

void H264PocTracker::Reset(int stream_index, const Options& options) {
    stream_index_ = stream_index;
    options_ = options;
    framing_ = Framing::Auto;
    prefix_bytes_ = 0;
    sps_ready_ = false;
    pps_ready_ = false;
    sps_ = model::H264SpsInfo{};
    pps_ = model::H264PpsInfo{};
    max_frame_num_ = 0;
    max_poc_lsb_ = 0;
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    prev_frame_num_ = 0;
    prev_frame_num_offset_ = 0;
    timeline_ = model::FrameOrderTimeline{};
    finished_ = false;
}

std::vector<NalUnit> H264PocTracker::SplitNals(const uint8_t* data, size_t size) {
    // ---- 一、容器已经明说了前缀宽度 → 直接用，不猜 ----
    //
    // ❗ 这一步**不能**省，也不能让位给下面的启发式：
    // 长度前缀是定长的，当某个 NAL 的长度落在 [0x100, 0x1FF]（也就是 256~511 字节，
    // 恰好是 P 帧最常见的量级）时，前缀四字节就是 `00 00 01 XX` —— 与 Annex B 的
    // 3 字节起始码**逐字节相同**。此时"找起始码"这条启发式必然误判，然后把
    // 258 字节的 slice 载荷当成一条 `nal_type = XX` 的 NAL 丢掉。
    //
    // 实测（libopenh264 + MP4, 40 帧）：样本大小落在 256~511 的有 33 个，
    // 于是 40 帧里只扫到 7 帧 —— 而且丢的那 33 帧毫无规律可言，
    // 从日志上看只像"某些包没有帧"，是最难查的那一类静默失效。
    //
    // 反过来，容器没给 extradata 时（TS / 裸流 / 无配置记录的封装）才需要猜，
    // 那种场景 prefix_bytes_ == 0，正好落到下面的分支。
    if (prefix_bytes_ > 0) {
        std::vector<NalUnit> nals =
            ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::H264, prefix_bytes_, nullptr);
        if (!nals.empty())
            return nals;
        // 前缀宽度是容器给的，取不出 NAL 说明这个包本身异常（截断 / 填充）。
        // 这时不要再拿启发式去猜 —— 猜出来的东西比"没有"更危险。
    }

    if (framing_ == Framing::AnnexB)
        return ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::H264);
    if (framing_ == Framing::LengthPrefixed) {
        return ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::H264,
                                                              prefix_bytes_ > 0 ? prefix_bytes_ : 4, nullptr);
    }

    // ---- 二、Auto：逐包判定（prefix_bytes_ == 0 时才走得到这里）----
    int detected = 0;
    if (DetectLengthPrefixed(data, size, detected))
        return ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::H264, detected, nullptr);
    return ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::H264);
}

bool H264PocTracker::DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const {
    // 只有"容器没给前缀宽度"时才需要这条启发式，见 SplitNals 里的说明。
    //
    // 顺序上先找起始码，是因为裸流 / TS 里"整段都是起始码"这件事比
    // "整段恰好能被长度链整除"更可信；代价就是上面那条 [256,511] 的坑，
    // 而那个坑已经由 prefix_bytes_ 分支挡住了。
    for (size_t i = 0; i + 3 <= size && i < 64; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < size && data[i + 3] == 1)))
            return false;
    }
    static const int kWidths[3] = {4, 2, 1};
    for (int width : kWidths) {
        if (ExtradataParser::LooksLikeLengthPrefixed(data, size, width)) {
            prefix_bytes = width;
            return true;
        }
    }
    return false;
}

int H264PocTracker::Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index) {
    if (data == nullptr || size == 0)
        return 0;

    std::vector<NalUnit> nals = SplitNals(data, size);
    int new_frames = 0;
    for (const NalUnit& nal : nals)
        FeedNal(nal, has_time, time_seconds, packet_index, new_frames);
    return new_frames;
}

bool H264PocTracker::HandleSps(const NalUnit& nal) {
    const model::H264SpsInfo sps = H264BitstreamParser::ParseFromNalUnit(nal);
    if (!sps.present)
        return false;
    // log2_*_minus4 的规范范围是 [0,12]；越界说明这个 SPS 本身不合法，
    // 拿它算 POC 只会得到无意义的值，所以宁可不分析。
    const int log2_frame_num = sps.log2_max_frame_num_minus4 + 4;
    const int log2_poc = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
    if (log2_frame_num < 1 || log2_frame_num > 16 || log2_poc < 1 || log2_poc > 16)
        return false;

    sps_ = sps;
    sps_ready_ = true;
    timeline_.poc_type = sps_.pic_order_cnt_type;
    max_frame_num_ = int64_t(1) << log2_frame_num;
    max_poc_lsb_ = int64_t(1) << log2_poc;
    // 参数集切换（分辨率/编码参数变了）意味着 POC 状态必须重来，
    // 否则跨配置的 POC 会被算到同一条时间轴上，制造出假的跳变。
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    prev_frame_num_ = 0;
    prev_frame_num_offset_ = 0;
    return true;
}

bool H264PocTracker::HandlePps(const NalUnit& nal) {
    const model::H264PpsInfo pps = H264BitstreamParser::ParsePpsFromNalUnit(nal);
    if (!pps.present)
        return false;
    pps_ = pps;
    pps_ready_ = true;
    return true;
}

bool H264PocTracker::FeedExtradata(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return false;
    // ExtradataParser 自己认 avcC / hvcC / Annex B，并统一产出 NalUnit，
    // 所以这里不用再分装格式 —— 与 VideoEsInspector 那条路保持一致。
    const ExtradataResult parsed = ExtradataParser::Parse(data, size);
    if (!parsed.valid)
        return false;
    std::vector<NalUnit> nals = parsed.nal_units;
    if (nals.empty()) {
        // 兜底：少数容器把 SPS/PPS 直接按"长度前缀 + NAL"粘在 extradata 里，
        // 既不是 Annex B 也不是标准配置记录，ExtradataParser 的自动识别认不出来。
        // 按宽 → 窄依次试，取第一个真的切出 NAL 的宽度。
        for (int width : {4, 2, 1}) {
            nals = ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::H264, width, nullptr);
            if (!nals.empty())
                break;
        }
    }
    bool got_sps = false;
    for (const NalUnit& nal : nals) {
        if (nal.type == 7)
            got_sps = HandleSps(nal) || got_sps;
        else if (nal.type == 8)
            HandlePps(nal);
    }
    return got_sps;
}

void H264PocTracker::FeedNal(const NalUnit& nal, bool has_time, double time_seconds, int64_t packet_index,
                             int& new_frames) {
    switch (nal.type) {
    case 7: { // SPS
        HandleSps(nal);
        return;
    }
    case 8: { // PPS
        HandlePps(nal);
        return;
    }
    case 1:   // 非 IDR 非分区 slice
    case 2:   // 数据分区 A（含完整 slice_header）
    case 5: { // IDR
        break;
    }
    default:
        return; // SEI / AUD / 分区 B、C 等：与 POC 无关
    }

    if (!sps_ready_ || !pps_ready_ || sps_.pic_order_cnt_type == 1) {
        // 参数集还没到（或 POC 类型不支持）：先记一条"为什么没数据"，
        // 不要让上层看到"0 帧"却不知道原因。
        if (timeline_.unavailable_reason.empty()) {
            if (!sps_ready_)
                timeline_.unavailable_reason = "扫描到 slice 之前没有解析到 SPS，无法计算 POC";
            else if (!pps_ready_)
                timeline_.unavailable_reason = "扫描到 slice 之前没有解析到 PPS，无法定位 slice_header 字段";
            else
                timeline_.unavailable_reason =
                    "SPS 使用 pic_order_cnt_type=1（需要 offset_for_ref_frame 表），本实现不计算 POC";
        }
        return;
    }

    SliceHead head;
    if (!ParseSliceHeader(nal, head))
        return;
    // 同一幅图像的后续 slice 不参与 POC 状态推进（规范里 prevPicOrderCnt 记的是
    // "上一幅图像"，不是"上一个 slice"）。不做这个限制，多 slice 帧会把状态推乱。
    if (head.first_mb_in_slice != 0)
        return;

    const int64_t poc = ComputePoc(head);
    PushFrame(head, poc, has_time, time_seconds, packet_index);
    ++new_frames;
}

bool H264PocTracker::ParseSliceHeader(const NalUnit& nal, SliceHead& out) {
    if (max_frame_num_ <= 0)
        return false;
    // 不用 H264BitstreamParser::GetRbsp()：那是 private，且它现在只等价于
    // UnescapeRbsp(NalUnit::data)（data 已不含 NAL header，见 GetRbsp 实现）。
    // 为一个纯工具函数放宽另一个类的访问级别不值当。
    const std::vector<uint8_t> rbsp = UnescapeRbsp(nal.data.data(), nal.data.size());
    if (rbsp.empty())
        return false;
    BitReader br;
    br.Reset(rbsp.data(), rbsp.size());

    out.first_mb_in_slice = br.ReadUEClamped(kMaxFirstMbInSlice);
    const uint32_t slice_type_raw = br.ReadUEClamped(9);
    if (slice_type_raw > 9)
        return false;
    // 规范里 5..9 是"同一幅图像所有 slice 都是这个类型"的重复编码，模 5 后语义相同
    out.slice_type = static_cast<int>(slice_type_raw % 5);
    br.ReadUEClamped(255); // pic_parameter_set_id（当前只维护一份 PPS）
    out.frame_num = static_cast<int>(br.ReadBits(sps_.log2_max_frame_num_minus4 + 4));

    if (!sps_.frame_mbs_only_flag) {
        out.field_pic = br.ReadBits(1) != 0;
        if (out.field_pic)
            out.bottom_field = br.ReadBits(1) != 0;
        // 只有真的出现了场图像才置位 —— 仅凭 frame_mbs_only_flag=0 置位会让
        // "逐行内容 + 允许隔行"的流被误判成隔行，进而关掉缺帧推断。
        if (out.field_pic)
            timeline_.field_coded = true;
    }
    out.idr = (nal.type == 5);
    if (out.idr)
        br.ReadUE(); // idr_pic_id

    if (sps_.pic_order_cnt_type == 0) {
        out.poc_lsb = static_cast<int>(br.ReadBits(sps_.log2_max_pic_order_cnt_lsb_minus4 + 4));
        if (pps_.bottom_field_pic_order_in_frame_present_flag && !out.field_pic)
            out.delta_poc_bottom = br.ReadSE();
    } else if (sps_.pic_order_cnt_type == 1) {
        if (!sps_.delta_pic_order_always_zero_flag) {
            out.delta_poc_top = br.ReadSE();
            if (pps_.bottom_field_pic_order_in_frame_present_flag && !out.field_pic)
                out.delta_poc_bottom_field = br.ReadSE();
        }
    }
    // ❗ 位读完之前就把数据耗尽的（被容器截断 / 半包），读出来的 frame_num / poc_lsb
    // 是凭空补的 0，把它当一帧存进去会凭空多出一帧"POC = 0 的 P 帧"。
    // 宁可丢这一帧，也不要造一条假的时间线。
    if (br.HasError())
        return false;
    return true;
}

int64_t H264PocTracker::ComputePoc(const SliceHead& head) {
    const int poc_type = sps_.pic_order_cnt_type;

    if (poc_type == 0) {
        if (head.idr) {
            prev_poc_msb_ = 0;
            prev_poc_lsb_ = 0;
        }
        const int64_t lsb = head.poc_lsb;
        int64_t poc_msb = prev_poc_msb_;
        // 8.2.1.1：lsb 跨越半个回绕区间时进位/借位。
        // IDR 不参与：它的 prev 已被显式归零（8.2.1 的 IDR 分支），再走回绕判断
        // 会把 poc_lsb 较大的 IDR 算成负 POC，制造出一次假的"POC 回退"。
        if (!head.idr) {
            if (lsb < prev_poc_lsb_ && (prev_poc_lsb_ - lsb) >= max_poc_lsb_ / 2)
                poc_msb = prev_poc_msb_ + max_poc_lsb_;
            else if (lsb > prev_poc_lsb_ && (lsb - prev_poc_lsb_) > max_poc_lsb_ / 2)
                poc_msb = prev_poc_msb_ - max_poc_lsb_;
        }

        const int64_t top = poc_msb + lsb;
        const int64_t bottom = head.field_pic ? top : top + head.delta_poc_bottom;
        prev_poc_msb_ = poc_msb;
        prev_poc_lsb_ = lsb;
        return std::min(top, bottom);
    }

    if (poc_type == 2) {
        if (head.idr) {
            prev_frame_num_ = 0;
            prev_frame_num_offset_ = 0;
        }
        int64_t offset = prev_frame_num_offset_;
        if (prev_frame_num_ > head.frame_num)
            offset = prev_frame_num_offset_ + max_frame_num_;
        prev_frame_num_ = head.frame_num;
        prev_frame_num_offset_ = offset;
        const int64_t abs_frame_num = offset + head.frame_num;
        // 8.2.1.3：frame_mbs_only_flag 时每帧占 2 个 POC 槽
        if (!sps_.frame_mbs_only_flag && head.field_pic)
            return abs_frame_num * 2 - (head.bottom_field ? 1 : 0) - 1;
        return abs_frame_num * 2;
    }

    // poc_type == 1：需要 offset_for_ref_frame 表，本实现不支持（见文件头）
    return 0;
}

void H264PocTracker::PushFrame(const SliceHead& head, int64_t poc, bool has_time, double time_seconds,
                               int64_t packet_index) {
    ++timeline_.frames_observed;
    switch (head.slice_type) {
    case 2:
        ++timeline_.i_count;
        break;
    case 0:
        ++timeline_.p_count;
        break;
    case 1:
        ++timeline_.b_count;
        break;
    default:
        ++timeline_.other_count;
        break;
    }
    if (head.idr)
        ++timeline_.idr_count;

    if (timeline_.frames.size() >= options_.max_frames) {
        timeline_.truncated = true;
        return;
    }
    model::FrameOrderEntry entry;
    entry.nal_unit_type = head.idr ? 5 : 1;
    entry.slice_type = head.slice_type;
    entry.idr = head.idr;
    entry.frame_num = head.frame_num;
    entry.poc = poc;
    entry.packet_index = packet_index;
    entry.has_time = has_time;
    entry.time_seconds = time_seconds;
    timeline_.frames.push_back(entry);
}

const model::FrameOrderTimeline& H264PocTracker::Finish() {
    if (finished_)
        return timeline_;
    finished_ = true;

    if (!timeline_.frames.empty() || timeline_.frames_observed > 0) {
        timeline_.available = true;
        timeline_.poc_type = sps_ready_ ? sps_.pic_order_cnt_type : -1;
        Recompute();
    } else if (timeline_.unavailable_reason.empty()) {
        timeline_.unavailable_reason = "没有扫到可解析的 H.264 slice（可能是裸流之外的封装或全部为非 VCL NAL）";
    }
    return timeline_;
}

void H264PocTracker::Recompute() {
    const std::vector<model::FrameOrderEntry>& f = timeline_.frames;
    if (f.empty())
        return;

    // ---- GOP 结构（先算：下面的重复 / 缺口都必须按段内统计）----
    // 边界判据：IDR；以及"非 IDR 的 I 帧且 POC 比前一帧更早"（开放 GOP 的起始，
    // 或者拼接点上一个独立编码的 I 帧）。首帧永远算第一段的开始。
    std::vector<size_t> starts;
    for (size_t i = 0; i < f.size(); ++i) {
        bool boundary = (i == 0) || f[i].idr;
        if (!boundary && f[i].slice_type == 2 && f[i].poc < f[i - 1].poc)
            boundary = true;
        if (boundary)
            starts.push_back(i);
    }
    timeline_.gop_count = static_cast<int>(starts.size());

    // ---- 显示顺序重排 ----
    // 解码顺序里 POC 变小，就说明这一帧的显示时间早于上一帧 —— 这是 B 帧存在的
    // 直接证据（不是靠 slice_type 猜）。回退幅度就是重排深度。
    //
    // IDR 不算：它在 GOP 边界把 POC 重置回 0，那不是"显示顺序往前插"，
    // 而是新的一段重新从 0 开始计。漏了这一判据，任何多 GOP 的普通文件都会
    // 报出"存在重排"，而且数量恰好等于 IDR 个数 —— 典型的一看就假的信号。
    for (size_t i = 1; i < f.size(); ++i) {
        if (f[i].idr)
            continue;
        if (f[i].poc < f[i - 1].poc) {
            ++timeline_.reorder_events;
            const int64_t back = f[i - 1].poc - f[i].poc;
            if (back > timeline_.reorder_depth)
                timeline_.reorder_depth = back;
        }
    }

    // ---- POC 重复 / 缺口（按 GOP 段内统计）----
    //
    // ❗ 不能把整条时间线的 POC 丢进一个集合里统计：POC 在每个 IDR 处重置，
    // 两个 GOP 的 POC 值域完全重叠。跨段统计会得到两个方向的假信号 ——
    // 重复数恒等于"帧数 - 不同 POC 数"（多 GOP 时必然 > 0），
    // 而缺口推断因为 min/max 只覆盖一个 GOP 的范围而永远推不出缺帧。
    for (size_t s = 0; s < starts.size(); ++s) {
        const size_t begin = starts[s];
        const size_t end = (s + 1 < starts.size()) ? starts[s + 1] : f.size();

        std::set<int64_t> uniq;
        for (size_t i = begin; i < end; ++i) {
            if (!uniq.insert(f[i].poc).second)
                ++timeline_.duplicate_poc_count;
        }

        if (timeline_.field_coded || uniq.size() < 2)
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
                timeline_.poc_gap_slots += expected - actual;
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
    // 它的长度是假的。用 timeline_.truncated 区分这件事，
    // 而不是"凡是最后一段都不算"。
    std::vector<int64_t> lengths;
    lengths.reserve(starts.size());
    for (size_t s = 0; s < starts.size(); ++s) {
        const size_t begin = starts[s];
        const size_t end = (s + 1 < starts.size()) ? starts[s + 1] : f.size();
        lengths.push_back(static_cast<int64_t>(end - begin));
    }
    if (timeline_.truncated && lengths.size() > 1)
        lengths.pop_back(); // 被上限截断的尾巴：长度无意义

    if (!lengths.empty()) {
        int64_t total = 0;
        for (int64_t len : lengths)
            total += len;
        timeline_.min_gop_frames = *std::min_element(lengths.begin(), lengths.end());
        timeline_.max_gop_frames = *std::max_element(lengths.begin(), lengths.end());
        timeline_.avg_gop_frames = static_cast<double>(total) / static_cast<double>(lengths.size());
    }
}

} // namespace videoeye
