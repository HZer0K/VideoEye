#pragma once

// 分析链路里最底层的公共值类型。
//
// 单独成一个文件是为了让"只要拼状体、不要结果/参数"的使用者（比如界面预览、
// 单纯做 demux 摘要的工具）不必拖着整个 AnalysisResult 的模型清单编译。

#include <cstdint>
#include <string>

namespace videoeye {
namespace analyzer {

// 全文件扫描的执行状态（取代原先的 bool completed）
//
// 区分"完整扫到 EOF"、"命中包数上限只抽样"、"被取消"、"读取/打开失败"，
// 让报告与 UI 能明确标注非完整结果，避免把截断/IO 错误或抽样当成完整 QC 结论。
//
// 为什么住在这里而不是 AnalysisOptions.h：它是**产出的状态**而不是**输入的参数**。
// 以前 AnalysisResult.h 为了拿它不得不 include 整个参数头，等于"读结果的人"被迫
// 拖着"传参数的人"—— reporting / UI 只想消费结果，却被 options 绑住。
// AnalysisOptions.h 仍然 include 本文件，所以老的调用点不用改。
enum class AnalysisStatus {
    Complete,    // 完整扫描到 EOF
    Sampled,     // 命中 max_packets 上限，仅完成抽样
    Cancelled,   // 被用户取消
    Failed,      // 打开 / 探测 / 读取数据包失败
};

const char* ToString(AnalysisStatus status);

// 单条流的静态摘要（demux 层，不解码）
struct StreamDigest {
    int index = -1;
    int media_type = -1;          // AVMediaType
    std::string codec_name;
    std::string profile_name;
    int width = 0;
    int height = 0;
    double avg_fps = 0.0;
    int sample_rate = 0;
    int channels = 0;
    int64_t bitrate_bps = 0;
    int64_t packet_count = 0;
    int64_t byte_count = 0;
    int64_t frame_count = 0;      // 视频按包估算
    int64_t key_frame_count = 0;
    double start_seconds = 0.0;
    double duration_seconds = 0.0;

    bool IsVideo() const { return media_type == 0; }  // AVMEDIA_TYPE_VIDEO
    bool IsAudio() const { return media_type == 1; }  // AVMEDIA_TYPE_AUDIO
};

} // namespace analyzer
} // namespace videoeye
