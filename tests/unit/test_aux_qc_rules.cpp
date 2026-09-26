// 功能 9 的规则侧测试：字幕 / 时码 / 章节 / SCTE-35（rule id 前缀 subtitle. / timecode. / chapter. / scte35.）
//
// 与 test_subtitle_timecode_aux.cpp 的分工：
//   那一份测"分析器有没有把问题找出来"（Parse/Validate 层）；
//   这一份测"规则引擎有没有把问题转成报告条目、级别对不对"——
//   也就是 QcRuleEngine::CheckRule 里新增的四个分支。
// 之所以单独测：规则表一旦被人改了严重度或 id，分析器测试全绿也发现不了。

#include <string>

#include <gtest/gtest.h>

#include "core/analyzer/AnalysisTask.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/model/QcReport.h"
#include "core/model/QcRule.h"

namespace {

using namespace videoeye;

bool HasIssue(const model::QcReport& report, const std::string& rule_id,
              model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id == rule_id) {
            if (out != nullptr) *out = issue;
            return true;
        }
    }
    return false;
}

// 造一个"字幕分析过、带指定问题"的最小 AnalysisResult
analyzer::AnalysisResult MakeSubtitleResult(model::SubtitleIssueType type,
                                            model::IssueSeverity severity, int count = 1) {
    analyzer::AnalysisResult result;
    result.subtitle_analyzed = true;

    model::SubtitleStreamInfo stream;
    stream.stream_index = 2;
    stream.format = model::SubtitleFormat::Srt;
    stream.kind = model::SubtitleKind::Text;
    stream.language = "chi";
    stream.cue_count = count;
    result.subtitle.streams.push_back(stream);

    for (int i = 0; i < count; ++i) {
        model::SubtitleIssue issue;
        issue.type = type;
        issue.severity = severity;
        issue.stream_index = 2;
        issue.cue_index = i;
        issue.start_seconds = 1.0 + static_cast<double>(i);
        issue.end_seconds = 2.0 + static_cast<double>(i);
        issue.detail = "测试用问题 " + std::to_string(i + 1);
        result.subtitle.issues.push_back(issue);
    }
    return result;
}

}  // namespace

TEST(AuxQcRulesTest, DefaultRulesCoverFeatureNine) {
    const std::vector<model::QcRule> rules = model::DefaultQcRules();
    const char* kRequired[] = {
        "subtitle.cue_empty", "subtitle.cue_overlap", "subtitle.cue_order",
        "subtitle.cue_too_short", "subtitle.cue_too_long", "subtitle.missing_language",
        "timecode.missing", "timecode.drop_frame_mismatch", "timecode.invalid_frame",
        "chapter.overlap", "chapter.out_of_range",
        "scte35.parse_error", "scte35.duration_missing",
    };
    for (const char* id : kRequired) {
        EXPECT_NE(model::FindQcRule(rules, id), nullptr) << "缺少规则: " << id;
    }
}

TEST(AuxQcRulesTest, SubtitleOverlapIsReportedAsWarning) {
    const analyzer::AnalysisResult result =
        MakeSubtitleResult(model::SubtitleIssueType::Overlap, model::IssueSeverity::Warning, 2);

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "subtitle.cue_overlap", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    EXPECT_EQ(issue.category, model::IssueCategory::Metadata);
    EXPECT_EQ(issue.occurrence_count, 2);
    EXPECT_EQ(issue.stream_index, 2);
    EXPECT_FALSE(issue.range.IsGlobal());   // 定位到第一条问题 cue 的时间
}

TEST(AuxQcRulesTest, SubtitleEmptyCueIsReportedAsWarning) {
    const analyzer::AnalysisResult result =
        MakeSubtitleResult(model::SubtitleIssueType::EmptyText, model::IssueSeverity::Warning, 3);

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "subtitle.cue_empty", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    EXPECT_EQ(issue.occurrence_count, 3);
    // 只勾了 EmptyText，别的字幕规则不该跟着报
    EXPECT_FALSE(HasIssue(report, "subtitle.cue_overlap"));
    EXPECT_FALSE(HasIssue(report, "subtitle.cue_order"));
}

TEST(AuxQcRulesTest, SubtitleOrderIsErrorNotWarning) {
    const analyzer::AnalysisResult result =
        MakeSubtitleResult(model::SubtitleIssueType::NonMonotonic, model::IssueSeverity::Error);

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "subtitle.cue_order", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Error);
}

TEST(AuxQcRulesTest, SubtitleRulesStaySilentWhenNotAnalyzed) {
    const analyzer::AnalysisResult result =
        MakeSubtitleResult(model::SubtitleIssueType::Overlap, model::IssueSeverity::Warning);
    analyzer::AnalysisResult skipped = result;
    skipped.subtitle_analyzed = false;   // 没跑过字幕分析（如没有字幕流）

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(skipped);
    EXPECT_FALSE(HasIssue(report, "subtitle.cue_overlap"));
}

TEST(AuxQcRulesTest, MissingTimecodeIsReported) {
    analyzer::AnalysisResult result;
    result.timecode_analyzed = true;     // 跑过但没找到时码
    result.timecode.has_primary = false;

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "timecode.missing"));

    // 找到了时码就不再报
    analyzer::AnalysisResult with_tc = result;
    with_tc.timecode.has_primary = true;
    with_tc.timecode.primary = model::TimecodeFromString("00:59:58:00");
    with_tc.timecode.primary_frame_rate = 25.0;
    EXPECT_FALSE(HasIssue(engine.Evaluate(with_tc), "timecode.missing"));
}

TEST(AuxQcRulesTest, DropFrameMismatchOnNtscFootage) {
    analyzer::AnalysisResult result;
    result.timecode_analyzed = true;
    result.timecode.has_primary = true;
    result.timecode.primary = model::TimecodeFromString("00:00:00:00");   // non-drop 写法
    result.timecode.primary_drop_frame = false;
    result.timecode.primary_frame_rate = 30000.0 / 1001.0;                // 29.97

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "timecode.drop_frame_mismatch"));

    // 25 fps + non-drop 是匹配的，不该报
    analyzer::AnalysisResult pal = result;
    pal.timecode.primary_frame_rate = 25.0;
    EXPECT_FALSE(HasIssue(engine.Evaluate(pal), "timecode.drop_frame_mismatch"));
}

TEST(AuxQcRulesTest, ChapterOverlapIsReported) {
    analyzer::AnalysisResult result;
    result.timecode_analyzed = true;
    result.timecode.media_duration_seconds = 60.0;

    model::ChapterIssue issue;
    issue.type = model::ChapterIssueType::Overlap;
    issue.severity = model::IssueSeverity::Warning;
    issue.chapter_index = 1;
    issue.start_seconds = 9.0;
    issue.end_seconds = 20.0;
    issue.detail = "章节 1 起点早于上一章节终点";
    result.timecode.chapter_issues.push_back(issue);

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);

    model::DiagnosticIssue reported;
    ASSERT_TRUE(HasIssue(report, "chapter.overlap", &reported));
    EXPECT_EQ(reported.severity, model::IssueSeverity::Warning);
    EXPECT_FALSE(reported.range.IsGlobal());
}

TEST(AuxQcRulesTest, Scte35MissingDurationIsReported) {
    analyzer::AnalysisResult result;
    result.aux_data_analyzed = true;

    model::Scte35Cue cue;
    cue.valid = true;
    cue.command = model::Scte35Command::SpliceInsert;
    cue.has_splice_time = true;
    cue.splice_time_seconds = 30.0;
    cue.has_duration = false;            // 没给 duration
    result.aux_data.cues.push_back(cue);
    result.aux_data.scte35_cue_count = 1;

    const analyzer::QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "scte35.duration_missing"));

    // 带上 duration 就不该报
    analyzer::AnalysisResult with_duration = result;
    with_duration.aux_data.cues[0].has_duration = true;
    EXPECT_FALSE(HasIssue(engine.Evaluate(with_duration), "scte35.duration_missing"));
}
