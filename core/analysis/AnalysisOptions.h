#pragma once

// 全文件分析的**参数**层。
//
// 为什么要有这个文件: 这些 Options 以前散落在各自的 Analyzer 头文件里, 于是任何想
// "传个参数"的模块(QcProfile / UI / 批处理)都得 include 一整排 Analyzer —— 编译
// 依赖面被撑到最大, 改一个分析器的注释都要重编译半个工程。
//
// 现在 Analyzer 反过来 include 本文件取自己的选项, 依赖方向变成:
//   analysis -> AnalysisOptions.h -> (仅 stdlib)
// 本文件不 include 任何 Analyzer、不 include FFmpeg。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace analyzer {

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


// 码率与 GOP 深度分析的配置项
//
// 所有阈值都集中在这里，便于 UI 暴露成可编辑项；分析器本身不内置魔法数字。
struct BitrateGopOptions {
    // 滑动窗口长度（秒）。第一项为默认窗口，UI 下拉框直接遍历此列表。
    // 0.5 / 1 / 2 / 5 秒是码率分析的常用档位。
    std::vector<double> windows_seconds = {1.0, 0.5, 2.0, 5.0};

    // 目标峰值码率（kbps）。<=0 时自动取 平均码率 * auto_peak_ratio。
    double target_peak_kbps = 0.0;
    double auto_peak_ratio = 2.0;

    // GOP 时长上限（秒）/ 帧数上限。任一超过即记为超长 GOP。
    double max_gop_seconds = 10.0;
    int max_gop_frames = 300;

    // 关键帧间隔（秒）标准差/均值上限，超过记为间隔不均匀
    double gop_irregular_ratio = 0.6;

    // 帧大小 > 全片平均帧大小 * large_frame_ratio → 异常大帧
    double large_frame_ratio = 8.0;

    // I 帧大小 > 平均 I 帧大小 * i_frame_oversize_ratio → I 帧过大
    double i_frame_oversize_ratio = 3.0;

    // 场景切换点前后多少秒内没有关键帧 → 判定"场景切换后缺少关键帧"
    double scene_key_tolerance_seconds = 0.5;
    // 低于该强度的切换点不参与关联（噪声过滤）
    double scene_score_threshold = 0.45;

    // 单类型异常的最大保留条数（防止长视频刷屏）
    int max_anomalies_per_type = 200;

    // 逐帧样本上限（0 = 不限制）。长文件（小时级）的 samples_ 会无界增长，
    // 超过上限即对样本做有界降采样（保留偶数下标），把内存夹在 ~max_samples，
    // 展示曲线本就是近似，降采样不影响 GOP 统计（GOP 由增量状态维护，不依赖 samples_）。
    int64_t max_samples = 0;
};

// 音频 QC 分析配置
//
// 说明：这里只放"检测参数"（窗口、门限、阈值），不放"合格判定阈值"——
// 后者属于 QC 规则（core/domain/model/QcModels.cpp 的 audio.* 规则），用户可在「规则与阈值」页改。
struct AudioQcOptions {
    // ---- 阶段开关 ----
    bool enable_loudness = true;     // BS.1770 响度（Integrated / Short-term / Momentary / LRA）
    bool enable_true_peak = true;    // 4× 过采样真峰值（最耗时的一项）
    bool enable_correlation = true;  // 声道相关性 / 反相检测

    // ---- 电平 ----
    // |x| >= clip_threshold 记一次削波。取 0.999（约 -0.009 dBFS）而不是 1.0，
    // 这样 16bit 满刻度（32767/32768 = 0.99997）也能被判为削波。
    double clip_threshold = 0.999;
    // 同一声道内两次削波间隔小于该秒数则归并为一段
    double clip_merge_gap_seconds = 0.05;

    // ---- 静音 ----
    double silence_threshold_dbfs = -60.0;
    double min_silence_seconds = 0.5;

    // ---- 相关性 ----
    // 低于该值即认为存在反相（EBU 常用 -0.5 作为"明显反相"分界）
    double out_of_phase_threshold = -0.5;

    // ---- BS.1770 门限 ----
    double absolute_gate_lufs = -70.0;   // 绝对门限
    double relative_gate_lu = -10.0;     // 相对门限（低于无门限均值 10 LU 的块被剔除）
    double lra_relative_gate_lu = -20.0; // LRA 的相对门限（EBU Tech 3341）

    // ---- 容量上限（防止长视频把 UI 与导出刷爆）----
    int max_loudness_points = 20000;
    int max_clip_events = 500;
    int max_silence_ranges = 500;
};

// 色彩与 HDR 元数据分析选项
struct ColorHdrOptions {
    // 容器/码流层面拿不到 HDR 元数据时，是否解码首帧读取 AVFrame side data。
    // 静态元数据（SMPTE ST 2086 / MaxCLL-MaxFALL）通常写在容器盒或 SEI 里，
    // 不需要解码；HDR10+/DV 的动态元数据则多数只在解码后的帧上出现。
    bool probe_decoded_frame = true;

    // 最多往后解几帧去找动态元数据（通常 1 帧就够，动态元数据一般都有每帧副本）
    int max_probe_frames = 2;
};

struct Mp4SampleTableOptions {
    // 单个轨道最多展开多少个样本（大文件保护；超出只做表级校验）
    uint32_t max_samples_per_track = 20000;
    // 是否逐样本展开（关闭时只做各表计数与分片校验，开销极低）
    bool expand_samples = true;
    // 音视频首个样本呈现时间差的告警阈值（毫秒）
    double av_start_tolerance_ms = 40.0;
    // 首帧被 elst 跳过的告警阈值（毫秒）
    double elst_shift_tolerance_ms = 33.0;
    // 负合成时间的告警阈值（毫秒，绝对值超过才告警）
    double negative_cts_tolerance_ms = 0.0;
    // 分片解码时间允许的误差（媒体时基单位，防止舍入误报）
    uint32_t fragment_time_tolerance = 1;
};

// 见 HlsManifestOptions 的注释：带 NSDMI 的结构体必须在 namespace 作用域。
struct SegmentQcOptions {
    // 每条 playlist / representation 最多探测多少个分片（大包保护）
    uint32_t max_probe_segments = 8;
    bool probe_segments = true;
    // 关键帧时间对齐容差（秒）。比这更小的偏差视为同一时刻。
    double keyframe_align_tolerance_s = 0.05;
    // 实测峰值段码率相对声明 BANDWIDTH 的允许偏差（比例）
    double bandwidth_tolerance_ratio = 0.10;
    bool check_keyframe_alignment = true;
    bool check_variant_consistency = true;
    bool check_av_segment_count = true;
};

struct Scte35Options {
    // 校验 section 末尾的 CRC_32（MPEG-2 多项式）。关闭后仍会解析字段。
    bool verify_crc = true;
    // 包载荷不是从 0xFC 开始时（前面有 pointer_field / 对齐字节），
    // 是否在前 16 字节内找 table_id 0xFC。
    bool scan_for_section_start = true;
};

struct SubtitleOptions {
    // 单条流最多解析多少条 cue（长片保护；超了只统计不入库）
    uint32_t max_cues_per_stream = 5000;

    // 停留时间窗口：短于下限来不及读，长于上限通常是忘记写结束时间
    double min_cue_duration_seconds = 0.5;
    double max_cue_duration_seconds = 8.0;

    // 阅读速度上限（字符/秒）。中文建议 12-16，英文 16-21。
    double max_chars_per_second = 21.0;

    // 重叠容差：相邻 cue 首尾相接（差几毫秒）不算重叠
    double overlap_tolerance_seconds = 0.005;

    bool parse_text_cues = true;
    bool check_overlap = true;
    bool check_empty = true;
    bool check_duration = true;
    bool check_order = true;
    bool check_reading_speed = true;
    bool check_language = true;
};

struct TimecodeOptions {
    // 读 metadata 里的 timecode tag
    bool read_metadata = true;
    // 读 MOV/MP4 tmcd 时码轨的首包
    bool read_timecode_track = true;
    // 检查章节越界 / 重叠 / 倒序
    bool check_chapters = true;
    // 检查 drop-frame 与帧率是否匹配
    bool check_drop_frame = true;
    // 29.97 / 59.94 且时码本身没有 drop-frame 标记时，按 NTSC 惯例假定 drop frame
    bool assume_drop_frame_on_ntsc = true;
    // 帧率提示（0 = 从第一条视频流推断）
    double fps_hint = 0.0;
};

struct AuxDataOptions {
    // 采集 metadata（容器 / 流 / 章节）
    bool collect_metadata = true;
    // 解析 data 流里的 SCTE-35 cue
    bool analyze_scte35 = true;
    // 单条流最多记录多少条 cue（大文件直播流保护）
    uint32_t max_cues = 4096;
    uint32_t max_metadata_entries = 400;
    Scte35Options scte35_options;
};


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

    // 编码码流解析（从 AVCodecParameters::extradata 解析 SPS/VPS/Sequence Header，
    // 与容器层宽高/位深/色彩做一致性对比，见 core/analysis/codec/BitstreamAnalyzer.h）。
    // 不解码、只读 KB 级 extradata，成本可忽略；目前支持 H.264 / HEVC / AV1，
    // VVC 只识别编码类型、不产出参数（见 BitstreamAnalyzer::ParseVvc）。
    bool analyze_bitstream = true;

    // MP4/fMP4 容器一致性校验（sample table / elst / moof-traf-trun / faststart）。
    // 走自研 core/media/container/IsobmffParser.h 的 IsobmffParser 直接读 stbl 与 moof，与 FFmpeg demux 是两套独立解析：
    // 只在容器属于 MP4 家族时才真正执行，其它格式直接跳过。
    bool analyze_mp4_sample_table = true;
    Mp4SampleTableOptions mp4_sample_table_options;

    // HLS / DASH 流媒体包（manifest + segment + 多码率 ladder）。
    // 只在输入是本地 .m3u8 / .mpd 时执行，此时 QtAnalysisController 会跳过 FFmpeg demux ——
    // avformat 会把清单当播放列表去发网络请求，离线 QC 场景既不可控也无法单测。
    // 全文件扫描只负责产出 QC 结果，UI 的「流媒体包」页读的是
    // ContainerStructureResult::streaming_package（打开文件时由 ContainerStructureAnalyzer 填）。
    bool analyze_streaming_package = true;
    SegmentQcOptions streaming_package_options;

    // 字幕轨（SRT / ASS-SSA / WebVTT / tx3g 的 cue 解析与校验；图形字幕与 CEA-708
    // 第一阶段只输出流 metadata 与包时间线）。只在存在字幕流时才有工作量。
    bool analyze_subtitle = true;
    SubtitleOptions subtitle_options;

    // 时码与章节（MOV/MP4 tmcd 轨首帧时码、metadata timecode tag、章节时间线）。
    // 只读 metadata 与 tmcd 首包，几乎零成本。
    bool analyze_timecode = true;
    TimecodeOptions timecode_options;

    // 辅助数据轨（data stream 枚举 + SCTE-35 cue 解析 + metadata key-value 采集）
    bool analyze_aux_data = true;
    AuxDataOptions aux_data_options;
};

} // namespace analyzer
} // namespace videoeye
