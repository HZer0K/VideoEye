#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/LoudnessPoint.h"

namespace videoeye {
namespace model {

// 声道在混音中的角色
//
// 由调用方（AnalysisCoordinator）把 FFmpeg 的 AVChannel 位置翻译成角色后再喂给分析器，
// 分析器本身不依赖 FFmpeg。角色的唯一用途是决定 BS.1770 的声道加权：
//   Front = 1.0 / Surround = 1.41（+1.5 dB）/ LowFrequency = 0（LFE 不参与响度）
enum class AudioChannelRole {
    Unknown = 0,
    Front,         // L / R / C / Lc / Rc / 顶部前置等
    Surround,      // Ls / Rs / Lb / Rb / 顶部环绕等
    LowFrequency,  // LFE
};

// 单声道描述（名字由调用方给出，如 "FL" / "LFE" / "Ls"）
struct AudioChannelInfo {
    std::string name;
    AudioChannelRole role = AudioChannelRole::Unknown;

    AudioChannelInfo() = default;
    AudioChannelInfo(std::string n, AudioChannelRole r) : name(std::move(n)), role(r) {}
};

// 单声道统计
struct AudioChannelStat {
    int index = 0;
    std::string name;
    AudioChannelRole role = AudioChannelRole::Unknown;
    double weight = 1.0;  // BS.1770 加权系数

    double peak = 0.0;                             // 线性采样峰值
    double peak_dbfs = kSilenceLevelDb;
    double true_peak_dbtp = kSilenceLevelDb;       // 4× 过采样真峰值
    double rms_dbfs = kSilenceLevelDb;
    double dc_offset = 0.0;                         // 线性直流分量（整轨均值）
    double dc_offset_dbfs = kSilenceLevelDb;        // 以 dBFS 表示的直流偏移
    int64_t clipping_samples = 0;                   // |x| >= 削波阈值 的样本数
    int64_t sample_count = 0;
    bool silent = false;                            // 整轨静音
};

// 一段连续的削波（削波不一定连续，按"间隔小于 merge_gap 秒"归并）
struct AudioClipEvent {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    int channel = -1;              // -1 = 多声道同时削波
    int64_t sample_count = 0;      // 该事件内的削波样本数
    double peak = 0.0;             // 事件内的最大 |x|
};

// 一段静音（RMS 低于阈值且持续超过最短时长）
struct AudioSilenceRange {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    double duration_seconds = 0.0;
    double rms_dbfs = kSilenceLevelDb;   // 段内平均 RMS
};

// 元数据一致性检查（声道布局 / 采样率 / 采样格式 / 位深 / 时长）
struct AudioQcMetadata {
    int sample_rate = 0;
    int channels = 0;
    std::string channel_layout;      // 如 "立体声 (2.0)" / "5.1"
    bool layout_confirmed = false;   // true = 容器/解码器明确给出了声道位置
    std::string sample_format;       // 如 "fltp" / "s16"
    int bits_per_sample = 0;

    double stream_duration_seconds = 0.0;
    double container_duration_seconds = 0.0;
    double video_duration_seconds = 0.0;
    bool has_video = false;

    // 时长偏差（秒）：音频流 vs 容器 / 音频流 vs 视频流
    double container_delta_seconds = 0.0;
    double video_delta_seconds = 0.0;

    std::vector<std::string> inconsistencies;   // 人类可读的不一致描述
};

// 音频 QC 分析结果
//
// 分三组指标：
//   1) 电平类（第一阶段）：采样峰值 / RMS / 削波 / 静音 / 声道能量 / DC offset
//   2) 响度类（第二阶段）：Integrated / Short-term / Momentary / LRA / True Peak
//   3) 元数据类：声道布局 / 采样率 / 采样格式 / 位深 / 与视频的时长一致性
//
// 该结构不依赖 FFmpeg / Qt，可直接由合成样本单测（tests/unit/test_audio_qc_analyzer.cpp）。
struct AudioQcResult {
    bool analyzed = false;    // true = 本次扫描确实分析过音频
    bool has_audio = false;   // 文件中是否存在音频流

    AudioQcMetadata metadata;

    // ---- 电平 ----
    double sample_peak = 0.0;
    double sample_peak_dbfs = kSilenceLevelDb;
    double true_peak_dbtp = kSilenceLevelDb;
    double rms_dbfs = kSilenceLevelDb;
    double max_dc_offset = 0.0;             // 所有声道中最大的 |DC|
    double max_dc_offset_dbfs = kSilenceLevelDb;
    int64_t total_samples = 0;             // 每声道样本数
    double duration_seconds = 0.0;

    int64_t clipping_sample_count = 0;
    int64_t clipping_event_count = 0;
    std::vector<AudioClipEvent> clipping_events;

    std::vector<AudioSilenceRange> silence_ranges;
    double silence_ratio = 0.0;            // 静音时长 / 总时长
    double longest_silence_seconds = 0.0;

    std::vector<AudioChannelStat> channels;

    // ---- 响度 ----
    double integrated_lufs = kSilenceLufs;
    double short_term_max_lufs = kSilenceLufs;
    double short_term_min_lufs = kSilenceLufs;
    double momentary_max_lufs = kSilenceLufs;
    double loudness_range_lu = 0.0;        // LRA（EBU Tech 3341：P95 - P10）
    double loudness_range_low_lufs = kSilenceLufs;    // P10
    double loudness_range_high_lufs = kSilenceLufs;   // P95
    int64_t gated_block_count = 0;         // 通过绝对+相对门限的 400ms 块数
    int64_t block_count = 0;

    // ---- 声道相位 / 相关性 ----
    bool correlation_available = false;    // 单声道无相关性
    double correlation_mean = 1.0;         // 按块平均的相关性
    double correlation_min = 1.0;          // 最差块的相关性
    double out_of_phase_ratio = 0.0;       // 相关性 < 阈值 的块占比

    // ---- 曲线（100 ms 步进，超长文件按 max_loudness_points 抽稀）----
    std::vector<LoudnessPoint> loudness_points;
    bool loudness_points_decimated = false;

    // ---- 说明 ----
    std::vector<std::string> notes;        // 分析过程中的降级/提示信息

    // 导出用的单行摘要
    std::string ToString() const;
};

}  // namespace model
}  // namespace videoeye
