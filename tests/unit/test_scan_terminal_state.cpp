// 扫描终态必须复位所有共用页面（复查 P1-4）
//
// 根因(已修): DiagnosticsPage::OnFacadeFailed 只重置诊断页自身，**不发任何终态信号**。
// 而码率/GOP、音频 QC、色彩/HDR（以及后来接入的字幕辅助 / 流媒体包 / 编码参数集）
// 共用同一次扫描，它们的扫描态靠面板转发扫描生命周期来维护 —— 于是失败之后这些页
// 永远停在扫描态：取消按钮还亮着、"开始分析"按钮永久禁用，与诊断页显示的"失败"
// 互相矛盾，用户只能重开文件才恢复。
//
// 修复: 三个终态（完成 / 取消 / 失败）统一成一个 ScanEnded(reason) 事件，且每次扫描
// 恰好发一次；面板在同一个处理函数里复位共用页的按钮与进度（失败时不分发空结果）。
// 「用户点取消时立刻发一个假终态」也一并去掉了 —— 那一刻任务其实还在跑。
//
// 这里跑一次真实的失败扫描（损坏输入），断言所有共用页面都回到了 Idle。
// 用 offscreen 平台，不需要显示器。
//
// 顺带覆盖: 这个测试第一次跑起来就撞出了 d621be1 引入的启动闪退 ——
// DiagnosticsPage 构造函数把 SetupUi() 放在 facade_ 之前，而 SetupUi() 末尾的
// RebuildRuleTable() 要读 facade_->rules()，即对空指针调成员函数。构造这个页
// 本身就会崩，所以"能构造出来"也是本测试的一条隐含断言。

#include <gtest/gtest.h>

#include <chrono>
#include <functional>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QThread>

#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/StreamStats.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/analysis_panel/AudioQcPage.h"
#include "ui/analysis_panel/BitrateGopPage.h"
#include "ui/analysis_panel/ColorHdrPage.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/analysis_panel/FramePacketView.h"
#include "ui/analysis_panel/SubtitleAuxPage.h"
#include "ui/analysis_panel/VideoFrameTableWidget.h"
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/main_window/macroblock_coordination.h"
#include "ui/main_window/navigation_model.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include "ui/streaming_panel/StreamingPanel.h"

#include "BlockingTcpEndpoint.h"

namespace {

using namespace videoeye;

// QApplication 会长期持有 argv 指针，所以必须用静态存储。
QApplication* EnsureApp() {
    static int argc = 1;
    static char arg0[] = "test_scan_terminal_state";
    static char* argv[] = {arg0, nullptr};
    if (!QApplication::instance()) {
        static QApplication app(argc, argv);
        return &app;
    }
    return qobject_cast<QApplication*>(QApplication::instance());
}

bool PumpUntil(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

// 没法被解析的输入: 分析必然走到"打开失败"分支(而不是"扫描完但结果为空")。
QString WriteCorruptInput() {
    const QString path = QDir::tempPath() + "/videoeye_scan_terminal_corrupt.mp4";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    QByteArray junk(4096, '\x00');
    for (int i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    f.write(junk);
    f.close();
    return path;
}

// 一份极小的 HLS 清单：清单解析走纯 stdlib 路径，能在测试里真正跑完成一次扫描，
// 用来构造"有结果 / 换文件清结果"这类需要真实结果的场景。
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

// 从外部 stack 里把"共用同一次扫描"的几页一次性抓出来（与 MainWindow 同路径）。
struct ScanPages {
    ui::DiagnosticsPage* diagnostics = nullptr;
    ui::BitrateGopPage* bitrate_gop = nullptr;
    ui::AudioQcPage* audio_qc = nullptr;
    ui::ColorHdrPage* color_hdr = nullptr;
    ui::SubtitleAuxPage* subtitle = nullptr;
    ui::StreamingPanel* streaming = nullptr;
    ui::BitstreamPanel* bitstream = nullptr;

    bool AllFound() const {
        return diagnostics && bitrate_gop && audio_qc && color_hdr && subtitle && streaming &&
               bitstream;
    }
    bool AllInactive() const {
        return !bitrate_gop->IsScanActive() && !audio_qc->IsScanActive() &&
               !color_hdr->IsScanActive() && !subtitle->IsScanActive() &&
               !streaming->IsScanActive() && !bitstream->IsScanActive();
    }
};

ScanPages FindScanPages(QStackedWidget& stack) {
    ScanPages p;
    p.diagnostics = stack.findChild<ui::DiagnosticsPage*>();
    p.bitrate_gop = stack.findChild<ui::BitrateGopPage*>();
    p.audio_qc = stack.findChild<ui::AudioQcPage*>();
    p.color_hdr = stack.findChild<ui::ColorHdrPage*>();
    p.subtitle = stack.findChild<ui::SubtitleAuxPage*>();
    p.streaming = stack.findChild<ui::StreamingPanel*>();
    p.bitstream = stack.findChild<ui::BitstreamPanel*>();
    return p;
}

} // namespace

TEST(ScanTerminalState, FailedScanRestoresAllSharedPages) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    const QString corrupt = WriteCorruptInput();
    ASSERT_FALSE(corrupt.isEmpty());

    // 构造面板本身就会把 DiagnosticsPage 建起来（这一步曾经是启动闪退的现场）。
    ui::AnalysisPanel panel;

    // 各页在 AddPageWithScroll 里被包进 QScrollArea，而 QScrollArea 要到
    // PopulateStackedWidget 才被挂到外部 stack 上 —— 在此之前它们没有父对象，
    // findChild 找不到。所以这里走和 MainWindow 一样的路径：给一个 stack 让它装满。
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);

    auto* diagnostics = stack.findChild<ui::DiagnosticsPage*>();
    auto* bitrate_gop = stack.findChild<ui::BitrateGopPage*>();
    auto* audio_qc = stack.findChild<ui::AudioQcPage*>();
    auto* color_hdr = stack.findChild<ui::ColorHdrPage*>();
    auto* subtitle = stack.findChild<ui::SubtitleAuxPage*>();
    auto* streaming = stack.findChild<ui::StreamingPanel*>();
    auto* bitstream = stack.findChild<ui::BitstreamPanel*>();
    ASSERT_TRUE(diagnostics != nullptr);
    ASSERT_TRUE(bitrate_gop != nullptr);
    ASSERT_TRUE(audio_qc != nullptr);
    ASSERT_TRUE(color_hdr != nullptr);
    ASSERT_TRUE(subtitle != nullptr);
    ASSERT_TRUE(streaming != nullptr);
    ASSERT_TRUE(bitstream != nullptr);

    int terminal_count = 0;
    auto last_reason = ui::DiagnosticsPage::ScanEndReason::Completed;
    QObject::connect(diagnostics, &ui::DiagnosticsPage::ScanEnded, diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason reason) {
                         last_reason = reason;
                         ++terminal_count;
                     });

    diagnostics->SetSourcePath(corrupt);
    diagnostics->StartScan(diagnostics->options());

    // 扫描一开始，共用的页面必须进入扫描态 —— 否则"结束时复位"这句话没有任何意义。
    EXPECT_TRUE(bitrate_gop->IsScanActive());
    EXPECT_TRUE(audio_qc->IsScanActive());
    EXPECT_TRUE(color_hdr->IsScanActive());
    EXPECT_TRUE(subtitle->IsScanActive());
    EXPECT_TRUE(streaming->IsScanActive());
    EXPECT_TRUE(bitstream->IsScanActive());

    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 30000))
        << "损坏输入没有在超时前产生扫描终态";

    EXPECT_EQ(1, terminal_count) << "一次扫描只应产生一个终态事件";
    EXPECT_TRUE(last_reason == ui::DiagnosticsPage::ScanEndReason::Failed)
        << "损坏输入必须走 Failed 终态";

    // 核心断言: 共用同一次扫描的页面都必须回到 Idle。
    // 修复前它们会一直停在扫描态（取消按钮亮着、"开始分析"永久禁用）。
    EXPECT_FALSE(bitrate_gop->IsScanActive()) << "码率/GOP 页仍停在扫描中";
    EXPECT_FALSE(audio_qc->IsScanActive()) << "音频 QC 页仍停在扫描中";
    EXPECT_FALSE(color_hdr->IsScanActive()) << "色彩/HDR 页仍停在扫描中";
    EXPECT_FALSE(subtitle->IsScanActive()) << "字幕辅助页仍停在扫描中";
    EXPECT_FALSE(streaming->IsScanActive()) << "流媒体包页仍停在扫描中";
    EXPECT_FALSE(bitstream->IsScanActive()) << "编码参数集页仍停在扫描中";

    QFile::remove(corrupt);
}

// ---------------------------------------------------------------------------
// 2.1：换文件必须清掉上一文件的扫描结果（旧结果不能挂到新文件上）。
// ---------------------------------------------------------------------------
TEST(ScanTerminalState, ReopenFileClearsPreviousScanResult) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    const QString manifest_a = WriteHlsManifest("videoeye_scan_reopen_a.m3u8");
    const QString manifest_b = WriteHlsManifest("videoeye_scan_reopen_b.m3u8");
    ASSERT_FALSE(manifest_a.isEmpty());
    ASSERT_FALSE(manifest_b.isEmpty());

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);
    const ScanPages pages = FindScanPages(stack);
    ASSERT_TRUE(pages.AllFound());

    int terminal_count = 0;
    QObject::connect(pages.diagnostics, &ui::DiagnosticsPage::ScanEnded, pages.diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason) { ++terminal_count; });

    // 第一次打开 A 并扫描成功 -> 有结果。
    panel.SetCurrentVideoPath(manifest_a);
    pages.diagnostics->StartScan(pages.diagnostics->options());
    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 20000)) << "首次扫描未回终态";
    ASSERT_TRUE(pages.diagnostics->hasResult()) << "首次扫描后应持有结果";

    // 第二次打开 B：上一文件的结果必须被清理，不能把 A 的结果展示在 B 上。
    panel.SetCurrentVideoPath(manifest_b);
    EXPECT_FALSE(pages.diagnostics->hasResult()) << "换文件后旧扫描结果没有被清理";

    QFile::remove(manifest_a);
    QFile::remove(manifest_b);
}

// ---------------------------------------------------------------------------
// 2.1 / 2.3：取消扫描必须在预算内回到终态，且共用页一起复位（按钮恢复可用）。
// ---------------------------------------------------------------------------
TEST(ScanTerminalState, CancelScanRestoresSharedPagesWithinBudget) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);
    const ScanPages pages = FindScanPages(stack);
    ASSERT_TRUE(pages.AllFound());

    int terminal_count = 0;
    auto last_reason = ui::DiagnosticsPage::ScanEndReason::Completed;
    QObject::connect(pages.diagnostics, &ui::DiagnosticsPage::ScanEnded, pages.diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason reason) {
                         last_reason = reason;
                         ++terminal_count;
                     });

    pages.diagnostics->SetSourcePath(QString::fromStdString(endpoint.tcp_url()));
    pages.diagnostics->StartScan(pages.diagnostics->options());

    ASSERT_TRUE(PumpUntil([&] { return pages.bitrate_gop->IsScanActive(); }, 3000))
        << "扫描没有进入扫描态";
    // 端口未真正阻塞（瞬间就回终态）时没测到取消路径，跳过。
    if (PumpUntil([&] { return terminal_count > 0; }, 200)) {
        GTEST_SKIP() << "阻塞输入瞬间就回终态（端口未真正阻塞），本轮不算测到取消路径";
    }

    const auto t0 = std::chrono::steady_clock::now();
    pages.diagnostics->CancelScan();
    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 8000))
        << "取消后没有在预算内回到终态";
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    EXPECT_LT(dt, 5000) << "取消到终态耗时 " << dt << "ms，接近/超过预算";
    EXPECT_EQ(terminal_count, 1) << "一次扫描只应产生一个终态事件";
    EXPECT_TRUE(last_reason == ui::DiagnosticsPage::ScanEndReason::Cancelled)
        << "取消必须走 Cancelled 终态";
    EXPECT_TRUE(pages.AllInactive()) << "取消后共用页必须回到 Idle（按钮恢复可用）";
}

// ---------------------------------------------------------------------------
// 2.1：诊断页的单文件扫描与报告页的任务调度是两套独立状态，互不牵连。
// ---------------------------------------------------------------------------
TEST(ScanTerminalState, SingleFileScanDoesNotDisturbReportingTask) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    const QString manifest = WriteHlsManifest("videoeye_scan_vs_batch.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);
    const ScanPages pages = FindScanPages(stack);
    ASSERT_TRUE(pages.AllFound());
    auto* reporting = stack.findChild<ui::ReportingPanel*>();
    ASSERT_TRUE(reporting != nullptr);

    ASSERT_FALSE(reporting->IsTaskRunning()) << "初始时报告页不应有任务在跑";

    int terminal_count = 0;
    QObject::connect(pages.diagnostics, &ui::DiagnosticsPage::ScanEnded, pages.diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason) { ++terminal_count; });

    panel.SetCurrentVideoPath(manifest);
    pages.diagnostics->StartScan(pages.diagnostics->options());
    EXPECT_FALSE(reporting->IsTaskRunning())
        << "诊断页的单文件扫描不应把报告页任务状态置为运行中";

    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 20000)) << "扫描未回终态";
    EXPECT_TRUE(pages.diagnostics->hasResult());
    EXPECT_FALSE(reporting->IsTaskRunning())
        << "诊断扫描结束后报告页任务状态被牵连";
    EXPECT_FALSE(reporting->HasFreshResult())
        << "诊断扫描不应在报告页产生'新鲜结果'";

    QFile::remove(manifest);
}

// ---------------------------------------------------------------------------
// 2.2：播放期帧表持续刷新；停止/换文件后旧数据清空；关闭开关不再接收新帧；
//      重新启用不会把旧数据叠加回来。
// ---------------------------------------------------------------------------
TEST(PlaybackLinkage, FrameTableRefreshesThenClearsAndDoesNotAccumulate) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);

    auto* frame_view = stack.findChild<ui::FramePacketView*>();
    auto* video_page = stack.findChild<ui::VideoFrameTableWidget*>();
    ASSERT_TRUE(frame_view != nullptr);
    ASSERT_TRUE(video_page != nullptr);
    auto* table = video_page->frameTable();
    ASSERT_TRUE(table != nullptr);

    // 视频帧默认开启（Master && VideoFrame 默认开）—— 播放期回吐的帧必须进表。
    panel.AppendVideoFrameInfo(0, 1, true, 0, 0.0);      // I 帧
    panel.AppendVideoFrameInfo(1, 2, false, 1000, 0.04);  // P 帧
    panel.AppendVideoFrameInfo(2, 2, false, 2000, 0.08);
    frame_view->FlushPending();
    EXPECT_EQ(table->rowCount(), 3) << "播放期帧表没有持续刷新";

    // 继续回吐 -> 行数持续增长（是"持续刷新"，不是只填一次）。
    panel.AppendVideoFrameInfo(3, 1, true, 3000, 0.12);
    panel.AppendVideoFrameInfo(4, 2, false, 4000, 0.16);
    frame_view->FlushPending();
    EXPECT_EQ(table->rowCount(), 5) << "新增帧没有继续进入帧表";

    // 关闭「启用分析」：新的回吐不再进表（旧数据保持不动）。
    auto* toggle = video_page->toggle();
    ASSERT_TRUE(toggle != nullptr);
    toggle->setChecked(false);
    panel.AppendVideoFrameInfo(5, 2, false, 5000, 0.20);
    frame_view->FlushPending();
    EXPECT_EQ(table->rowCount(), 5) << "关闭开关后仍在接收新帧";

    // 停止 / 换文件：清空旧数据。
    panel.ResetVideoFrameList();
    frame_view->FlushPending();
    EXPECT_EQ(table->rowCount(), 0) << "停止后旧帧数据没有被清空";

    // 重新启用：只应看到新回吐的帧，绝不把清空前的旧数据叠加回来。
    toggle->setChecked(true);
    panel.AppendVideoFrameInfo(6, 1, true, 6000, 0.24);
    frame_view->FlushPending();
    EXPECT_EQ(table->rowCount(), 1) << "重新启用后叠加了旧数据或未接收新数据";
}

// ---------------------------------------------------------------------------
// 2.2：播放期回吐的统计 / 帧属于"播放期口径"，不得改写全文件扫描结果
//      （两种口径互不混用）。
// ---------------------------------------------------------------------------
TEST(PlaybackLinkage, PlaybackFeedDoesNotBleedIntoFullFileResult) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    const QString manifest = WriteHlsManifest("videoeye_playback_vs_fullfile.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);
    const ScanPages pages = FindScanPages(stack);
    ASSERT_TRUE(pages.AllFound());

    int terminal_count = 0;
    QObject::connect(pages.diagnostics, &ui::DiagnosticsPage::ScanEnded, pages.diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason) { ++terminal_count; });

    panel.SetCurrentVideoPath(manifest);
    pages.diagnostics->StartScan(pages.diagnostics->options());
    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 20000)) << "扫描未回终态";
    ASSERT_TRUE(pages.diagnostics->hasResult());

    const QString full_container =
        QString::fromStdString(pages.diagnostics->result().container_format);
    const double full_duration = pages.diagnostics->result().duration_seconds;

    // 播放期回吐：一份"大得离谱"的播放期统计 + 一帧解码帧。
    model::StreamStats stats;
    stats.total_packets = 999999;
    stats.duration_seconds = 12345.0;
    panel.UpdateStreamStats(stats);
    panel.AppendVideoFrameInfo(0, 1, true, 0, 0.0);

    // 全文件扫描结果必须纹丝不动 —— 播放期数据走的是另一条展示通道。
    EXPECT_EQ(QString::fromStdString(pages.diagnostics->result().container_format), full_container);
    EXPECT_DOUBLE_EQ(pages.diagnostics->result().duration_seconds, full_duration);

    QFile::remove(manifest);
}

// ---------------------------------------------------------------------------
// 2.2：在分析页面之间来回切换，不应改变已落库的扫描结果（数据在页间保持一致）。
// ---------------------------------------------------------------------------
TEST(PlaybackLinkage, SwitchingPagesKeepsScanResultConsistent) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    const QString manifest = WriteHlsManifest("videoeye_page_switch.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);
    const ScanPages pages = FindScanPages(stack);
    ASSERT_TRUE(pages.AllFound());

    int terminal_count = 0;
    QObject::connect(pages.diagnostics, &ui::DiagnosticsPage::ScanEnded, pages.diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason) { ++terminal_count; });

    panel.SetCurrentVideoPath(manifest);
    pages.diagnostics->StartScan(pages.diagnostics->options());
    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 20000)) << "扫描未回终态";

    const QString container = QString::fromStdString(pages.diagnostics->result().container_format);
    const double duration = pages.diagnostics->result().duration_seconds;

    // 模拟用户点侧栏在分析页间来回切换。
    for (int i = 0; i < stack.count(); ++i) stack.setCurrentIndex(i);
    stack.setCurrentIndex(0);

    EXPECT_TRUE(pages.diagnostics->hasResult()) << "切换页面把扫描结果弄丢了";
    EXPECT_EQ(QString::fromStdString(pages.diagnostics->result().container_format), container);
    EXPECT_DOUBLE_EQ(pages.diagnostics->result().duration_seconds, duration);

    QFile::remove(manifest);
}

// ---------------------------------------------------------------------------
// 宏块分析 / MV 叠加的协调状态（纯逻辑，不依赖 Qt）。
// 实际采集状态取"用户启用"与"叠加需求"的或；关闭叠加只回退到用户选择；
// 用户关闭分析时叠加因失去依赖一并失效。
// ---------------------------------------------------------------------------
TEST(MacroblockCoordinationTest, OverlayAloneEnablesAnalysis) {
    ui::MacroblockCoordination c;
    EXPECT_FALSE(c.ActualEnabled());

    c.OnOverlayToggle(true); // 仅打开叠加，用户没主动开分析
    EXPECT_TRUE(c.ActualEnabled());
    EXPECT_FALSE(c.user_enabled);

    // 关闭叠加且用户未启用 -> 分析关
    c.OnOverlayToggle(false);
    EXPECT_FALSE(c.ActualEnabled());
}

TEST(MacroblockCoordinationTest, ClosingOverlayKeepsUserEnabledAnalysis) {
    ui::MacroblockCoordination c;
    c.OnUserToggle(true); // 用户主动开分析
    EXPECT_TRUE(c.ActualEnabled());

    c.OnOverlayToggle(true); // 再叠加，实际仍为开
    EXPECT_TRUE(c.ActualEnabled());

    c.OnOverlayToggle(false); // 关叠加：必须保留用户原先主动开启的分析
    EXPECT_TRUE(c.ActualEnabled());
    EXPECT_TRUE(c.user_enabled);
    EXPECT_FALSE(c.overlay_enabled);
}

TEST(MacroblockCoordinationTest, UserDisableDropsOverlayDependency) {
    ui::MacroblockCoordination c;
    c.OnUserToggle(true);
    c.OnOverlayToggle(true);
    EXPECT_TRUE(c.ActualEnabled());

    c.OnUserToggle(false); // 用户关分析 -> 叠加失去依赖，一并失效
    EXPECT_FALSE(c.ActualEnabled());
    EXPECT_FALSE(c.user_enabled);
    EXPECT_FALSE(c.overlay_enabled);
}

// 面板接口：SetMacroblockAnalysisEnabled 同步 feature 表且**不**发 AnalysisFeatureToggled，
// 否则与协调层形成回环（协调层收到信号又回头下发一次）。
TEST(MacroblockCoordinationTest, SetMacroblockEnabledSyncsWithoutSignal) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    ui::AnalysisPanel panel;
    int toggled_count = 0;
    QObject::connect(&panel, &ui::AnalysisPanel::AnalysisFeatureToggled, &panel,
                     [&](int, bool) { ++toggled_count; });

    panel.SetMacroblockAnalysisEnabled(true);
    EXPECT_TRUE(panel.IsFeatureEnabled(model::AnalysisFeature::Macroblock));
    EXPECT_EQ(toggled_count, 0);

    panel.SetMacroblockAnalysisEnabled(false);
    EXPECT_FALSE(panel.IsFeatureEnabled(model::AnalysisFeature::Macroblock));
    EXPECT_EQ(toggled_count, 0);
}

// ---------------------------------------------------------------------------
// 侧栏导航元数据模型（纯逻辑）
// ---------------------------------------------------------------------------
TEST(NavigationModelTest, GroupScopeGoesToHeaderAndPageTitlesStayPlain) {
    QVector<ui::PageNavigation> pages = {
        {QStringLiteral("b"), QStringLiteral("B页"), QStringLiteral("g2"), QString(), 1, 5},
        {QStringLiteral("a"), QStringLiteral("A页"), QStringLiteral("g1"), QStringLiteral("全文件"), 0, 2},
        {QStringLiteral("c"), QStringLiteral("C页"), QStringLiteral("g1"), QStringLiteral("全文件"), 1, 7},
    };
    const QVector<ui::NavGroup> groups = {
        {QStringLiteral("g1"), QStringLiteral("一组")},
        {QStringLiteral("g2"), QStringLiteral("二组")},
    };

    const QVector<ui::NavRow> rows = ui::BuildNavigationRows(pages, groups);
    // g1 有两页且口径一致（全文件）-> 口径并入分组标题；g2 只有一个页面 -> 不显示标题。
    ASSERT_EQ(rows.size(), 4);
    EXPECT_TRUE(rows[0].is_header);
    EXPECT_EQ(rows[0].text, QStringLiteral("一组（全文件）"));
    // 页面项只留标题本身，不再拼口径后缀
    EXPECT_FALSE(rows[1].is_header);
    EXPECT_EQ(rows[1].stack_index, 2);
    EXPECT_EQ(rows[1].text, QStringLiteral("A页"));
    EXPECT_EQ(rows[2].stack_index, 7);
    EXPECT_EQ(rows[2].text, QStringLiteral("C页"));
    EXPECT_FALSE(rows[3].is_header);
    EXPECT_EQ(rows[3].stack_index, 5);
    EXPECT_EQ(rows[3].text, QStringLiteral("B页"));
}

TEST(NavigationModelTest, GroupScopeSkippedWhenPagesDisagree) {
    const QVector<ui::PageNavigation> pages = {
        {QStringLiteral("a"), QStringLiteral("A页"), QStringLiteral("g1"), QStringLiteral("播放期"), 0, 0},
        {QStringLiteral("b"), QStringLiteral("B页"), QStringLiteral("g1"), QStringLiteral("全文件"), 1, 1},
    };
    const QVector<ui::NavGroup> groups = {{QStringLiteral("g1"), QStringLiteral("一组")}};

    const QVector<ui::NavRow> rows = ui::BuildNavigationRows(pages, groups);
    ASSERT_EQ(rows.size(), 3);
    EXPECT_TRUE(rows[0].is_header);
    EXPECT_EQ(rows[0].text, QStringLiteral("一组"));  // 口径不一致 -> 不标，避免误导
}

TEST(NavigationModelTest, GroupScopeIgnoresPagesWithoutScope) {
    const QVector<ui::PageNavigation> pages = {
        {QStringLiteral("a"), QStringLiteral("A页"), QStringLiteral("g1"), QStringLiteral("播放期"), 0, 0},
        {QStringLiteral("b"), QStringLiteral("B页"), QStringLiteral("g1"), QString(), 1, 1},
    };
    const QVector<ui::NavGroup> groups = {{QStringLiteral("g1"), QStringLiteral("一组")}};

    const QVector<ui::NavRow> rows = ui::BuildNavigationRows(pages, groups);
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows[0].text, QStringLiteral("一组（播放期）"));  // 未标口径的页面不参与判断
    EXPECT_EQ(rows[1].text, QStringLiteral("A页"));
    EXPECT_EQ(rows[2].text, QStringLiteral("B页"));
}

TEST(NavigationModelTest, UnknownGroupPagesGoLastWithoutHeader) {
    const QVector<ui::PageNavigation> pages = {
        {QStringLiteral("x"), QStringLiteral("X页"), QStringLiteral("unknown"), QString(), 0, 9},
        {QStringLiteral("a"), QStringLiteral("A页"), QStringLiteral("g1"), QString(), 0, 1},
        {QStringLiteral("b"), QStringLiteral("B页"), QStringLiteral("g1"), QString(), 1, 4},
    };
    const QVector<ui::NavGroup> groups = {{QStringLiteral("g1"), QStringLiteral("G1")}};

    const QVector<ui::NavRow> rows = ui::BuildNavigationRows(pages, groups);
    ASSERT_EQ(rows.size(), 4);
    EXPECT_TRUE(rows[0].is_header);      // g1 有两个页面 -> 有标题
    EXPECT_EQ(rows[1].stack_index, 1);
    EXPECT_EQ(rows[2].stack_index, 4);
    EXPECT_FALSE(rows[3].is_header);     // 未登记分组：排最后，不加标题头
    EXPECT_EQ(rows[3].stack_index, 9);
}

// 集成：AnalysisPanel 的 14 个分析页都登记了 pageId / 分组 / 排序 / 口径，
// 按分组顺序排出来正好是目标导航（播放监看 -> 文件解析 -> 全片质量 -> 报告与工具）。
// 直接对拍字符串，等于把「每个入口指向哪个页面」钉死。
// 注意：该测试只有 AnalysisPanel 的页面，report_tools 组内只有一个页面，
// 按「单页分组不显示标题」的规则它没有标题（真机上 FFmpeg 页也在该组，标题才会出现）。
TEST(NavigationModelTest, AnalysisPanelPagesMapToExpectedNavigation) {
    ASSERT_TRUE(EnsureApp() != nullptr);

    ui::AnalysisPanel panel;
    QStackedWidget stack;
    panel.PopulateStackedWidget(&stack);
    ASSERT_EQ(stack.count(), 14);

    QVector<ui::PageNavigation> pages;
    QStringList ids;
    for (int i = 0; i < stack.count(); ++i) {
        QWidget* w = stack.widget(i);
        ui::PageNavigation nav;
        nav.stack_index = i;
        nav.page_id = w->property("pageId").toString();
        nav.title = w->property("pageTitle").toString();
        nav.group_id = w->property("pageGroup").toString();
        nav.scope = w->property("pageScope").toString();
        nav.order = w->property("pageOrder").toInt();
        EXPECT_FALSE(nav.page_id.isEmpty()) << "第 " << i << " 页缺少 pageId";
        EXPECT_FALSE(nav.group_id.isEmpty()) << "第 " << i << " 页缺少分组";
        EXPECT_FALSE(nav.title.isEmpty()) << "第 " << i << " 页缺少标题";
        EXPECT_FALSE(ids.contains(nav.page_id)) << "pageId 重复: " << nav.page_id.toStdString();
        ids << nav.page_id;
        pages.append(nav);
    }

    const QVector<ui::NavGroup> groups = {
        {QStringLiteral("overview"), QStringLiteral("概览")},
        {QStringLiteral("playback"), QStringLiteral("播放监看")},
        {QStringLiteral("file_parse"), QStringLiteral("文件解析")},
        {QStringLiteral("full_quality"), QStringLiteral("全片质量")},
        {QStringLiteral("report_tools"), QStringLiteral("报告与工具")},
    };
    const QVector<ui::NavRow> rows = ui::BuildNavigationRows(pages, groups);

    QStringList headers;
    QStringList texts;
    for (const ui::NavRow& row : rows) {
        if (row.is_header) headers << row.text;
        else texts << row.text;
    }

    const QStringList expected_headers{QStringLiteral("播放监看（播放期）"),
                                       QStringLiteral("文件解析（全文件）"),
                                       QStringLiteral("全片质量（全文件）")};
    EXPECT_EQ(headers, expected_headers);

    // 页面标题只留标题本身；口径已上提到分组标题（播放监看/全片质量）。
    const QStringList expected_texts{
        QStringLiteral("播放统计与帧包"),           QStringLiteral("运动矢量与块分析"),
        QStringLiteral("场景切换"),                 QStringLiteral("画面质量"),
        QStringLiteral("事件与时间轴"),             QStringLiteral("文件结构"),
        QStringLiteral("编码参数集"),               QStringLiteral("流媒体包"),
        QStringLiteral("字幕与辅助数据"),           QStringLiteral("码率与 GOP"),
        QStringLiteral("音频 QC"),                  QStringLiteral("色彩与 HDR"),
        QStringLiteral("质量诊断"),                 QStringLiteral("报告与批量 QC")};
    EXPECT_EQ(texts, expected_texts);
}
