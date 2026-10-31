#include "core/analysis/codec/VvcPocTracker.h"

#include "core/analysis/codec/FrameOrderStats.h"
#include "core/analysis/codec/VvcBitstreamParser.h"
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataParser.h"

namespace videoeye {

namespace {

// H.266 7.4.1 Table 5 里与本文件相关的 NAL 类型。
// 写成常量而不是魔法数字：IRAP 区间 7..11 与 HEVC 的 16..23 完全不同，
// 记错一位整条判据就反了（7/8 是 IDR，9 是 CRA，10 是 GDR，11 是保留 IRAP）。
constexpr int kNalVps = 14;
constexpr int kNalSps = 15;
constexpr int kNalPps = 16;
constexpr int kNalPh = 19;
constexpr int kNalEos = 21;

constexpr int kNalIdrWRadl = 7;
constexpr int kNalIdrNLp = 8;
constexpr int kNalCra = 9;
constexpr int kNalGdr = 10;
constexpr int kNalRsvIrap = 11;

// VCL 是 0..11（TRAIL/STSA/RADL/RASL/IDR/CRA/GDR/RSV_IRAP）。
constexpr int kNalVclFirst = 0;
constexpr int kNalVclLast = 11;

bool IsIrap(int type) {
    return type >= kNalIdrWRadl && type <= kNalRsvIrap;
}

// NoRaslOutputFlag 恒为 1 的只有 IDR。CRA / GDR 要等"码流第一幅图 / EOS 之后"，
// 那种情形由调用方按 frames_observed / eos_seen_ 另行判断。
bool IsAlwaysNoRasl(int type) {
    return type == kNalIdrWRadl || type == kNalIdrNLp;
}

// 前导图像：RADL(2) / RASL(3)。它们的 POC 天生小于所属 IRAP，不是 B 帧重排。
// ❗ 千万别沿用 HEVC 的 "6..9" 判据 —— VVC 的 7/8/9 是 IDR/CRA。
bool IsLeadingPicture(int type) {
    return type == 2 || type == 3;
}

// VVC 的 sh_slice_type（0=B 1=P 2=I）→ 项目内部约定（0=P 1=B 2=I）。
int NormalizeSliceType(uint32_t vvc_slice_type) {
    switch (vvc_slice_type) {
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

void VvcPocTracker::Reset(int stream_index, const Options& options) {
    stream_index_ = stream_index;
    options_ = options;
    framing_ = Framing::Auto;
    prefix_bytes_ = 0;
    for (int i = 0; i < kMaxSpsId; ++i)
        sps_[i] = SpsState{};
    for (int i = 0; i < kMaxPpsId; ++i)
        pps_[i] = PpsState{};
    any_sps_ = false;
    pending_ = PhState{};
    eos_seen_ = false;
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
    timeline_ = model::FrameOrderTimeline{};
    finished_ = false;
}

void VvcPocTracker::NoteReason(const std::string& reason) {
    if (timeline_.unavailable_reason.empty())
        timeline_.unavailable_reason = reason;
}

bool VvcPocTracker::DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const {
    // 与 H.264 / HEVC 同一套启发式（含同一个 [0x100,0x1FF] 坑，由 prefix_bytes_ 分支挡住）。
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

int VvcPocTracker::Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index) {
    if (data == nullptr || size == 0)
        return 0;

    int new_frames = 0;
    bool any = false;
    auto visit = [&](const NalView& view) {
        any = true;
        FeedNal(view, has_time, time_seconds, packet_index, new_frames);
    };

    // 优先级同 H264/Hevc：容器给了前缀宽度就直接用，别让找起始码的启发式插队。
    if (prefix_bytes_ > 0) {
        ExtradataParser::ForEachLengthPrefixedNalUnit(data, size, NalSyntax::Vvc, prefix_bytes_, nullptr, visit);
        if (any)
            return new_frames;
    }

    if (framing_ == Framing::AnnexB) {
        ExtradataParser::ForEachAnnexBNalUnit(data, size, NalSyntax::Vvc, visit);
        return new_frames;
    }
    if (framing_ == Framing::LengthPrefixed) {
        ExtradataParser::ForEachLengthPrefixedNalUnit(data, size, NalSyntax::Vvc, prefix_bytes_ > 0 ? prefix_bytes_ : 4,
                                                      nullptr, visit);
        return new_frames;
    }

    int detected = 0;
    if (DetectLengthPrefixed(data, size, detected)) {
        ExtradataParser::ForEachLengthPrefixedNalUnit(data, size, NalSyntax::Vvc, detected, nullptr, visit);
    } else {
        ExtradataParser::ForEachAnnexBNalUnit(data, size, NalSyntax::Vvc, visit);
    }
    return new_frames;
}

bool VvcPocTracker::HandleSps(const NalView& nal) {
    // 复用码流解析器，不在追踪器里再写一份 SPS 语法：两份实现迟早会分叉，
    // 而"哪一份是对的"在面板上完全看不出来。
    NalUnit unit;
    unit.type = nal.type;
    unit.size = nal.size;
    unit.data.assign(nal.payload, nal.payload + nal.size);
    const model::VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(unit);
    if (!sps.present)
        return false;

    const int id = sps.sps_seq_parameter_set_id;
    if (id < 0 || id >= kMaxSpsId)
        return false;
    // 规范范围 [0,12]（即位宽 4..16）。解析器读的是 u(4)，天然 0..15，
    // 13..15 是非法值：拿它做 `1 << x` 与"按 30 位读 lsb"都会造出一条假时间线。
    const int minus4 = sps.sps_log2_max_pic_order_cnt_lsb_minus4;
    if (minus4 < 0 || minus4 > 12)
        return false;

    SpsState& slot = sps_[id];
    slot.ready = true;
    slot.log2_max_poc_lsb = minus4 + 4;
    slot.poc_msb_cycle_flag = sps.sps_poc_msb_cycle_flag;
    slot.subpic_info_present = sps.sps_subpic_info_present_flag;
    slot.subpic_id_len = sps.sps_subpic_id_len_minus1 + 1;

    slot.num_extra_ph_bytes = sps.sps_num_extra_ph_bytes;
    for (int i = 0; i < 8; ++i)
        slot.extra_ph_present[i] = sps.sps_extra_ph_bit_present_flag[i];
    slot.num_extra_sh_bytes = sps.sps_num_extra_sh_bytes;
    for (int i = 0; i < 8; ++i)
        slot.extra_sh_present[i] = sps.sps_extra_sh_bit_present_flag[i];

    any_sps_ = true;
    return true;
}

bool VvcPocTracker::HandlePps(const NalView& nal) {
    NalUnit unit;
    unit.type = nal.type;
    unit.size = nal.size;
    unit.data.assign(nal.payload, nal.payload + nal.size);
    const model::VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(unit);
    if (!pps.present)
        return false;

    const int id = pps.pps_pic_parameter_set_id;
    if (id < 0 || id >= kMaxPpsId)
        return false;
    const int sps_id = pps.pps_seq_parameter_set_id;
    if (sps_id < 0 || sps_id >= kMaxSpsId)
        return false;

    PpsState& slot = pps_[id];
    slot.ready = true;
    slot.sps_id = sps_id;
    slot.no_pic_partition = pps.pps_no_pic_partition_flag;
    slot.rect_slice = pps.pps_rect_slice_flag;
    slot.single_slice_per_subpic = pps.pps_single_slice_per_subpic_flag;
    return true;
}

bool VvcPocTracker::FeedExtradata(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return false;

    // 与 HEVC 同样的多路尝试：MP4 的 VVC extradata 是 vvcC 记录本体
    // （box 头已剥掉），里面的 SPS/PPS 载荷本身可能含起始码，任何单一
    // 启发式都会在某一类文件上判错，于是逐条尝试、以"真的出现 15/16 号 NAL"为准。
    auto collect = [](const std::vector<NalUnit>& nals) {
        for (const NalUnit& nal : nals) {
            if (nal.type == kNalSps || nal.type == kNalPps)
                return true;
        }
        return false;
    };

    std::vector<NalUnit> nals;
    const bool looks_annexb =
        size >= 4 && ((data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1))));
    if (looks_annexb)
        nals = ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::Vvc);
    if (!collect(nals))
        nals = ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC, data, size).nal_units;
    if (!collect(nals))
        nals = ExtradataParser::Parse(data, size).nal_units;
    if (!collect(nals)) {
        for (int width : {4, 2, 1}) {
            nals = ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::Vvc, width, nullptr);
            if (collect(nals))
                break;
        }
    }

    bool got_sps = false;
    for (const NalUnit& nal : nals) {
        if (nal.type == kNalSps)
            got_sps = HandleSps(ToNalView(nal)) || got_sps;
        else if (nal.type == kNalPps)
            HandlePps(ToNalView(nal));
    }
    return got_sps;
}

void VvcPocTracker::FeedNal(const NalView& nal, bool has_time, double time_seconds, int64_t packet_index,
                            int& new_frames) {
    const int type = static_cast<int>(nal.type);
    switch (type) {
    case kNalVps:
        return;
    case kNalSps:
        HandleSps(nal);
        return;
    case kNalPps:
        HandlePps(nal);
        return;
    case kNalPh: {
        const std::vector<uint8_t> rbsp = UnescapeRbsp(nal.payload, nal.size);
        if (rbsp.empty())
            return;
        BitReader reader;
        reader.Reset(rbsp.data(), rbsp.size());
        PhState ph;
        if (!ReadPictureHeader(reader, type, ph))
            return;
        ph.valid = true;
        ph.consumed = false;
        // 上一个 PH 还没被任何 slice 消费就被新的 PH 覆盖：那是一幅没有
        // VCL 的畸形图像，丢掉即可（不要拿它凑一帧）。
        pending_ = ph;
        return;
    }
    case kNalEos:
        // 8.1.3：EOS 之后的第一幅 IRAP 才把 NoRaslOutputFlag 置 1。
        eos_seen_ = true;
        return;
    default:
        break;
    }

    if (type < kNalVclFirst || type > kNalVclLast)
        return; // AUD / OPI / DCI / APS / SEI / FD：与 POC 无关

    if (ReadSlice(nal, has_time, time_seconds, packet_index))
        ++new_frames;
}

bool VvcPocTracker::ReadPictureHeader(BitReader& reader, int nal_unit_type, PhState& out) {
    // ---- picture_header_structure()（7.3.2.8），只读到 POC 为止 ----
    // ❗ 前两位都无条件读：ph_gdr_or_irap_pic_flag / ph_non_ref_pic_flag。
    //    以前按 HEVC 的直觉只读一位，后面整体错位。
    out.nal_unit_type = nal_unit_type;
    out.gdr_or_irap = reader.ReadBit();
    out.non_ref = reader.ReadBit();
    out.gdr = out.gdr_or_irap ? reader.ReadBit() : false;
    out.inter_slice_allowed = reader.ReadBit();
    out.intra_slice_allowed = out.inter_slice_allowed ? reader.ReadBit() : true;

    // ph_pic_parameter_set_id 是 ue(v)（不是 u(6)）。
    const uint32_t pps_id = reader.ReadUEClamped(static_cast<uint32_t>(kMaxPpsId - 1));
    if (!pps_[pps_id].ready) {
        NoteReason("扫描到 PH/slice 之前没有解析到它引用的 PPS（id=" + std::to_string(pps_id) +
                   "），无法确定 MaxPicOrderCntLsb");
        return false;
    }
    const PpsState pps = pps_[pps_id];
    const int sps_id = pps.sps_id;
    if (sps_id < 0 || sps_id >= kMaxSpsId || !sps_[sps_id].ready) {
        NoteReason("PPS 引用的 SPS（id=" + std::to_string(sps_id) + "）没有解析到，无法计算 POC");
        return false;
    }
    const SpsState& sps = sps_[sps_id];
    if (sps.poc_msb_cycle_flag) {
        // 规范允许 SPS 启用 ph_poc_msb_cycle_val 来显式携带 POC 高位。本实现
        // **没有**真实样本验证这条路径（生成器与解析器互证不算验证），所以
        // 按 H.264 pic_order_cnt_type==1 的同样口径处理：明确拒绝，不猜。
        // 猜错的后果是一整条时间线偏一个 lsb 周期，而每一步都"解析成功"。
        NoteReason("VVC SPS 启用了 POC MSB cycle，本实现未验证该路径，拒绝给出显示顺序（不猜）");
        return false;
    }

    out.pps_id = static_cast<int>(pps_id);
    out.max_poc_lsb = int64_t(1) << sps.log2_max_poc_lsb;
    // ph_pic_order_cnt_lsb 无条件存在（IDR 也有，规范要求写 0）。
    out.poc_lsb = static_cast<int>(reader.ReadBits(sps.log2_max_poc_lsb));

    if (out.gdr) {
        // ph_recovery_poc_cnt：规范范围 [0, MaxPicOrderCntLsb - 1]，与 POC 无关，
        // 但必须精确吃掉这一位段。有界读只为"别被一个荒谬 ue(v) 拖住"。
        reader.ReadUEClamped(static_cast<uint32_t>(out.max_poc_lsb - 1));
    }

    for (int i = 0; i < sps.num_extra_ph_bytes * 8; ++i) {
        if (sps.extra_ph_present[i])
            reader.ReadBit(); // ph_extra_bit[i]
    }

    // 位读完之前就耗尽的（截断 / 半包）：读出来的是凭空补的 0，
    // 把它当一帧存进去会凭空多出一幅 POC=0 的画像。宁可丢这一帧。
    if (reader.HasError())
        return false;
    return true;
}

bool VvcPocTracker::ReadSlice(const NalView& nal, bool has_time, double time_seconds, int64_t packet_index) {
    const int type = static_cast<int>(nal.type);
    const std::vector<uint8_t> rbsp = UnescapeRbsp(nal.payload, nal.size);
    if (rbsp.empty())
        return false;

    BitReader reader;
    reader.Reset(rbsp.data(), rbsp.size());

    // ---- slice_header()（7.3.2.9）----
    const bool inline_ph = reader.ReadBit();
    PhState ph;
    if (inline_ph) {
        if (!ReadPictureHeader(reader, type, ph))
            return false;
        ph.valid = true;
        ph.consumed = false;
        // 内联 PH 的语义约束是"该图像只有一个 slice"（否则 PH 会被重复携带），
        // 所以每次内联 PH 就是一幅新图像，直接替换 pending_。
        pending_ = ph;
    } else {
        if (!pending_.valid) {
            NoteReason("扫描到 slice 之前没有解析到 PH（picture header，NAL 19），"
                       "VVC 的 POC 只写在 PH 里");
            return false;
        }
        ph = pending_;
        ph.nal_unit_type = type;
        // 用当前 slice 的 NAL 类型判定 IRAP/前导：PH 里没有 nal_unit_type。
        ph.gdr_or_irap = IsIrap(type);
    }

    // 同一幅图像的后续 slice 不重复落帧（规范里 prevPoc 记的是"上一幅图像"）。
    if (pending_.consumed)
        return false;

    int slice_type = -1;
    if (!ph.inter_slice_allowed) {
        // ph_inter_slice_allowed_flag == 0 时 sh_slice_type 不存在，规范推断为 I。
        slice_type = 2;
    } else {
        const PpsState pps = pps_[ph.pps_id];
        const SpsState& sps = sps_[pps.sps_id];

        // 只有能证明 sh_slice_address 宽度为 0 时才继续读 slice_type。
        // 宽度由 tile/slice 划分决定，而 PPS 里没有足够的字段（explicit tile 宽度、
        // 显式 slice 布局）反推它 —— 猜一个宽度读出来的 slice_type 看着很合理，
        // 但 I/P/B 计数会整体错，且没有任何报错。
        bool address_absent = false;
        if (!sps.subpic_info_present) {
            if (pps.no_pic_partition) {
                // 不划分图像：NumTilesInPic = 1、每 tile 一个 slice ⇒ 没有地址字段
                address_absent = true;
            } else if (pps.rect_slice && pps.single_slice_per_subpic) {
                // 每个 subpic 一个 slice，且只有一个 subpic ⇒ 没有地址字段
                address_absent = true;
            }
        } else if (pps.rect_slice && pps.single_slice_per_subpic) {
            // 有 subpic 时先有 sh_subpic_id（定长），地址仍然不存在
            if (sps.subpic_id_len >= 1 && sps.subpic_id_len <= 16) {
                reader.ReadBits(sps.subpic_id_len);
                address_absent = true;
            }
        }

        if (address_absent) {
            for (int i = 0; i < sps.num_extra_sh_bytes * 8; ++i) {
                if (sps.extra_sh_present[i])
                    reader.ReadBit(); // sh_extra_bit[i]
            }
            // sh_num_tiles_in_slice_minus1 只在"非矩形 slice 且 tile 数 > 1"时存在，
            // 而上面能走到 address_absent 的两种情况都满足 tile 数 == 1。
            const uint32_t raw = reader.ReadUEClamped(2);
            slice_type = NormalizeSliceType(raw);
            if (slice_type < 0 || reader.HasError())
                return false;
        }
    }

    const bool no_rasl = DecideNoRasl(ph);
    const int64_t poc = ComputePoc(ph, no_rasl);
    PushFrame(ph, slice_type, poc, no_rasl, has_time, time_seconds, packet_index);
    pending_.consumed = true;
    return true;
}

bool VvcPocTracker::DecideNoRasl(const PhState& ph) const {
    // IDR 恒 NoRasl=1；CRA / GDR 只有"码流第一幅图或 EOS 之后第一幅"才是。
    // 首幅图像按"能看到的第一幅"处理（等价于一次复位）—— 否则从 CRA 开头的
    // 文件会拿初始 0 当 prev 算 MSB，POC 整体偏出一个回绕周期。
    const bool first_picture = timeline_.frames_observed == 0;
    return IsAlwaysNoRasl(ph.nal_unit_type) ||
           ((ph.nal_unit_type == kNalCra || ph.nal_unit_type == kNalGdr) && (first_picture || eos_seen_));
}

int64_t VvcPocTracker::ComputePoc(const PhState& ph, bool no_rasl) {
    // 8.3.1 的回绕推算。ph_poc_msb_cycle_present_flag == 1 的分支不支持：
    // 那种 SPS 在 ReadPictureHeader 里就被拒绝了（见那里的注释）。
    if (no_rasl) {
        prev_poc_msb_ = 0;
        prev_poc_lsb_ = 0;
        eos_seen_ = false;
    }

    const int64_t max_lsb = ph.max_poc_lsb > 0 ? ph.max_poc_lsb : 1;
    const int64_t lsb = ph.poc_lsb;
    int64_t poc_msb = prev_poc_msb_;
    if (!no_rasl) {
        // 与 HEVC 同一条回绕判据（两个方向的不等号不同，别只抄一边）
        if (lsb < prev_poc_lsb_ && (prev_poc_lsb_ - lsb) >= max_lsb / 2)
            poc_msb = prev_poc_msb_ + max_lsb;
        else if (lsb > prev_poc_lsb_ && (lsb - prev_poc_lsb_) > max_lsb / 2)
            poc_msb = prev_poc_msb_ - max_lsb;
    }

    prev_poc_msb_ = poc_msb;
    prev_poc_lsb_ = lsb;
    return poc_msb + lsb;
}

void VvcPocTracker::PushFrame(const PhState& ph, int slice_type, int64_t poc, bool no_rasl, bool has_time,
                              double time_seconds, int64_t packet_index) {
    ++timeline_.frames_observed;

    switch (slice_type) {
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

    const bool idr = no_rasl;
    const bool irap = IsIrap(ph.nal_unit_type);
    if (idr)
        ++timeline_.idr_count;
    if (irap)
        ++timeline_.irap_count;

    if (timeline_.frames.size() >= options_.max_frames) {
        timeline_.truncated = true;
        return;
    }
    model::FrameOrderEntry entry;
    // 存真实的 VVC NAL 类型（7..11 = IRAP）：IDR/CRA/GDR 的区分有诊断价值。
    entry.nal_unit_type = ph.nal_unit_type;
    entry.slice_type = slice_type;
    entry.idr = idr;
    entry.irap = irap;
    // 前导图像标记必须在这里落：FrameOrderStats 的"POC 回退不算重排"要靠它，
    // 而 VVC 的类型编号与 HEVC 的 6..9 完全不同（见 FrameOrderInfo 的注释）。
    entry.leading = IsLeadingPicture(ph.nal_unit_type);
    entry.frame_num = 0; // VVC 没有 frame_num
    entry.poc = poc;
    entry.packet_index = packet_index;
    entry.has_time = has_time;
    entry.time_seconds = time_seconds;
    timeline_.frames.push_back(entry);
}

const model::FrameOrderTimeline& VvcPocTracker::Finish() {
    if (finished_)
        return timeline_;
    finished_ = true;

    if (!timeline_.frames.empty() || timeline_.frames_observed > 0) {
        timeline_.available = true;
        // VVC 只有一套 POC 公式（回绕推算 + 可选的 msb cycle）；对外仍报 0，
        // 让上层不必为每种编码各写一套显示文案。
        timeline_.poc_type = any_sps_ ? 0 : -1;
        // VVC 没有场编码，恒 false 才能让 POC 步长推断生效。
        Recompute();
    } else if (timeline_.unavailable_reason.empty()) {
        timeline_.unavailable_reason =
            "没有扫到可解析的 VVC 图像（PH 或 slice 缺失、参数集不在 extradata 里，或全部为非 VCL NAL）";
    }
    return timeline_;
}

void VvcPocTracker::Recompute() {
    // 重排 / GOP / 重复 / 缺口的规则与 H.264 / HEVC 共用一份（FrameOrderStats.cpp）。
    ComputeFrameOrderStatistics(timeline_);
}

} // namespace videoeye
