#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/AudioQcResult.h"
#include "core/model/LoudnessPoint.h"

namespace videoeye {
namespace analyzer {

// 音频 QC 分析配置
//
// 说明：这里只放"检测参数"（窗口、门限、阈值），不放"合格判定阈值"——
// 后者属于 QC 规则（core/model/QcModels.cpp 的 audio.* 规则），用户可在「规则与阈值」页改。
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

// 二阶 IIR（transposed direct form II，状态用 double 保精度）
// 放在类外是为了让 K 加权系数的构造函数可以访问它
struct BiquadFilter {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void Reset() { z1 = 0.0; z2 = 0.0; }
    float Process(float x) {
        const double y = b0 * static_cast<double>(x) + z1;
        z1 = b1 * static_cast<double>(x) - a1 * y + z2;
        z2 = b2 * static_cast<double>(x) - a2 * y;
        return static_cast<float>(y);
    }
};

// 音频 QC 分析器
//
// 职责:
//   1) 接收解码后的 float planar PCM（调用方负责 decode + resample/format 转换）
//   2) 第一阶段指标: 峰值 / RMS / 削波 / 静音段 / 声道能量 / DC offset
//   3) 第二阶段指标: BS.1770-4 K 加权响度（M / S / I / LRA）与 4× 过采样真峰值
//   4) 声道相关性（Pearson）与反相检测
//   5) 元数据一致性: 声道布局 / 采样率 / 采样格式 / 位深 / 与视频时长差
//
// 刻意不依赖 FFmpeg / Qt: 只吃 float planar + 采样率 + 声道信息，
// 这样单测可以直接喂合成数据跑验收（见 tests/unit/test_audio_qc_analyzer.cpp）。
//
// 时间轴口径: 400 ms 块 + 75% 重叠（每 100 ms 出一个点），与 libebur128 / FFmpeg ebur128 一致。
class AudioQcAnalyzer {
public:
    void Reset(const AudioQcOptions& options = AudioQcOptions{});
    void SetOptions(const AudioQcOptions& options);
    const AudioQcOptions& options() const { return options_; }

    // ---- 元数据 ----
    // 必须在首次 OnSamples 之前调用（块长依赖采样率）
    void SetStreamInfo(int sample_rate, int channels,
                       const std::string& sample_format = std::string(),
                       int bits_per_sample = 0);
    void SetChannelInfo(const std::vector<model::AudioChannelInfo>& channels);
    void SetDurations(double stream_seconds, double container_seconds, double video_seconds,
                      bool has_video);

    // ---- 数据输入 ----
    // planar float，每个声道一段连续内存；frames = 每声道样本数。
    // timestamp_seconds < 0 表示"沿用内部时钟"（多帧连续喂时只需首帧给时间）。
    void OnSamples(const float* const* planar, int frames, double timestamp_seconds = -1.0);
    // interleaved float 便捷入口（内部转为 planar 处理）
    void OnSamplesInterleaved(const float* data, int frames, double timestamp_seconds = -1.0);

    // ---- 结果 ----
    const model::AudioQcResult& Finish();
    const model::AudioQcResult& result() const { return result_; }
    bool finished() const { return finished_; }

    // ---- 静态工具 ----
    // 由声道数与角色推断布局名（"单声道 (1.0)" / "立体声 (2.0)" / "5.1" ...）
    static std::string DescribeLayout(int channels,
                                      const std::vector<model::AudioChannelRole>& roles);
    // BS.1770 声道加权: 前置 1.0 / 环绕 1.41 / LFE 0
    static double ChannelWeight(model::AudioChannelRole role);

private:
    struct ChannelState {
        BiquadFilter shelf;   // K 加权 stage 1: 高频搁架
        BiquadFilter rlb;     // K 加权 stage 2: RLB 高通
        // 环形缓冲: raw 用于真峰值/相关性/RMS, kweighted 用于响度
        std::vector<float> raw_ring;
        std::vector<float> kw_ring;
        // 整轨累计
        double sum = 0.0;         // DC 累计
        double sum_sq = 0.0;      // RMS 累计
        double peak = 0.0;
        int64_t samples = 0;
        int64_t clip_samples = 0;
        // 削波段归并状态
        bool clip_open = false;
        double clip_start = 0.0;
        double clip_last = 0.0;
        int64_t clip_count = 0;
        double clip_peak = 0.0;
    };

    void EnsureReady();
    void PrepareFilters();
    void PushSample(int ch, float sample, double seconds);
    void ProcessBlock(double end_seconds);
    void CloseClipEvent(int ch, double seconds);
    void UpdateSilence(const model::LoudnessPoint& point);
    void DecimatePointsIfNeeded();
    void FinalizeIntegrated();
    void FinalizeLoudnessRange();
    void FinalizeMetadata();
    void AddNote(const std::string& note);

    // 真峰值: 对 raw_ring 中"最近一个块"做 4× 过采样取最大 |y|
    double ComputeTruePeak(int ch) const;

    AudioQcOptions options_;
    model::AudioQcResult result_;
    std::vector<ChannelState> channels_;
    std::vector<model::AudioChannelInfo> channel_info_;

    // 4× 过采样 FIR（零值内插 + Hann 窗 sinc，与 libebur128 同一设计）
    std::vector<double> interp_coeffs_;
    std::vector<double> interp_phase_[4];   // 多相分解: phase p 的抽头 h[4m+p]
    double interp_abs_sum_ = 4.0;           // Σ|h|，用于跳过"不可能刷新真峰值"的块
    int interp_margin_ = 0;                 // 每个相位需要的历史样本数

    int sample_rate_ = 0;
    int channel_count_ = 0;
    int block_samples_ = 0;    // 400 ms
    int hop_samples_ = 0;      // 100 ms
    int ring_size_ = 0;        // block + 真峰值所需的历史余量
    int ring_write_ = 0;       // 环形写指针
    int64_t sample_index_ = 0; // 每声道已写入的样本数
    double clock_seconds_ = 0.0;

    // 400 ms 块功率（用于积分响度 + 短期响度窗口）
    std::vector<double> block_powers_;      // 全部块（积分门限用）
    std::vector<double> short_term_window_; // 最近 30 个块（3 s）
    size_t short_term_pos_ = 0;
    double short_term_sum_ = 0.0;
    std::vector<double> short_term_values_; // 短期响度序列（LRA 用）

    double running_sample_peak_ = 0.0;
    double running_true_peak_ = 0.0;
    double correlation_sum_ = 0.0;
    double correlation_min_ = 1.0;
    int64_t correlation_blocks_ = 0;
    int64_t out_of_phase_blocks_ = 0;

    // 曲线累计量（不受抽稀影响）
    int64_t blocks_emitted_ = 0;
    int point_stride_ = 1;
    double momentary_max_ = model::kSilenceLufs;
    double short_term_max_ = model::kSilenceLufs;
    double short_term_min_ = model::kSilenceLufs;

    // 积分响度的增量近似累加器（精确值在 Finish() 里做两遍门限）
    double abs_gated_sum_ = 0.0;
    int64_t abs_gated_count_ = 0;
    double rel_gated_sum_ = 0.0;
    int64_t rel_gated_count_ = 0;

    // 真峰值插值用的线性暂存区
    std::vector<float> scratch_;

    // 静音段归并状态
    bool silence_open_ = false;
    double silence_start_ = 0.0;
    double silence_acc_dbfs_ = 0.0;
    int silence_acc_blocks_ = 0;

    bool stream_info_set_ = false;
    bool filters_ready_ = false;
    bool finished_ = false;
};

}  // namespace analyzer
}  // namespace videoeye
