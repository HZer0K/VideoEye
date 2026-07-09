#pragma once

#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
}

#include "core/model/MacroblockInfo.h"

namespace videoeye {
namespace analyzer {

// 宏块分析器 - 从解码帧提取运动矢量与宏块级编码信息
//
// 依赖 FFmpeg 的 export_mvs 机制:
//   VideoDecoder 在 avcodec_open2 前设置 codec_ctx->export_mvs = 1,
//   解码后 AVFrame 的 side data 中会携带 AV_FRAME_DATA_MOTION_VECTORS,
//   每个 AVMotionVector 对应一个编码块的运动信息。
//
// 适用编码: H.264 (AVC) / H.265 (HEVC) / VP9 等支持运动补偿的编解码器。
// I 帧无运动矢量, 分析器会返回 has_motion_vectors=false 并填充 intra_count。
class MacroblockAnalyzer {
public:
    MacroblockAnalyzer();
    ~MacroblockAnalyzer();

    // 从 AVFrame 提取完整宏块分析数据
    // frame         - 解码后的 AVFrame (需带有 motion vectors side data)
    // frame_index   - 帧序号
    // pts           - 帧 PTS
    // timestamp     - 帧时间戳 (秒)
    // frame_type    - AVPictureType (1=I, 2=P, 3=B)
    model::MacroblockFrameAnalysis AnalyzeFrame(const AVFrame* frame,
                                                int frame_index,
                                                int64_t pts,
                                                double timestamp,
                                                int frame_type);

    // 仅提取运动矢量列表 (不做统计)
    std::vector<model::MotionVectorInfo> ExtractMotionVectors(const AVFrame* frame);

    // 根据运动矢量列表计算统计信息
    model::MacroblockStats ComputeStats(const std::vector<model::MotionVectorInfo>& mvs,
                                        int frame_width, int frame_height);

private:
    // 将 AVMotionVector 转换为 MotionVectorInfo
    static model::MotionVectorInfo ConvertMotionVector(const AVMotionVector& mv);

    // 块大小分类辅助
    static void AccumulateBlockSize(model::MacroblockStats& stats, uint8_t w, uint8_t h);

    // 运动幅度分桶辅助
    static void AccumulateMagnitude(model::MacroblockStats& stats, double magnitude);
};

} // namespace analyzer
} // namespace videoeye
