#include "core/analysis/quality/MacroblockAnalyzer.h"
#include "infrastructure/logging/Logger.h"

#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace videoeye {

namespace {

// 块去重键: B 帧的同一块会同时给出前向 / 后向两条记录，dst 与 block_w/h 完全相同，
// 只有 source 不同。用 (中心 X, 中心 Y, 宽, 高) 作为块的身份。
// 坐标是像素位置，16 位足够覆盖到 65535 (8K 也只有 7680)。
uint64_t MakeBlockKey(const model::MotionVectorInfo& mv) {
    return static_cast<uint64_t>(static_cast<uint16_t>(mv.block_x)) |
           (static_cast<uint64_t>(static_cast<uint16_t>(mv.block_y)) << 16) |
           (static_cast<uint64_t>(mv.block_w) << 32) | (static_cast<uint64_t>(mv.block_h) << 40);
}

struct BlockAggregate {
    uint8_t w = 0;
    uint8_t h = 0;
    double magnitude = 0.0; // 同一块的前/后向记录取较大幅度
};

} // namespace

MacroblockAnalyzer::MacroblockAnalyzer() {
    LOG_INFO("宏块分析器已初始化");
}

MacroblockAnalyzer::~MacroblockAnalyzer() = default;

model::MacroblockFrameAnalysis MacroblockAnalyzer::AnalyzeFrame(const AVFrame* frame, int frame_index, int64_t pts,
                                                                double timestamp, int frame_type, int codec_id) {

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

    if (frame_type == AV_PICTURE_TYPE_I && frame->width > 0 && frame->height > 0 &&
        (codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC)) {
        bool is_hevc = (codec_id == AV_CODEC_ID_HEVC);
        int block = is_hevc ? 64 : 16; // HEVC CTU 64x64, H.264 宏块 16x16
        int mb_w = (frame->width + block - 1) / block;
        int mb_h = (frame->height + block - 1) / block;
        result.stats.intra_count = mb_w * mb_h;
        result.stats.total_blocks = result.stats.intra_count;
        result.stats.block_counts_estimated = true;
        result.stats.intra_count_available = true;
    }

    return result;
}

std::vector<model::MotionVectorInfo> MacroblockAnalyzer::ExtractMotionVectors(const AVFrame* frame) {
    std::vector<model::MotionVectorInfo> result;

    if (!frame)
        return result;

    // 从 side data 中获取运动矢量
    AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (!sd || sd->size <= 0) {
        return result;
    }

    const int mv_count = sd->size / static_cast<int>(sizeof(AVMotionVector));
    if (mv_count <= 0)
        return result;

    result.reserve(mv_count);
    const AVMotionVector* mvs = reinterpret_cast<const AVMotionVector*>(sd->data);

    for (int i = 0; i < mv_count; ++i) {
        result.push_back(ConvertMotionVector(mvs[i]));
    }

    return result;
}

model::MacroblockStats MacroblockAnalyzer::ComputeStats(const std::vector<model::MotionVectorInfo>& mvs,
                                                        int frame_width, int frame_height, int codec_id) {

    model::MacroblockStats stats;

    if (mvs.empty())
        return stats;

    const bool is_hevc = (codec_id == AV_CODEC_ID_HEVC);
    const bool supports_intra_estimate = (codec_id == AV_CODEC_ID_H264 || is_hevc);
    const int grid_block = is_hevc ? 64 : 16; // HEVC CTU 64x64, H.264/其余按 16x16 宏块

    // 块尺寸按 codec 自适应铺一次网格: 这是"总块数"的唯一来源。
    int grid_w = 0;
    int grid_h = 0;
    std::vector<uint8_t> covered; // 每个网格单元是否被至少一个 inter 块覆盖
    if (frame_width > 0 && frame_height > 0) {
        grid_w = (frame_width + grid_block - 1) / grid_block;
        grid_h = (frame_height + grid_block - 1) / grid_block;
        stats.total_blocks = grid_w * grid_h;
        stats.block_counts_estimated = true;
        if (supports_intra_estimate)
            covered.assign(static_cast<std::size_t>(grid_w) * static_cast<std::size_t>(grid_h), 0);
    }

    // 去重: B 帧同一块的前向 / 后向两条记录只算一块。否则块大小分布与平均幅度都会翻倍。
    std::unordered_map<uint64_t, BlockAggregate> blocks;
    blocks.reserve(mvs.size());

    int covered_cells = 0;
    for (const auto& mv : mvs) {
        // 参考方向仍按记录数统计: 一块双向前后各一条，是两条真实的参考记录。
        if (mv.source < 0) {
            stats.forward_count++;
        } else if (mv.source > 0) {
            stats.backward_count++;
        }

        const auto [it, inserted] =
            blocks.try_emplace(MakeBlockKey(mv), BlockAggregate{mv.block_w, mv.block_h, mv.motion_magnitude});
        if (!inserted && mv.motion_magnitude > it->second.magnitude) {
            it->second.magnitude = mv.motion_magnitude;
        }

        // 网格覆盖: 把该块的像素范围映射到宏块/CTU 单元，用于 P/B 帧的帧内块估算。
        if (!covered.empty()) {
            const int left = std::max(0, mv.block_x - mv.block_w / 2);
            const int right = std::min(frame_width - 1, mv.block_x + mv.block_w / 2 - 1);
            const int top = std::max(0, mv.block_y - mv.block_h / 2);
            const int bottom = std::min(frame_height - 1, mv.block_y + mv.block_h / 2 - 1);
            if (right >= left && bottom >= top) {
                const int x0 = left / grid_block;
                const int x1 = right / grid_block;
                const int y0 = top / grid_block;
                const int y1 = bottom / grid_block;
                for (int gy = y0; gy <= y1; ++gy) {
                    for (int gx = x0; gx <= x1; ++gx) {
                        const auto index = static_cast<std::size_t>(gy) * static_cast<std::size_t>(grid_w) + gx;
                        if (covered[index] == 0) {
                            covered[index] = 1;
                            covered_cells++;
                        }
                    }
                }
            }
        }
    }

    stats.distinct_block_count = static_cast<int>(blocks.size());

    double sum_magnitude = 0.0;
    for (const auto& entry : blocks) {
        const BlockAggregate& block = entry.second;
        // 块大小分布 (统一覆盖 H.264 与 HEVC 全部 PU 尺寸)
        AccumulateBlockSize(stats, block.w, block.h);

        // 运动幅度统计 (按去重块)
        AccumulateMagnitude(stats, block.magnitude);
        sum_magnitude += block.magnitude;
        if (block.magnitude > stats.max_motion_magnitude) {
            stats.max_motion_magnitude = block.magnitude;
        }
    }

    if (!blocks.empty())
        stats.avg_motion_magnitude = sum_magnitude / static_cast<double>(blocks.size());

    // P/B 帧帧内估算: 网格单元减去被 inter 块覆盖的单元。H.264 的 8x8/16x8 分区会落在
    // 同一个 16x16 单元里，按单元去重后不会重复扣减，比"记录数对不上网格"可靠。
    if (supports_intra_estimate && !covered.empty()) {
        stats.intra_count = stats.total_blocks - covered_cells;
        stats.intra_count_available = true;
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

} // namespace videoeye
