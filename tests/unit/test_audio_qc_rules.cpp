// ============================================================================
// 音频 QC 规则（AudioQcAnalyzer -> AnalysisResult -> QcRuleEngine）单元测试
//
// 关注"合格判定"这一层：
//   - 响度偏离目标 -> 输出 warning
//   - 真峰值/削波 -> 输出 error
//   - 静音 / DC / 反相 / metadata 不一致 -> 各自命中对应规则
//   - 未跑过音频 QC（analyzed=false）时不得误报
// ============================================================================

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/analyzer/AudioQcAnalyzer.h"
#include "core/analyzer/AnalysisTask.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/model/QcReport.h"

namespace {

using namespace videoeye;

constexpr int kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// 用合成 PCM 跑一遍 AudioQcAnalyzer，填出 AnalysisResult.audio_qc
analyzer::AnalysisResult BuildResult(int channels, int frames, double amplitude,
                                     bool invert_second_channel = false, double dc = 0.0,
                                     double container_duration = 0.0) {
    std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                           std::vector<float>(static_cast<size_t>(frames), 0.0f));
    std::vector<const float*> ptrs(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        auto& plane = planes[static_cast<size_t>(c)];
        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            double v = dc + amplitude * std::sin(2.0 * kPi * 1000.0 * t);
            if (c == 1 && invert_second_channel) v = -(v - dc) + dc;
            plane[static_cast<size_t>(i)] = static_cast<float>(v);
        }
        ptrs[static_cast<size_t>(c)] = plane.data();
    }

    std::vector<model::AudioChannelInfo> info;
    if (channels >= 2) {
        info.emplace_back("FL", model::AudioChannelRole::Front);
        info.emplace_back("FR", model::AudioChannelRole::Front);
    }
    for (int c = 2; c < channels; ++c) info.emplace_back("Ch" + std::to_string(c + 1),
                                                         model::AudioChannelRole::Front);

    analyzer::AudioQcAnalyzer analyzer;
    analyzer.Reset();
    analyzer.SetStreamInfo(kSampleRate, channels, "fltp", 32);
    analyzer.SetChannelInfo(info);
    if (container_duration > 0.0) {
        analyzer.SetDurations(static_cast<double>(frames) / kSampleRate, container_duration,
                              container_duration, false);
    }
    analyzer.OnSamples(ptrs.data(), frames, 0.0);

    analyzer::AnalysisResult result;
    result.file_path = "test.wav";
    result.container_format = "wav";
    result.duration_seconds = container_duration;
    analyzer::StreamDigest audio_stream;
    audio_stream.index = 1;
    audio_stream.media_type = 1;   // AVMEDIA_TYPE_AUDIO
    audio_stream.sample_rate = kSampleRate;
    audio_stream.channels = channels;
    result.streams.push_back(audio_stream);
    result.audio_qc = analyzer.Finish();
    return result;
}

bool HasIssue(const model::QcReport& report, const std::string& rule_id,
              model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id) continue;
        if (out) *out = issue;
        return true;
    }
    return false;
}

}  // namespace

TEST(AudioQcRulesTest, LoudLoudnessTriggersHighTargetRule) {
    const auto result = BuildResult(1, kSampleRate * 5, 1.0);   // 满刻度 -> 约 -3 LUFS
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "audio.loudness.target_high", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    EXPECT_NEAR(issue.metric_value, -3.0, 0.3);
    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_low"));
}

TEST(AudioQcRulesTest, QuietLoudnessTriggersLowTargetRule) {
    const auto result = BuildResult(1, kSampleRate * 5, 0.0005);
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    EXPECT_TRUE(HasIssue(report, "audio.loudness.target_low"));
    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_high"));
}

TEST(AudioQcRulesTest, TargetLoudnessPassesBothRules) {
    const auto result = BuildResult(1, kSampleRate * 5, 0.1);   // 约 -23 LUFS
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_high"));
    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_low"));
}

TEST(AudioQcRulesTest, FullScaleTriggersTruePeakAndClipping) {
    std::vector<float> plane(static_cast<size_t>(kSampleRate * 3));
    for (size_t i = 0; i < plane.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double v = 1.6 * std::sin(2.0 * kPi * 440.0 * t);   // 超过满刻度
        plane[i] = static_cast<float>(std::max(-1.0, std::min(1.0, v)));
    }
    const float* ptrs[1] = {plane.data()};

    analyzer::AudioQcAnalyzer analyzer;
    analyzer.Reset();
    analyzer.SetStreamInfo(kSampleRate, 1, "fltp", 32);
    analyzer.OnSamples(ptrs, static_cast<int>(plane.size()), 0.0);

    analyzer::AnalysisResult result;
    result.file_path = "clip.wav";
    result.audio_qc = analyzer.Finish();

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue tp;
    ASSERT_TRUE(HasIssue(report, "audio.true_peak", &tp));
    EXPECT_EQ(tp.severity, model::IssueSeverity::Error);
    EXPECT_GT(tp.metric_value, -1.0);

    model::DiagnosticIssue clip;
    ASSERT_TRUE(HasIssue(report, "audio.clipping", &clip));
    EXPECT_EQ(clip.severity, model::IssueSeverity::Error);
    EXPECT_GT(clip.occurrence_count, 0);
}

TEST(AudioQcRulesTest, DigitalSilenceTriggersSilenceRules) {
    // 全静音 15 秒：最长静音 15s 超过 10s 阈值 -> 命中 longest；占比 100% 超过 50% -> 命中 ratio
    const auto result = BuildResult(2, kSampleRate * 15, 0.0);
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    EXPECT_TRUE(HasIssue(report, "audio.silence.longest"));
    EXPECT_TRUE(HasIssue(report, "audio.silence.ratio"));
}

TEST(AudioQcRulesTest, DcOffsetAndOutOfPhaseAreReported) {
    const auto result = BuildResult(2, kSampleRate * 4, 0.1, true, 0.05);
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue dc;
    ASSERT_TRUE(HasIssue(report, "audio.dc_offset", &dc));
    EXPECT_NEAR(dc.metric_value, 0.05, 0.01);

    model::DiagnosticIssue phase;
    ASSERT_TRUE(HasIssue(report, "audio.phase_correlation", &phase));
    EXPECT_LT(phase.metric_value, -0.9);
}

TEST(AudioQcRulesTest, DurationMismatchIsReported) {
    const auto result = BuildResult(2, kSampleRate * 4, 0.1, false, 0.0, 30.0);
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "audio.metadata.duration_mismatch"));
}

TEST(AudioQcRulesTest, NoFalsePositiveWithoutAudioAnalysis) {
    analyzer::AnalysisResult result;
    result.file_path = "noaudio.mp4";
    result.duration_seconds = 10.0;
    // audio_qc 保持默认: analyzed=false
    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    for (const auto& issue : report.issues) {
        EXPECT_NE(issue.rule_id, "audio.loudness.target_high");
        EXPECT_NE(issue.rule_id, "audio.true_peak");
        EXPECT_NE(issue.rule_id, "audio.clipping");
        EXPECT_NE(issue.rule_id, "audio.dc_offset");
    }
}

TEST(AudioQcRulesTest, AudioIssuesLowerTheScore) {
    const auto quiet = BuildResult(1, kSampleRate * 5, 0.1);
    const auto loud = BuildResult(1, kSampleRate * 5, 1.0);
    analyzer::QcRuleEngine engine;
    const model::QcReport quiet_report = engine.Evaluate(quiet);
    const model::QcReport loud_report = engine.Evaluate(loud);
    EXPECT_LT(loud_report.score, quiet_report.score);
}
