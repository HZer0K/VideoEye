#include "MacroblockAnalyzer.h"
#include "utils/Logger.h"

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif

extern "C" {
#include <libavutil/motion_vector.h>
#include <libavutil/frame.h>
#include <libavcodec/codec_id.h>
}

#include <cmath>
#include <algorithm>

namespace videoeye {
namespace analyzer {

MacroblockAnalyzer::MacroblockAnalyzer() {
    LOG_INFO("宏块分析器已初始化");
}

MacroblockAnalyzer::~MacroblockAnalyzer() = default;

model::MacroblockFrameAnalysis MacroblockAnalyzer::AnalyzeFrame(
    const AVFrame* frame,
    int frame_index,
    int64_t pts,
    double timestamp,
    int frame_type,
    int codec_id) {

    model::MacroblockFrameAnalysis result;
    result.frame_index = frame_index;
    result.pts = pts;
    result.timestamp = timestamp;
    result.frame_type = frame_type;
    result.codec_id = codec_id;

    if (!frame) {
        LOG_WARN("宏块分析: AVFrame 为空");
        return result;
    }

    result.frame_width = frame->width;
    result.frame_height = frame->height;

    // 提取运动矢量
    auto mvs = ExtractMotionVectors(frame);
    result.has_motion_vectors = !mvs.empty();
    result.motion_vectors = std::move(mvs);

    // 计算统计
    result.stats = ComputeStats(result.motion_vectors, frame->width, frame->height, codec_id);

    // I 帧 (无运动矢量) 估算帧内块数 — 按 codec 自适应块尺寸
    if (result.motion_vectors.empty() && frame->width > 0 && frame->height > 0) {
        bool is_hevc = (codec_id == AV_CODEC_ID_HEVC);
        int block = is_hevc ? 64 : 16;  // HEVC CTU 64x64, H.264 宏块 16x16
        int mb_w = (frame->width + block - 1) / block;
        int mb_h = (frame->height + block - 1) / block;
        result.stats.intra_count = mb_w * mb_h;
        result.stats.total_blocks = result.stats.intra_count;
    }

    return result;
}

std::vector<model::MotionVectorInfo> MacroblockAnalyzer::ExtractMotionVectors(const AVFrame* frame) {
    std::vector<model::MotionVectorInfo> result;

    if (!frame) return result;

    // 从 side data 中获取运动矢量
    AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (!sd || sd->size <= 0) {
        return result;
    }

    const int mv_count = sd->size / static_cast<int>(sizeof(AVMotionVector));
    if (mv_count <= 0) return result;

    result.reserve(mv_count);
    const AVMotionVector* mvs = reinterpret_cast<const AVMotionVector*>(sd->data);

    for (int i = 0; i < mv_count; ++i) {
        result.push_back(ConvertMotionVector(mvs[i]));
    }

    return result;
}

model::MacroblockStats MacroblockAnalyzer::ComputeStats(
    const std::vector<model::MotionVectorInfo>& mvs,
    int frame_width, int frame_height,
    int codec_id) {

    model::MacroblockStats stats;
    stats.total_blocks = static_cast<int>(mvs.size());

    if (mvs.empty()) return stats;

    double sum_magnitude = 0.0;

    for (const auto& mv : mvs) {
        // 参考方向统计
        if (mv.source < 0) {
            stats.forward_count++;
        } else if (mv.source > 0) {
            stats.backward_count++;
        }

        // 块大小分布 (统一覆盖 H.264 与 HEVC 全部 PU 尺寸)
        AccumulateBlockSize(stats, mv.block_w, mv.block_h);

        // 运动幅度统计
        AccumulateMagnitude(stats, mv.motion_magnitude);

        sum_magnitude += mv.motion_magnitude;
        if (mv.motion_magnitude > stats.max_motion_magnitude) {
            stats.max_motion_magnitude = mv.motion_magnitude;
        }
    }

    stats.avg_motion_magnitude = sum_magnitude / static_cast<double>(mvs.size());

    // 估算帧内块数 (基于帧尺寸和运动矢量覆盖区域)
    // 块尺寸按 codec 自适应: HEVC CTU 64x64, 其余按 16x16 宏块
    if (frame_width > 0 && frame_height > 0) {
        bool is_hevc = (codec_id == AV_CODEC_ID_HEVC);
        int block = is_hevc ? 64 : 16;
        int mb_w = (frame_width + block - 1) / block;
        int mb_h = (frame_height + block - 1) / block;
        int total_mb = mb_w * mb_h;
        // 帧内块 = 总块数 - 有运动矢量的块数 (粗略估算)
        stats.intra_count = std::max(0, total_mb - stats.total_blocks);
        stats.total_blocks = total_mb;
    }

    return stats;
}

model::MotionVectorInfo MacroblockAnalyzer::ConvertMotionVector(const AVMotionVector& mv) {
    model::MotionVectorInfo info;
    info.block_x = mv.dst_x;
    info.block_y = mv.dst_y;
    info.block_w = mv.w;
    info.block_h = mv.h;
    info.motion_x = mv.motion_x;
    info.motion_y = mv.motion_y;
    info.motion_scale = mv.motion_scale;
    info.source = mv.source;

    // 计算像素精度下的运动矢量
    double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
    double px_motion_x = mv.motion_x / scale;
    double px_motion_y = mv.motion_y / scale;

    info.motion_magnitude = std::sqrt(px_motion_x * px_motion_x + px_motion_y * px_motion_y);

    // 计算角度 (0-360 度)
    if (info.motion_magnitude > 0.001) {
        info.motion_angle = std::atan2(px_motion_y, px_motion_x) * 180.0 / M_PI;
        if (info.motion_angle < 0) {
            info.motion_angle += 360.0;
        }
    }

    return info;
}

void MacroblockAnalyzer::AccumulateBlockSize(model::MacroblockStats& stats, uint8_t w, uint8_t h) {
    // HEVC CTU/CU 大尺寸分区 (H.264 不产生)
    if (w == 64 && h == 64) {
        stats.count_64x64++;
    } else if (w == 32 && h == 32) {
        stats.count_32x32++;
    } else if (w == 32 && h == 16) {
        stats.count_32x16++;
    } else if (w == 16 && h == 32) {
        stats.count_16x32++;
    } else if (w == 32 && h == 8) {
        stats.count_32x8++;
    } else if (w == 8 && h == 32) {
        stats.count_8x32++;
    }
    // H.264 宏块尺寸 (HEVC 小尺寸 PU 同样命中)
    else if (w == 16 && h == 16) {
        stats.count_16x16++;
    } else if (w == 16 && h == 8) {
        stats.count_16x8++;
    } else if (w == 8 && h == 16) {
        stats.count_8x16++;
    } else if (w == 8 && h == 8) {
        stats.count_8x8++;
    } else if (w == 8 && h == 4) {
        stats.count_8x4++;
    } else if (w == 4 && h == 8) {
        stats.count_4x8++;
    } else if (w == 4 && h == 4) {
        stats.count_4x4++;
    } else {
        stats.count_other++;
    }
}

void MacroblockAnalyzer::AccumulateMagnitude(model::MacroblockStats& stats, double magnitude) {
    if (magnitude < 2.0) {
        stats.mag_0_2++;
    } else if (magnitude < 4.0) {
        stats.mag_2_4++;
    } else if (magnitude < 8.0) {
        stats.mag_4_8++;
    } else if (magnitude < 16.0) {
        stats.mag_8_16++;
    } else {
        stats.mag_16_plus++;
    }
}

} // namespace analyzer
} // namespace videoeye
