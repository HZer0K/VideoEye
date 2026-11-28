#include "core/analysis/codec/VvcSliceHeader.h"

#include "core/media/codec/BitReader.h"

namespace videoeye {

namespace {

// H.266 7.4.1 Table 5：VVC 的 IRAP 区间是 7..11（HEVC 是 16..23）。
constexpr int kNalIdrWRadl = 7;
constexpr int kNalIdrNLp = 8;
constexpr int kNalCra = 9;
constexpr int kNalGdr = 10;

bool IsIdr(int nal_unit_type) {
    return nal_unit_type == kNalIdrWRadl || nal_unit_type == kNalIdrNLp;
}

// sh_no_output_of_prior_pics_flag 只在这四种 NAL 类型上存在（H.266 7.3.2.9）。
bool HasNoOutputOfPriorPicsFlag(int nal_unit_type) {
    return nal_unit_type == kNalIdrWRadl || nal_unit_type == kNalIdrNLp || nal_unit_type == kNalCra ||
           nal_unit_type == kNalGdr;
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

    // ---- 到 sh_qp_delta 的通道必须是"宽度可证固定" ----
    if (out.slice_type != 2) {
        out.stop_reason = "非 I slice：sh_qp_delta 之前有 ref_pic_lists() 相关可变段与 pred_weight_table()";
        return out;
    }
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
    if (!pps.pps_rpl_info_in_ph_flag && !(IsIdr(nal_unit_type) && !sps.sps_idr_rpl_present_flag)) {
        // 条件与规范一致：!rpl_info_in_ph_flag && (nal != IDR || sps_idr_rpl_present_flag)
        // ——注意它**不看** slice_type，I slice 也照样要读 ref_pic_lists()。
        out.stop_reason = "ref_pic_lists() 出现在 slice 里（可变长，需按 RPL 条数与 POC 位宽逐项跳过）";
        return out;
    }

    // ---- 从 sh_slice_type 到 sh_qp_delta 之间剩下的两段，都是定长 ----
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
