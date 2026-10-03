// 诊断结果流转回归测试（对应评审 P0 / P2）
//
// 根因(已修): DiagnosticsPage::OnFacadeFinished 收到扫描结果却没有调用
// facade_->SetResult(result)，导致 Evaluate() 读到默认构造的空 AnalysisResult，
// 问题清单/评分/码率-GOP/音频QC/HDR/字幕页面全部拿到空数据，报告导出也基于空结果。
//
// 现在保存职责上移到 AnalysisFacade：它在发出 AnalysisFinished **之前**先把结果写进
// result()，页面只负责展示。前两个测试锁定 SetResult/result 的契约；最后一个
// 端到端跑通 controller 线程 -> 引擎 -> 回包 -> facade 落库 -> 信号 这条真实链路，
// 证明"信号到达时结果已经就位"，而不是只证明 SetResult 能被调用。

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "ui/AnalysisFacade.h"

namespace {

using namespace videoeye;

TEST(DiagnosticsResultFlow, SetResultThenResultReflectsScan) {
    ui::AnalysisFacade facade;

    // 扫描前: result() 是默认构造的空结果
    EXPECT_EQ(facade.result().total_packets, 0);
    EXPECT_DOUBLE_EQ(facade.result().duration_seconds, 0.0);

    model::AnalysisResult scan;
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

    model::AnalysisResult scan;
    scan.total_packets = 1357;
    scan.duration_seconds = 12.0;

    // 模拟旧 DiagnosticsPage: 拿到 result 却没调用 SetResult, 直接用 facade->result()
    facade.Evaluate(facade.result());
    EXPECT_EQ(facade.result().total_packets, 0);  // 默认空结果, 不是 1357

    facade.SetResult(scan);
    facade.Evaluate(facade.result());
    EXPECT_EQ(facade.result().total_packets, 1357);  // 写回后才是真实数据
}

// 端到端信号链路：观察者在 AnalysisFinished 里读 facade.result()，必须已经是本次结果。
//
// 输入用一份极小的 HLS 清单 —— 清单解析走纯 stdlib 路径（不经 FFmpeg 解码），
// 快且确定，但仍然完整地穿过"工作线程跑引擎 -> 回调 -> 排队投递 -> facade 落库 -> 信号"
// 这一整条链。这正是以前漏写 SetResult 时唯一能暴露问题的位置：只测 SetResult 本身
// 是测不出"页面忘了写"的。
TEST(DiagnosticsResultFlow, FacadeSavesResultBeforeEmittingFinished) {
    int argc = 1;
    char arg0[] = "test_diagnostics_result_flow";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    const QString manifest = QDir::tempPath() + "/videoeye_result_flow.m3u8";
    {
        QFile f(manifest);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << "无法写入测试清单";
        f.write("#EXTM3U\n"
                "#EXT-X-VERSION:3\n"
                "#EXT-X-TARGETDURATION:4\n"
                "#EXT-X-MEDIA-SEQUENCE:0\n"
                "#EXTINF:4.0,\n"
                "seg0.ts\n"
                "#EXTINF:4.0,\n"
                "seg1.ts\n"
                "#EXT-X-ENDLIST\n");
        f.close();
    }

    ui::AnalysisFacade facade;

    bool finished = false;
    bool completed = false;
    quint64 observed_gen = 0;
    // 观察者故意**只读 facade.result()**，不碰回调参数里的 result ——
    // 这样一旦 facade 没保存，断言必然失败（正是旧 bug 的表现）。
    model::AnalysisResult seen_from_facade;

    QObject::connect(&facade, &ui::AnalysisFacade::AnalysisFinished, &facade,
                     [&](quint64 generation, bool done, const model::AnalysisResult&) {
                         seen_from_facade = facade.result();
                         observed_gen = generation;
                         completed = done;
                         finished = true;
                     });

    const quint64 gen = facade.StartAnalysis(manifest.toStdString());
    QElapsedTimer timer;
    timer.start();
    while (!finished && timer.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }

    ASSERT_TRUE(finished) << "分析未在超时前回包";
    EXPECT_EQ(observed_gen, gen);
    EXPECT_TRUE(completed);

    // 核心断言: 信号到达时 facade.result() 已经是本次扫描的结果。
    EXPECT_EQ(seen_from_facade.container_format, "hls");
    EXPECT_TRUE(seen_from_facade.streaming_analyzed);
    EXPECT_EQ(seen_from_facade.streaming_package.playlists.size(), 1u);
    EXPECT_DOUBLE_EQ(seen_from_facade.duration_seconds, 8.0);
    EXPECT_GT(seen_from_facade.file_size_bytes, 0);

    QFile::remove(manifest);
}

} // namespace
