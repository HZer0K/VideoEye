#pragma once

// ==========================================================================
// VVC (H.266) 帧级 slice_header 解析 —— 只产出两样东西：
//   1. sh_slice_type（逐 slice 的类型分布，以及"这一 slice 是不是一幅新图像的开头"）
//   2. sh_qp_delta → SliceQpY（逐帧 QP 曲线，仅当整条位宽路径可证）
//
// 为什么不复用 VvcPocTracker：
//   追踪器的产物是 POC，它明确把 slice_type 当"尽力而为"，地址宽度不可证就记未知；
//   巡检器要的正是类型与 QP。两者共享的只有"PH 前缀怎么读"，那部分放这里，
//   追踪器与巡检器都调它。**不共享**的是各自的失败策略：追踪器可以为了 POC 继续，
//   巡检器宁可整条 slice 不计入，也不给一个错位的类型。
//
// 语法以 FFmpeg libavcodec/cbs_h266_syntax_template.c 的 slice_header() 为准
// （与 H.266 7.3.2.9 逐条对应），顺序不要凭印象改。
//
// ==========================================================================
// 明确不支持、并且**报出原因**而不是猜的边界：
//
//   * 内联 PH：sh_picture_header_in_slice_header_flag == 1 时整份
//     picture_header_structure() 内联在 slice 开头，要跳到 slice 字段必须解完 PH
//     的全部语法（ALF/虚拟边界/RPL/加权预测…）。不支持。
//   * 显式 slice 布局（pps_num_slices_in_pic_minus1 > 0）：PPS 解析在这一档就
//     返回 false，NumSlicesInSubpic 不可知 → sh_slice_address 宽度不可知。
//   * 有 subpic 且不是"每 subpic 一片"：CurrSubpicIdx 要查 PPS 的 subpic id 映射
//     表（未实现）→ 地址宽度不可知。
//   * QP 需要一个"从头到尾宽度固定"的通道，以下任何一条成立就只给类型不给 QP：
//       - 非 I slice：sh_slice_type 之后有 ref_pic_lists() 相关的
//         sh_num_ref_idx_active_override_flag 与 pred_weight_table()（可变长，且
//         取决于 RPL 条数）
//       - pps_qp_delta_info_in_ph_flag == 1：delta 在 PH 里，要解完 PH 才拿得到
//       - sps_lmcs_enabled_flag / sps_explicit_scaling_list_enabled_flag：slice 里
//         各多一位，而该位是否存在取决于 PH 的对应标志
//       - ref_pic_lists() 出现在 slice 里（既不是 in-PH，也不是"IDR 且 SPS 不携带
//         RPL"）
//     这四条之外的两段照解（都是定长）：IRAP 的 sh_no_output_of_prior_pics_flag、
//     sh_alf_enabled_flag 及其 APS id 组。
// ==========================================================================

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/domain/model/VvcBitstreamInfo.h"

namespace videoeye {

class BitReader;

// picture_header 里"解析 slice_header 必须知道"的最小字段集。
//
// 只有 PH 的前几个语法单元 —— 但它们决定了 sh_slice_type 是否存在、以及这一
// slice 该用哪套 PPS：
//   ph_gdr_or_irap_pic_flag u(1)
//   ph_non_ref_pic_flag     u(1)
//   [ ph_gdr_pic_flag       u(1) ]         gdr_or_irap == 1 时
//   ph_inter_slice_allowed_flag u(1)
//   [ ph_intra_slice_allowed_flag u(1) ]   inter_slice_allowed == 1 时
//   ph_pic_parameter_set_id ue(v)
struct VvcPhPrefix {
    bool valid = false;
    int pps_id = -1;
    bool inter_slice_allowed = true;
    bool intra_slice_allowed = true;
};

struct VvcSliceHeaderInfo {
    // 至少成功读到了 sh_slice_type（或按规范推导出恒 I）。
    bool parsed = false;

    // 归一化到项目内部约定：0=P 1=B 2=I（VVC 码流里是 0=B 1=P 2=I）。
    int slice_type = -1;

    // sh_slice_address == 0（无 subpic 时）⇒ 这一 slice 开启一幅新图像。
    bool first_slice_of_picture = false;

    bool has_qp = false;
    int qp = 0;

    // 非空 = 为什么没给出 QP。**只在 parsed 时才有意义**（解析都没成功时
    // 它写的是解析失败的原因）。
    std::string stop_reason;
};

class VvcSliceHeaderParser {
public:
    // 从 reader 当前位置读 PH 前缀。返回 false = 位流截断或 pps_id 越界。
    static bool ReadPhPrefix(BitReader& reader, VvcPhPrefix& out);

    // rbsp: 已去 emulation prevention 的 slice NAL 载荷（不含 2 字节 NAL header）。
    static VvcSliceHeaderInfo Parse(const uint8_t* rbsp, std::size_t size, int nal_unit_type, const VvcPhPrefix& ph,
                                    const model::VvcSpsInfo& sps, const model::VvcPpsInfo& pps);
};

} // namespace videoeye
