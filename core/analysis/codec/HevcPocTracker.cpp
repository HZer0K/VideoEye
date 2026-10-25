#include "core/analysis/codec/HevcPocTracker.h"

#include "core/analysis/codec/FrameOrderStats.h"
#include "core/analysis/codec/HevcBitstreamParser.h"
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataParser.h"

namespace videoeye {

namespace {

// HEVC 的 NAL 单元类型（7.4.1 Table 7-1）里与本文件相关的那些。
// 写成常量而不是魔法数字：16..23 这个区间的边界记错一位，整条判据就反了。
constexpr int kNalVps = 32;
constexpr int kNalSps = 33;
constexpr int kNalPps = 34;
constexpr int kNalIdrWRadl = 19;
constexpr int kNalIdrNLp = 20;

// VCL 是 0..31；其中 16..23 是 IRAP，24..31 是保留类型。
constexpr int kNalVclLast = 31;
constexpr int kNalIrapFirst = 16;
constexpr int kNalIrapLast = 23;
constexpr int kNalReservedVclFirst = 24;

bool IsIrap(int type) {
    return type >= kNalIrapFirst && type <= kNalIrapLast;
}

// NoRaslOutputFlag 恒为 1 的 NAL 类型：IDR(19/20) 与 BLA(16/17/18)。
// CRA(21) 不在内 —— 它只有"码流第一幅图 / EOS 之后"才置 1，那种情况由
// 调用方按"是不是扫到的第一幅图像"另行判断。22/23 是保留 IRAP，按 CRA 处理
// （不复位），宁可少复位也不要凭空清零 POC。
bool IsAlwaysNoRasl(int type) {
    return type >= kNalIrapFirst && type <= kNalIdrNLp;
}

// HEVC 的 slice_type（0=B 1=P 2=I）→ 项目内部约定（0=P 1=B 2=I）。
int NormalizeSliceType(uint32_t hevc_slice_type) {
    switch (hevc_slice_type) {
    case 0:
        return 1; // B
    case 1:
        return 0; // P
    case 2:
        return 2; // I
    default:
        return -1;
    }
}

} // namespace

void HevcPocTracker::Reset(int stream_index, const Options& options) {
    stream_index_ = stream_index;
    options_ = options;
    framing_ = Framing::Auto;
    prefix_bytes_ = 0;
    for (int i = 0; i < kMaxSpsId; ++i)
        sps_[i] = SpsPocState{};
    for (int i = 0; i < kMaxPpsId; ++i)
        pps_[i] = PpsPocState{};
    any_sps_ = false;
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    timeline_ = model::FrameOrderTimeline{};
    finished_ = false;
}

void HevcPocTracker::NoteReason(const std::string& reason) {
    if (timeline_.unavailable_reason.empty())
        timeline_.unavailable_reason = reason;
}

std::vector<NalUnit> HevcPocTracker::SplitNals(const uint8_t* data, size_t size) {
    // 与 H264PocTracker::SplitNals 同样的优先级，理由也一样：
    // 容器给了前缀宽度就直接用，不能让"找起始码"的启发式插队 ——
    // 长度落在 [0x100, 0x1FF] 的 NAL，其 4 字节长度前缀与 Annex B 的
    // `00 00 01 XX` 三个字节逐字节相同，启发式必然误判并把载荷整条丢掉。
    if (prefix_bytes_ > 0) {
        std::vector<NalUnit> nals =
            ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::Hevc, prefix_bytes_, nullptr);
        if (!nals.empty())
            return nals;
        // 取不出来说明这包本身异常（截断 / 填充）。此时不要再拿启发式去猜。
    }

    if (framing_ == Framing::AnnexB)
        return ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::Hevc);
    if (framing_ == Framing::LengthPrefixed) {
        return ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::Hevc,
                                                              prefix_bytes_ > 0 ? prefix_bytes_ : 4, nullptr);
    }

    int detected = 0;
    if (DetectLengthPrefixed(data, size, detected))
        return ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::Hevc, detected, nullptr);
    return ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::Hevc);
}

bool HevcPocTracker::DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const {
    // 先找起始码：裸流 / TS 里"整段都是起始码"比"整段恰好能被长度链整除"更可信。
    // 代价是上面 SplitNals 里说的 [0x100,0x1FF] 坑，那个坑由 prefix_bytes_ 分支挡住。
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

int HevcPocTracker::Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index) {
    if (data == nullptr || size == 0)
        return 0;

    std::vector<NalUnit> nals = SplitNals(data, size);
    int new_frames = 0;
    for (const NalUnit& nal : nals)
        FeedNal(nal, has_time, time_seconds, packet_index, new_frames);
    return new_frames;
}

bool HevcPocTracker::HandleSps(const NalUnit& nal) {
    const model::HevcSpsInfo sps = HevcBitstreamParser::ParseSpfFromNalUnit(nal);
    if (!sps.present)
        return false;
    const int id = sps.sps_seq_parameter_set_id;
    if (id < 0 || id >= kMaxSpsId)
        return false;
    // log2_max_pic_order_cnt_lsb_minus4 的规范范围是 [0,12]（即位宽 4..16）。
    // 解析器里它走的是无上界 ReadUE()，一个畸形值能直接撑爆后面的
    // `1 << x` —— 越界就当作"没拿到这份 SPS"，不要拿它算 POC。
    const int minus4 = sps.log2_max_pic_order_cnt_lsb_minus4;
    if (minus4 < 0 || minus4 > 12)
        return false;

    SpsPocState& slot = sps_[id];
    slot.ready = true;
    slot.log2_max_poc_lsb = minus4 + 4;
    slot.separate_colour_plane = (sps.separate_colour_plane_flag != 0);
    any_sps_ = true;
    return true;
}

bool HevcPocTracker::HandlePps(const NalUnit& nal) {
    const model::HevcPpsInfo pps = HevcBitstreamParser::ParsePpsFromNalUnit(nal);
    if (!pps.present)
        return false;
    const int id = pps.pic_parameter_set_id;
    if (id < 0 || id >= kMaxPpsId)
        return false;
    // num_extra_slice_header_bits 是 u(3)，天然 0..7。解析器已经按 3 位读，
    // 这里再夹一次只是为了"绝不拿它当无界循环上界"。
    int extra = pps.num_extra_slice_header_bits;
    if (extra < 0)
        extra = 0;
    if (extra > 7)
        extra = 7;

    PpsPocState& slot = pps_[id];
    slot.ready = true;
    slot.sps_id = pps.seq_parameter_set_id;
    slot.num_extra_slice_header_bits = extra;
    slot.output_flag_present = (pps.output_flag_present_flag != 0);
    return true;
}

bool HevcPocTracker::FeedExtradata(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return false;

    // 判据统一用"解析结果里真的出现了 32/33/34 号 NAL"，逐个候选试 ——
    // 不能只靠 ExtradataParser::Parse 的自动识别，HEVC 上有两个坑：
    //   * FFmpeg 从 MP4 交出来的 extradata 是 **hvcC 记录本体**（box 头已剥掉），
    //     首字节同样是 1、与 avcC 撞车；而 DetectFormat 里没有一条能把
    //     hvcC 本体认出来（它只认带 box 头的形态），于是会落到 Annex B 分支。
    //   * hvcC 记录里嵌的 SPS/PPS 载荷本身就可能含 00 00 01，于是
    //     "看起来像 Annex B"这条启发式也跟着误判。
    // 结果是解析出一堆垃圾 NAL、一份参数集都没有 —— 表现出来是"帧顺序不可用"，
    // 原因写作"没解析到 PPS"，指向完全错误的方向。
    auto collect = [](const std::vector<NalUnit>& nals) {
        for (const NalUnit& nal : nals) {
            if (nal.type == kNalSps || nal.type == kNalPps)
                return true;
        }
        return false;
    };

    std::vector<NalUnit> nals;
    // Annex B 有唯一可靠的前缀判据，先按它走，避免把裸流误当 hvcC 去解释。
    const bool looks_annexb =
        size >= 4 && ((data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))));
    if (looks_annexb)
        nals = ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::Hevc);
    if (!collect(nals))
        nals = ExtradataParser::ParseWithFormat(ExtradataFormat::HvcC, data, size).nal_units;
    if (!collect(nals))
        nals = ExtradataParser::Parse(data, size).nal_units;
    if (!collect(nals)) {
        for (int width : {4, 2, 1}) {
            nals = ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::Hevc, width, nullptr);
            if (collect(nals))
                break;
        }
    }

    bool got_sps = false;
    for (const NalUnit& nal : nals) {
        if (nal.type == kNalSps)
            got_sps = HandleSps(nal) || got_sps;
        else if (nal.type == kNalPps)
            HandlePps(nal);
    }
    return got_sps;
}

void HevcPocTracker::FeedNal(const NalUnit& nal, bool has_time, double time_seconds, int64_t packet_index,
                             int& new_frames) {
    const int type = static_cast<int>(nal.type);
    if (type == kNalSps) {
        HandleSps(nal);
        return;
    }
    if (type == kNalPps) {
        HandlePps(nal);
        return;
    }
    if (type == kNalVps)
        return;
    if (type > kNalVclLast)
        return; // AUD / EOS / EOB / FD / SEI(39,40)：与 POC 无关
    if (type >= kNalReservedVclFirst)
        return; // 24..31 保留 VCL：语法未定义，不猜

    PictureHead head;
    if (!ReadPicture(nal, head))
        return;

    // 规范里 CRA 的 NoRaslOutputFlag 只在"码流第一幅图 / EOS 之后第一幅"为 1。
    // 我们的扫描起点就是它能看到的第一个位置，所以第一幅图像是 IRAP 时按
    // NoRaslOutputFlag == 1 处理（等价于一次复位）。不这么做，从 CRA 开头的
    // 文件会拿初始 0 当 prev 去算 MSB，POC 会整体偏出一个回绕周期。
    if (head.irap && timeline_.frames_observed == 0)
        head.idr = true;

    const int64_t poc = ComputePoc(head);
    PushFrame(head, poc, has_time, time_seconds, packet_index);
    ++new_frames;
}

bool HevcPocTracker::ReadPicture(const NalUnit& nal, PictureHead& out) {
    const int type = static_cast<int>(nal.type);
    const std::vector<uint8_t> rbsp = UnescapeRbsp(nal.data.data(), nal.data.size());
    if (rbsp.empty())
        return false;
    BitReader br;
    br.Reset(rbsp.data(), rbsp.size());

    // ---- slice_segment_header()（7.3.6.1），只读到 slice_type 为止 ----
    out.nal_unit_type = type;
    out.irap = IsIrap(type);
    out.idr = IsAlwaysNoRasl(type);

    const bool first_segment = br.ReadBit();
    // 只要图像的第一段：后续 slice / dependent slice segment 不推进 POC 状态
    // （规范里 prevPicOrderCnt* 记的是"上一幅图像"，不是"上一段"）。
    // 直接返回的好处是：不必为了跳过 slice_segment_address 去算 PicSizeInCtbsY。
    if (!first_segment)
        return false;

    if (out.irap)
        br.ReadBit(); // no_output_of_prior_pics_flag

    const uint32_t pps_id = br.ReadUEClamped(static_cast<uint32_t>(kMaxPpsId - 1));
    if (!pps_[pps_id].ready) {
        NoteReason("扫描到 slice 之前没有解析到它引用的 PPS（id=" + std::to_string(pps_id) +
                   "），无法定位 slice_segment_header 字段");
        return false;
    }
    const PpsPocState pps = pps_[pps_id];
    const int sps_id = pps.sps_id;
    if (sps_id < 0 || sps_id >= kMaxSpsId || !sps_[sps_id].ready) {
        NoteReason("PPS 引用的 SPS（id=" + std::to_string(sps_id) + "）没有解析到，无法计算 POC");
        return false;
    }
    const SpsPocState& sps = sps_[sps_id];
    out.max_poc_lsb = int64_t(1) << sps.log2_max_poc_lsb;

    for (int i = 0; i < pps.num_extra_slice_header_bits; ++i)
        br.ReadBit(); // slice_reserved_flag[i]

    const uint32_t slice_type_raw = br.ReadUEClamped(2);
    out.slice_type = NormalizeSliceType(slice_type_raw);
    if (out.slice_type < 0)
        return false;

    if (pps.output_flag_present)
        br.ReadBit(); // pic_output_flag
    if (sps.separate_colour_plane)
        br.ReadBits(2); // colour_plane_id

    if (type != kNalIdrWRadl && type != kNalIdrNLp) {
        out.poc_lsb = static_cast<int>(br.ReadBits(sps.log2_max_poc_lsb));
        out.has_poc_lsb = true;
    }
    // ❗ 位读完之前就把数据耗尽的（被容器截断 / 半包），读出来的 poc_lsb 是
    // 凭空补的 0，把它当一帧存进去会凭空多出一幅"POC = 0 的帧"。
    // 宁可丢这一帧，也不要造一条假的时间线。
    if (br.HasError())
        return false;
    return true;
}

int64_t HevcPocTracker::ComputePoc(const PictureHead& head) {
    // 8.3.1：HEVC 只有一套公式（没有 H.264 的 pic_order_cnt_type 分支）。
    if (head.idr) {
        prev_poc_msb_ = 0;
        prev_poc_lsb_ = 0;
    }
    const int64_t max_poc_lsb = head.max_poc_lsb > 0 ? head.max_poc_lsb : 1;
    const int64_t lsb = head.has_poc_lsb ? head.poc_lsb : 0;
    int64_t poc_msb = prev_poc_msb_;
    // 与 H.264 type 0 同一条回绕判据，但两个方向的不等号不同（一个是 >=
    // 半周期，一个是 > 半周期），照抄时最容易在这里差一个等号。
    // 复位帧不参与：它的 prev 已被显式归零，再走回绕判断会把 lsb 较大的
    // 随机接入点算成负 POC，制造一次假的"POC 回退"。
    if (!head.idr) {
        if (lsb < prev_poc_lsb_ && (prev_poc_lsb_ - lsb) >= max_poc_lsb / 2)
            poc_msb = prev_poc_msb_ + max_poc_lsb;
        else if (lsb > prev_poc_lsb_ && (lsb - prev_poc_lsb_) > max_poc_lsb / 2)
            poc_msb = prev_poc_msb_ - max_poc_lsb;
    }

    prev_poc_msb_ = poc_msb;
    prev_poc_lsb_ = lsb;
    return poc_msb + lsb;
}

void HevcPocTracker::PushFrame(const PictureHead& head, int64_t poc, bool has_time, double time_seconds,
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
    if (head.irap)
        ++timeline_.irap_count; // 含中间的 CRA：可跳转，但不复位 POC

    if (timeline_.frames.size() >= options_.max_frames) {
        timeline_.truncated = true;
        return;
    }
    model::FrameOrderEntry entry;
    // 存真实的 HEVC NAL 类型（16..23 = IRAP），不像 H.264 那样压成 1/5 ——
    // HEVC 的 IRAP 子类型（IDR/BLA/CRA）是有诊断价值的，压平就丢掉了。
    entry.nal_unit_type = head.nal_unit_type;
    entry.slice_type = head.slice_type;
    entry.idr = head.idr;
    entry.irap = head.irap;
    entry.frame_num = 0; // HEVC 没有 frame_num
    entry.poc = poc;
    entry.packet_index = packet_index;
    entry.has_time = has_time;
    entry.time_seconds = time_seconds;
    timeline_.frames.push_back(entry);
}

const model::FrameOrderTimeline& HevcPocTracker::Finish() {
    if (finished_)
        return timeline_;
    finished_ = true;

    if (!timeline_.frames.empty() || timeline_.frames_observed > 0) {
        timeline_.available = true;
        // HEVC 没有 pic_order_cnt_type 字段；对外仍按"只有一套公式"报 0，
        // 让上层不必为两种编码各写一套显示文案。
        timeline_.poc_type = any_sps_ ? 0 : -1;
        // HEVC 只有帧编码，永远不存在场图像，field_coded 保持 false：
        // 它是"关掉缺帧推断"的开关，恒 false 才能让 POC 步长推断生效。
        Recompute();
    } else if (timeline_.unavailable_reason.empty()) {
        timeline_.unavailable_reason = "没有扫到可解析的 HEVC slice（可能是裸流之外的封装或全部为非 VCL NAL）";
    }
    return timeline_;
}

void HevcPocTracker::Recompute() {
    // 重排 / GOP / 重复 / 缺口的判定规则与 H.264 完全一样，只维护一份
    // （见 FrameOrderStats.cpp 里对"为什么不能跨 GOP 统计 POC"的说明）。
    ComputeFrameOrderStatistics(timeline_);
}

} // namespace videoeye
