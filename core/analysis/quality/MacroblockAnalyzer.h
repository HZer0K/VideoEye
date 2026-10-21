#pragma once

#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
}

#include "core/domain/model/MacroblockInfo.h"

namespace videoeye {

// 宏块/CTU 分析器 - 从解码帧提取运动矢量与块级编码信息
//
// 依赖 FFmpeg 的 export_mvs 机制:
//   VideoDecoder 在 avcodec_open2 前设置 codec_ctx->export_mvs = 1,
//   解码后 AVFrame 的 side data 中会携带 AV_FRAME_DATA_MOTION_VECTORS,
//   每个 AVMotionVector 对应一个编码块的运动信息。
//
// 适用编码: H.264 (AVC, 宏块 16x16) / H.265 (HEVC, CTU 最大 64x64) /
//           AV1 / VP9 等支持运动补偿的编解码器。
// 块大小分桶统一覆盖 H.264 与 HEVC 全部 PU 尺寸, 无需按编码区分统计逻辑。
//
// 帧内块估算只对 H.264 / HEVC 的 I 帧做 (按 16x16 宏块 / 64x64 CTU 的固定网格),
// 结果同时置 intra_count_available 与 block_counts_estimated。其余编码 (AV1 / VP9 /
// MPEG-2 ...) 的运动矢量语义不同, 照 16x16 网格硬套会给出错误数字, 所以明确报
// "不可用"(intra_count_available=false) 而不是估一个值。
// 总块数 (total_blocks) 只有两条路径会填: 带运动矢量时 ComputeStats 按网格铺一次,
// 或上面那条 I 帧分支。两条路径都会置 block_counts_estimated; 其余情况 total_blocks 停在
// 0 —— 那是"没算", 不是"0 个块", 所以 UI 报"不可用"而不是显示 0。
// 注意运动矢量"条数" ≠ 编码块数 —— 一个块可能有多条 MV, 缺失 MV 也不代表该块是帧内块,
// 所以不要用 total_blocks - mvs.size() 反推 intra_count。
class MacroblockAnalyzer {
public:
    MacroblockAnalyzer();
    ~MacroblockAnalyzer();

    // 从 AVFrame 提取完整宏块/CTU 分析数据
    // frame         - 解码后的 AVFrame (需带有 motion vectors side data)
    // frame_index   - 帧序号
    // pts           - 帧 PTS
    // timestamp     - 帧时间戳 (秒)
    // frame_type    - AVPictureType (1=I, 2=P, 3=B)
    // codec_id      - AV_CODEC_ID_* (用于帧内块估算自适应与 UI 术语)
    model::MacroblockFrameAnalysis AnalyzeFrame(const AVFrame* frame,
                                                int frame_index,
                                                int64_t pts,
                                                double timestamp,
                                                int frame_type,
                                                int codec_id = 0);

    // 仅提取运动矢量列表 (不做统计)
    std::vector<model::MotionVectorInfo> ExtractMotionVectors(const AVFrame* frame);

    // 根据运动矢量列表计算统计信息
    // codec_id 只决定块网格尺寸 (HEVC 按 64x64 CTU, 其余按 16x16 宏块)。
    // 总块数在这里按网格给出并置 block_counts_estimated; 帧内块数不在这里推导
    // (见类注释: 运动矢量条数推不出帧内块数)。
    model::MacroblockStats ComputeStats(const std::vector<model::MotionVectorInfo>& mvs,
                                        int frame_width, int frame_height,
                                        int codec_id = 0);

private:
    // 将 AVMotionVector 转换为 MotionVectorInfo
    static model::MotionVectorInfo ConvertMotionVector(const AVMotionVector& mv);

    // 块大小分类辅助
    static void AccumulateBlockSize(model::MacroblockStats& stats, uint8_t w, uint8_t h);

    // 运动幅度分桶辅助
    static void AccumulateMagnitude(model::MacroblockStats& stats, double magnitude);
};

} // namespace videoeye
