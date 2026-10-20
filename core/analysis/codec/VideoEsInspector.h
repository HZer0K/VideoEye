#pragma once

// ==========================================================================
// 带内基本码流（Elementary Stream）巡检器：slice header + SEI —— C4
//
// 输入是一段**视频包头数据**（FFmpeg 的一个 AVPacket.data），不是 extradata。
// 逐包喂进来，累积 slice QP / slice type / SEI 元数据，到 Finish() 汇总。
//
// 为什么不放在 BitstreamAnalyzer 里：
//   BitstreamAnalyzer 的输入是 extradata（参数集），读一次就够；slice/SEI 只在
//   码流包里，必须逐包推进、还要有扫描上限。两者的输入、生命周期、失败语义都
//   不一样，塞一起只会互相污染（最典型的坑：把"extradata 里没这东西"当成
//   "这条流没有 SEI"）。
//
// ⚠️ 覆盖范围要说清楚：
//   * SEI     —— H.264 / HEVC 都能解（同一个 wasm payloadType 表），主要盯
//                MDCV(137) / CLL(144) / HDR10+(T.35 4) / DV RPU(HEVC NAL 62)
//                / pic timing(1) / film grain(19)
//   * slice   —— **目前只有 H.264**。HEVC 的 slice_qp_delta 前面压着一整套
//                st_ref_pic_set() 与 NumPocTotalCurr 的推导，只做一半就会得到
//                错位的 QP —— 那种数比没有更糟，所以宁可不产出（supported=false
//                + unsupported_reason 写明原因），也不要给一个看着合理的假值。
// ==========================================================================

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/domain/model/BitstreamInfo.h"
#include "core/domain/model/SliceSeiInfo.h"
#include "core/media/codec/ExtradataTypes.h"

namespace videoeye {

class VideoEsInspector {
public:
    struct Options {
        // 最多看多少个包。取前两三百个就够拿到稳定的 QP 分布与 SEI 密度，
        // 全文件扫一遍是纯粹的浪费（QP 曲线的细节在进入完整 GOP 后就重复了）。
        int max_packets = 300;
        // QP 曲线抽稀后的点数上限
        int max_curve_points = 600;
    };

    void Reset(int codec_id);
    void Reset(int codec_id, const Options& options);

    // 参数集已解出的情况下配置它们：slice QP 需要 PPS 的 init_qp 作基准，
    // slice header 的位偏移需要 SPS 的几个开关。拿不到也能跑，只是 QP 判为无效。
    void SetH264ParameterSets(const model::H264SpsInfo& sps, const model::H264PpsInfo& pps);
    void SetHevcParameterSets(const model::HevcSpsInfo& sps, const model::HevcPpsInfo& pps);

    // 这个编码有没有可做的巡检（slice 或 SEI 至少占一样）
    bool enabled() const { return enabled_; }
    bool done() const { return done_; }

    // 喂一个视频包。返回 false 表示已经不作为（未启用 / 收工 / 见过够了）。
    bool Feed(const uint8_t* data, size_t size);

    // 收尾：算出 QP 的均值/最值与抽稀曲线
    void Finish();

    const model::SliceSeiSummary& result() const { return result_; }
    model::SliceSeiSummary& mutable_result() { return result_; }

private:
    void InspectH264(const uint8_t* data, size_t size);
    void InspectHevc(const uint8_t* data, size_t size);
    void InspectAv1(const uint8_t* data, size_t size);

    // H.264 / HEVC 的 SEI payload 结构完全相同：都会进这里
    void ParseSeiRbsp(const uint8_t* data, size_t size, bool is_hevc);
    void HandleSeiPayload(int payload_type, const uint8_t* p, size_t n);

    // H.264 slice_header()：走到 slice_qp_delta 为止
    bool ParseH264SliceHeader(const uint8_t* data, size_t size, bool is_idr,
                              int& slice_type, int& slice_qp);

    int codec_id_ = 0;
    Options options_;
    bool enabled_ = false;
    bool done_ = false;
    int packets_ = 0;

    model::H264SpsInfo h264_sps_;
    model::H264PpsInfo h264_pps_;
    model::HevcSpsInfo hevc_sps_;
    model::HevcPpsInfo hevc_pps_;
    bool has_h264_params_ = false;

    // 累积态（Finish() 时汇总进 result_）
    std::vector<int> slice_types_;   // 0=P 1=B 2=I ...（H.264 的 slice_type 值）
    std::vector<int> slice_qps_;

    model::SliceSeiSummary result_;
};

}  // namespace videoeye
