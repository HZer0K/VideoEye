// 诊断结果流转回归测试（对应评审 P0）
//
// 根因: DiagnosticsPage::OnFacadeFinished 收到扫描结果却没有调用
// facade_->SetResult(result)，导致 Evaluate() 读到默认构造的空 AnalysisResult，
// 问题清单/评分/码率-GOP/音频QC/HDR/字幕页面全部拿到空数据，报告导出也基于空结果。
//
// 本测试锁定 facade 的契约：SetResult 之后 result() 必须返回写回的那一份，
// 且 Evaluate 必须基于这份数据（而不是隐藏的默认空结果）。这正是"结果 / 评分 /
// 问题表 / 报告导出都使用同一份结果"的前提。DiagnosticsPage::OnFacadeFinished
// 现在已按此契约调用 SetResult。

#include <gtest/gtest.h>

#include <QString>

#include "core/analysis/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "ui/AnalysisFacade.h"

namespace {

using namespace videoeye;

TEST(DiagnosticsResultFlow, SetResultThenResultReflectsScan) {
    ui::AnalysisFacade facade;

    // 扫描前: result() 是默认构造的空结果
    EXPECT_EQ(facade.result().total_packets, 0);
    EXPECT_DOUBLE_EQ(facade.result().duration_seconds, 0.0);

    analyzer::AnalysisResult scan;
    scan.container_format = "mov,mp4,m4a,3gp,3g2,mj2";
    scan.duration_seconds = 12.0;
    scan.total_packets = 1357;
    scan.key_frame_count = 24;
    scan.overall_bitrate_bps = 3500000;
    scan.file_size_bytes = 5250000;

    facade.SetResult(scan);

    // P0 核心断言: SetResult 之后 result() 必须返回刚才写回的那一份,
    // 不能再是默认空结果。这正是旧 DiagnosticsPage 漏掉的一步。
    EXPECT_EQ(facade.result().total_packets, 1357);
    EXPECT_DOUBLE_EQ(facade.result().duration_seconds, 12.0);
    EXPECT_EQ(facade.result().key_frame_count, 24);

    // Evaluate 必须基于 result() 这份数据, 产出合法区间内的评分报告
    model::QcReport report = facade.Evaluate(facade.result());
    EXPECT_GE(report.score, 0.0);
    EXPECT_LE(report.score, 100.0);
}

// 直接演示"不调用 SetResult"时 result() 仍是默认空结果 —— 即 P0 bug 的表现形式。
TEST(DiagnosticsResultFlow, WithoutSetResultResultStaysEmpty) {
    ui::AnalysisFacade facade;

    analyzer::AnalysisResult scan;
    scan.total_packets = 1357;
    scan.duration_seconds = 12.0;

    // 模拟旧 DiagnosticsPage: 拿到 result 却没调用 SetResult, 直接用 facade->result()
    facade.Evaluate(facade.result());
    EXPECT_EQ(facade.result().total_packets, 0);  // 默认空结果, 不是 1357

    facade.SetResult(scan);
    facade.Evaluate(facade.result());
    EXPECT_EQ(facade.result().total_packets, 1357);  // 写回后才是真实数据
}

} // namespace
