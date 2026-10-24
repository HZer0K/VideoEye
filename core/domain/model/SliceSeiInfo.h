#pragma once

// ==========================================================================
// slice header / SEI 的**自研**解析结果（C4）
//
// 以前这些全部依赖 FFmpeg 的 packet side data（AV_FRAME_DATA / AV_PKT_DATA_*）：
// HDR10+、Dolby Vision 的判定不过是把 side data 换个名字抄过来，一旦打包链路里
// side data 没下传（很多 remux 工具会丢），面板就"没元数据"了 —— 但实际码流里
// 那几条 SEI 好好地在。这里从**码流本身**再解一遍，两条路径互为交叉验证。
//
// 覆盖范围：
//   * SEI      —— mastering display (MDCV, type 137)、content light level
//                 (CLL, type 144)、pic timing (type 1)、HDR10+ (T.35 type 4)、
//                 Dolby Vision RPU (HEVC NAL 62 + T.35)、film grain (type 19)、
//                 recovery point (type 6)
//   * slice    —— H.264 的 slice_type 分布与**逐帧 QP**（slice_qp_delta + PPS
//                 的 pic_init_qp_minus26）。HEVC 侧见 VideoEsInspector.h 的说明。
//
// 为什么 analyzer（BitstreamAnalyzer）不做这件事：
//   参数集在 extradata 里、slice 与 SEI 只在**码流包**里。两者的输入来源不同，
//   生命周期也不同（前者读一次，后者要逐包喂），硬塞进一个类只会让两边都不干净。
// ==========================================================================

#include <cstdint>
// std::snprintf：Summary() 里做定点格式化用（domain 层不能有 Qt 的 QString）
#include <cstdio>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// --------------------------------------------------------------------------
// SEI 统计
// --------------------------------------------------------------------------
struct SeiSummary {
    bool analyzed = false;
    int total_messages = 0;

    // ---- HDR 静态元数据 ----
    //
    // 单位在入库时统一换算好了，别再乘倍数：
    //   H.264/HEVC 的 MDCV 原数据是 chromaticity * 0.00002、luminance * 0.0001；
    //   AV1 的 HDR MDCV metadata OBU 是 chromaticity * 0.00016、luminance 直接就是值。
    // 两边换算到同一套单位后，UI 与报告才不用来回除以不同的系数。
    bool has_mastering_display = false;
    double display_primaries_x[3] = {0.0, 0.0, 0.0}; // 归一化到 [0,1]，G/B/R 顺序
    double display_primaries_y[3] = {0.0, 0.0, 0.0};
    double white_point_x = 0.0;
    double white_point_y = 0.0;
    double max_display_mastering_luminance = 0.0; // cd/m2
    double min_display_mastering_luminance = 0.0; // cd/m2

    bool has_content_light_level = false;
    uint32_t max_content_light_level = 0;     // MaxCLL，cd/m2
    uint32_t max_pic_average_light_level = 0; // MaxFALL，cd/m2

    // ---- HDR 动态元数据 ----
    // HDR10+ 走 SEI type 4（ITT T.35）+ ST 2094-40 的应用标识
    bool has_hdr10plus = false;
    int hdr10plus_count = 0;
    // Dolby Vision：HEVC 里是 NAL type 62（unspecified）里的 RPU，
    // 带内不一定每次都出现，所以用"出现过的次数"而不是 bool。
    bool has_dolby_vision_rpu = false;
    int dolby_vision_rpu_count = 0;

    // ---- picture timing（type 1）----
    bool has_pic_timing = false;
    int pic_timing_count = 0;
    // pic_struct（H.264 表 D.1 / HEVC 同样取值）：0=帧 1=顶场 2=底场 3 top+bottom ...
    int pic_struct = 0;

    // ---- film grain（type 19）----
    // ⚠️ 注意：AV1 的 film grain 在 frame header 的 film_grain_params() 里，
    // 与这里的 SEI type 19 是两套东西；本模块只认 H.264/HEVC 的 SEI 那条。
    bool has_film_grain = false;
    int film_grain_count = 0;
    bool film_grain_cancel_flag = false;

    bool has_recovery_point = false;

    // 见到过但本模块还没展开的 payloadType（用于日志与"还差什么"的盘点）
    std::vector<int> unhandled_payload_types;

    std::string Summary() const;
};

// --------------------------------------------------------------------------
// slice 统计（逐帧 QP / slice type）
// --------------------------------------------------------------------------
struct SliceSummary {
    bool analyzed = false;
    bool supported = false; // false = 该编码目前没有在本模块实现 slice 解析
    std::string unsupported_reason;

    int slice_count = 0; // 解出的独立 slice 段数（不含 dependent slice）
    int frames = 0;      // first_mb_in_slice == 0 的 slice 数（≈ 帧数）
    int type_i = 0;
    int type_p = 0;
    int type_b = 0;
    int type_other = 0; // SP / SI

    int qp_min = 0;
    int qp_max = 0;
    double qp_avg = 0.0;
    // 成功解出 QP 的 slice 数（qp_min/max/avg 的分母）。
    // ❗ slice_count 可能大于它：slice header 解到一半失败时，类型分布算不出来、
    // QP 更算不出来。把两者混为一个数会让"QP 均值"看着像覆盖了全部 slice。
    int qp_parsed_slices = 0;
    // QP 是不是真的算了：需要 PPS 给出 pic_init_qp_minus26 作为基准，且至少有一个
    // slice 的 QP 通过取值范围校验。拿不到 PPS 时只能把 slice_qp_delta 相加，
    // 那不是真的 QP —— 一律不算。
    bool qp_valid = false;

    // QP 曲线（已抽稀到 kMaxCurvePoints 个点以内，供 UI 画折线）
    std::vector<int> qp_curve;

    std::string Summary() const;
};

// --------------------------------------------------------------------------
// 汇总：挂在 BitstreamAnalysisResult 上
// --------------------------------------------------------------------------
struct SliceSeiSummary {
    bool analyzed = false;
    std::string codec_name;

    SliceSummary slices;
    SeiSummary sei;

    // 扫描窗口（给"为什么曲线只有 60 个点"一个解释）
    int packets_scanned = 0;
    bool truncated = false; // 达到上限提前收工

    std::string Summary() const;
};

} // namespace model
} // namespace videoeye
