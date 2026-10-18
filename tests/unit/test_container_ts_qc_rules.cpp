// MPEG-TS 传输层 QC 规则（container.ts.*）单元测试
//
// 与分析器测试的分工：
//   test_container_structure_analyzers.cpp 测「TS 解析器算出来的传输层数字对不对」；
//   这一份测「规则引擎有没有把这些数字转成报告、级别与阈值对不对」。
//
// 样本一律手工构造 AnalysisResult::ts_transport —— 规则只认这份摘要，不读文件，
// 因此这里不需要任何真实 TS 资产。

#include <gtest/gtest.h>

#include <string>
#include <vector>

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

// container.ts.* 这一族有没有冒出任何问题
bool HasAnyTsIssue(const model::QcReport& report) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id.rfind("container.ts.", 0) == 0) return true;
    }
    return false;
}

// 一条"什么都没坏"的传输流：1 节目 / 3 条流 / 有 PAT+PMT+SDT / PCR 40 ms / 全文件扫完
model::TransportStreamInfo HealthyTs() {
    model::TransportStreamInfo ts;
    ts.analyzed = true;
    ts.packet_size = 188;
    ts.scanned_packets = 200000;
    ts.estimated_packets = 200000;
    ts.scan_complete = true;
    ts.programs = 1;
    ts.streams = 3;
    ts.services = 1;
    ts.has_pat = true;
    ts.has_pmt = true;
    ts.has_sdt = true;
    ts.pcr_max_gap_ms = 40.0;
    return ts;
}

model::AnalysisResult MakeResult(const model::TransportStreamInfo& ts) {
    model::AnalysisResult result;
    result.file_path = "clip.ts";
    result.file_extension = "ts";
    result.container_format = "mpegts";
    result.duration_seconds = 60.0;
    result.ts_transport = ts;
    return result;
}

}  // namespace

// ---------------- 健康流：一条都不该报 ----------------

TEST(TsQcRulesTest, HealthyTransportProducesNoIssues) {
    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(HealthyTs()));
    EXPECT_FALSE(HasAnyTsIssue(report)) << "一条干净的传输流不该出现在问题表里";
}

// ---------------- 节目表缺失 ----------------

TEST(TsQcRulesTest, MissingPatBecomesCritical) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.has_pat = false;
    ts.programs = 0;
    ts.has_pmt = false;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    const model::QcReport report = engine.Evaluate(MakeResult(ts));
    ASSERT_TRUE(HasIssue(report, "container.ts.missing_pat", &issue));
    // 没有 PAT 就无法定节目，多数播放器直接判无效流 —— 必须是最高级别
    EXPECT_EQ(issue.severity, model::IssueSeverity::Critical);
    EXPECT_TRUE(issue.IsGlobal());
}

TEST(TsQcRulesTest, MissingPmtOnlyFiresWhenPatExists) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.has_pmt = false;

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(ts));
    // PAT 在、PMT 缺：报"流表不完整"
    EXPECT_TRUE(HasIssue(report, "container.ts.missing_pmt"));
    // 但不该同时报"缺 PAT"
    EXPECT_FALSE(HasIssue(report, "container.ts.missing_pat"));

    // PAT 本身就没有时，"缺 PMT"是冗余信息 —— 由缺 PAT 那条负责
    ts.has_pat = false;
    const model::QcReport report2 = engine.Evaluate(MakeResult(ts));
    EXPECT_FALSE(HasIssue(report2, "container.ts.missing_pmt"));
}

// ---------------- 传输损伤 ----------------

TEST(TsQcRulesTest, SyncLossBecomesError) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.sync_loss = 3;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    const model::QcReport report = engine.Evaluate(MakeResult(ts));
    ASSERT_TRUE(HasIssue(report, "container.ts.sync_loss", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Error);
    EXPECT_DOUBLE_EQ(issue.metric_value, 3.0);
}

TEST(TsQcRulesTest, ContinuityErrorUsesRatioNotAbsoluteCount) {
    QcRuleEngine engine;

    // 20 万包里错 1 个 = 0.0005%，低于 0.01% 阈值 —— 长文件的偶发丢包不该报警
    model::TransportStreamInfo low = HealthyTs();
    low.continuity_errors = 1;
    const model::QcReport low_report = engine.Evaluate(MakeResult(low));
    EXPECT_FALSE(HasIssue(low_report, "container.ts.continuity_error"));

    // 20 万包里错 100 个 = 0.05%，超过阈值 —— 持续丢包要报
    model::TransportStreamInfo high = HealthyTs();
    high.continuity_errors = 100;
    model::DiagnosticIssue issue;
    const model::QcReport high_report = engine.Evaluate(MakeResult(high));
    ASSERT_TRUE(HasIssue(high_report, "container.ts.continuity_error", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    // 度量值必须是百分比（0.05），不是绝对条数（100）—— 否则阈值单位就说不通了
    EXPECT_NEAR(issue.metric_value, 0.05, 1e-9);
}

TEST(TsQcRulesTest, PcrGapAboveThresholdFires) {
    QcRuleEngine engine;

    model::TransportStreamInfo good = HealthyTs();  // 40 ms
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(good)), "container.ts.pcr_gap"));

    model::TransportStreamInfo bad = HealthyTs();
    bad.pcr_max_gap_ms = 250.0;
    model::DiagnosticIssue issue;
    const model::QcReport report = engine.Evaluate(MakeResult(bad));
    ASSERT_TRUE(HasIssue(report, "container.ts.pcr_gap", &issue));
    EXPECT_DOUBLE_EQ(issue.metric_value, 250.0);
}

TEST(TsQcRulesTest, NoPcrDataDoesNotFabricateIssue) {
    // 没采到 PCR（纯音频流 / 加扰 / 无 PCR PID）时 pcr_max_gap_ms 为 0：
    // 不能拿 0 去和阈值比就说"间隔良好"，也不能报成异常 —— 应当什么都不报。
    model::TransportStreamInfo ts = HealthyTs();
    ts.pcr_max_gap_ms = 0.0;

    QcRuleEngine engine;
    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(ts)), "container.ts.pcr_gap"));
}

TEST(TsQcRulesTest, ScrambledIsInfoNotError) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.scrambled_packets = 195000;  // 几乎整条流都加扰

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    const model::QcReport report = engine.Evaluate(MakeResult(ts));
    ASSERT_TRUE(HasIssue(report, "container.ts.scrambled", &issue));
    // 加扰是"看不进去"，不是"流坏了" —— 报 Info，别把加密内容判成缺陷
    EXPECT_EQ(issue.severity, model::IssueSeverity::Info);
}

// ---------------- 扫描范围 ----------------

TEST(TsQcRulesTest, IncompleteScanIsReportedAsInfo) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.scanned_packets = 200000;
    ts.estimated_packets = 2700000;  // 5 GB 左右的 TS，只扫了前 200k 包
    ts.scan_complete = false;

    QcRuleEngine engine;
    model::DiagnosticIssue issue;
    const model::QcReport report = engine.Evaluate(MakeResult(ts));
    ASSERT_TRUE(HasIssue(report, "container.ts.scan_incomplete", &issue));
    EXPECT_EQ(issue.severity, model::IssueSeverity::Info);
    // 说明里必须带出"扫了多少 / 共多少"，否则用户会把窗口结论当整条流结论
    EXPECT_NE(issue.detail.find("200000"), std::string::npos);
    EXPECT_NE(issue.detail.find("2700000"), std::string::npos);
}

// ---------------- 没跑过 / 规则可关 ----------------

TEST(TsQcRulesTest, NotAnalyzedSkipsWholeFamily) {
    // 字段故意全填成"坏"的值，但 analyzed=false：拿不到证据就不该出现在问题表里
    model::TransportStreamInfo ts;
    ts.analyzed = false;
    ts.has_pat = false;
    ts.has_pmt = false;
    ts.sync_loss = 9;
    ts.continuity_errors = 9999;
    ts.pcr_max_gap_ms = 10000.0;
    ts.scrambled_packets = 123456;
    ts.scan_complete = false;

    QcRuleEngine engine;
    const model::QcReport report = engine.Evaluate(MakeResult(ts));
    EXPECT_FALSE(HasAnyTsIssue(report)) << "没跑过传输层体检时，整族规则都必须静默";
}

TEST(TsQcRulesTest, DisabledRuleSuppressesTsIssue) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.sync_loss = 5;

    QcRuleEngine engine;
    std::vector<model::QcRule> rules = engine.rules();
    model::QcRule* rule = model::FindQcRule(rules, "container.ts.sync_loss");
    ASSERT_TRUE(rule != nullptr);
    rule->enabled = false;
    engine.SetRules(rules);

    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(ts)), "container.ts.sync_loss"));
}

TEST(TsQcRulesTest, UserRaisedThresholdIsRespected) {
    model::TransportStreamInfo ts = HealthyTs();
    ts.pcr_max_gap_ms = 250.0;

    QcRuleEngine engine;
    std::vector<model::QcRule> rules = engine.rules();
    model::QcRule* rule = model::FindQcRule(rules, "container.ts.pcr_gap");
    ASSERT_TRUE(rule != nullptr);
    rule->threshold = 500.0;  // 用户把门槛放宽到 500 ms
    engine.SetRules(rules);

    EXPECT_FALSE(HasIssue(engine.Evaluate(MakeResult(ts)), "container.ts.pcr_gap"))
        << "250 ms 在 500 ms 门槛下不该报";
}

// ---------------- 规则表注册 ----------------

TEST(TsQcRulesTest, DefaultRulesContainTsFamily) {
    std::vector<model::QcRule> rules = model::DefaultQcRules();
    for (const char* id : {"container.ts.missing_pat", "container.ts.missing_pmt",
                           "container.ts.sync_loss", "container.ts.continuity_error",
                           "container.ts.pcr_gap", "container.ts.scrambled",
                           "container.ts.scan_incomplete"}) {
        EXPECT_TRUE(model::FindQcRule(rules, id) != nullptr) << "规则表里漏了 " << id;
    }
}
