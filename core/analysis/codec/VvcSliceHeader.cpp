#include "core/analysis/codec/VvcSliceHeader.h"

#include "core/analysis/codec/VvcNalTypes.h"
#include "core/media/codec/BitReader.h"

namespace videoeye {

namespace {

bool IsIdr(int nal_unit_type) {
    return nal_unit_type == kVvcNutIdrWRadl || nal_unit_type == kVvcNutIdrNLp;
}

// sh_no_output_of_prior_pics_flag 只在这四种 NAL 类型上存在（H.266 7.3.2.9）。
bool HasNoOutputOfPriorPicsFlag(int nal_unit_type) {
    return nal_unit_type == kVvcNutIdrWRadl || nal_unit_type == kVvcNutIdrNLp || nal_unit_type == kVvcNutCra ||
           nal_unit_type == kVvcNutGdr;
}

// ceil(log2(v))，给 sh_slice_address 的位宽用。v <= 1 时返回 0（宽度为 0）。
int CeilLog2(uint32_t v) {
    int bits = 0;
    while (bits < 32 && (1u << bits) < v)
        ++bits;
    return bits;
}

// VVC 的 sh_slice_type（0=B 1=P 2=I）→ 项目内部约定（0=P 1=B 2=I，与 H.264 的
// slice_type % 5 同口径，Aggregate 那边只认这一套）。
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

// --------------------------------------------------------------------------
// ref_pic_lists() 相关（H.266 7.3.2.9 与 7.3.2.5）
//
// 这一段是"P/B slice 能不能给出 QP"的唯一门槛。它不是"可以糊弄过去的可变段"：
// 里面 num_ltrp_entries 依赖每一项的长/短期属性，数不对就整段错位，而错位后
// 读出的 sh_qp_delta 看着完全合理 —— 这比没有更糟。所以宁可在这里失败并报出
// 原因，也不返回一个"大概是这个 QP"的值。
// --------------------------------------------------------------------------

// 一条参考列表的"形状"：项数 + 其中长期参考项的个数 + ltrp 标志放哪。
struct RefListShape {
    int num_entries = 0;
    int num_ltrp_entries = 0;
    bool ltrp_in_header = false;
};

// 从 SPS 记录下来的结构里取形状（rpl_sps_flag == 1 的路径）。
bool ShapeFromSps(const model::VvcRefPicListStruct& list, RefListShape& out) {
    if (!list.recorded)
        return false;
    if (static_cast<int>(list.st_ref_pic_flag.size()) < list.num_ref_entries ||
        static_cast<int>(list.inter_layer_ref_pic_flag.size()) < list.num_ref_entries) {
        return false;
    }
    int ltrp = 0;
    for (int k = 0; k < list.num_ref_entries; ++k) {
        const bool inter_layer = list.inter_layer_ref_pic_flag[static_cast<size_t>(k)] != 0;
        const bool st = list.st_ref_pic_flag[static_cast<size_t>(k)] != 0;
        if (!inter_layer && !st)
            ++ltrp;
    }
    out.num_entries = list.num_ref_entries;
    out.num_ltrp_entries = ltrp;
    out.ltrp_in_header = list.ltrp_in_header_flag;
    return true;
}

// 读一条内联在 slice 里的 ref_pic_list_struct（rpl_sps_flag == 0）。
bool ReadInlineRefPicListStruct(BitReader& reader, const model::VvcSpsInfo& sps, int poc_lsb_bits, RefListShape& out) {
    const uint32_t num_ref_entries = reader.ReadUEBounded(static_cast<uint32_t>(model::kMaxRecordedRefEntries));
    if (reader.HasError())
        return false;
    // 内联位置对应 rpls_idx == sps_num_ref_pic_lists[list]，规范在这一档把
    // ltrp_in_header_flag 推断为 1 —— 也就是说这一个 bit **不在**码流里。
    // 照 SPS 的分支去读会多吃一位，后面全错。
    const bool ltrp_in_header = sps.sps_long_term_ref_pics_flag;

    int ltrp = 0;
    for (uint32_t i = 0; i < num_ref_entries; ++i) {
        bool inter_layer_ref_pic = false;
        if (sps.sps_inter_layer_prediction_enabled_flag)
            inter_layer_ref_pic = reader.ReadBit();
        if (!inter_layer_ref_pic) {
            bool st_ref_pic = true;
            if (sps.sps_long_term_ref_pics_flag)
                st_ref_pic = reader.ReadBit();
            if (st_ref_pic) {
                const uint32_t abs_delta_poc_minus1 = reader.ReadUE();
                // 与 SPS 侧同一处口径：加权预测开启且 i > 0 时 ue(v) 直接就是
                // abs_delta_poc_st，否则要 +1 才能判断符号位是否存在。
                uint32_t abs_delta_poc_st = abs_delta_poc_minus1;
                if (!((sps.sps_weighted_pred_flag || sps.sps_weighted_bipred_flag) && i > 0))
                    abs_delta_poc_st += 1;
                if (abs_delta_poc_st > 0)
                    reader.ReadBit(); // strp_entry_sign_flag
            } else if (!ltrp_in_header) {
                reader.SkipBits(poc_lsb_bits); // rpls_poc_lsb_lt
                ++ltrp;
            } else {
                ++ltrp;
            }
        } else {
            reader.ReadUE(); // ilrp_idx[i]（多层才可能出现）
        }
    }
    if (reader.HasError())
        return false;
    out.num_entries = static_cast<int>(num_ref_entries);
    out.num_ltrp_entries = ltrp;
    out.ltrp_in_header = ltrp_in_header;
    return true;
}

// 跳过 ltrp 段：长期参考项的 POC 低位与 msb cycle。
bool SkipLtrpEntries(BitReader& reader, const RefListShape& shape, int poc_lsb_bits) {
    for (int j = 0; j < shape.num_ltrp_entries; ++j) {
        if (shape.ltrp_in_header)
            reader.SkipBits(poc_lsb_bits); // poc_lsb_lt
        const bool msb_cycle_present = reader.ReadBit();
        if (msb_cycle_present)
            reader.ReadUE(); // delta_poc_msb_cycle_lt
        if (reader.HasError())
            return false;
    }
    return true;
}

// 完整的 ref_pic_lists()：两条 list 的项数从 out_num_entries 返回。
bool SkipRefPicLists(BitReader& reader, const model::VvcSpsInfo& sps, const model::VvcPpsInfo& pps, int poc_lsb_bits,
                     int out_num_entries[2]) {
    bool rpl_sps_flag[2] = {false, false};
    int rpl_idx[2] = {0, 0};

    for (int i = 0; i < 2; ++i) {
        RefListShape shape;
        if (sps.sps_num_ref_pic_lists[i] > 0 && (i == 0 || pps.pps_rpl1_idx_present_flag)) {
            rpl_sps_flag[i] = reader.ReadBit();
        } else if (sps.sps_num_ref_pic_lists[i] == 0) {
            rpl_sps_flag[i] = false;
        } else if (!pps.pps_rpl1_idx_present_flag && i == 1) {
            // list 1 跟随 list 0（规范 7.4.3.4）
            rpl_sps_flag[1] = rpl_sps_flag[0];
        }

        if (rpl_sps_flag[i]) {
            if (sps.sps_num_ref_pic_lists[i] > 1 && (i == 0 || (i == 1 && pps.pps_rpl1_idx_present_flag))) {
                const int bits = CeilLog2(static_cast<uint32_t>(sps.sps_num_ref_pic_lists[i]));
                if (bits <= 0 || bits > 8)
                    return false;
                rpl_idx[i] = static_cast<int>(reader.ReadBits(bits));
            } else if (sps.sps_num_ref_pic_lists[i] == 1) {
                rpl_idx[i] = 0;
            } else if (i == 1 && !pps.pps_rpl1_idx_present_flag) {
                rpl_idx[1] = rpl_idx[0];
            } else {
                // 规范推不出 rpl_idx（cbs 在这一档也直接报错），不猜。
                return false;
            }
            if (rpl_idx[i] < 0 || rpl_idx[i] >= sps.sps_num_ref_pic_lists[i])
                return false;
            if (!ShapeFromSps(sps.sps_ref_pic_lists[i][rpl_idx[i]], shape))
                return false;
        } else {
            if (!ReadInlineRefPicListStruct(reader, sps, poc_lsb_bits, shape))
                return false;
        }

        if (!SkipLtrpEntries(reader, shape, poc_lsb_bits))
            return false;
        out_num_entries[i] = shape.num_entries;
    }
    return !reader.HasError();
}

// pred_weight_table()（H.266 7.3.2.10）。num_ref_idx_active 由调用方按
// sh_num_ref_idx_active_override_flag 推好。
bool SkipPredWeightTable(BitReader& reader, const model::VvcSpsInfo& sps, const model::VvcPpsInfo& pps,
                         const int num_ref_idx_active[2]) {
    const bool has_chroma = sps.sps_chroma_format_idc != 0;
    reader.ReadUEBounded(7); // luma_log2_weight_denom
    if (has_chroma)
        reader.ReadSE(); // delta_chroma_log2_weight_denom

    // pps_wp_info_in_ph_flag == 1 时权重在 PH 里，slice 里根本不会有这张表，
    // 所以走到这里的 num_weights_l0 一定来自 num_ref_idx_active。
    const int num_weights_l0 = num_ref_idx_active[0];
    if (num_weights_l0 < 0 || num_weights_l0 > 15)
        return false;

    bool luma_flag[15] = {};
    bool chroma_flag[15] = {};
    for (int i = 0; i < num_weights_l0; ++i)
        luma_flag[i] = reader.ReadBit();
    if (has_chroma) {
        for (int i = 0; i < num_weights_l0; ++i)
            chroma_flag[i] = reader.ReadBit();
    }
    for (int i = 0; i < num_weights_l0; ++i) {
        if (luma_flag[i]) {
            reader.ReadSE(); // delta_luma_weight_l0
            reader.ReadSE(); // luma_offset_l0
        }
        if (chroma_flag[i]) {
            for (int j = 0; j < 2; ++j) {
                reader.ReadSE(); // delta_chroma_weight_l0
                reader.ReadSE(); // delta_chroma_offset_l0
            }
        }
        if (reader.HasError())
            return false;
    }

    int num_weights_l1 = 0;
    if (pps.pps_weighted_bipred_flag && num_ref_idx_active[1] > 0)
        num_weights_l1 = num_ref_idx_active[1];
    if (num_weights_l1 < 0 || num_weights_l1 > 15)
        return false;

    bool luma_flag_l1[15] = {};
    bool chroma_flag_l1[15] = {};
    for (int i = 0; i < num_weights_l1; ++i)
        luma_flag_l1[i] = reader.ReadBit();
    if (has_chroma) {
        for (int i = 0; i < num_weights_l1; ++i)
            chroma_flag_l1[i] = reader.ReadBit();
    }
    for (int i = 0; i < num_weights_l1; ++i) {
        if (luma_flag_l1[i]) {
            reader.ReadSE();
            reader.ReadSE();
        }
        if (chroma_flag_l1[i]) {
            for (int j = 0; j < 2; ++j) {
                reader.ReadSE();
                reader.ReadSE();
            }
        }
        if (reader.HasError())
            return false;
    }
    return !reader.HasError();
}

} // namespace

bool VvcSliceHeaderParser::ReadPhPrefix(BitReader& reader, VvcPhPrefix& out) {
    // ❗ 前两位**无条件**读：ph_gdr_or_irap_pic_flag / ph_non_ref_pic_flag。
    //    按 HEVC 的直觉只读一位，后面整体错位（与 VvcPocTracker 同一条坑）。
    const bool gdr_or_irap = reader.ReadBit();
    reader.ReadBit(); // ph_non_ref_pic_flag
    if (gdr_or_irap)
        reader.ReadBit(); // ph_gdr_pic_flag
    out.inter_slice_allowed = reader.ReadBit();
    out.intra_slice_allowed = out.inter_slice_allowed ? reader.ReadBit() : true;
    // ph_pic_parameter_set_id 是 ue(v)，不是定长 u(6)。
    const uint32_t pps_id = reader.ReadUEBounded(63);
    if (reader.HasError())
        return false;
    out.pps_id = static_cast<int>(pps_id);
    out.valid = true;
    return true;
}

VvcSliceHeaderInfo VvcSliceHeaderParser::Parse(const uint8_t* rbsp, std::size_t size, int nal_unit_type,
                                               const VvcPhPrefix& ph, const model::VvcSpsInfo& sps,
                                               const model::VvcPpsInfo& pps) {
    VvcSliceHeaderInfo out;
    if (rbsp == nullptr || size == 0) {
        out.stop_reason = "slice NAL 载荷为空";
        return out;
    }

    BitReader reader;
    reader.Reset(rbsp, size);

    // ---- slice_header()（7.3.2.9）----
    const bool inline_ph = reader.ReadBit();
    if (reader.HasError()) {
        out.stop_reason = "slice 载荷截断";
        return out;
    }
    if (inline_ph) {
        out.stop_reason =
            "picture_header 内联在 slice 开头（sh_picture_header_in_slice_header_flag=1），未实现 PH 全量解析";
        return out;
    }
    if (!ph.valid) {
        out.stop_reason = "slice 之前没有解析到 picture header（NAL 19）";
        return out;
    }

    // ---- sh_subpic_id / 地址宽度 ----
    // 地址宽度决定后面每一个字段的位置，是本文件唯一"猜错就全错"的地方。
    bool address_present = false;
    int address_bits = 0;

    if (sps.sps_subpic_info_present_flag) {
        const int id_len = sps.sps_subpic_id_len_minus1 + 1;
        if (id_len < 1 || id_len > 16) {
            out.stop_reason = "SPS 的 sps_subpic_id_len_minus1 越界（1..16），无法定位 sh_subpic_id";
            return out;
        }
        // 定长，先精确吃掉。
        reader.ReadBits(id_len); // sh_subpic_id
        if (reader.HasError()) {
            out.stop_reason = "slice 载荷截断";
            return out;
        }
    }

    if (pps.pps_rect_slice_flag) {
        // 矩形 slice：地址在 subpic 内编号，宽度 = ceil(log2(NumSlicesInSubpic))。
        int num_slices_in_subpic = 0;
        if (pps.pps_single_slice_per_subpic_flag) {
            num_slices_in_subpic = 1; // 每个 subpic 恰好一片
        } else if (!sps.sps_subpic_info_present_flag) {
            // 无 subpic ⇒ 整个图像就是 subpic 0，条数就是 pps_num_slices_in_pic。
            // 但显式 slice 布局会让 PPS 解析在这一档失败（present=false），
            // 那种 PPS 根本进不到这里；这里只做防御性判定。
            num_slices_in_subpic = pps.pps_num_slices_in_pic_minus1 + 1;
        } else {
            out.stop_reason = "多 subpic 且非每 subpic 一片：NumSlicesInSubpic 需要 subpic id 映射表（未实现）";
            return out;
        }
        if (num_slices_in_subpic > 1) {
            address_present = true;
            address_bits = CeilLog2(static_cast<uint32_t>(num_slices_in_subpic));
        }
    } else {
        // 非矩形 slice：地址是 tile 编号，宽度 = ceil(log2(NumTilesInPic))。
        const int num_tiles = pps.NumTilesInPic();
        if (num_tiles > 1) {
            address_present = true;
            address_bits = CeilLog2(static_cast<uint32_t>(num_tiles));
        }
    }

    uint32_t address = 0;
    if (address_present) {
        if (address_bits <= 0 || address_bits > 16) {
            out.stop_reason = "sh_slice_address 位宽越界";
            return out;
        }
        address = reader.ReadBits(address_bits);
    }

    // sh_extra_bit[i]：SPS 声明的"存在即一位"，数量与哪几位存在分开记。
    // 少了它，sh_slice_type 直接读成别的值（I/P/B 分布全错却不报错）。
    for (int i = 0; i < sps.sps_num_extra_sh_bytes * 8; ++i) {
        if (sps.sps_extra_sh_bit_present_flag[i])
            reader.ReadBit();
    }

    // 非矩形 slice 且该 tile 地址之后还有 tile 时才有这个 ue(v)。
    if (!pps.pps_rect_slice_flag) {
        const int remaining = pps.NumTilesInPic() - static_cast<int>(address);
        if (remaining > 1)
            reader.ReadUEBounded(static_cast<uint32_t>(remaining - 1));
    }

    if (reader.HasError()) {
        out.stop_reason = "slice 载荷截断或字段越界";
        return out;
    }

    // ---- sh_slice_type ----
    if (ph.inter_slice_allowed) {
        const uint32_t raw = reader.ReadUEBounded(2); // 0=B 1=P 2=I
        if (reader.HasError()) {
            out.stop_reason = "sh_slice_type 非法（ue 值 > 2）或位流截断";
            return out;
        }
        out.slice_type = NormalizeSliceType(raw);
    } else {
        // ph_inter_slice_allowed_flag == 0 时 sh_slice_type 不存在，规范推断为 I。
        out.slice_type = 2;
    }
    if (out.slice_type < 0) {
        out.stop_reason = "sh_slice_type 取值无法归一化";
        return out;
    }
    out.parsed = true;

    // "这一 slice 是不是一幅新图像的开头"：只有能确定图像内编号时才敢断言。
    if (!sps.sps_subpic_info_present_flag || sps.sps_num_subpics_minus1 == 0 || address_present) {
        // 后两种情况下 0 号地址必然属于图像（或唯一 subpic）的第一片。
        out.first_slice_of_picture = (address == 0);
    }

    // ---- 到 sh_qp_delta 的通道 ----
    //
    // 三条与 slice_type 无关的硬条件：满足不了就不是"少读几位"的问题，
    // 而是这一段的存在与否取决于 PH 里的标志（本文件不解析 PH）。
    if (pps.pps_qp_delta_info_in_ph_flag) {
        out.stop_reason = "PPS 把 QP delta 放在 picture header（pps_qp_delta_info_in_ph_flag=1）";
        return out;
    }
    if (sps.sps_lmcs_enabled_flag) {
        out.stop_reason = "SPS 启用了 LMCS：sh_lmcs_used_flag 是否存在取决于 PH 标志";
        return out;
    }
    if (sps.sps_explicit_scaling_list_enabled_flag) {
        out.stop_reason = "SPS 启用了显式缩放矩阵：sh_explicit_scaling_list_used_flag 取决于 PH 标志";
        return out;
    }

    // ---- IRAP 的 no_output 位 + ALF 段（I 与 P/B 都要读，位置在 RPL 之前）----
    if (HasNoOutputOfPriorPicsFlag(nal_unit_type))
        reader.ReadBit(); // sh_no_output_of_prior_pics_flag

    if (sps.sps_alf_enabled_flag && !pps.pps_alf_info_in_ph_flag) {
        const bool alf = reader.ReadBit(); // sh_alf_enabled_flag
        if (alf) {
            const uint32_t aps_ids = reader.ReadBits(3); // sh_num_alf_aps_ids_luma（u(3)）
            for (uint32_t i = 0; i < aps_ids && !reader.HasError(); ++i)
                reader.ReadBits(3); // sh_alf_aps_id_luma[i]
            if (sps.sps_chroma_format_idc != 0) {
                const bool cb = reader.ReadBit(); // sh_alf_cb_enabled_flag
                const bool cr = reader.ReadBit(); // sh_alf_cr_enabled_flag
                if (cb || cr)
                    reader.ReadBits(3); // sh_alf_aps_id_chroma
            }
            if (sps.sps_ccalf_enabled_flag) {
                const bool cc_cb = reader.ReadBit();
                if (cc_cb)
                    reader.ReadBits(3); // sh_alf_cc_cb_aps_id
                const bool cc_cr = reader.ReadBit();
                if (cc_cr)
                    reader.ReadBits(3); // sh_alf_cc_cr_aps_id
            }
        }
    }

    if (reader.HasError()) {
        out.stop_reason = "slice 载荷在 sh_qp_delta 之前截断";
        return out;
    }

    // ---- ref_pic_lists() ----
    // 条件与规范一致：!rpl_info_in_ph_flag && (nal != IDR || sps_idr_rpl_present_flag)
    // ——注意它**不看** slice_type，I slice 也照样要读。
    const bool rpl_in_slice = !pps.pps_rpl_info_in_ph_flag && (!IsIdr(nal_unit_type) || sps.sps_idr_rpl_present_flag);
    int num_entries[2] = {0, 0};
    if (rpl_in_slice) {
        const int poc_lsb_bits = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
        if (poc_lsb_bits < 4 || poc_lsb_bits > 24) {
            out.stop_reason = "sps_log2_max_pic_order_cnt_lsb_minus4 越界，无法算 rpls_poc_lsb_lt 的位宽";
            return out;
        }
        if (!SkipRefPicLists(reader, sps, pps, poc_lsb_bits, num_entries)) {
            out.stop_reason = "ref_pic_lists() 无法精确跳过（SPS 未记录 RPL 结构，或位宽无法确定）";
            return out;
        }
    } else if (out.slice_type != 2) {
        // RPL 在 PH 里，而 num_ref_idx_active 要靠它的 num_ref_entries 推。
        // I slice 不需要（num_ref_idx_active 恒 0），P/B 需要 —— 只能拒绝。
        out.stop_reason = "P/B slice 的参考列表在 picture header 里（pps_rpl_info_in_ph_flag=1，"
                          "或 IDR 未携带 RPL），本文件不解析 PH 的 ref_pic_lists()";
        return out;
    }

    // ---- 非 I slice 的 num_ref_idx / cabac_init / temporal MVP / 加权预测 ----
    if (out.slice_type != 2) {
        if (sps.sps_temporal_mvp_enabled_flag) {
            // sh_collocated_from_l0_flag / sh_collocated_ref_idx 是否存在，取决于 PH 的
            // ph_temporal_mvp_enabled_flag —— 而那一位在 PH 的 ref_pic_lists() 之后。
            out.stop_reason = "SPS 启用了 temporal MVP：sh_collocated_* 段取决于 PH 的 "
                              "ph_temporal_mvp_enabled_flag（需解析 PH 的 RPL 之后部分），未实现";
            return out;
        }

        bool override_flag = true;
        if (num_entries[0] > 1 || (out.slice_type == 1 && num_entries[1] > 1)) {
            override_flag = reader.ReadBit(); // sh_num_ref_idx_active_override_flag
        }
        int num_ref_idx_active[2] = {0, 0};
        const int list_count = (out.slice_type == 1) ? 2 : 1; // B 用两条，P 只用 list 0
        for (int i = 0; i < list_count; ++i) {
            int minus1 = 0;
            if (override_flag) {
                if (num_entries[i] > 1)
                    minus1 = static_cast<int>(reader.ReadUEBounded(14)); // sh_num_ref_idx_active_minus1[i]
            }
            if (override_flag) {
                num_ref_idx_active[i] = minus1 + 1;
            } else {
                const int dflt = pps.pps_num_ref_idx_default_active_minus1[i] + 1;
                num_ref_idx_active[i] = num_entries[i] < dflt ? num_entries[i] : dflt;
            }
        }
        if (reader.HasError()) {
            out.stop_reason = "sh_num_ref_idx_active 段截断";
            return out;
        }

        if (pps.pps_cabac_init_present_flag)
            reader.ReadBit(); // sh_cabac_init_flag

        if (!pps.pps_wp_info_in_ph_flag && ((pps.pps_weighted_pred_flag && out.slice_type == 0) ||
                                            (pps.pps_weighted_bipred_flag && out.slice_type == 1))) {
            if (!SkipPredWeightTable(reader, sps, pps, num_ref_idx_active)) {
                out.stop_reason = "pred_weight_table() 跳过失败（位流截断或权重项数越界）";
                return out;
            }
        }
        if (reader.HasError()) {
            out.stop_reason = "slice 载荷在 sh_qp_delta 之前截断";
            return out;
        }
    }

    const int delta = reader.ReadSE();
    if (reader.HasError()) {
        out.stop_reason = "sh_qp_delta 读取失败（位流截断）";
        return out;
    }

    const int qp = 26 + pps.pps_pic_init_qp_minus26 + delta;
    // 取值范围是最后一道筛：错位不报错，只会给出"看着合理"的数。
    // 高位深时下界是负的（-6*(bit_depth-8)），上界恒 51（H.266 7.4.3.3）。
    const int qp_bd_offset = 6 * sps.sps_bitdepth_minus8;
    if (qp < -qp_bd_offset || qp > 51) {
        out.stop_reason = "sh_qp_delta 推出的 SliceQpY 越界（" + std::to_string(qp) + "）";
        return out;
    }

    out.has_qp = true;
    out.qp = qp;
    return out;
}

} // namespace videoeye
