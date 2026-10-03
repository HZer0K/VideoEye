#pragma once

// 扫描链路里最底层的公共值类型。
//
// 住在这里（而不是 core/analysis/）的原因：这两个类型都是**产出的值**，
// 不含任何分析逻辑，也不碰 FFmpeg / Qt —— 和 domain 里其它 *Result.h 一样
// 是纯数据。"谁生产它"和"谁读它"不该被绑在一起：报告导出、UI 展示、QC 对比
// 都只想消费结果，不该为了拿一个枚举或一个流摘要就把整个 analysis 层拖进来。
//
// 单独成一个文件是为了让"只要拼状体、不要结果/参数"的使用者（比如界面预览、
// 单纯做 demux 摘要的工具）不必拖着整个 AnalysisResult 的模型清单编译。
//
// 历史：本文件原为 core/analysis/AnalysisTypes.h（namespace videoeye::analyzer）。
// 2026-10-04 随 AnalysisResult 一起下放到 domain，命名空间随之改成 model。

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

// 全文件扫描的执行状态（取代原先的 bool completed）
//
// 区分"完整扫到 EOF"、"命中包数上限只抽样"、"被取消"、"读取/打开失败"，
// 让报告与 UI 能明确标注非完整结果，避免把截断/IO 错误或抽样当成完整 QC 结论。
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

} // namespace model
} // namespace videoeye
