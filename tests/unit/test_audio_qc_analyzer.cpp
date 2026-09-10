// ============================================================================
// AudioQcAnalyzer 单元测试
//
// 该模块不依赖 FFmpeg / Qt，因此可以直接喂合成 PCM 跑验收：
//   - 全静音文件 -> 完整静音段
//   - 人工削波样本 -> 输出 clipping
//   - 单声道 / 立体声 / 5.1 -> 正确识别布局与加权
//   - 已知电平的正弦 -> Integrated LUFS / True Peak 落在理论值附近
//   - L = -R -> 相关性 -1（反相）
//
// 参考值说明（48 kHz，K 加权在 1 kHz 约为 +0.70 dB）：
//   满刻度 1 kHz 正弦:  -0.691 + 10log10(1/2) + 0.70 ≈ -3.00 LUFS
//   峰值 0.1 (-20 dBFS): ≈ -23.0 LUFS
// ============================================================================

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "core/analyzer/AudioQcAnalyzer.h"

namespace {

using videoeye::analyzer::AudioQcAnalyzer;
using videoeye::analyzer::AudioQcOptions;
using videoeye::model::AudioChannelInfo;
using videoeye::model::AudioChannelRole;
using videoeye::model::AudioQcResult;
using videoeye::model::kSilenceLevelDb;
using videoeye::model::kSilenceLufs;

constexpr int kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;  // MSVC 默认不导出 M_PI

// 生成 seconds 秒的正弦（幅度 amplitude），写入 planar 缓冲
struct PlanarAudio {
    int channels = 1;
    std::vector<std::vector<float>> planes;
    std::vector<const float*> ptrs;

    PlanarAudio(int ch, int frames) : channels(ch), planes(static_cast<size_t>(ch)) {
        for (auto& p : planes) p.assign(static_cast<size_t>(frames), 0.0f);
        ptrs.resize(static_cast<size_t>(ch));
        SyncPtrs();
    }
    void SyncPtrs() {
        for (int c = 0; c < channels; ++c) ptrs[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
    }
    int Frames() const { return static_cast<int>(planes[0].size()); }
};

void FillSine(PlanarAudio& audio, double frequency_hz, double amplitude, int sample_rate,
              double phase = 0.0) {
    for (int c = 0; c < audio.channels; ++c) {
        auto& plane = audio.planes[static_cast<size_t>(c)];
        for (size_t i = 0; i < plane.size(); ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
            plane[i] = static_cast<float>(amplitude *
                                         std::sin(2.0 * kPi * frequency_hz * t + phase));
        }
    }
}

AudioQcResult Analyze(PlanarAudio& audio, int sample_rate,
                      const std::vector<AudioChannelInfo>& info = {},
                      const AudioQcOptions& options = AudioQcOptions{}) {
    AudioQcAnalyzer analyzer;
    analyzer.Reset(options);
    analyzer.SetStreamInfo(sample_rate, audio.channels, "fltp", 32);
    if (!info.empty()) analyzer.SetChannelInfo(info);
    analyzer.OnSamples(audio.ptrs.data(), audio.Frames(), 0.0);
    return analyzer.Finish();
}

}  // namespace

// ---------------------------------------------------------------- 静音

TEST(AudioQcAnalyzerTest, DigitalSilenceProducesFullSilenceRange) {
    PlanarAudio audio(2, kSampleRate * 5);   // 5 s 立体声全零
    const AudioQcResult result = Analyze(audio, kSampleRate);

    ASSERT_TRUE(result.analyzed);
    EXPECT_EQ(result.silence_ranges.size(), 1u);
    if (!result.silence_ranges.empty()) {
        EXPECT_NEAR(result.silence_ranges[0].start_seconds, 0.0, 0.2);
        EXPECT_NEAR(result.silence_ranges[0].duration_seconds, 5.0, 0.3);
    }
    EXPECT_NEAR(result.silence_ratio, 1.0, 0.05);
    EXPECT_NEAR(result.duration_seconds, 5.0, 0.1);
    EXPECT_EQ(result.sample_peak_dbfs, kSilenceLevelDb);
    EXPECT_EQ(result.integrated_lufs, kSilenceLufs);
    EXPECT_FALSE(result.notes.empty());
}

TEST(AudioQcAnalyzerTest, SilenceGapInTheMiddleIsDetected) {
    PlanarAudio audio(2, kSampleRate * 7);
    // 前 2 s 与后 2 s 有信号，中间 3 s 静音
    FillSine(audio, 1000.0, 0.1, kSampleRate);
    for (int c = 0; c < audio.channels; ++c) {
        auto& plane = audio.planes[static_cast<size_t>(c)];
        for (int i = kSampleRate * 2; i < kSampleRate * 5; ++i) plane[static_cast<size_t>(i)] = 0.0f;
    }
    const AudioQcResult result = Analyze(audio, kSampleRate);

    ASSERT_EQ(result.silence_ranges.size(), 1u);
    EXPECT_NEAR(result.silence_ranges[0].start_seconds, 2.0, 0.2);
    EXPECT_NEAR(result.silence_ranges[0].duration_seconds, 3.0, 0.3);
    EXPECT_LT(result.silence_ratio, 0.6);
}

// ---------------------------------------------------------------- 削波

TEST(AudioQcAnalyzerTest, ClippedSignalIsDetected) {
    PlanarAudio audio(1, kSampleRate * 2);
    auto& plane = audio.planes[0];
    for (size_t i = 0; i < plane.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double v = 1.6 * std::sin(2.0 * kPi * 440.0 * t);   // 超过满刻度
        plane[i] = static_cast<float>(std::max(-1.0, std::min(1.0, v)));
    }
    const AudioQcResult result = Analyze(audio, kSampleRate);

    EXPECT_GT(result.clipping_sample_count, 0);
    EXPECT_GT(result.clipping_event_count, 0u);
    EXPECT_FALSE(result.clipping_events.empty());
    EXPECT_NEAR(result.sample_peak_dbfs, 0.0, 0.01);
    EXPECT_GT(result.true_peak_dbtp, -0.2);   // 已削到 0 dBFS，真峰值不会低于采样峰值
}

TEST(AudioQcAnalyzerTest, CleanSignalHasNoClipping) {
    PlanarAudio audio(1, kSampleRate * 2);
    FillSine(audio, 440.0, 0.5, kSampleRate);
    const AudioQcResult result = Analyze(audio, kSampleRate);
    EXPECT_EQ(result.clipping_sample_count, 0);
    EXPECT_TRUE(result.clipping_events.empty());
}

// ---------------------------------------------------------------- 声道布局

TEST(AudioQcAnalyzerTest, ChannelLayoutsAreRecognized) {
    EXPECT_EQ(AudioQcAnalyzer::DescribeLayout(1, {}), "单声道 (1.0)");
    EXPECT_EQ(AudioQcAnalyzer::DescribeLayout(2, {}), "立体声 (2.0)");

    const std::vector<AudioChannelInfo> stereo = {
        AudioChannelInfo("FL", AudioChannelRole::Front),
        AudioChannelInfo("FR", AudioChannelRole::Front)};
    AudioQcAnalyzer a2;
    a2.Reset();
    a2.SetStreamInfo(kSampleRate, 2, "fltp", 32);
    a2.SetChannelInfo(stereo);
    EXPECT_EQ(a2.result().metadata.channel_layout, "立体声 (2.0)");
    EXPECT_TRUE(a2.result().metadata.layout_confirmed);

    const std::vector<AudioChannelInfo> surround = {
        AudioChannelInfo("FL", AudioChannelRole::Front),
        AudioChannelInfo("FR", AudioChannelRole::Front),
        AudioChannelInfo("C", AudioChannelRole::Front),
        AudioChannelInfo("LFE", AudioChannelRole::LowFrequency),
        AudioChannelInfo("Ls", AudioChannelRole::Surround),
        AudioChannelInfo("Rs", AudioChannelRole::Surround)};
    AudioQcAnalyzer a6;
    a6.Reset();
    a6.SetStreamInfo(kSampleRate, 6, "fltp", 32);
    a6.SetChannelInfo(surround);
    EXPECT_EQ(a6.result().metadata.channel_layout, "5.1 (6ch)");
    ASSERT_EQ(a6.result().channels.size(), 6u);
    EXPECT_DOUBLE_EQ(a6.result().channels[0].weight, 1.0);
    EXPECT_DOUBLE_EQ(a6.result().channels[3].weight, 0.0);      // LFE 不参与响度
    EXPECT_DOUBLE_EQ(a6.result().channels[4].weight, 1.41);     // 环绕 +1.5 dB
}

TEST(AudioQcAnalyzerTest, MultiChannelWithoutLayoutIsFlagged) {
    PlanarAudio audio(6, kSampleRate);
    FillSine(audio, 1000.0, 0.1, kSampleRate);
    const AudioQcResult result = Analyze(audio, kSampleRate);   // 不传声道信息
    EXPECT_FALSE(result.metadata.layout_confirmed);
    EXPECT_FALSE(result.metadata.inconsistencies.empty());
}

// ---------------------------------------------------------------- 响度

TEST(AudioQcAnalyzerTest, FullScaleSineMatchesTheory) {
    PlanarAudio audio(1, kSampleRate * 6);
    FillSine(audio, 1000.0, 1.0, kSampleRate);
    const AudioQcResult result = Analyze(audio, kSampleRate);

    EXPECT_NEAR(result.sample_peak_dbfs, 0.0, 0.05);
    EXPECT_NEAR(result.integrated_lufs, -3.0, 0.2);
    EXPECT_NEAR(result.momentary_max_lufs, -3.0, 0.3);
    EXPECT_NEAR(result.short_term_max_lufs, -3.0, 0.3);
    EXPECT_LT(result.loudness_range_lu, 1.0);      // 稳态信号 LRA 应接近 0
    EXPECT_GT(result.true_peak_dbtp, result.sample_peak_dbfs - 0.2);
    EXPECT_LE(result.true_peak_dbtp, result.sample_peak_dbfs + 0.5);
}

TEST(AudioQcAnalyzerTest, TargetLoudnessSineMeasuresMinus23) {
    PlanarAudio audio(1, kSampleRate * 8);
    FillSine(audio, 1000.0, 0.1, kSampleRate);   // 单声道 -20 dBFS 峰值 -> 约 -23 LUFS
    const AudioQcResult mono = Analyze(audio, kSampleRate);

    EXPECT_NEAR(mono.sample_peak_dbfs, -20.0, 0.1);
    EXPECT_NEAR(mono.integrated_lufs, -23.0, 0.4);

    // 同样的信号铺成两声道，BS.1770 按功率求和 -> 比单声道高 3 dB
    PlanarAudio stereo(2, kSampleRate * 8);
    FillSine(stereo, 1000.0, 0.1, kSampleRate);
    const AudioQcResult both = Analyze(stereo, kSampleRate);
    EXPECT_NEAR(both.integrated_lufs, mono.integrated_lufs + 3.01, 0.2);
}

TEST(AudioQcAnalyzerTest, LoudnessRangeReflectsLevelChange) {
    // 前 6 s 轻声、后 6 s 大声 -> LRA 应接近 20 LU
    PlanarAudio audio(1, kSampleRate * 12);
    auto& plane = audio.planes[0];
    for (size_t i = 0; i < plane.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double amp = (t < 6.0) ? 0.01 : 0.1;
        plane[i] = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * t));
    }
    const AudioQcResult result = Analyze(audio, kSampleRate);

    EXPECT_NEAR(result.loudness_range_lu, 20.0, 3.0);
    EXPECT_GT(result.momentary_max_lufs, result.integrated_lufs);
}

// ---------------------------------------------------------------- 相位 / DC

TEST(AudioQcAnalyzerTest, InPhaseStereoHasCorrelationOne) {
    PlanarAudio audio(2, kSampleRate * 3);
    FillSine(audio, 1000.0, 0.5, kSampleRate);   // 两声道完全相同
    const AudioQcResult result = Analyze(audio, kSampleRate);

    ASSERT_TRUE(result.correlation_available);
    EXPECT_NEAR(result.correlation_min, 1.0, 0.01);
    EXPECT_NEAR(result.out_of_phase_ratio, 0.0, 0.01);
}

TEST(AudioQcAnalyzerTest, OutOfPhaseStereoIsDetected) {
    PlanarAudio audio(2, kSampleRate * 3);
    FillSine(audio, 1000.0, 0.5, kSampleRate);
    // 右声道取反 -> 相关性 -1
    for (auto& v : audio.planes[1]) v = -v;

    const AudioQcResult result = Analyze(audio, kSampleRate);
    ASSERT_TRUE(result.correlation_available);
    EXPECT_NEAR(result.correlation_min, -1.0, 0.01);
    EXPECT_GT(result.out_of_phase_ratio, 0.8);
}

TEST(AudioQcAnalyzerTest, MonoHasNoCorrelation) {
    PlanarAudio audio(1, kSampleRate * 2);
    FillSine(audio, 1000.0, 0.5, kSampleRate);
    const AudioQcResult result = Analyze(audio, kSampleRate);
    EXPECT_FALSE(result.correlation_available);
}

TEST(AudioQcAnalyzerTest, DcOffsetIsMeasured) {
    PlanarAudio audio(1, kSampleRate * 2);
    auto& plane = audio.planes[0];
    for (size_t i = 0; i < plane.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        plane[i] = static_cast<float>(0.05 + 0.1 * std::sin(2.0 * kPi * 1000.0 * t));
    }
    const AudioQcResult result = Analyze(audio, kSampleRate);

    ASSERT_FALSE(result.channels.empty());
    EXPECT_NEAR(result.channels[0].dc_offset, 0.05, 0.005);
    EXPECT_NEAR(result.max_dc_offset, 0.05, 0.005);
}

// ---------------------------------------------------------------- 元数据

TEST(AudioQcAnalyzerTest, DurationMismatchIsReported) {
    PlanarAudio audio(2, kSampleRate * 4);
    FillSine(audio, 1000.0, 0.1, kSampleRate);

    AudioQcAnalyzer analyzer;
    analyzer.Reset();
    analyzer.SetStreamInfo(kSampleRate, 2, "fltp", 32);
    analyzer.SetDurations(4.0, 10.0, 4.0, true);   // 音频 4 s，容器 10 s
    analyzer.OnSamples(audio.ptrs.data(), audio.Frames(), 0.0);
    const AudioQcResult result = analyzer.Finish();

    EXPECT_FALSE(result.metadata.inconsistencies.empty());
    EXPECT_NEAR(result.metadata.container_delta_seconds, -6.0, 0.1);
    EXPECT_EQ(result.metadata.channels, 2);
    EXPECT_EQ(result.metadata.sample_rate, kSampleRate);
    EXPECT_EQ(result.metadata.bits_per_sample, 32);
}

TEST(AudioQcAnalyzerTest, LoudnessCurveIsPopulated) {
    PlanarAudio audio(2, kSampleRate * 4);
    FillSine(audio, 1000.0, 0.1, kSampleRate);
    const AudioQcResult result = Analyze(audio, kSampleRate);

    // 4 s -> 前 3 s 无短期响度，之后每 100 ms 一个点
    EXPECT_GT(result.loudness_points.size(), 5u);
    EXPECT_FALSE(result.loudness_points.empty());
    double last_ts = -1.0;
    for (const auto& p : result.loudness_points) {
        EXPECT_GE(p.timestamp_seconds, last_ts);   // 单调递增
        last_ts = p.timestamp_seconds;
    }
}

TEST(AudioQcAnalyzerTest, InterleavedInputMatchesPlanar) {
    PlanarAudio audio(2, kSampleRate * 2);
    FillSine(audio, 1000.0, 0.2, kSampleRate);

    std::vector<float> interleaved(static_cast<size_t>(audio.Frames() * 2));
    for (int i = 0; i < audio.Frames(); ++i) {
        interleaved[static_cast<size_t>(i * 2 + 0)] = audio.planes[0][static_cast<size_t>(i)];
        interleaved[static_cast<size_t>(i * 2 + 1)] = audio.planes[1][static_cast<size_t>(i)];
    }

    AudioQcAnalyzer analyzer;
    analyzer.Reset();
    analyzer.SetStreamInfo(kSampleRate, 2, "fltp", 32);
    analyzer.OnSamplesInterleaved(interleaved.data(), audio.Frames(), 0.0);
    const AudioQcResult& interleaved_result = analyzer.Finish();

    const AudioQcResult planar_result = Analyze(audio, kSampleRate);
    EXPECT_NEAR(interleaved_result.integrated_lufs, planar_result.integrated_lufs, 0.01);
    EXPECT_NEAR(interleaved_result.rms_dbfs, planar_result.rms_dbfs, 0.01);
}
