// ============================================================================
// 视觉画质 QC 规则单元测试（只依赖 domain + 规则层，不 include 分析器/Qt/FFmpeg）
//
// 立这条门的原因：`visual.defects` 一开始是 NonZero（任何缺陷都命中），而
// `Letterbox` / `Pillarbox` 的严重度是 Info、静音时段的冻结也会降级为 Info。
// 结果就是每部宽银幕影片都会凭空多一条 Warning，而且与非缺陷的
// "没有黑边" 无法区分。这里钉住三件事：
//   1) Info 级缺陷不参与任何 visual.* 规则；
//   2) Warning 级只命中 `visual.defects`，不命中 `visual.defects_severe`；
//   3) Error 级两条规则都命中；
//   4) 维度没执行（默认关闭）时，即使结果里残留缺陷也不得凭空报。
// ============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DimensionStatus.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/VisualDefect.h"
#include "core/qc/QcRuleEngine.h"

namespace {

using namespace videoeye;

model::AnalysisResult MakeResult(bool visual_executed) {
    model::AnalysisResult result;
    result.file_path = "test.mp4";
    result.container_format = "mov,mp4,m4a,3gp,3g2,mj2";
    result.duration_seconds = 60.0;

    model::StreamDigest video;
    video.index = 0;
    video.media_type = 0; // AVMEDIA_TYPE_VIDEO
    video.codec_name = "h264";
    video.width = 1920;
    video.height = 1080;
    result.streams.push_back(video);
    result.primary_video_stream_index = 0;

    result.visual_qc.analyzed = true;
    result.visual_qc.analyzed_frames = 1500;
    result.visual_qc.total_frames = 1500;

    if (visual_executed) {
        model::DimensionStatus status;
        status.dimension = model::AnalysisDimension::VisualQuality;
        status.executed = true;
        status.capability = model::AnalysisCapability::Supported;
        status.scope_unit = "帧";
        status.units_scanned = 1500;
        status.units_total = 1500;
        status.has_coverage = true;
        status.coverage_ratio = 1.0;
        result.dimensions.push_back(status);
    }
    return result;
}

model::VisualDefect MakeDefect(model::VisualDefectType type, model::VisualDefectSeverity severity, double start,
                               double end, const std::string& description = "test") {
    model::VisualDefect defect;
    defect.type = type;
    defect.severity = severity;
    defect.start_seconds = start;
    defect.end_seconds = end;
    defect.description = description;
    return defect;
}

bool HasIssue(const model::QcReport& report, const std::string& rule_id, model::DiagnosticIssue* out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id)
            continue;
        if (out != nullptr)
            *out = issue;
        return true;
    }
    return false;
}

} // namespace

// 宽银幕影片必然带上下黑边（Info），不能因此命中"存在画面缺陷"。
TEST(VisualQcRules, InfoOnlyDefectsDoNotTriggerAnyRule) {
    model::AnalysisResult result = MakeResult(true);
    result.visual_qc.defects.push_back(
        MakeDefect(model::VisualDefectType::Letterbox, model::VisualDefectSeverity::Info, 0.0, 60.0, "上下黑边 12%"));
    result.visual_qc.defects.push_back(
        MakeDefect(model::VisualDefectType::Pillarbox, model::VisualDefectSeverity::Info, 0.0, 60.0, "左右黑边 5%"));

    const model::QcReport report = qc::QcRuleEngine().Evaluate(result);
    EXPECT_FALSE(HasIssue(report, "visual.defects"));
    EXPECT_FALSE(HasIssue(report, "visual.defects_severe"));
}

TEST(VisualQcRules, WarningDefectTriggersOnlyTheNormalRule) {
    model::AnalysisResult result = MakeResult(true);
    result.visual_qc.defects.push_back(
        MakeDefect(model::VisualDefectType::BlackFrame, model::VisualDefectSeverity::Warning, 10.0, 10.4));

    const model::QcReport report = qc::QcRuleEngine().Evaluate(result);

    model::DiagnosticIssue issue;
    ASSERT_TRUE(HasIssue(report, "visual.defects", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    EXPECT_EQ(issue.stream_index, 0);
    // 展开的条目要带上缺陷的时间范围，UI 才能直接跳转
    EXPECT_NEAR(issue.range.start_seconds, 10.0, 1e-6);
    EXPECT_NEAR(issue.range.end_seconds, 10.4, 1e-6);

    EXPECT_FALSE(HasIssue(report, "visual.defects_severe"));
}

TEST(VisualQcRules, SevereDefectTriggersBothRules) {
    model::AnalysisResult result = MakeResult(true);
    result.visual_qc.defects.push_back(
        MakeDefect(model::VisualDefectType::FreezeFrame, model::VisualDefectSeverity::Error, 30.0, 35.0));

    const model::QcReport report = qc::QcRuleEngine().Evaluate(result);
    EXPECT_TRUE(HasIssue(report, "visual.defects"));
    EXPECT_TRUE(HasIssue(report, "visual.defects_severe"));
}

// 默认关闭时维度没执行：残留的缺陷（比如调用方手工塞进去的）不能被读成"检出问题"。
TEST(VisualQcRules, NotExecutedDimensionProducesNoIssues) {
    model::AnalysisResult result = MakeResult(false);
    result.visual_qc.defects.push_back(
        MakeDefect(model::VisualDefectType::BlackFrame, model::VisualDefectSeverity::Critical, 0.0, 1.0));

    const model::QcReport report = qc::QcRuleEngine().Evaluate(result);
    EXPECT_FALSE(HasIssue(report, "visual.defects"));
    EXPECT_FALSE(HasIssue(report, "visual.defects_severe"));
}
