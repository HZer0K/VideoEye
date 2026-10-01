#pragma once

// 「码流分析」页拆分成两个组件后共用的记录结构。
//
// 原先这 4 个结构是 AnalysisPanel 的私有嵌套类型。把码流分析页拆成
// StreamOverviewView（顶部：流指标 + 码率/FPS/GOP 三条趋势曲线 + 导出报告）
// 和 FramePacketView（底部：视频帧 / 包 / GOP 摘要 / 音频帧 四张表）之后，
// 两边都要用到它们 —— 尤其是 GopSummary：由 FramePacketView 从解码帧的
// pict_type 推导产出，StreamOverviewView 拿它填「最大GOP大小」指标并画
// 「GOP 帧数分布」曲线。放在这里比塞进任一侧更合适。

#include <QtGlobal>

namespace videoeye {
namespace ui {

// 视频帧表的一行（与 FramePacketView 帧表 6 列表头一一对应）
struct VideoFrameRecord {
    int index = 0;
    int frame_type = 0;
    bool is_key_frame = false;
    qint64 pts = 0;
    double timestamp_seconds = 0.0;
    int gop_index = 0;
    int gop_position = 0;
};

// 一个 GOP 段的汇总（与 GOP 摘要表 9 列表头一一对应）
struct GopSummary {
    int gop_index = 0;
    int start_frame = 0;
    int end_frame = 0;
    double start_ts = 0.0;
    double end_ts = 0.0;
    int total_frames = 0;
    int i_count = 0;
    int p_count = 0;
    int b_count = 0;
    int key_count = 0;
};

// 音频帧表的一行
struct AudioFrameRecord {
    int index = 0;
    qint64 pts = 0;
    double timestamp_seconds = 0.0;
    int sample_count = 0;
    int sample_rate = 0;
    int channels = 0;
    int byte_count = 0;
};

// 包表的一行
struct PacketRecord {
    int index = 0;
    int stream_index = -1;
    int stream_type = -1;
    qint64 pts = 0;
    qint64 dts = 0;
    qint64 duration = 0;
    int size = 0;
    int flags = 0;
    qint64 pos = -1;
    double timestamp_seconds = 0.0;
};

}  // namespace ui
}  // namespace videoeye
