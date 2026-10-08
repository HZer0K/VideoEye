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

#include <functional>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QThread>

#include "core/domain/model/AnalysisFeature.h"
#include "ui/analysis_panel/AnalysisPanel.h"
#include "ui/analysis_panel/AudioQcPage.h"
#include "ui/analysis_panel/BitrateGopPage.h"
#include "ui/analysis_panel/ColorHdrPage.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/analysis_panel/SubtitleAuxPage.h"
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/main_window/macroblock_coordination.h"
#include "ui/main_window/navigation_model.h"
#include "ui/streaming_panel/StreamingPanel.h"

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
