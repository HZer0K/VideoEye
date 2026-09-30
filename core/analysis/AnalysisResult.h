#pragma once

// 全文件扫描的结果容器（诊断的唯一数据源）。
//
// 这里是"分析产出了什么"，线路执行相关的东西不在此文件：
//   * 参数 -> core/analysis/AnalysisOptions.h
//   * 编排 -> core/qt/QtAnalysisController.h（Qt 信号）/ core/analysis/orchestration/AnalysisEngine.h（执行）

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"   // AnalysisStatus（扫描结果状态）目前住在参数头里
#include "core/analysis/AnalysisTypes.h"
#include "core/domain/model/AudioQcResult.h"
#include "core/domain/model/AuxiliaryDataInfo.h"
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/BitstreamInfo.h"
#include "core/domain/model/ColorHdrResult.h"
#include "core/domain/model/MetricSeries.h"
#include "core/domain/model/Mp4SampleInfo.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/StreamPackageInfo.h"
#include "core/domain/model/SubtitleCueInfo.h"
#include "core/domain/model/TimecodeInfo.h"
#include "core/domain/model/TimelineDiagnostic.h"

namespace videoeye {
namespace analyzer {

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
    model::BitrateGopAnalysis bitrate_gop;

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

    // 音频 QC（解码 + 重采样/格式归一后统计，见 core/analysis/quality/AudioQcAnalyzer.h）
    model::AudioQcResult audio_qc;

    // 色彩与 HDR 元数据（demux 层 + 可选的首帧解码，见 core/analysis/quality/ColorHdrAnalyzer.h）
    model::ColorHdrAnalysis color_hdr;

    // 时间轴与同步诊断（demux 层，不解码）
    model::TimelineAnalysisResult timeline;

    // MP4/fMP4 容器一致性（自研 IsobmffParser 解析，见 core/analysis/container/Mp4SampleTableAnalyzer.h）
    // mp4_samples_analyzed=true 才表示跑过（非 MP4 家族或解析失败时不跑）
    model::Mp4SampleTableResult mp4_samples;
    bool mp4_samples_analyzed = false;

    // 编码码流解析（从 extradata 解析 SPS/VPS/OBU 等，见 core/analysis/codec/BitstreamAnalyzer.h）
    // bitstream_analysis 不为空表示已执行码流分析
    model::BitstreamAnalysisResult bitstream_analysis;
    bool bitstream_analyzed = false;

    // HLS / DASH 流媒体包（见 core/analysis/streaming/HlsManifestAnalyzer.h / DashManifestAnalyzer.h）
    // 只有输入是清单文件时才跑；streaming_analyzed=true 才表示跑过。
    model::StreamingPackageResult streaming_package;
    bool streaming_analyzed = false;

    // 字幕轨（见 core/analysis/diagnostics/SubtitleAnalyzer.h）
    // subtitle_analyzed=true 才表示跑过（没有字幕流时不跑）
    model::SubtitleAnalysisResult subtitle;
    bool subtitle_analyzed = false;

    // 时码与章节（见 core/analysis/diagnostics/TimecodeAnalyzer.h）
    model::TimecodeAnalysisResult timecode;
    bool timecode_analyzed = false;

    // 辅助数据轨（见 core/analysis/diagnostics/AuxDataAnalyzer.h：data 流 + SCTE-35 + metadata）
    model::AuxiliaryDataResult aux_data;
    bool aux_data_analyzed = false;

    // 执行状态
    AnalysisStatus scan_status = AnalysisStatus::Complete;
    int scan_error_code = 0;        // 0 = 无；否则为 FFmpeg 错误码或 -1
    int64_t scanned_packets = 0;    // 已扫描包数（取消/截断时也是部分结果）
    std::string error_message;

    int VideoStreamCount() const;
    int AudioStreamCount() const;
    const StreamDigest* FirstVideoStream() const;
    const StreamDigest* FirstAudioStream() const;
};

} // namespace analyzer
} // namespace videoeye
