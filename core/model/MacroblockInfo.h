#pragma once

#include <QMetaType>
#include <cstdint>
#include <vector>

namespace videoeye {
namespace model {

// 单个运动矢量 / 宏块信息
struct MotionVectorInfo {
    int block_x = 0;                   // 宏块目标位置 X (像素)
    int block_y = 0;                   // 宏块目标位置 Y (像素)
    uint8_t block_w = 0;               // 块宽度 (像素)
    uint8_t block_h = 0;               // 块高度 (像素)
    int32_t motion_x = 0;              // 运动矢量 X 分量 (1/motion_scale 像素精度)
    int32_t motion_y = 0;              // 运动矢量 Y 分量 (1/motion_scale 像素精度)
    uint16_t motion_scale = 0;         // 运动矢量精度分母 (通常 4 = 1/4 像素)
    int32_t source = 0;                // 参考帧方向 (< 0 = 前向/过去, > 0 = 后向/未来)
    double motion_magnitude = 0.0;     // 运动矢量幅度 (像素)
    double motion_angle = 0.0;         // 运动矢量角度 (度, 0-360)
};

// 一帧的宏块统计概览
struct MacroblockStats {
    int total_blocks = 0;              // 总宏块/块数
    int forward_count = 0;             // 前向参考数 (source < 0)
    int backward_count = 0;            // 后向参考数 (source > 0)
    int intra_count = 0;               // 帧内块数 (无运动矢量, I 帧宏块)
    double avg_motion_magnitude = 0.0; // 平均运动矢量幅度 (像素)
    double max_motion_magnitude = 0.0; // 最大运动矢量幅度 (像素)

    // 块大小分布统计
    int count_16x16 = 0;
    int count_16x8 = 0;
    int count_8x16 = 0;
    int count_8x8 = 0;
    int count_8x4 = 0;
    int count_4x8 = 0;
    int count_4x4 = 0;
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
    int frame_type = 0;               // AVPictureType (1=I, 2=P, 3=B)
    bool has_motion_vectors = false;   // 是否包含运动矢量数据
    MacroblockStats stats;
    std::vector<MotionVectorInfo> motion_vectors;  // 运动矢量列表
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::MacroblockFrameAnalysis)
