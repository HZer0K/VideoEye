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

#include "core/analysis/quality/AudioQcAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "core/qc/QcRuleEngine.h"

namespace {

using namespace videoeye;

constexpr int kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// 用合成 PCM 跑一遍 AudioQcAnalyzer，填出 AnalysisResult.audio_qc
model::AnalysisResult BuildResult(int channels, int frames, double amplitude, bool invert_second_channel = false,
                                  double dc = 0.0, double container_duration = 0.0) {
    std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                           std::vector<float>(static_cast<size_t>(frames), 0.0f));
    std::vector<const float*> ptrs(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        auto& plane = planes[static_cast<size_t>(c)];
        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            double v = dc + amplitude * std::sin(2.0 * kPi * 1000.0 * t);
            if (c == 1 && invert_second_channel)
                v = -(v - dc) + dc;
            plane[static_cast<size_t>(i)] = static_cast<float>(v);
        }
        ptrs[static_cast<size_t>(c)] = plane.data();
    }

    std::vector<model::AudioChannelInfo> info;
    if (channels >= 2) {
        info.emplace_back("FL", model::AudioChannelRole::Front);
        info.emplace_back("FR", model::AudioChannelRole::Front);
    }
    for (int c = 2; c < channels; ++c)
        info.emplace_back("Ch" + std::to_string(c + 1), model::AudioChannelRole::Front);

    videoeye::AudioQcAnalyzer analyzer;
    analyzer.Reset();
    analyzer.SetStreamInfo(kSampleRate, channels, "fltp", 32);
    analyzer.SetChannelInfo(info);
    if (container_duration > 0.0) {
        analyzer.SetDurations(static_cast<double>(frames) / kSampleRate, container_duration, container_duration, false);
    }
    analyzer.OnSamples(ptrs.data(), frames, 0.0);

    model::AnalysisResult result;
    result.file_path = "test.wav";
    result.container_format = "wav";
    result.duration_seconds = container_duration;
    model::StreamDigest audio_stream;
    audio_stream.index = 1;
    audio_stream.media_type = 1; // AVMEDIA_TYPE_AUDIO
    audio_stream.sample_rate = kSampleRate;
    audio_stream.channels = channels;
    result.streams.push_back(audio_stream);
    result.audio_qc = analyzer.Finish();
    return result;
}

bool HasIssue(const model::QcReport& report, const std::string& rule_id, model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id)
            continue;
        if (out)
            *out = issue;
        return true;
    }
    return false;
}

} // namespace

TEST(AudioQcRulesTest, LoudLoudnessTriggersHighTargetRule) {
    const auto result = BuildResult(1, kSampleRate * 5, 1.0); // 满刻度 -> 约 -3 LUFS
    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "audio.loudness.target_high", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    EXPECT_NEAR(issue.metric_value, -3.0, 0.3);
    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_low"));
}

TEST(AudioQcRulesTest, QuietLoudnessTriggersLowTargetRule) {
    const auto result = BuildResult(1, kSampleRate * 5, 0.0005);
    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    EXPECT_TRUE(HasIssue(report, "audio.loudness.target_low"));
    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_high"));
}

TEST(AudioQcRulesTest, TargetLoudnessPassesBothRules) {
    const auto result = BuildResult(1, kSampleRate * 5, 0.1); // 约 -23 LUFS
    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_high"));
    EXPECT_FALSE(HasIssue(report, "audio.loudness.target_low"));
}

TEST(AudioQcRulesTest, FullScaleTriggersTruePeakAndClipping) {
    std::vector<float> plane(static_cast<size_t>(kSampleRate * 3));
    for (size_t i = 0; i < plane.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double v = 1.6 * std::sin(2.0 * kPi * 440.0 * t); // 超过满刻度
        plane[i] = static_cast<float>(std::max(-1.0, std::min(1.0, v)));
    }
    const float* ptrs[1] = {plane.data()};

    videoeye::AudioQcAnalyzer analyzer;
    analyzer.Reset();
    analyzer.SetStreamInfo(kSampleRate, 1, "fltp", 32);
    analyzer.OnSamples(ptrs, static_cast<int>(plane.size()), 0.0);

    model::AnalysisResult result;
    result.file_path = "clip.wav";
    result.audio_qc = analyzer.Finish();

    videoeye::qc::QcRuleEngine engine;
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
    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    EXPECT_TRUE(HasIssue(report, "audio.silence.longest"));
    EXPECT_TRUE(HasIssue(report, "audio.silence.ratio"));
}

TEST(AudioQcRulesTest, DcOffsetAndOutOfPhaseAreReported) {
    const auto result = BuildResult(2, kSampleRate * 4, 0.1, true, 0.05);
    videoeye::qc::QcRuleEngine engine;
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
    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "audio.metadata.duration_mismatch"));
}

TEST(AudioQcRulesTest, NoFalsePositiveWithoutAudioAnalysis) {
    model::AnalysisResult result;
    result.file_path = "noaudio.mp4";
    result.duration_seconds = 10.0;
    // audio_qc 保持默认: analyzed=false
    videoeye::qc::QcRuleEngine engine;
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
    videoeye::qc::QcRuleEngine engine;
    const model::QcReport quiet_report = engine.Evaluate(quiet);
    const model::QcReport loud_report = engine.Evaluate(loud);
    EXPECT_LT(loud_report.score, quiet_report.score);
}

// ---------------------------------------------------------------------------
// 多音轨（R2）：次轨的问题必须出现在 QC 结论里，并带自己的 stream_index。
//
// 以前规则只读 result.audio_qc（主轨兼容字段），构造"主轨正常、次轨 100 个削波
// 样本"时 secondary_clipping_issues=0、verdict=通过 —— 次轨等于没检查。
// ---------------------------------------------------------------------------
TEST(AudioQcRulesTest, SecondaryTrackClippingIsReportedWithItsStreamIndex) {
    model::AnalysisResult result;
    result.file_path = "multi_audio.mkv";
    result.primary_audio_stream_index = 1;
    for (int index : {1, 2}) {
        model::StreamDigest audio;
        audio.index = index;
        audio.media_type = 1; // AVMEDIA_TYPE_AUDIO
        audio.sample_rate = kSampleRate;
        audio.channels = 2;
        result.streams.push_back(audio);
    }

    model::AudioQcResult primary;
    primary.analyzed = true;
    primary.coverage_ratio = 1.0;
    primary.completeness = model::AudioQcResult::Completeness::Complete;

    model::AudioQcResult secondary;
    secondary.analyzed = true;
    secondary.coverage_ratio = 1.0;
    secondary.completeness = model::AudioQcResult::Completeness::Complete;
    secondary.clipping_sample_count = 100;
    secondary.clipping_event_count = 1;

    result.audio_qc = primary; // 主轨兼容字段：正常
    result.audio_qc_streams = {primary, secondary};
    result.audio_qc_stream_indices = {1, 2};
    result.audio_qc_tracks_analyzed = 2;
    result.audio_qc_tracks_total = 2;

    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "audio.clipping", &issue)) << "次轨削波未进入 QC 结论";
    EXPECT_EQ(issue.stream_index, 2) << "问题必须标在真正削波的那条轨上";
    EXPECT_NE(issue.detail.find("流 #2"), std::string::npos) << "多轨时明细要说明是哪条流";
    EXPECT_GT(issue.metric_value, 90.0);
}

// ---------------------------------------------------------------------------
// 全部解码失败（R3）：analyzed=false 不是"跳过判定"的理由。
// 以前 audio.qc.incomplete 和响度规则共用 if (!qc.analyzed) return，
// 一条 PCM 都没拿到时失败被"没有足够数据判响度"顺带屏蔽了。
// ---------------------------------------------------------------------------
TEST(AudioQcRulesTest, AllDecodeFailuresTriggerIncompleteRule) {
    model::AnalysisResult result;
    result.file_path = "broken_audio.mp4";
    model::StreamDigest audio;
    audio.index = 1;
    audio.media_type = 1;
    result.streams.push_back(audio);

    result.audio_qc.analyzed = false;
    result.audio_qc.completeness = model::AudioQcResult::Completeness::Failed;
    result.audio_qc_tracks_analyzed = 0;
    result.audio_qc_tracks_total = 1;

    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "audio.qc.incomplete")) << "全部解码失败时必须报不完整，不能静默通过";
}

// ---------------------------------------------------------------------------
// 维度级结论（R3）：规则没命中 ≠ 完整通过。
// ---------------------------------------------------------------------------
TEST(AudioQcRulesTest, FailedDimensionForcesNonPassVerdict) {
    model::AnalysisResult result;
    result.file_path = "dimension_failed.wav";
    // 基线先做成"干净通过"：时长/流齐全，避免 container.duration_invalid 之类的
    // Critical 提前把结论打成"不通过"，那样这条用例就测不出维度降级了。
    result.duration_seconds = 10.0;
    result.container_format = "wav";
    model::StreamDigest video;
    video.index = 0;
    video.media_type = 0; // AVMEDIA_TYPE_VIDEO
    result.streams.push_back(video);
    model::StreamDigest audio;
    audio.index = 1;
    audio.media_type = 1; // AVMEDIA_TYPE_AUDIO
    audio.sample_rate = kSampleRate;
    audio.channels = 2;
    result.streams.push_back(audio);

    model::DimensionStatus status;
    status.dimension = model::AnalysisDimension::AudioQc;
    status.executed = true;
    status.capability = model::AnalysisCapability::Failed;
    result.SetDimension(status);

    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_EQ(report.verdict, "不通过") << "维度 Failed 时不能给出通过结论";
}

TEST(AudioQcRulesTest, PartialDimensionDowngradesPassVerdict) {
    model::AnalysisResult result;
    result.file_path = "dimension_partial.wav";
    result.duration_seconds = 10.0;
    result.container_format = "wav";
    model::StreamDigest video;
    video.index = 0;
    video.media_type = 0;
    result.streams.push_back(video);
    model::StreamDigest audio;
    audio.index = 1;
    audio.media_type = 1;
    audio.sample_rate = kSampleRate;
    audio.channels = 2;
    result.streams.push_back(audio);

    model::DimensionStatus status;
    status.dimension = model::AnalysisDimension::AudioQc;
    status.executed = true;
    status.capability = model::AnalysisCapability::Partial;
    result.SetDimension(status);

    videoeye::qc::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_EQ(report.verdict, "警告") << "维度 Partial 时不能继续给「通过」";
}
