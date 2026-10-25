#pragma once

// H.265 (HEVC) 帧级 POC 追踪器。
//
// 与 H264PocTracker 解决同一个问题：容器给解码顺序（sample 顺序 + DTS），
// 显示顺序只能从 POC 推。这里只写**与 H.264 不同的那三处**：
//   1. NAL 切分语法（2 字节 header，type = (byte0 >> 1) & 0x3F）
//   2. slice_segment_header() 的布局与字段顺序
//   3. POC 的推导公式
// 扫包骨架（SplitNals / Feed / FeedExtradata / Finish）与统计汇总
// （FrameOrderStats）两边一致，统计规则只在 FrameOrderStats.cpp 维护一份。
//
// ==========================================================================
// HEVC 的 POC 比 H.264 简单，别被 H.264 的经验带偏：
//
//   * **没有 pic_order_cnt_type**。SPS 里压根没有这个字段（7.3.2.2.1），
//     POC 只有一套公式（8.3.1）：从 slice_pic_order_cnt_lsb 加回绕进位得到。
//     H.264 的 type 1 / type 2 分支在 HEVC 里不存在 —— 别照抄。
//   * **没有场编码**。HEVC 只有帧，所以既没有 delta_pic_order_cnt_bottom，
//     也没有 frame_num。FrameOrderEntry 里这两个字段对 HEVC 恒为空值。
//   * `slice_pic_order_cnt_lsb` 在两种 NAL 类型里**不存在**：IDR_W_RADL(19) 与
//     IDR_N_LP(20)。它们的 POC 由"复位后 lsb = 0"直接得 0，不要去位流里读
//     （读出来的是 slice_type 之后的字段，会把后面的解析全部错位一位以上）。
//   * slice_type 的取值顺序和 H.264 不一样：HEVC 是 0=B / 1=P / 2=I，
//     而本项目内部统一用 0=P / 1=B / 2=I，必须换一次。不换的后果是
//     I/P/B 计数全错，而且"B 帧数 = P 帧数"这种结果看起来还挺合理。
//
// 复位点（8.3.1 + 8.1.3）：只有"IRAP 且 NoRaslOutputFlag == 1"才把
// prevPicOrderCntMsb / prevPicOrderCntLsb 归零。NoRaslOutputFlag 为 1 的情形：
//   * IDR（19/20）、BLA（16/17/18）—— 恒为 1
//   * CRA（21）—— 仅当它是"码流第一幅图"或 EOS 之后的第一幅
// 所以**不能**见到 IRAP 就复位：开放 GOP 里插在中间的 CRA 是不复位的。
//
// 明确不做：
//   * 不重建参考帧列表 / DPB（判"能不能播"用不上，POC 异常已经能报警）
//   * 不按 temporal sub-layer 区分 prevTid0Pic，逐幅图像累进（见下）
//   * 不据 POC 步长推断"丢了哪一帧"，只给缺帧的槽位数

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ❗ 必须 include media 层的 NalUnit（理由见 H264PocTracker.h 同位置注释：
// domain 层的 model::NalUnit 是同名字段但不同类型，include 错会得到一串
// "missing type specifier"）。
#include "core/media/codec/ExtradataTypes.h"

#include "core/analysis/codec/FrameOrderTypes.h"
#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {

class HevcPocTracker {
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
    // 返回这一包里有几幅新的图像（first_slice_segment_in_pic_flag == 1 的 slice 数）。
    int Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);

    // 用容器 extradata（hvcC / Annex B / 长度前缀）里的参数集做初始化。
    // 返回是否至少拿到一份 SPS。
    bool FeedExtradata(const uint8_t* data, size_t size);

    // 收尾并计算 reorder / GOP / 重复 / 缺口。可重复调用（幂等）。
    const model::FrameOrderTimeline& Finish();
    const model::FrameOrderTimeline& timeline() const {
        return timeline_;
    }
    int stream_index() const {
        return stream_index_;
    }

private:
    // 一幅图像在解码顺序上的位置。字段刻意只留"算 POC 与归类要用"的那些。
    struct PictureHead {
        int nal_unit_type = -1;
        int slice_type = -1; // 已归一化到 0=P 1=B 2=I
        bool idr = false;    // POC 归零的随机接入点（NoRaslOutputFlag == 1）
        bool irap = false;   // 随机接入点（16..23 全部，含 CRA）
        bool has_poc_lsb = false;
        int poc_lsb = 0;
        // 本幅图像引用的那份 SPS 给出的 MaxPicOrderCntLsb。
        // ❗ 必须逐幅带过来，不能用"最近一份 SPS"的全局值：一段码流里
        // log2_max_pic_order_cnt_lsb_minus4 可以随 SPS 变，用全局值会在
        // 参数集切换处算错回绕进位，症状是"换了分辨率之后 POC 突然倒退"。
        int64_t max_poc_lsb = 0;
    };

    // 算 POC 只需要 SPS 的两个字段，不必留着整份 HevcSpsInfo：
    // 后者带一堆 vector，按 id 存 16 份会让追踪器无谓地重。
    struct SpsPocState {
        bool ready = false;
        int log2_max_poc_lsb = 0; // 已经 +4（即直接是位宽）
        bool separate_colour_plane = false;
    };
    struct PpsPocState {
        bool ready = false;
        int sps_id = 0;
        int num_extra_slice_header_bits = 0;
        bool output_flag_present = false;
    };

    // PPS id 规范上限 63、SPS id 上限 15。按 id 建表而不是"只留最新一份"：
    // 一段码流里存在多份 PPS 是合法的（不同 num_extra_slice_header_bits），
    // 只留最新那份会让引用旧 PPS 的 slice 按错的位宽去读，静默错位。
    static constexpr int kMaxSpsId = 16;
    static constexpr int kMaxPpsId = 64;

    std::vector<NalUnit> SplitNals(const uint8_t* data, size_t size);
    bool DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const;
    void FeedNal(const NalUnit& nal, bool has_time, double time_seconds, int64_t packet_index, int& new_frames);
    bool HandleSps(const NalUnit& nal);
    bool HandlePps(const NalUnit& nal);
    // 读 slice_segment_header 里算 POC 需要的前缀。返回 false = 不是图像第一段 /
    // 参数集缺失 / 位流被截断 —— 三种情况都不该记一帧。
    bool ReadPicture(const NalUnit& nal, PictureHead& out);
    int64_t ComputePoc(const PictureHead& head);
    void PushFrame(const PictureHead& head, int64_t poc, bool has_time, double time_seconds, int64_t packet_index);
    void Recompute();
    // 只在第一条原因时写入，避免被后面的帧覆盖成最后一条
    void NoteReason(const std::string& reason);

    int stream_index_ = -1;
    Options options_;
    Framing framing_ = Framing::Auto;
    int prefix_bytes_ = 0;

    SpsPocState sps_[kMaxSpsId];
    PpsPocState pps_[kMaxPpsId];
    bool any_sps_ = false;

    // POC 状态机（8.3.1）。MaxPicOrderCntLsb 不在这里：它跟着 SPS 走（见 PictureHead）。
    int64_t prev_poc_msb_ = 0;
    int64_t prev_poc_lsb_ = 0;

    model::FrameOrderTimeline timeline_;
    bool finished_ = false;
};

} // namespace videoeye
