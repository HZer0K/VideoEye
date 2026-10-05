// 诊断结果流转回归测试（对应评审 P0 / P2）
//
// 根因(已修): DiagnosticsPage::OnFacadeFinished 收到扫描结果却没有调用
// facade_->SetResult(result)，导致 Evaluate() 读到默认构造的空 AnalysisResult，
// 问题清单/评分/码率-GOP/音频QC/HDR/字幕页面全部拿到空数据，报告导出也基于空结果。
//
// 现在保存职责上移到 AnalysisFacade：它在发出 AnalysisFinished **之前**先把结果写进
// result()，页面只负责展示。
//
// （P2 收口）对外写入口 SetResult() 已删除 —— 结果存储是只读快照，页面不再有
// "改写分析结果"的手段，从根上杜绝"页面忘了写回"。因此这里的回归不再测 SetResult，
// 而是守住两件事：
//   1. 扫描前 result() 是空快照、且 QC 引擎对空结果仍然给出合法评分（不崩、不越界）；
//   2. 端到端: 信号到达时 result() 已经是本次扫描的结果；
//   3. 唯一允许的就地改动（ApplySceneChanges）不得波及扫描事实字段。

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

// 扫描前 result() 必须是默认空快照 —— 它是"只读快照"而不是可写槽位，
// 页面拿不到写入口，也就无从"忘记写回"。
// 顺带锁住 QC 引擎对空结果的健壮性：空结果正是当年 P0 在界面上的表现形式，
// 即便真拿到一份空的，评分也必须落在合法区间而不是崩掉或越界。
TEST(DiagnosticsResultFlow, ResultIsEmptySnapshotBeforeAnyAnalysis) {
    ui::AnalysisFacade facade;

    EXPECT_EQ(facade.result().total_packets, 0);
    EXPECT_DOUBLE_EQ(facade.result().duration_seconds, 0.0);
    EXPECT_EQ(facade.result().key_frame_count, 0);

    model::QcReport report = facade.Evaluate(facade.result());
    EXPECT_GE(report.score, 0.0);
    EXPECT_LE(report.score, 100.0);

    // Evaluate 是纯函数（只读入参），不改动存储里的快照
    EXPECT_EQ(facade.result().total_packets, 0);
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

    // 唯一允许的就地改动: ApplySceneChanges 只该动 bitrate_gop 这一支。
    // 扫描事实（容器格式 / 时长 / 文件大小）一旦落库就是历史 —— 页面改它不是"刷新"
    // 而是篡改诊断结论。这里顺手用一份真实结果（本测试正好刚跑完一次扫描）锁住它。
    const auto before_container = facade.result().container_format;
    const double before_duration = facade.result().duration_seconds;
    const int64_t before_size = facade.result().file_size_bytes;
    facade.ApplySceneChanges({model::SceneChangeResult{0, 1.0, 0.9},
                              model::SceneChangeResult{120, 5.0, 0.8}},
                             analyzer::BitrateGopOptions{});
    EXPECT_EQ(facade.result().container_format, before_container);
    EXPECT_DOUBLE_EQ(facade.result().duration_seconds, before_duration);
    EXPECT_EQ(facade.result().file_size_bytes, before_size);

    QFile::remove(manifest);
}

} // namespace
