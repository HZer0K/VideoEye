// 流媒体包 QC 规则（container.hls.* / container.dash.* / container.streaming.*）单元测试
//
// 与分析器测试的分工：
//   test_streaming_package.cpp 测"分析器有没有发现问题"；
//   这一份测"规则引擎有没有把发现转成报告、级别对不对"——
//   也就是 QcRuleEngine::CheckRule 里那三个 rfind 前缀分支。

#include <string>

#include <gtest/gtest.h>

#include "core/analyzer/AnalysisTask.h"
#include "core/analyzer/DashManifestAnalyzer.h"
#include "core/analyzer/HlsManifestAnalyzer.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/model/QcReport.h"

namespace {

using namespace videoeye;

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

// 超过 EXT-X-TARGETDURATION 的 media playlist
const char* kOverTargetPlaylist = "#EXTM3U\n"
                                  "#EXT-X-TARGETDURATION:4\n"
                                  "#EXTINF:4.000,\n"
                                  "seg0.ts\n"
                                  "#EXTINF:6.500,\n"
                                  "seg1.ts\n"
                                  "#EXT-X-ENDLIST\n";

// SegmentTimeline 有 4 秒缺口的 MPD
const char* kGapMpd = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                      "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                      "mediaPresentationDuration=\"PT16S\">\n"
                      "  <Period id=\"0\" duration=\"PT16S\">\n"
                      "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
                      "      <Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
                      "codecs=\"avc1.64001f\">\n"
                      "        <SegmentTemplate timescale=\"1000\" media=\"v0/seg-$Number$.m4s\">\n"
                      "          <SegmentTimeline>\n"
                      "            <S t=\"0\" d=\"4000\" r=\"1\"/>\n"
                      "            <S t=\"12000\" d=\"4000\"/>\n"
                      "          </SegmentTimeline>\n"
                      "        </SegmentTemplate>\n"
                      "      </Representation>\n"
                      "    </AdaptationSet>\n"
                      "  </Period>\n"
                      "</MPD>\n";

} // namespace

// ---------------- HLS: 分片时长超过目标时长 -> Warning ----------------

TEST(StreamingQcRulesTest, HlsSegmentOverTargetBecomesWarningIssue) {
    analyzer::AnalysisResult result;
    result.file_path = "over.m3u8";
    result.file_extension = "m3u8";
    result.container_format = "hls";
    result.duration_seconds = 12.0;

    analyzer::HlsManifestAnalyzer hls;
    model::StreamingPackageResult& pkg = result.streaming_package;
    pkg.manifest_path = "over.m3u8";
    ASSERT_TRUE(hls.ParseText(kOverTargetPlaylist, ".", pkg));
    hls.Validate(pkg);
    result.streaming_analyzed = true;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, model::StreamingIssueCode::kHlsSegmentOverTarget, &issue));
    EXPECT_EQ(model::IssueSeverity::Warning, issue.severity);
    EXPECT_EQ("分片时长超过目标时长", issue.title);
    EXPECT_DOUBLE_EQ(6.5, issue.metric_value);
    EXPECT_FALSE(issue.IsGlobal()); // 定位到出问题的那个分片
}

// ---------------- DASH: SegmentTimeline 缺口 -> Error ----------------

TEST(StreamingQcRulesTest, DashSegmentTimelineGapBecomesErrorIssue) {
    analyzer::AnalysisResult result;
    result.file_path = "gap.mpd";
    result.file_extension = "mpd";
    result.container_format = "dash";
    result.duration_seconds = 16.0;

    analyzer::DashManifestAnalyzer dash;
    model::StreamingPackageResult& pkg = result.streaming_package;
    ASSERT_TRUE(dash.ParseText(kGapMpd, ".", pkg));
    dash.Validate(pkg);
    result.streaming_analyzed = true;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, model::StreamingIssueCode::kDashSegmentTimelineGap, &issue));
    EXPECT_EQ(model::IssueSeverity::Error, issue.severity);
    EXPECT_EQ("SegmentTimeline 存在时间缺口", issue.title);
    EXPECT_DOUBLE_EQ(4.0, issue.metric_value);
}

// ---------------- 没跑过流媒体分析时不应有流媒体问题 ----------------

TEST(StreamingQcRulesTest, NoStreamingIssuesWhenNotAnalyzed) {
    analyzer::AnalysisResult result;
    result.file_path = "over.m3u8";
    result.file_extension = "m3u8";
    result.container_format = "hls";

    analyzer::HlsManifestAnalyzer hls;
    model::StreamingPackageResult& pkg = result.streaming_package;
    pkg.manifest_path = "over.m3u8";
    ASSERT_TRUE(hls.ParseText(kOverTargetPlaylist, ".", pkg));
    hls.Validate(pkg);
    // 故意不置 streaming_analyzed：规则引擎必须整体跳过这一族规则
    result.streaming_analyzed = false;

    analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(report, model::StreamingIssueCode::kHlsSegmentOverTarget));
}

// ---------------- 规则可关：enabled=false 时不上报 ----------------

TEST(StreamingQcRulesTest, DisabledRuleSuppressesStreamingIssue) {
    analyzer::AnalysisResult result;
    result.file_path = "over.m3u8";
    result.file_extension = "m3u8";
    result.container_format = "hls";

    analyzer::HlsManifestAnalyzer hls;
    model::StreamingPackageResult& pkg = result.streaming_package;
    pkg.manifest_path = "over.m3u8";
    ASSERT_TRUE(hls.ParseText(kOverTargetPlaylist, ".", pkg));
    hls.Validate(pkg);
    result.streaming_analyzed = true;

    analyzer::QcRuleEngine engine;
    std::vector<model::QcRule> rules = engine.rules();
    model::QcRule* rule = model::FindQcRule(rules, model::StreamingIssueCode::kHlsSegmentOverTarget);
    ASSERT_NE(nullptr, rule);
    rule->enabled = false;
    engine.SetRules(rules);

    const model::QcReport report = engine.Evaluate(result);
    EXPECT_FALSE(HasIssue(report, model::StreamingIssueCode::kHlsSegmentOverTarget));
}

// ---------------- 规则表里确实注册了这些规则 ----------------

TEST(StreamingQcRulesTest, DefaultRulesContainStreamingFamily) {
    std::vector<model::QcRule> rules = model::DefaultQcRules();
    EXPECT_NE(nullptr, model::FindQcRule(rules, model::StreamingIssueCode::kHlsSegmentOverTarget));
    EXPECT_NE(nullptr, model::FindQcRule(rules, model::StreamingIssueCode::kHlsVariantKeyframeMisalign));
    EXPECT_NE(nullptr, model::FindQcRule(rules, model::StreamingIssueCode::kDashSegmentTimelineGap));
    EXPECT_NE(nullptr, model::FindQcRule(rules, model::StreamingIssueCode::kSegmentMissingFile));
}
