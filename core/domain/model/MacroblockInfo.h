#pragma once

#include <cstdint>
#include <vector>

namespace videoeye {
namespace model {

// 单个运动矢量 / 宏块信息
//
// 坐标系约定（来自 FFmpeg AVMotionVector）: block_x / block_y 是块中心的像素坐标，
// 不是左上角。绘制箭头时以它为起点，绘制块热力图时要减去半个块宽/高。
struct MotionVectorInfo {
    int block_x = 0;               // 块中心 X (像素)
    int block_y = 0;               // 块中心 Y (像素)
    uint8_t block_w = 0;           // 块宽度 (像素)
    uint8_t block_h = 0;           // 块高度 (像素)
    int32_t motion_x = 0;          // 运动矢量 X 分量 (1/motion_scale 像素精度)
    int32_t motion_y = 0;          // 运动矢量 Y 分量 (1/motion_scale 像素精度)
    uint16_t motion_scale = 0;     // 运动矢量精度分母 (通常 4 = 1/4 像素)
    int32_t source = 0;            // 参考帧方向 (< 0 = 前向/过去, > 0 = 后向/未来)
    double motion_magnitude = 0.0; // 运动矢量幅度 (像素)
    double motion_angle = 0.0;     // 运动矢量角度 (度, 0-360)
};

// 一帧的宏块统计概览
struct MacroblockStats {
    bool block_counts_estimated = false;
    bool intra_count_available = false;
    int total_blocks = 0;              // 网格估算的总宏块/CTU 数
    int distinct_block_count = 0;      // 去重后的运动矢量块数 (B 帧前/后向记录合并为一块)
    int forward_count = 0;             // 前向参考记录数 (source < 0)
    int backward_count = 0;            // 后向参考记录数 (source > 0)
    int intra_count = 0;               // 帧内块数 (I 帧整帧估算 / P·B 帧为网格减 inter 覆盖)
    double avg_motion_magnitude = 0.0; // 平均运动矢量幅度 (像素, 按去重块统计)
    double max_motion_magnitude = 0.0; // 最大运动矢量幅度 (像素, 按去重块统计)

    // 块大小分布统计 (H.264 宏块尺寸)
    int count_16x16 = 0;
    int count_16x8 = 0;
    int count_8x16 = 0;
    int count_8x8 = 0;
    int count_8x4 = 0;
    int count_4x8 = 0;
    int count_4x4 = 0;
    // HEVC CTU/CU 大尺寸分区 (H.264 不产生, 计数恒为 0)
    int count_64x64 = 0;
    int count_32x32 = 0;
    int count_32x16 = 0;
    int count_16x32 = 0;
    int count_32x8 = 0;
    int count_8x32 = 0;
    int count_other = 0;

    // 运动矢量幅度分布 (0-2, 2-4, 4-8, 8-16, 16+ 像素)
    int mag_0_2 = 0;
    int mag_2_4 = 0;
    int mag_4_8 = 0;
    int mag_8_16 = 0;
    int mag_16_plus = 0;
};

// 一帧的完整宏块分析结果
struct MacroblockFrameAnalysis {
    int frame_index = 0;
    int64_t pts = 0;
    double timestamp = 0.0;
    int frame_width = 0;
    int frame_height = 0;
    int frame_type = 0;              // AVPictureType (1=I, 2=P, 3=B)
    int codec_id = 0;                // AV_CODEC_ID_* (H264/HEVC/AV1/VP9...), 决定术语: 宏块 vs CTU
    bool has_motion_vectors = false; // 是否包含运动矢量数据
    MacroblockStats stats;
    std::vector<MotionVectorInfo> motion_vectors; // 运动矢量列表
};

} // namespace model

// 编码类型辅助判断 (数值与 FFmpeg AV_CODEC_ID_* 对应, 避免 UI 层引入 FFmpeg 头依赖)
// AV_CODEC_ID_HEVC 在 FFmpeg 中为稳定值 173 (自 2.0 起未变)
namespace CodecType {
constexpr int kMpeg1Video = 1;
constexpr int kMpeg2Video = 2;
constexpr int kH263 = 4;
constexpr int kMpeg4 = 12;
constexpr int kMsmpeg4v1 = 14;
constexpr int kMsmpeg4v2 = 15;
constexpr int kMsmpeg4v3 = 16;
constexpr int kH263p = 19;
constexpr int kFlv1 = 21;
constexpr int kH264 = 27;
constexpr int kVp8 = 139;
constexpr int kVp9 = 167;
constexpr int kHevc = 173;
constexpr int kAv1 = 225;

inline bool IsHevc(int codec_id) {
    return codec_id == kHevc;
}

// FFmpeg 的 export_mvs 只在部分解码器实现:
//   - mpegvideo 系 (MPEG-1/2/4、H.263、FLV1、MSMPEG4) 与 H.264 会写
//     AV_FRAME_DATA_MOTION_VECTORS side data;
//   - HEVC / VP9 / AV1 解码器不写该 side data（实测内置 FFmpeg 8.x: P/B 帧同样 0 条）。
// UI 用这个函数区分「本编码拿不到运动矢量」和「该帧暂时没有运动矢量」，
// 避免把解码器能力不足显示成分析失败。
inline bool SupportsMotionVectorExport(int codec_id) {
    switch (codec_id) {
    case kMpeg1Video:
    case kMpeg2Video:
    case kH263:
    case kH263p:
    case kMpeg4:
    case kMsmpeg4v1:
    case kMsmpeg4v2:
    case kMsmpeg4v3:
    case kFlv1:
    case kH264:
        return true;
    default:
        return false;
    }
}

// 常见编码的短名, 仅用于界面提示; 未收录的编码返回空串。
inline const char* DisplayName(int codec_id) {
    switch (codec_id) {
    case kMpeg1Video:
        return "MPEG-1";
    case kMpeg2Video:
        return "MPEG-2";
    case kH263:
    case kH263p:
        return "H.263";
    case kMpeg4:
        return "MPEG-4";
    case kFlv1:
        return "FLV1";
    case kH264:
        return "H.264";
    case kVp8:
        return "VP8";
    case kVp9:
        return "VP9";
    case kHevc:
        return "HEVC";
    case kAv1:
        return "AV1";
    default:
        return "";
    }
}
} // namespace CodecType

} // namespace videoeye
