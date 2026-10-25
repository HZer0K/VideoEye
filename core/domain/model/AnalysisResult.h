#pragma once

// 全文件扫描的结果容器（诊断的唯一数据源）。
//
// 这里是"分析产出了什么"，线路执行相关的东西不在此文件：
//   * 参数 -> core/domain/model/AnalysisOptions.h
//   * 编排 -> core/qt/QtAnalysisController.h（Qt 信号）/ core/analysis/orchestration/AnalysisEngine.h（执行）
//
// 住在这里（而不是 core/analysis/）的原因和 AnalysisTypes.h 一样：整个结构体
// 由 domain 的值类型拼成，不含任何分析逻辑。放在 domain 之后，"消费结果的人"
// （报告导出 / UI / QC 对比）不再需要为了一个容器去依赖"生产结果的人"。
//
// 历史：本文件原为 core/analysis/AnalysisResult.h（namespace videoeye::analyzer）。
// 2026-10-04 下放到 domain，命名空间随之改成 model。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/AnalysisTypes.h" // StreamDigest + AnalysisStatus
#include "core/domain/model/AudioBitstreamInfo.h"
#include "core/domain/model/AudioQcResult.h"
#include "core/domain/model/AuxiliaryDataInfo.h"
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/BitstreamInfo.h"
#include "core/domain/model/ColorHdrResult.h"
#include "core/domain/model/ContainerDiagnosticsInfo.h"
#include "core/domain/model/DimensionStatus.h"
#include "core/domain/model/FrameOrderInfo.h"
#include "core/domain/model/MetricSeries.h"
#include "core/domain/model/Mp4SampleInfo.h"
#include "core/domain/model/ParameterTimeline.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/StreamPackageInfo.h"
#include "core/domain/model/SubtitleCueInfo.h"
#include "core/domain/model/TimecodeInfo.h"
#include "core/domain/model/TimelineDiagnostic.h"
#include "core/domain/model/TransportStreamInfo.h"

namespace videoeye {
namespace model {

// 全文件扫描结果（诊断的唯一数据源）
struct AnalysisResult {
    // 文件级
    std::string file_path;
    std::string file_extension; // 小写扩展名（无扩展名/URL 为空），供扩展名与容器一致性检查
    std::string container_format;
    double duration_seconds = 0.0;
    int64_t file_size_bytes = 0;
    int64_t overall_bitrate_bps = 0;
    bool seekable = true;
    bool moov_after_mdat = false; // 仅 MP4 家族有效

    // 流级
    std::vector<StreamDigest> streams;
    // 主轨（由容器 DEFAULT 标记 + 类型选出，封面图不参与）。-1 = 没有该类流。
    int primary_video_stream_index = -1;
    int primary_audio_stream_index = -1;

    // 时间序列指标
    MetricSeries total_bitrate_kbps;
    MetricSeries video_bitrate_kbps;
    MetricSeries video_fps;

    // GOP / 关键帧（主视频轨的 demux 层轻量统计，供 QC 规则与报告使用）
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

    // 音频 QC（解码 + 重采样/格式归一后统计，见 core/analysis/quality/AudioQcAnalyzer.h）
    //
    // ❗ audio_qc 恒等于"主音频轨"的那一份（primary_audio_stream_index），
    // 为兼容既有消费方（面板/报告/规则）而保留。多音轨结果在 audio_qc_streams。
    AudioQcResult audio_qc;
    // 逐轨 PCM QC 结果（每条参与 QC 的音频流一份）。
    //
    // 为什么不做成 map<stream_index, AudioQcResult>：消费方基本都是"遍历+展示"，
    // 而 AudioQcResult 里没有 stream_index 字段（它描述一条流的内部指标），
    // 用 vector + 平行下标最省事。顺序与 AudioQcTrackOrder() 一致。
    std::vector<AudioQcResult> audio_qc_streams;
    std::vector<int> audio_qc_stream_indices;
    // 本次 QC 实际覆盖了几条音轨 / 一共有几条（用于"只看主轨"时说明覆盖不足）
    int audio_qc_tracks_analyzed = 0;
    int audio_qc_tracks_total = 0;

    // 音频位流参数（D1：逐包解析 ADTS/LATM/AC-3/E-AC-3/Opus 帧头，不解码）。
    // audio_bitstream_analyzed=true 才表示跑过（没有音频流时不跑）。
    // 与 audio_qc 是两条独立通路：这条只看头，因此即使解码器打不开也有结果。
    //
    // ⚠️ 这是**生产阶段的中间标志**：消费方一律用 DimensionExecuted() 判断"跑没跑"，
    // 收尾时 AnalysisResultAssembler 会用 dimensions 表回填这里的值。
    AudioBitstreamResult audio_bitstream;
    bool audio_bitstream_analyzed = false;

    // 色彩与 HDR 元数据（demux 层 + 可选的首帧解码，见 core/analysis/quality/ColorHdrAnalyzer.h）
    ColorHdrAnalysis color_hdr;

    // 时间轴与同步诊断（demux 层，不解码）
    TimelineAnalysisResult timeline;

    // MP4/fMP4 容器一致性（自研 IsobmffParser 解析，见 core/analysis/container/Mp4SampleTableAnalyzer.h）
    // mp4_samples_analyzed=true 才表示跑过（非 MP4 家族或解析失败时不跑）。同 audio_bitstream_analyzed：
    // 这是生产中间标志，消费方用 DimensionExecuted(Mp4SampleTable)。
    Mp4SampleTableResult mp4_samples;
    bool mp4_samples_analyzed = false;

    // 编码码流解析（从 extradata 解析 SPS/VPS/OBU 等，见 core/analysis/codec/BitstreamAnalyzer.h）
    // bitstream_analysis 不为空表示已执行码流分析。bitstream_analyzed 同样是生产中间标志，
    // 消费方用 DimensionExecuted(VideoBitstream)。
    BitstreamAnalysisResult bitstream_analysis;
    bool bitstream_analyzed = false;

    // 每条视频轨各自的码流解析结果（C3：多节目 / 多角度。以前只解析第一条）。
    //
    // bitstream_analysis 仍是"主的那一条"（按 PrepareBitstream 的优先级顺序取第一个
    // 解析成功的），保持向后兼容 —— 面板、报告、导出都还在读它。这里多存一份全量，
    // 是为了让"第二条视频轨也有一份独立结论"这件事真的可查：TS 多节目、多机位 MP4
    // 里，主轨之外的那条往往才是真正出问题的那条。
    //
    // 每条的 stream_index 标着归属（与 streams[] 的下标一致）。为空 = 没有任何一条
    // 视频轨解析出结果。
    std::vector<BitstreamAnalysisResult> bitstream_streams;

    // HLS / DASH 流媒体包（见 core/analysis/streaming/HlsManifestAnalyzer.h / DashManifestAnalyzer.h）
    // 只有输入是清单文件时才跑；是否跑过读 DimensionExecuted(Streaming)，
    // streaming_analyzed 只是生产中间标志。
    StreamingPackageResult streaming_package;
    bool streaming_analyzed = false;

    // 字幕轨（见 core/analysis/diagnostics/SubtitleAnalyzer.h）
    // 没有字幕流时不跑；是否跑过读 DimensionExecuted(Subtitle)，subtitle_analyzed 只是生产中间标志。
    SubtitleAnalysisResult subtitle;
    bool subtitle_analyzed = false;

    // 时码与章节（见 core/analysis/diagnostics/TimecodeAnalyzer.h）
    // 是否跑过读 DimensionExecuted(Timecode)，timecode_analyzed 只是生产中间标志。
    TimecodeAnalysisResult timecode;
    bool timecode_analyzed = false;

    // 辅助数据轨（见 core/analysis/diagnostics/AuxDataAnalyzer.h：data 流 + SCTE-35 + metadata）
    // 是否跑过读 DimensionExecuted(AuxiliaryData)，aux_data_analyzed 只是生产中间标志。
    AuxiliaryDataResult aux_data;
    bool aux_data_analyzed = false;

    // MPEG-TS 传输层摘要（失步 / 连续计数 / PCR / 加扰），供 container.ts.* 规则使用。
    // 只在输入确实是 MPEG-TS 且是本地文件时填充：这些是"传输层事实"，而 avformat 的
    // demux 把 TS 包结构藏起来了，只能按字节再扫一遍文件头（预算见
    // AnalysisOptions::ts_transport_max_packets）。
    TransportStreamInfo ts_transport;

    // AVI / FLV / ASF / OGG 的容器级体检事实（索引、时间戳、页/EOS、对象大小等），
    // 供 container.avi.* / container.flv.* / container.asf.* / container.ogg.* 规则使用。
    // 只有输入确实是这四种容器且是本地文件时才填充（网络源不重扫）。
    ContainerDiagnosticsInfo container_diagnostics;

    // 参数集全程变化时间线（每条视频轨一份，见 core/analysis/codec/ParameterSetTracker.h）。
    //
    // 与 bitstream_analysis 是**两个不同的问题**：后者回答"现在的配置是什么"（单份快照），
    // 这里回答"配置在文件里怎么变过来的"（序列 + 引用统计）。拼接、编码器重启、
    // 中途换分辨率/位深，只有看这条时间线才发现得了。
    std::vector<ParameterTimeline> parameter_timelines;

    // 帧级显示/解码顺序（每条视频轨一份，见 FrameOrderTracker / H264PocTracker /
    // HevcPocTracker）。
    //
    // 与 bitrate_gop 的分工：后者按包统计"关键帧间隔"，只有长度；这里给的是
    // 逐帧 POC，能回答"显示顺序重排了多深""GOP 边界在哪一帧""POC 有没有重复/缺失"。
    // 支持 H.264 与 HEVC —— AV1/VP9/VVC 的 POC 需要各自的 block/frame header 与
    // 参考结构，尚未实现，对应维度会明确标为不可用而不是给空结果。
    std::vector<FrameOrderTimeline> frame_order_timelines;

    // 每个分析维度的证据与覆盖状态（见 core/domain/model/DimensionStatus.h）。
    //
    // 由 AnalysisResultAssembler 统一填充，一项一行。消费方（报告/UI/规则）
    // 必须用它来回答"这条结论覆盖了多少内容、能不能当通过"，不要再去猜
    // 各个 *_analyzed 布尔的组合含义。
    std::vector<DimensionStatus> dimensions;

    // ---- 维度状态表的读写助手 ----
    // 同维度重复设置时覆盖（Assembler 按固定顺序填，天然不会重复；覆盖语义是为了幂等）
    void SetDimension(const DimensionStatus& status);
    const DimensionStatus* FindDimension(AnalysisDimension dimension) const;
    // 该维度是否真的执行过 —— **唯一口径**。
    //
    // 历史遗留的 7 个 *_analyzed 布尔只是生产阶段的中间标志（逐包扫描要边跑边判断），
    // 靠它们回答"这个维度跑没跑"要逐个猜组合含义，且"选项关闭 / 输入不适用 / 依赖缺失"
    // 三种没跑的情况在结构上无法区分。现在一律读 dimensions 表：
    //   * 生产方仍写 flags（扫描循环要用）；
    //   * AnalysisResultAssembler 收尾时用本表回填 flags，保证两者一致；
    //   * 消费方（QC 规则 / 报告 / UI）只调本方法，不再直接读 flags。
    bool DimensionExecuted(AnalysisDimension dimension) const;

    // 该维度是否"已执行且结论可用"
    bool DimensionConclusive(AnalysisDimension dimension) const;

    // 执行状态
    AnalysisStatus scan_status = AnalysisStatus::Complete;
    int scan_error_code = 0;     // 0 = 无；否则为 FFmpeg 错误码或 -1
    int64_t scanned_packets = 0; // 已扫描包数（取消/截断时也是部分结果）
    std::string error_message;

    int VideoStreamCount() const;
    int AudioStreamCount() const;
    const StreamDigest* FirstVideoStream() const;
    const StreamDigest* FirstAudioStream() const;
};

} // namespace model
} // namespace videoeye
