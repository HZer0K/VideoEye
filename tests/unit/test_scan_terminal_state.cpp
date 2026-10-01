// 扫描终态必须复位所有共用页面（复查 P1-4）
//
// 根因(已修): DiagnosticsPage::OnFacadeFailed 只重置诊断页自身，**不发任何终态信号**。
// 而码率/GOP、音频 QC、色彩/HDR 三页共用同一次扫描，它们的扫描态靠面板转发
// 扫描生命周期来维护 —— 于是失败之后这三页永远停在扫描态：取消按钮还亮着、
// "开始分析"按钮永久禁用，与诊断页显示的"失败"互相矛盾，用户只能重开文件才恢复。
//
// 修复: 三个终态（完成 / 取消 / 失败）统一成一个 ScanEnded(reason) 事件，且每次扫描
// 恰好发一次；面板在同一个处理函数里复位共用页的按钮与进度（失败时不分发空结果）。
// 「用户点取消时立刻发一个假终态」也一并去掉了 —— 那一刻任务其实还在跑。
//
// 这里跑一次真实的失败扫描（损坏输入），断言三个页面都回到了 Idle。
// 用 offscreen 平台，不需要显示器。
//
// 顺带覆盖: 这个测试第一次跑起来就撞出了 d621be1 引入的启动闪退 ——
// DiagnosticsPage 构造函数把 SetupUi() 放在 facade_ 之前，而 SetupUi() 末尾的
// RebuildRuleTable() 要读 facade_->rules()，即对空指针调成员函数。构造这个页
// 本身就会崩，所以"能构造出来"也是本测试的一条隐含断言。

#include <gtest/gtest.h>

#include <functional>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStackedWidget>
#include <QString>
#include <QThread>

#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/analysis_panel/AudioQcPage.h"
#include "ui/analysis_panel/BitrateGopPage.h"
#include "ui/analysis_panel/ColorHdrPage.h"
#include "ui/analysis_panel/DiagnosticsPage.h"

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
    ASSERT_TRUE(diagnostics != nullptr);
    ASSERT_TRUE(bitrate_gop != nullptr);
    ASSERT_TRUE(audio_qc != nullptr);
    ASSERT_TRUE(color_hdr != nullptr);

    int terminal_count = 0;
    auto last_reason = ui::DiagnosticsPage::ScanEndReason::Completed;
    QObject::connect(diagnostics, &ui::DiagnosticsPage::ScanEnded, diagnostics,
                     [&](ui::DiagnosticsPage::ScanEndReason reason) {
                         last_reason = reason;
                         ++terminal_count;
                     });

    diagnostics->SetSourcePath(corrupt);
    diagnostics->StartScan(diagnostics->options());

    // 扫描一开始，三页必须进入扫描态 —— 否则"结束时复位"这句话没有任何意义。
    EXPECT_TRUE(bitrate_gop->IsScanActive());
    EXPECT_TRUE(audio_qc->IsScanActive());
    EXPECT_TRUE(color_hdr->IsScanActive());

    ASSERT_TRUE(PumpUntil([&] { return terminal_count > 0; }, 30000))
        << "损坏输入没有在超时前产生扫描终态";

    EXPECT_EQ(1, terminal_count) << "一次扫描只应产生一个终态事件";
    EXPECT_TRUE(last_reason == ui::DiagnosticsPage::ScanEndReason::Failed)
        << "损坏输入必须走 Failed 终态";

    // 核心断言: 共用同一次扫描的三页都必须回到 Idle。
    // 修复前它们会一直停在扫描态（取消按钮亮着、"开始分析"永久禁用）。
    EXPECT_FALSE(bitrate_gop->IsScanActive()) << "码率/GOP 页仍停在扫描中";
    EXPECT_FALSE(audio_qc->IsScanActive()) << "音频 QC 页仍停在扫描中";
    EXPECT_FALSE(color_hdr->IsScanActive()) << "色彩/HDR 页仍停在扫描中";

    QFile::remove(corrupt);
}
