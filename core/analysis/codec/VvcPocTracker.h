#pragma once

// H.266 (VVC) 帧级 POC 追踪器。
//
// 与 H264PocTracker / HevcPocTracker 解决同一个问题：容器只给解码顺序，
// 显示顺序只能从码流里的 POC 推。但 VVC 的"POC 写在哪"跟前两者都不一样，
// 照抄 HEVC 的 slice_header 解析会一条都读不对：
//
//   1. **POC 不在 slice_header 里，而在 picture_header（PH，NAL type 19）里**。
//      PH 是独立 NAL，先于该图像的所有 slice 出现；若
//      sh_picture_header_in_slice_header_flag == 1，则 PH 直接内联在 slice
//      header 开头（此时码流里没有 PH NAL）。两种情况都要支持。
//   2. PH 里的 ph_pic_order_cnt_lsb **无条件存在**（IDR 也有，规范要求写 0）。
//      HEVC 的"IDR 没有 pic_order_cnt_lsb"这条经验在这里是错的。
//   3. ph_pic_parameter_set_id 是 **ue(v)**（不是定长 u(6)），且第 1、2 位不是
//      slice 相关字段：PH 开头是 ph_gdr_or_irap_pic_flag / ph_non_ref_pic_flag，
//      **两位都无条件读**，之后才是 gdr / inter / intra 三个条件位。
//   4. slice_type 之前有一串变长段（subpic id、slice address、extra_sh_bit、
//      num_tiles_in_slice）。地址宽度依赖 PPS 的 tile/slice 划分，不是常量 ——
//      这一段读错一位，slice_type 就会读成别的值，I/P/B 计数全错却不报错。
//
// ==========================================================================
// 本实现的分层取舍（每一条都对应一个"不假装支持"的边界）：
//
//   * **POC 是主产物，slice_type 是尽力而为**。POC 只依赖 PH + SPS/PPS 的
//     两三个字段，能稳定推出；slice_type 依赖 address 字段宽度。
//     只有"宽度可证为 0"的流才去读它：
//       - ph_inter_slice_allowed_flag == 0 → 规范规定 sh_slice_type 不存在，恒 I
//       - 无 subpic 且无 extra_sh_bit，且 PPS 是"不划分图像"或
//         "每 subpic 单 slice"（这两种情况下 sh_slice_address 宽度为 0）
//     其余流（多 tile / 多 slice / 显式 slice 布局）**不猜**：POC 照常给，
//     slice_type 记未知（报告里显示 '?'、计入 other_count），而不是从错位的位里
//     读一个看起来合理的数字。
//
//   * 一幅图像只记一帧。PH 打开一幅图像，该图像的第一个 slice 落帧；
//     同一 PH 之后的 slice 不重复落（多 slice 图像不会被算成多帧）。
//     内联 PH 的 slice 属于"一个 slice 一幅图像"的形态，直接落帧；若该流同时
//     有可读的 address 且非 0，说明是同一幅图像的后续 slice，跳过。
//
//   * 不重建参考帧列表（RPL / DPB），不按 temporal sub-layer 区分 prevTid0Pic
//     （与 HEVC 同口径，逐幅图像累进）。
//
//   * POC MSB cycle（sps_poc_msb_cycle_flag）**不支持**，而且不是静默降级：
//     遇到启用了该开关的 SPS 直接拒绝整条时间线并写明原因。理由与 H.264 的
//     pic_order_cnt_type==1 相同 —— 没有真实样本验证过的分支，宁可报"不可用"，
//     也不要用一条偏了一个周期的 POC 线去支撑"无重排"的结论。
//
//   * 复位点（H.266 8.3.1）：IRAP 且 NoRaslOutputFlag == 1 才归零 prevPoc。
//     IDR_W_RADL(7) / IDR_N_LP(8) 恒为 1；CRA(9) / GDR(10) 仅在"码流第一幅图
//     或 EOS 之后第一幅"为 1。见到 IRAP 就复位会让开放 GOP 的 middle CRA
//     整体偏一个回绕周期。
//
//   * 前导图像（RADL=2 / RASL=3）的 POC 天生比所属 IRAP 小，那不是 B 帧重排。
//     entry.leading 由本文件置位，FrameOrderStats 按它排除 —— **不能**沿用
//     "nal_unit_type ∈ [6,9]" 那条 HEVC 判据：VVC 的 7/8/9 是 IDR/CRA，
//     照抄会把每个随机接入点之后的重排统计整段吞掉。

#include <cstddef>
#include <cstdint>
#include <string>

// ❗ 必须 include media 层的 NalView（理由同 HevcPocTracker.h：domain 层有
// 同名字段但不同类型的 NalUnit，include 错会得到一串 "missing type specifier"）。
#include "core/media/codec/ExtradataTypes.h"

#include "core/analysis/codec/FrameOrderTypes.h"
#include "core/domain/model/FrameOrderInfo.h"
// 只为拿 kMaxVvcExtraBitFlags：SpsState 里那两组开关必须与 SPS 解析结果同尺寸，
// 各写一个 8/24 迟早会再错一次。
#include "core/domain/model/VvcBitstreamInfo.h"

namespace videoeye {

class BitReader;

class VvcPocTracker {
public:
    using Framing = NalFraming;
    using Options = FrameOrderOptions;

    void Reset(int stream_index, const Options& options = Options{});
    void SetFraming(Framing framing) {
        framing_ = framing;
    }
    void SetLengthPrefixBytes(int bytes) {
        prefix_bytes_ = bytes;
    }

    // 喂一个媒体包的负载（Annex B 或长度前缀均可，Auto 时逐包判定）。
    // 返回这一包里有几幅新的图像（PH 打开且首个 slice 落帧的次数）。
    int Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);

    // 用容器 extradata（vvcC / Annex B / 长度前缀）里的 SPS、PPS 做初始化。
    // 返回是否至少拿到一份 SPS。
    bool FeedExtradata(const uint8_t* data, size_t size);

    const model::FrameOrderTimeline& Finish();
    const model::FrameOrderTimeline& timeline() const {
        return timeline_;
    }
    int stream_index() const {
        return stream_index_;
    }

private:
    // 一份 PH 里算 POC / 归类要用到的字段。PH 本身还有几十个字段（ALF/LMCS/
    // 缩放列表/参考帧列表…），与 POC 无关，一律不解析。
    struct PhState {
        bool valid = false;
        // 本幅图像是否已经落过帧（后续 slice 不再记）
        bool consumed = false;

        int nal_unit_type = -1;
        int pps_id = -1;
        int poc_lsb = 0;
        int64_t max_poc_lsb = 0;
        bool gdr_or_irap = false;
        bool gdr = false;
        bool non_ref = false;
        bool inter_slice_allowed = true;
        bool intra_slice_allowed = true;
    };

    struct SpsState {
        bool ready = false;
        int log2_max_poc_lsb = 0; // 已 +4（即位宽）
        // 该 SPS 启用了 POC MSB cycle：本实现拒绝为它算 POC（未验证，别猜）
        bool poc_msb_cycle_flag = false;
        bool subpic_info_present = false;
        int subpic_id_len = 1;
        int num_extra_ph_bytes = 0;
        // 与 model::VvcSpsInfo 的 sps_extra_*_bit_present_flag 同尺寸 —— 数量是 u(2)
        // （≤3 字节）→ 最多 24 个开关。那边按 [8] 声明时 num = 3 直接把
        // "index 8 out of bounds" 写进了 UBSan 报告，这里跟着同步。
        bool extra_ph_present[model::kMaxVvcExtraBitFlags] = {};
        int num_extra_sh_bytes = 0;
        bool extra_sh_present[model::kMaxVvcExtraBitFlags] = {};
    };

    struct PpsState {
        bool ready = false;
        int sps_id = -1;
        bool no_pic_partition = false;
        bool rect_slice = false;
        bool single_slice_per_subpic = false;
    };

    // PPS id 上限 63、SPS id 上限 15（H.266 7.4.3.2 / 7.4.3.3）。按 id 建表而不是
    // "只留最新一份"：一段码流里多份 PPS 是合法的，留错那份会让 PH 按错的
    // MaxPicOrderCntLsb 去读 lsb —— 结果是 POC 整体错位而解析"成功"。
    static constexpr int kMaxSpsId = 16;
    static constexpr int kMaxPpsId = 64;

    bool DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const;
    void FeedNal(const NalView& nal, bool has_time, double time_seconds, int64_t packet_index, int& new_frames);
    bool HandleSps(const NalView& nal);
    bool HandlePps(const NalView& nal);

    // 从 reader 当前位置解析一份 picture_header_structure()（H.266 7.3.2.8），
    // 只读到 ph_pic_order_cnt_lsb（及它前面的 GDR 恢复点）为止。返回 false =
    // 引用的 PPS/SPS 缺失、SPS 启用了未验证的 MSB cycle、字段越界或位流截断 ——
    // 这些情况都不该记一帧。
    bool ReadPictureHeader(BitReader& reader, int nal_unit_type, PhState& out);
    // 读 slice_header 的 POC/类型前缀（7.3.2.9）。返回 false = 该 slice 不该落帧。
    bool ReadSlice(const NalView& nal, bool has_time, double time_seconds, int64_t packet_index);
    // 8.1.3：IRAP 且 NoRaslOutputFlag == 1 才复位 POC。判定与使用必须同源 ——
    // 算 POC 和记 idr 各判一次，是"复位了但没标 idr"这类错误的温床。
    bool DecideNoRasl(const PhState& ph) const;
    int64_t ComputePoc(const PhState& ph, bool no_rasl);
    void PushFrame(const PhState& ph, int slice_type, int64_t poc, bool no_rasl, bool has_time, double time_seconds,
                   int64_t packet_index);
    void Recompute();
    // 只在第一条原因时写入，避免被后面的帧覆盖成最后一条
    void NoteReason(const std::string& reason);

    int stream_index_ = -1;
    Options options_;
    Framing framing_ = Framing::Auto;
    int prefix_bytes_ = 0;

    SpsState sps_[kMaxSpsId];
    PpsState pps_[kMaxPpsId];
    bool any_sps_ = false;

    PhState pending_;
    // EOS(21) 之后的第一幅 IRAP 才置 NoRaslOutputFlag = 1（8.1.3）。
    bool eos_seen_ = false;

    // POC 状态机（8.3.1）。MaxPicOrderCntLsb 不在这里：它跟着 SPS 走（见 PhState）。
    int64_t prev_poc_msb_ = 0;
    int64_t prev_poc_lsb_ = 0;

    model::FrameOrderTimeline timeline_;
    bool finished_ = false;
};

} // namespace videoeye
