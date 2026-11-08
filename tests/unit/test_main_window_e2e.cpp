// 主窗口端到端冒烟（Qt offscreen）
//
// 为什么要有这一层
// ----------------
// 打开会话的事务式异步链路（OpenSessionController -> MediaPlayer::OpenAsync ->
// 队列投递回 UI -> MainWindow 收尾）在单元测试里被拆成了若干纯口径用例
// （OpenSessionTests / PlayerOpenAsyncTests / 资源稳定性）。它们各自都绿，却都
// 不回答"把真实的 MainWindow 接起来之后，用户到底看到了什么"：
//   * 状态栏有没有从"就绪"切到"已打开 / 无法播放"；
//   * 应用栏的媒体路径标签有没有跟着变；
//   * 首页是否登记了文件、完整媒体信息是否从占位文案换成了解析结果；
//   * 取消扫描之后，扫描任务条会不会一直停在"取消中…"。
// 这些断点全在 QWidget 之间的事件流里，纯口径测试覆盖不到；而它们断掉时程序
// 不崩、不报错，只是界面"看起来没反应"。
//
// 判据只依赖公开可观察面：状态栏文案、ElidedLabel::FullText()、OverviewPage::
// HasFile()、QTextEdit 的纯文本、AnalysisPanel 的扫描快照、ScanTaskBar 的
// 可见性/取消态。不截图、不比对像素（offscreen 字体度量与真实平台有差异）。
//
// 预算取得比产品阈值宽：这条用例要抓的是"永远不收敛"，不是毫秒级抖动。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QStatusBar>
#include <QTextEdit>
#include <QThread>

#include <functional>
#include <string>

#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/main_window/ElidedLabel.h"
#include "ui/main_window/MainWindow.h"
#include "ui/main_window/OverviewPage.h"
#include "ui/main_window/ScanTaskBar.h"

#include "HttpTestServer.h"

#ifndef VIDEOEYE_CORPUS_DIR
#error "本用例依赖真实语料: 需要在 CMake 里定义 VIDEOEYE_CORPUS_DIR"
#endif

namespace {

using namespace videoeye::ui;

// 打开（含后台 avformat 探测 + 收尾回 UI）的收敛预算。产品侧 kOpenBudgetMs 是
// 3 秒；这里放宽到 5 秒，避免 CI 上首次加载 DLL / 冷文件缓存造成假红。
constexpr int kOpenBudgetMs = 5000;
// 全文件扫描的收敛预算（93KB 的合成样本，正常在几十毫秒内完成）。
constexpr int kScanBudgetMs = 8000;
// 取消后 UI 回到空闲的预算：这条断言守的是"取消请求被接住且任务真的收尾"，
// 不是取消延迟的精确值（精确值由第三批的取消延迟探针单独测）。
constexpr int kCancelBudgetMs = 5000;

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // 本地 HTTP 用例：清掉进程内代理变量，否则系统代理会把 127.0.0.1 的请求改道。
    qunsetenv("http_proxy");
    qunsetenv("https_proxy");
    qputenv("no_proxy", "127.0.0.1,localhost");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_main_window_e2e";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QString CorpusPath(const char* relative) {
    return QString(VIDEOEYE_CORPUS_DIR) + QStringLiteral("/") + QString::fromUtf8(relative);
}

std::string ReadCorpusBytes(const char* relative) {
    QFile file(CorpusPath(relative));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

// 泵事件直到 predicate 为真或超时。打开链路的结果都经 Qt 队列投递回 UI 线程，
// 不跑事件循环就永远观察不到（这也是"只 sleep 不 processEvents"的用例最常见的假红）。
bool PumpUntil(const std::function<bool()>& predicate, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() >= timeout_ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return true;
}

// 每个用例都要的四个观察点：首页、分析面板、扫描任务条、应用栏媒体路径标签。
struct Observers {
    OverviewPage* overview = nullptr;
    AnalysisPanel* panel = nullptr;
    ScanTaskBar* bar = nullptr;
    ElidedLabel* media_path = nullptr;

    bool Attach(MainWindow& window) {
        overview = window.findChild<OverviewPage*>();
        panel = window.findChild<AnalysisPanel*>();
        bar = window.findChild<ScanTaskBar*>();
        media_path = window.findChild<ElidedLabel*>(QStringLiteral("MediaPath"));
        return overview && panel && bar && media_path;
    }
};

// 打开一个真实 MP4：状态栏、应用栏、首页摘要、完整媒体信息都必须落到"已打开"。
TEST(MainWindowE2E, OpensRealMediaAndShowsParseResult) {
    EnsureApp();
    MainWindow window;
    window.show();
    QApplication::processEvents();

    Observers obs;
    ASSERT_TRUE(obs.Attach(window));

    const QString mp4 = CorpusPath("container/real_h264_aac_faststart.mp4");
    ASSERT_TRUE(QFileInfo::exists(mp4)) << mp4.toStdString();
    ASSERT_TRUE(window.OpenMedia(mp4, /*autoplay=*/false));

    ASSERT_TRUE(PumpUntil([&]() { return window.statusBar()->currentMessage().contains(QStringLiteral("已打开:")); },
                          kOpenBudgetMs))
        << "打开后状态栏没有切到「已打开: …」，实际: " << window.statusBar()->currentMessage().toStdString();

    // 结果展示：首页登记了文件、应用栏显示的是"媒体路径"而不是失败标签。
    EXPECT_TRUE(obs.overview->HasFile()) << "打开成功但首页没有登记文件（SetFileSummary 没跑到？）";
    EXPECT_TRUE(obs.media_path->FullText().contains(QStringLiteral("real_h264_aac_faststart.mp4")))
        << "应用栏媒体路径标签: " << obs.media_path->FullText().toStdString();
    EXPECT_FALSE(obs.media_path->FullText().contains(QStringLiteral("无法播放")));

    // 完整媒体信息必须从占位文案换成后台探测的结果：这是"解析完成"的可观察终态。
    QTextEdit* info = obs.overview->findChild<QTextEdit*>();
    ASSERT_NE(info, nullptr) << "首页缺少完整媒体信息文本框";
    ASSERT_TRUE(PumpUntil(
        [&]() {
            const QString text = info->toPlainText();
            return !text.isEmpty() && !text.contains(QStringLiteral("正在解析媒体信息"));
        },
        kOpenBudgetMs))
        << "媒体信息一直停在占位文案: " << info->toPlainText().left(120).toStdString();
}

// 打开一个不存在的文件：不弹模态框、不进死循环，仍然进入"分析模式"，且错误提示
// 落在状态栏（timeout=0，常驻）与应用栏标签上。
TEST(MainWindowE2E, MissingFileEntersAnalysisModeWithErrorHint) {
    EnsureApp();
    MainWindow window;
    window.show();
    QApplication::processEvents();

    Observers obs;
    ASSERT_TRUE(obs.Attach(window));

    const QString missing = CorpusPath("container/no_such_file_for_e2e.mp4");
    ASSERT_FALSE(QFileInfo::exists(missing));
    ASSERT_TRUE(window.OpenMedia(missing, /*autoplay=*/false));

    ASSERT_TRUE(PumpUntil(
        [&]() { return window.statusBar()->currentMessage().contains(QStringLiteral("无法播放 (已进入分析模式)")); },
        kOpenBudgetMs))
        << "失败打开没有给出「无法播放 (已进入分析模式)」提示，实际: "
        << window.statusBar()->currentMessage().toStdString();

    // 失败也必须把文件名留在首页（分析模式），不能继续显示上一个文件的结论。
    EXPECT_TRUE(obs.overview->HasFile()) << "分析模式没有登记文件名";
    EXPECT_TRUE(obs.media_path->FullText().contains(QStringLiteral("(无法播放)")) ||
                obs.media_path->FullText().contains(QStringLiteral("无法播放")))
        << "应用栏没有标记无法播放: " << obs.media_path->FullText().toStdString();
    EXPECT_TRUE(obs.media_path->FullText().contains(QStringLiteral("no_such_file_for_e2e.mp4")));
}

// 本地 HLS 清单（引用的分片不存在）：打开链路必须在预算内给出终态（成功或分析模式），
// 不能挂在 avformat 探测上让状态栏永远停在"就绪"。
TEST(MainWindowE2E, ManifestOpenConvergesWithinBudget) {
    EnsureApp();
    MainWindow window;
    window.show();
    QApplication::processEvents();

    Observers obs;
    ASSERT_TRUE(obs.Attach(window));

    const QString manifest = CorpusPath("streaming/hls_bom_lowercase.m3u8");
    ASSERT_TRUE(QFileInfo::exists(manifest)) << manifest.toStdString();
    ASSERT_TRUE(window.OpenMedia(manifest, /*autoplay=*/false));

    ASSERT_TRUE(PumpUntil(
        [&]() {
            const QString msg = window.statusBar()->currentMessage();
            return msg.contains(QStringLiteral("已打开:")) || msg.contains(QStringLiteral("无法播放 (已进入分析模式)"));
        },
        kOpenBudgetMs))
        << "清单打开没有收敛，状态栏仍是: " << window.statusBar()->currentMessage().toStdString();

    EXPECT_TRUE(obs.overview->HasFile());
    EXPECT_TRUE(obs.media_path->FullText().contains(QStringLiteral("hls_bom_lowercase.m3u8")))
        << "应用栏媒体路径标签: " << obs.media_path->FullText().toStdString();
}

// 取消扫描：走真实接线（任务条按钮 -> MainWindow -> AnalysisPanel），
// 取消之后任务条必须隐藏、"取消中"标记必须清掉，绝不停在「取消中…（后台仍在收尾）」。
TEST(MainWindowE2E, CancelScanThroughTaskBarReturnsToIdle) {
    EnsureApp();
    MainWindow window;
    window.show();
    QApplication::processEvents();

    Observers obs;
    ASSERT_TRUE(obs.Attach(window));

    const QString mp4 = CorpusPath("container/real_h264_aac_faststart.mp4");
    ASSERT_TRUE(QFileInfo::exists(mp4)) << mp4.toStdString();
    ASSERT_TRUE(window.OpenMedia(mp4, /*autoplay=*/false));

    ASSERT_TRUE(PumpUntil([&]() { return window.statusBar()->currentMessage().contains(QStringLiteral("已打开:")); },
                          kOpenBudgetMs));
    // 打开即分析：先等这次自动扫描收敛，保证下面发起的是"干净的一次新扫描"。
    ASSERT_TRUE(PumpUntil([&]() { return !obs.panel->IsScanRunning(); }, kScanBudgetMs))
        << "打开后的自动扫描一直不收敛";

    // 重新发起一次扫描：OnScanStarted 同步置扫描态，任务条应在这一瞬间出现。
    obs.panel->StartDiagnosticsScanForCurrentFile();
    ASSERT_TRUE(obs.panel->IsScanRunning()) << "StartDiagnosticsScanForCurrentFile 没有进入扫描态";
    EXPECT_TRUE(obs.bar->isVisible()) << "扫描已开始，跨页面任务条却没有显示";

    // 点任务条上的取消按钮，而不是直接调面板 API —— 要守的正是这条接线。
    QPushButton* cancel_button = nullptr;
    for (QPushButton* button : obs.bar->findChildren<QPushButton*>()) {
        if (button->text().contains(QStringLiteral("取消扫描")))
            cancel_button = button;
    }
    ASSERT_NE(cancel_button, nullptr) << "任务条上找不到「取消扫描」按钮";
    cancel_button->click();
    EXPECT_TRUE(obs.bar->IsCancelling()) << "点了取消，任务条没有进入「取消中」";

    // 终态必须回到空闲；任务条随之隐藏并清掉取消标记（SetScanState(false) 负责）。
    EXPECT_TRUE(PumpUntil([&]() { return !obs.panel->IsScanRunning(); }, kCancelBudgetMs))
        << "取消后扫描一直不收敛（IsScanRunning 仍为 true）";
    EXPECT_TRUE(PumpUntil([&]() { return !obs.bar->isVisible(); }, kCancelBudgetMs)) << "取消收敛后任务条仍然可见";
    EXPECT_FALSE(obs.bar->IsCancelling()) << "任务条停在「取消中」，没有清掉 cancelling_";
}

// URL 打开成功：走 OpenSessionController 的通用媒体分支（不是 Raw/PCM），
// 状态栏、应用栏、首页三处观察点必须一致地落到"已打开"。
TEST(MainWindowE2E, OpensHttpUrlAndShowsItInAppBar) {
    EnsureApp();

    const std::string body = ReadCorpusBytes("container/real_h264_aac_faststart.mp4");
    ASSERT_FALSE(body.empty()) << "缺少真实语料 real_h264_aac_faststart.mp4";
    videoeye_test::HttpTestServer server({
        videoeye_test::HttpTestServer::BodyRoute("/clip.mp4", body, "video/mp4"),
    });
    ASSERT_TRUE(server.valid()) << "无法启动本地 HTTP 服务";

    MainWindow window;
    window.show();
    QApplication::processEvents();

    Observers obs;
    ASSERT_TRUE(obs.Attach(window));

    const QString url = QString::fromStdString(server.url("/clip.mp4"));
    ASSERT_TRUE(window.OpenMedia(url, /*autoplay=*/false));

    ASSERT_TRUE(PumpUntil([&]() { return window.statusBar()->currentMessage().contains(QStringLiteral("已打开:")); },
                          kOpenBudgetMs))
        << "URL 打开后状态栏没有切到「已打开: …」，实际: " << window.statusBar()->currentMessage().toStdString();

    EXPECT_TRUE(obs.media_path->FullText().contains(QStringLiteral("http://127.0.0.1:")))
        << "应用栏没有显示 URL: " << obs.media_path->FullText().toStdString();
    EXPECT_TRUE(obs.overview->HasFile()) << "URL 打开成功但首页没有登记文件";
}

// URL 404：与本地文件失败同样收尾为"分析模式"，状态栏常驻错误提示、
// 应用栏标记无法播放 —— 不能停在"正在打开"。
TEST(MainWindowE2E, HttpNotFoundShowsAnalysisModeHint) {
    EnsureApp();

    videoeye_test::HttpTestServer server({}); // 未登记路径一律 404
    ASSERT_TRUE(server.valid());

    MainWindow window;
    window.show();
    QApplication::processEvents();

    Observers obs;
    ASSERT_TRUE(obs.Attach(window));

    ASSERT_TRUE(window.OpenMedia(QString::fromStdString(server.url("/missing.mp4")), /*autoplay=*/false));

    ASSERT_TRUE(PumpUntil(
        [&]() { return window.statusBar()->currentMessage().contains(QStringLiteral("无法播放 (已进入分析模式)")); },
        kOpenBudgetMs))
        << "URL 404 没有给出分析模式提示，实际: " << window.statusBar()->currentMessage().toStdString();

    EXPECT_TRUE(obs.overview->HasFile()) << "分析模式没有登记 URL";
    EXPECT_TRUE(obs.media_path->FullText().contains(QStringLiteral("无法播放")))
        << "应用栏没有标记无法播放: " << obs.media_path->FullText().toStdString();
}

} // namespace
