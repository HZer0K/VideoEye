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

#include <functional>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QThread>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "core/reporting/QcReportExporter.h"
#include "ui/AnalysisFacade.h"

#include "BlockingTcpEndpoint.h"

namespace {

using namespace videoeye;

// QCoreApplication 长期持有 argv 指针，故 argc/argv 必须是静态存储 —— 用栈上的
// 局部数组会在测试函数返回后留下悬垂指针。多个用例共用一个静态实例，避免重复构造。
void EnsureApp() {
    static int argc = 1;
    static char arg0[] = "test_diagnostics_result_flow";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
}

// 泵事件直到谓词成立或超时。
bool PumpUntil(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

// 单纯把事件队列泵空一段时间（确保迟到的重复事件也已经被消费）。
void PumpFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
}

// 落一份极小的 HLS 清单 —— 清单解析走纯 stdlib 路径（不经 FFmpeg 解码），
// 快且确定，但完整穿过"工作线程 -> 引擎 -> 回调 -> 排队投递 -> facade 落库 -> 信号"。
QString WriteHlsManifest(const QString& name) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
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
    return path;
}

// 损坏媒体头：打开 / 探测必然走到**失败**分支。8KB 伪随机字节，避免全 0 被某些
// 探测当成空文件提前短路。
QString WriteCorruptInput(const QString& name) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    QByteArray junk(8192, '\0');
    for (int i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    f.write(junk);
    f.close();
    return path;
}

// 收集一次分析全过程对外可见的事件：进度次数、终态类型与代际。
// 用于锁住"一次分析恰好一个终态，且终态类型与输入匹配"。
struct FlowSink {
    int progress_count = 0;
    quint64 last_progress_gen = 0;
    double last_progress_percent = -1.0;

    bool finished = false;
    quint64 finished_gen = 0;
    bool finished_completed = false;

    bool failed = false;
    quint64 failed_gen = 0;
    QString failed_message;

    int TerminalCount() const { return (finished ? 1 : 0) + (failed ? 1 : 0); }
    bool Saw(quint64 gen) const {
        return (finished && finished_gen == gen) || (failed && failed_gen == gen);
    }
};

void WireFlow(videoeye::ui::AnalysisFacade& facade, FlowSink& sink) {
    QObject::connect(&facade, &videoeye::ui::AnalysisFacade::ProgressReported, &facade,
                     [&sink](quint64 gen, double percent, const QString&) {
                         ++sink.progress_count;
                         sink.last_progress_gen = gen;
                         sink.last_progress_percent = percent;
                     });
    QObject::connect(&facade, &videoeye::ui::AnalysisFacade::AnalysisFinished, &facade,
                     [&sink](quint64 gen, bool completed, const model::AnalysisResult&) {
                         sink.finished = true;
                         sink.finished_gen = gen;
                         sink.finished_completed = completed;
                     });
    QObject::connect(&facade, &videoeye::ui::AnalysisFacade::AnalysisFailed, &facade,
                     [&sink](quint64 gen, const QString& message) {
                         sink.failed = true;
                         sink.failed_gen = gen;
                         sink.failed_message = message;
                     });
}

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
    EnsureApp();

    const QString manifest = WriteHlsManifest("videoeye_result_flow.m3u8");
    ASSERT_FALSE(manifest.isEmpty()) << "无法写入测试清单";

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
    ASSERT_TRUE(PumpUntil([&] { return finished; }, 15000)) << "分析未在超时前回包";
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
                             videoeye::BitrateGopOptions{});
    EXPECT_EQ(facade.result().container_format, before_container);
    EXPECT_DOUBLE_EQ(facade.result().duration_seconds, before_duration);
    EXPECT_EQ(facade.result().file_size_bytes, before_size);

    QFile::remove(manifest);
}

// 主流程端到端（2.1）：打开文件 -> 分析开始 -> 收到进度 -> 分析完成 ->
// 结果进入 facade（= 诊断页唯一数据源）-> QC 报告生成 -> 导出报告。
//
// 这里把用户视角把这条链走完：只要有任一环节断开（进度不再上报、结果没落库、
// Evaluate 拿到空结果、导出写不出文件），本用例都会失败。
TEST(DiagnosticsResultFlow, ProgressFinishThenEvaluateAndExportProducesReportFile) {
    EnsureApp();

    const QString manifest = WriteHlsManifest("videoeye_flow_export.m3u8");
    ASSERT_FALSE(manifest.isEmpty()) << "无法写入测试清单";

    ui::AnalysisFacade facade;
    FlowSink sink;
    WireFlow(facade, sink);

    const quint64 gen = facade.StartAnalysis(manifest.toStdString());
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, 15000)) << "分析未在超时前回包";

    // 完整（而非失败）终态，且代际匹配当前任务。
    EXPECT_TRUE(sink.finished);
    EXPECT_FALSE(sink.failed);
    EXPECT_EQ(sink.finished_gen, gen);
    EXPECT_TRUE(sink.finished_completed);

    // 分析过程上报过进度（清单路径至少发一次 NotifyProgress）。
    EXPECT_GT(sink.progress_count, 0) << "分析过程没有上报任何进度，界面进度条会一直空着";
    EXPECT_EQ(sink.last_progress_gen, gen);

    // 结果进入 facade —— 诊断页展示 / 评分 / 导出都以它为唯一数据源。
    const model::AnalysisResult& result = facade.result();
    EXPECT_EQ(result.container_format, "hls");
    EXPECT_TRUE(result.streaming_analyzed);

    // QC 报告生成：对结果评估，评分必须落在合法区间。
    const model::QcReport report = facade.Evaluate(result);
    EXPECT_GE(report.score, 0.0);
    EXPECT_LE(report.score, 100.0);

    // 导出报告：走诊断页导出按钮所依赖的同一条纯函数路径
    // （按钮本身会弹 QFileDialog，模态对话框无法在无头测试里点）。
    const QString report_path = QDir::tempPath() + "/videoeye_flow_export_report.json";
    QFile::remove(report_path);
    ASSERT_TRUE(reporting::QcReportExporter::ExportReport(report_path.toStdString(), report))
        << "导出报告失败";
    const QFileInfo exported(report_path);
    ASSERT_TRUE(exported.exists()) << "导出后报告文件不存在";
    EXPECT_GT(exported.size(), 0) << "导出的报告文件为空";

    QFile::remove(report_path);
    QFile::remove(manifest);
}

// 失败终态（2.1）：损坏媒体头 -> 只发 AnalysisFailed，绝不发 AnalysisFinished，
// 且一次分析恰好一个终态（界面据此恢复按钮、不再停在"分析中"）。
TEST(DiagnosticsResultFlow, FailedInputEmitsFailedNotFinished) {
    EnsureApp();

    const QString corrupt = WriteCorruptInput("videoeye_flow_corrupt.mp4");
    ASSERT_FALSE(corrupt.isEmpty()) << "无法写入损坏输入";

    ui::AnalysisFacade facade;
    FlowSink sink;
    WireFlow(facade, sink);

    const quint64 gen = facade.StartAnalysis(corrupt.toStdString());
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, 15000))
        << "损坏输入没有在超时前产生终态";

    EXPECT_TRUE(sink.failed);
    EXPECT_FALSE(sink.finished) << "失败输入不应发出 AnalysisFinished";
    EXPECT_EQ(sink.failed_gen, gen);
    EXPECT_FALSE(sink.failed_message.isEmpty()) << "失败必须带可展示的错误信息";

    // 留出窗口，确认迟到的重复终态不会出现（"恰好一次终态"）。
    PumpFor(150);
    EXPECT_EQ(sink.TerminalCount(), 1) << "一次分析产生了多个终态";

    QFile::remove(corrupt);
}

// 取消终态（2.1 / 2.3）：阻塞网络输入下点取消 -> 以"完成但 incomplete"收尾
// （scan_status=Cancelled），而不是被当成失败；运行态必须归位，按钮因此恢复可用。
TEST(DiagnosticsResultFlow, CancelOnBlockingInputFinishesAsIncomplete) {
    EnsureApp();

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    ui::AnalysisFacade facade;
    FlowSink sink;
    WireFlow(facade, sink);

    const quint64 gen = facade.StartAnalysis(endpoint.tcp_url());

    ASSERT_TRUE(PumpUntil([&] { return facade.IsRunning(); }, 3000))
        << "分析没有进入运行态，无法验证取消路径";
    // 若输入瞬间就回包（端口未按预期阻塞），本轮没测到"取消打断阻塞 IO"，跳过。
    if (PumpUntil([&] { return sink.Saw(gen); }, 200)) {
        GTEST_SKIP() << "阻塞输入瞬间就回包了（端口未真正阻塞），本轮不算测到取消路径";
    }

    facade.Cancel();
    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, 8000))
        << "取消后没有在预算内回到终态（gen=" << gen << "）";

    EXPECT_TRUE(sink.finished) << "取消应产生 AnalysisFinished(completed=false)";
    EXPECT_FALSE(sink.failed) << "取消不应被当作失败";
    EXPECT_EQ(sink.finished_gen, gen);
    EXPECT_FALSE(sink.finished_completed) << "取消的结果 completed 必须为 false";

    // 界面按钮恢复可用的前置条件：运行态归位，不会永久停在"分析中"。
    EXPECT_TRUE(PumpUntil([&] { return !facade.IsRunning(); }, 3000))
        << "取消后 IsRunning() 仍为 true";
}

} // namespace
