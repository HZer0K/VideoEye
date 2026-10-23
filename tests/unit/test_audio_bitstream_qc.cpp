// ============================================================================
// 音频位流参数规则（audio.bitstream.* / audio.dialnorm.*）单元测试
//
// 这一组的价值在于"不解码也能判"：直接构造 model::AudioBitstreamResult，
// 不碰 FFmpeg、不碰 PCM。重点验证三件事：
//   1. 没有跑过位流分析时一条都不报（避免"没数据"被当成"有缺陷"）；
//   2. 阈值按结构化字段判定，不依赖 inconsistencies 里的中文串；
//   3. dialnorm 那两条的守卫：缺字段只对 AC-3 家族说，偏差必须同时有声明与实测。
// ============================================================================

#include <gtest/gtest.h>

#include <string>

#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"

namespace {

using namespace videoeye;

bool HasIssue(const model::QcReport& report, const std::string& rule_id,
              model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id) continue;
        if (out != nullptr) *out = issue;
        return true;
    }
    return false;
}

model::AudioBitstreamStream MakeStream(model::AudioBitstreamCodec codec) {
    model::AudioBitstreamStream stream;
    stream.stream_index = 1;
    stream.codec = codec;
    stream.codec_name = (codec == model::AudioBitstreamCodec::Ac3) ? "ac3" : "aac";
    stream.sample_rate = 48000;
    stream.channels = 2;
    stream.parsed_packets = 100;
    stream.frame_count = 100;
    stream.packet_count = 100;
    return stream;
}

model::AnalysisResult MakeResult() {
    model::AnalysisResult result;
    result.file_path = "bitstream.mkv";
    result.audio_bitstream_analyzed = true;
    result.audio_bitstream.analyzed = true;
    return result;
}

}  // namespace

TEST(AudioBitstreamRulesTest, RulesExistInDefaultCatalog) {
    const auto rules = model::DefaultQcRules();
    const char* ids[] = {
        "audio.bitstream.container_mismatch", "audio.bitstream.header_invalid",
        "audio.bitstream.param_change",       "audio.dialnorm.missing",
        "audio.dialnorm.mismatch",
    };
    for (const char* id : ids) {
        const model::QcRule* rule = model::FindQcRule(rules, id);
        ASSERT_TRUE(rule != nullptr) << id;
        EXPECT_TRUE(rule->enabled) << id;
        EXPECT_FALSE(rule->suggestion.empty()) << id;
    }
}

TEST(AudioBitstreamRulesTest, ContainerMismatchTriggersOnceWithFieldCount) {
    auto result = MakeResult();
    auto stream = MakeStream(model::AudioBitstreamCodec::AacAdts);
    stream.container_sample_rate = 44100;
    stream.container_channels = 6;
    stream.container_mismatch = true;
    stream.container_mismatch_fields = 2;
    stream.inconsistencies.push_back("容器声明采样率 44100 Hz，位流头是 48000 Hz");
    result.audio_bitstream.streams.push_back(stream);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "audio.bitstream.container_mismatch", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Error);
    EXPECT_NEAR(issue.metric_value, 2.0, 1e-9);
    EXPECT_NE(issue.detail.find("44100"), std::string::npos);
}

TEST(AudioBitstreamRulesTest, ConsistentStreamDoesNotTriggerMismatch) {
    auto result = MakeResult();
    auto stream = MakeStream(model::AudioBitstreamCodec::AacAdts);
    stream.container_sample_rate = 48000;
    stream.container_channels = 2;
    result.audio_bitstream.streams.push_back(stream);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(report, "audio.bitstream.container_mismatch"));
    EXPECT_FALSE(HasIssue(report, "audio.bitstream.header_invalid"));
    EXPECT_FALSE(HasIssue(report, "audio.bitstream.param_change"));
}

TEST(AudioBitstreamRulesTest, HeaderInvalidRatioUsesPercentThreshold) {
    auto result = MakeResult();
    auto stream = MakeStream(model::AudioBitstreamCodec::AacAdts);
    stream.parsed_packets = 90;
    stream.invalid_packets = 10;   // 10% > 5%
    result.audio_bitstream.streams.push_back(stream);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "audio.bitstream.header_invalid", &issue));
    EXPECT_NEAR(issue.metric_value, 10.0, 1e-6);

    // 1/101 ≈ 1%，不该报
    result.audio_bitstream.streams.front().parsed_packets = 100;
    result.audio_bitstream.streams.front().invalid_packets = 1;
    const model::QcReport quiet = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(quiet, "audio.bitstream.header_invalid"));
}

TEST(AudioBitstreamRulesTest, UnsupportedCodecIsNotCountedAsHeaderFailure) {
    // PCM / MP3 / FLAC 这类编码只登记容器参数，逐包解析没实现。
    // 这些包既不算成功也不算失败 —— 记进 invalid 会让它们变成 100% 失败率并触发告警。
    auto result = MakeResult();
    model::AudioBitstreamStream pcm;
    pcm.stream_index = 1;
    pcm.codec = model::AudioBitstreamCodec::Other;
    pcm.codec_name = "pcm_s16le";
    pcm.sample_rate = 48000;
    pcm.channels = 2;
    pcm.packet_count = 400;
    pcm.unsupported_packets = 400;   // 全部未参与判定
    pcm.note = "该编码尚未实现逐帧位流头解析，400 个包未参与帧头判定";
    result.audio_bitstream.streams.push_back(pcm);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(report, "audio.bitstream.header_invalid"))
        << "未支持解析不能报成帧头失败";

    // 同一份结果里再放一条真的坏了的 AAC：失败率只按它自己算（10/100），
    // 而且明细里要说明还有多少包没参与判定。
    auto aac = MakeStream(model::AudioBitstreamCodec::AacAdts);
    aac.parsed_packets = 90;
    aac.invalid_packets = 10;
    aac.packet_count = 100;
    result.audio_bitstream.streams.push_back(aac);

    const model::QcReport mixed = engine.Evaluate(result);
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(mixed, "audio.bitstream.header_invalid", &issue));
    EXPECT_NEAR(issue.metric_value, 10.0, 1e-6) << "分母只算参与过判定的 100 个包";
    EXPECT_NE(issue.detail.find("400"), std::string::npos)
        << "未参与判定的包数要在明细里单独说明";
}

TEST(AudioBitstreamRulesTest, ParamChangeMidstreamTriggers) {
    auto result = MakeResult();
    auto stream = MakeStream(model::AudioBitstreamCodec::Ac3);
    stream.params_changed_midstream = true;
    result.audio_bitstream.streams.push_back(stream);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "audio.bitstream.param_change"));
}

TEST(AudioBitstreamRulesTest, DialnormMissingOnlyForAc3Family) {
    auto result = MakeResult();
    auto ac3 = MakeStream(model::AudioBitstreamCodec::Ac3);
    ac3.has_dialnorm = false;
    result.audio_bitstream.streams.push_back(ac3);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "audio.dialnorm.missing"));

    // AAC 本来就没有 dialnorm 字段：口径一致的话不该被算作缺失
    auto aac_result = MakeResult();
    auto aac = MakeStream(model::AudioBitstreamCodec::AacAdts);
    aac.has_dialnorm = false;
    aac_result.audio_bitstream.streams.push_back(aac);
    const model::QcReport aac_report = engine.Evaluate(aac_result);
    EXPECT_FALSE(HasIssue(aac_report, "audio.dialnorm.missing"));
}

TEST(AudioBitstreamRulesTest, DialnormMismatchNeedsMeasuredLoudness) {
    auto result = MakeResult();
    auto stream = MakeStream(model::AudioBitstreamCodec::Ac3);
    stream.has_dialnorm = true;
    stream.dialnorm_dbfs = -27;
    stream.dialnorm_delta_available = true;
    stream.dialnorm_delta_lu = 7.0;
    result.audio_bitstream.streams.push_back(stream);

    // 没有实测响度（audio_qc 未跑）时不得判定："声明 vs 实测"缺一半
    QcRuleEngine engine;
    const model::QcReport no_pcm = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(no_pcm, "audio.dialnorm.mismatch"));

    result.audio_qc.analyzed = true;
    result.audio_qc.integrated_lufs = -20.0;
    const model::QcReport measured = engine.Evaluate(result);
    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(measured, "audio.dialnorm.mismatch", &issue));
    EXPECT_NEAR(issue.metric_value, 7.0, 1e-6);
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);

    // 偏差 1 LU 属于正常范围
    result.audio_bitstream.streams.front().dialnorm_delta_lu = 1.0;
    const model::QcReport small = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(small, "audio.dialnorm.mismatch"));
}

TEST(AudioBitstreamRulesTest, NoBitstreamAnalysisMeansNoIssues) {
    model::AnalysisResult result;
    result.file_path = "no_bitstream.mkv";
    result.audio_bitstream_analyzed = false;
    // 即使 streams 里塞了"有问题"的数据，没跑过就不该报
    auto stream = MakeStream(model::AudioBitstreamCodec::Ac3);
    stream.container_mismatch = true;
    stream.container_mismatch_fields = 2;
    stream.params_changed_midstream = true;
    stream.has_dialnorm = false;
    result.audio_bitstream.streams.push_back(stream);

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(report, "audio.bitstream.container_mismatch"));
    EXPECT_FALSE(HasIssue(report, "audio.bitstream.param_change"));
    EXPECT_FALSE(HasIssue(report, "audio.dialnorm.missing"));
}
