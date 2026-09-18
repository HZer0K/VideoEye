#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/analyzer/AudioQcAnalyzer.h"
#include "core/analyzer/BitrateGopAnalyzer.h"
#include "core/analyzer/ColorHdrAnalyzer.h"
#include "core/analyzer/Mp4SampleTableAnalyzer.h"
#include "core/model/MetricSeries.h"
#include "core/model/TimelineDiagnostic.h"

namespace videoeye {
namespace analyzer {

// 全文件分析的可选项
struct AnalysisOptions {
    double sample_interval_seconds = 1.0;  // 码率/帧率序列的采样粒度
    int64_t max_packets = 0;               // 0 表示不限制（超长文件可设上限做抽样）
    bool detect_container_layout = true;   // MP4 家族是否检测 moov/mdat 顺序

    // 码率与 GOP 深度分析（滑动窗口码率 / I-P-B / GOP 列表 / 异常识别）
    bool analyze_bitrate_gop = true;
    BitrateGopOptions bitrate_gop_options;

    // 是否解码视频帧以获取精确帧类型（I/P/B）。
    // 关闭时改用 codec parser（几乎零成本）；解析器无法判定时帧类型记为"未知"。
    // 开启后结果最准确，但要完整解码一遍视频，长文件会明显变慢。
    bool decode_frame_types = false;

    // 音频 QC（响度 / 真峰值 / 削波 / 静音 / 声道相位 / metadata 一致性）。
    // 需要把第一条音频流完整解码一遍（音频解码开销远小于视频），与码率/GOP 扫描同一次 demux。
    bool analyze_audio_qc = true;
    AudioQcOptions audio_qc_options;

    // 色彩与 HDR 元数据分析（primaries / transfer / matrix / range / bit depth /
    // chroma subsampling / mastering display / MaxCLL-MaxFALL / Dolby Vision）。
    // 本身几乎零成本：主要读 AVCodecParameters 与 coded_side_data；
    // 只有在 HDR 静态元数据不全时才会额外解码首帧去读 AVFrame side data。
    bool analyze_color_hdr = true;
    ColorHdrOptions color_hdr_options;

    // MP4/fMP4 容器一致性校验（sample table / elst / moof-traf-trun / faststart）。
    // 走 Bento4 直接读 stbl 与 moof，与 FFmpeg demux 是两套独立解析：
    // 只在容器属于 MP4 家族时才真正执行，其它格式直接跳过。
    bool analyze_mp4_sample_table = true;
    Mp4SampleTableOptions mp4_sample_table_options;
};

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

// 全文件扫描结果（诊断的唯一数据源）
struct AnalysisResult {
    // 文件级
    std::string file_path;
    std::string file_extension;     // 小写扩展名（无扩展名/URL 为空），供扩展名与容器一致性检查
    std::string container_format;
    double duration_seconds = 0.0;
    int64_t file_size_bytes = 0;
    int64_t overall_bitrate_bps = 0;
    bool seekable = true;
    bool moov_after_mdat = false;   // 仅 MP4 家族有效

    // 流级
    std::vector<StreamDigest> streams;

    // 时间序列指标
    model::MetricSeries total_bitrate_kbps;
    model::MetricSeries video_bitrate_kbps;
    model::MetricSeries video_fps;

    // GOP / 关键帧（demux 层的轻量统计，供 QC 规则与报告使用）
    std::vector<double> gop_intervals_seconds;
    std::vector<int> gop_frame_sizes;
    double max_gop_interval_seconds = 0.0;
    int max_gop_frames = 0;

    // 码率与 GOP 深度分析结果（滑动窗口码率、I/P/B、GOP 列表、异常与建议）
    BitrateGopAnalysis bitrate_gop;

    // 时间戳健康度（PTS 非单调 / 时间戳跳变现由 TimelineAnalyzer 统一产出，
    // 不再在此重复统计；DTS 缺失占比仍用于 timing.dts_missing 规则）
    int64_t total_packets = 0;
    int64_t total_bytes = 0;
    int64_t video_packets = 0;
    int64_t key_frame_count = 0;
    int64_t packets_missing_dts = 0;

    // 包大小
    int64_t max_packet_bytes = 0;
    double avg_packet_bytes = 0.0;

    // 音频 QC（解码 + 重采样/格式归一后统计，见 core/analyzer/AudioQcAnalyzer.h）
    model::AudioQcResult audio_qc;

    // 色彩与 HDR 元数据（demux 层 + 可选的首帧解码，见 core/analyzer/ColorHdrAnalyzer.h）
    ColorHdrAnalysis color_hdr;

    // 时间轴与同步诊断（demux 层，不解码）
    model::TimelineAnalysisResult timeline;

    // MP4/fMP4 容器一致性（Bento4 解析，见 core/analyzer/Mp4SampleTableAnalyzer.h）
    // mp4_samples_analyzed=true 才表示跑过（非 MP4 家族或 Bento4 不可用时不跑）
    model::Mp4SampleTableResult mp4_samples;
    bool mp4_samples_analyzed = false;

    // 执行状态
    bool completed = true;        // false = 被取消
    std::string error_message;

    int VideoStreamCount() const;
    int AudioStreamCount() const;
    const StreamDigest* FirstVideoStream() const;
    const StreamDigest* FirstAudioStream() const;
};

} // namespace analyzer
} // namespace videoeye
