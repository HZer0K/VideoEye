// 「证据与覆盖」状态模型（阶段 5）的正反例回归测试。
//
// 这套结构的唯一目的是**消灭假通过**：报告里"未发现问题"必须能被读者回溯成
// "跑了、覆盖够了、无失败"，而不是"根本没跑"。所以测试重点全在
// MarkConclusive() 的口径边界上 —— 这里是唯一能产生"假通过"的地方。

#include <gtest/gtest.h>

#include <string>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DimensionStatus.h"

namespace {

using videoeye::model::AnalysisCapability;
using videoeye::model::AnalysisDimension;
using videoeye::model::AnalysisResult;
using videoeye::model::DimensionStatus;
using videoeye::model::MakeFailedDimension;
using videoeye::model::MakeSkippedDimension;

// 一个"什么都对"的基线：执行过、能力完整、无失败、全覆盖
DimensionStatus MakeHealthy() {
    DimensionStatus status;
    status.dimension = AnalysisDimension::BitrateGop;
    status.executed = true;
    status.capability = AnalysisCapability::Supported;
    status.scope_unit = "帧";
    status.units_scanned = 1000;
    status.units_total = 1000;
    status.has_coverage = true;
    status.coverage_ratio = 1.0;
    return status;
}

} // namespace

// ---------------------------------------------------------------------------
// 正例：全条件满足才允许 conclusive
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, HealthyDimensionIsConclusive) {
    DimensionStatus status = MakeHealthy();
    status.MarkConclusive();
    EXPECT_TRUE(status.conclusive);
}

// 总量未知（裸流算不出总帧数）但扫到了内容：不能因为没有覆盖率就判"结论不可用"，
// 否则所有裸流分析都会被标成不可信。口径是"有覆盖信息时才检查覆盖率"。
TEST(DimensionStatusTest, UnknownTotalDoesNotBlockConclusive) {
    DimensionStatus status = MakeHealthy();
    status.has_coverage = false;
    status.coverage_ratio = 0.0;
    status.units_total = 0;
    status.MarkConclusive();
    EXPECT_TRUE(status.conclusive);
}

// ---------------------------------------------------------------------------
// 反例：四种"看起来跑过了"其实不能当通过用的情形
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, NotExecutedIsNeverConclusive) {
    DimensionStatus status = MakeHealthy();
    status.executed = false;
    status.MarkConclusive();
    EXPECT_FALSE(status.conclusive) << "没跑过的维度绝不能算通过";
}

TEST(DimensionStatusTest, PartialCapabilityIsNotConclusive) {
    DimensionStatus status = MakeHealthy();
    status.capability = AnalysisCapability::Partial;
    status.MarkConclusive();
    EXPECT_FALSE(status.conclusive) << "部分实现不能给出可用的通过结论";
}

TEST(DimensionStatusTest, FailedCapabilityIsNotConclusive) {
    DimensionStatus status = MakeHealthy();
    status.capability = AnalysisCapability::Failed;
    status.MarkConclusive();
    EXPECT_FALSE(status.conclusive);
}

TEST(DimensionStatusTest, FailureCountBlocksConclusiveEvenWhenSupported) {
    DimensionStatus status = MakeHealthy();
    status.capability = AnalysisCapability::Supported;
    status.failures = 1;
    status.MarkConclusive();
    EXPECT_FALSE(status.conclusive) << "有失败计数就不能算通过，哪怕能力评级是 Supported";
}

TEST(DimensionStatusTest, FailureReasonBlocksConclusive) {
    DimensionStatus status = MakeHealthy();
    status.failure_reason = "部分样本未参与统计";
    status.MarkConclusive();
    EXPECT_FALSE(status.conclusive);
}

// 覆盖率边界：99% 是阈值（与音频 QC 的 completeness 口径一致）。
// 98.9% 不算通过，99% 算 —— 这个边界值必须有测试盯着，否则口径会被悄悄改掉。
TEST(DimensionStatusTest, CoverageBelowThresholdBlocksConclusive) {
    DimensionStatus status = MakeHealthy();
    status.coverage_ratio = 0.989;
    status.MarkConclusive();
    EXPECT_FALSE(status.conclusive);
}

TEST(DimensionStatusTest, CoverageAtThresholdIsConclusive) {
    DimensionStatus status = MakeHealthy();
    status.coverage_ratio = 0.99;
    status.MarkConclusive();
    EXPECT_TRUE(status.conclusive);
}

// ---------------------------------------------------------------------------
// CoveragePercent：无覆盖信息必须返回 -1，不能用 0 冒充
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, CoveragePercentWithoutCoverageReturnsMinusOne) {
    DimensionStatus status = MakeHealthy();
    status.has_coverage = false;
    EXPECT_LT(status.CoveragePercent(), 0.0) << "「总量未知」不能报成「覆盖 0%」";
}

TEST(DimensionStatusTest, CoveragePercentIsPercentage) {
    DimensionStatus status = MakeHealthy();
    status.coverage_ratio = 0.42;
    EXPECT_DOUBLE_EQ(status.CoveragePercent(), 42.0);
}

// ---------------------------------------------------------------------------
// 便捷构造：跳过 / 失败 两条路径的默认值必须"安全"
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, MakeSkippedCarriesReasonAndIsNotConclusive) {
    const DimensionStatus status = MakeSkippedDimension(AnalysisDimension::TsTransport, "不是 MPEG-TS 容器");
    EXPECT_FALSE(status.executed);
    EXPECT_EQ(status.capability, AnalysisCapability::Unavailable);
    EXPECT_EQ(status.failure_reason, "不是 MPEG-TS 容器");
    EXPECT_FALSE(status.conclusive);
    // Summary 必须把原因写出来，否则 UI/报告里只剩一个「未执行」
    EXPECT_NE(status.Summary().find("不是 MPEG-TS 容器"), std::string::npos);
}

TEST(DimensionStatusTest, MakeFailedCarriesCountsAndIsNotConclusive) {
    const DimensionStatus status = MakeFailedDimension(AnalysisDimension::AudioQc, "全部音轨都未能解出 PCM", 3);
    EXPECT_TRUE(status.executed);
    EXPECT_EQ(status.capability, AnalysisCapability::Failed);
    EXPECT_EQ(status.failures, 3);
    EXPECT_FALSE(status.conclusive);
}

// ---------------------------------------------------------------------------
// AnalysisResult::DimensionExecuted：消费方唯一的"跑没跑"口径
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, DimensionExecutedReadsOnlyTheTable) {
    AnalysisResult result;
    EXPECT_FALSE(result.DimensionExecuted(AnalysisDimension::Subtitle)) << "没有该维度记录 = 没跑过";

    DimensionStatus status;
    status.dimension = AnalysisDimension::Subtitle;
    status.executed = true;
    result.SetDimension(status);
    EXPECT_TRUE(result.DimensionExecuted(AnalysisDimension::Subtitle));
    EXPECT_FALSE(result.DimensionExecuted(AnalysisDimension::Timecode)) << "不能串到别的维度";
}

// 生产阶段的 *_analyzed 布尔不是消费口径：只有 Assembler 收尾把它们回填之后才对得上。
// 这条用例把"分离"钉住，防止以后又有人手写 result.xxx_analyzed 去做判断。
TEST(DimensionStatusTest, DimensionExecutedIgnoresProducerFlagsBeforeAssembly) {
    AnalysisResult result;
    result.subtitle_analyzed = true;
    EXPECT_FALSE(result.DimensionExecuted(AnalysisDimension::Subtitle));
}

// ---------------------------------------------------------------------------
// 枚举码稳定性：JSON 的 dimension 字段就是它，改名等于破坏 schema
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, DimensionCodesAreStable) {
    EXPECT_STREQ(videoeye::model::DimensionCode(AnalysisDimension::FileProbe), "file_probe");
    EXPECT_STREQ(videoeye::model::DimensionCode(AnalysisDimension::ParameterTimeline), "parameter_timeline");
    EXPECT_STREQ(videoeye::model::DimensionCode(AnalysisDimension::AudioQc), "audio_qc");
    EXPECT_STREQ(videoeye::model::DimensionCode(AnalysisDimension::Streaming), "streaming");
    EXPECT_STREQ(videoeye::model::DimensionCode(AnalysisDimension::Count), "unknown");
    // 每个维度都必须有中文名，UI/报告直接展示
    for (int i = 0; i < static_cast<int>(AnalysisDimension::Count); ++i) {
        const auto dimension = static_cast<AnalysisDimension>(i);
        EXPECT_STRNE(videoeye::model::DimensionName(dimension), "未知维度") << "维度索引 " << i << " 缺少中文名";
    }
}

// ---------------------------------------------------------------------------
// Summary 的可读性：报告/UI 直接用这一行，缺字段就会读成另一种意思
// ---------------------------------------------------------------------------
TEST(DimensionStatusTest, SummaryMentionsCoverageAndLimitation) {
    DimensionStatus status = MakeHealthy();
    status.units_scanned = 500;
    status.units_total = 1000;
    status.coverage_ratio = 0.5;
    status.capability = AnalysisCapability::Partial;
    status.failure_reason = "已到扫描窗口上限";
    status.MarkConclusive();
    const std::string summary = status.Summary();
    EXPECT_NE(summary.find("500/1000"), std::string::npos) << summary;
    EXPECT_NE(summary.find("50.0%"), std::string::npos) << summary;
    EXPECT_NE(summary.find("已到扫描窗口上限"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("结论可用"), std::string::npos) << "不可用时不能出现「结论可用」";
}
