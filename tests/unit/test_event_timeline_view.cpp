// EventTimelineView 页头「启用分析」聚合开关的初始化回归测试。
//
// 根因: 勾选框在构造函数里就已建好，那时 AnalysisPanel 还没注入功能开关钩子，控件只能
// 按默认值创建；SetFeatureHooks() 注入之后若不回写，界面显示「启用分析」而面板
// feature_enabled_ 里其实是关的 —— 数据处理被静默过滤，界面与实际行为不一致。
//
// 两个页面曾经各挂 3 个同名「启用分析」（异常事件 / 同步分析 / 统一时间轴），
// 与其它页"页头一个开关"对不上。现在收敛成页头一个聚合开关，本测试同时守住：
//   1) 三态（全开 / 全关 / 部分开启）如实反映钩子里的真实状态；
//   2) 注入时的回写属于"初始化同步"，不能伪造出 set_enabled 回调；
//   3) 子页里不再有同名勾选框（否则又退回"一页多开关"）；
//   4) 点击 = 一键全开 / 全关。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTabWidget>
#include <QTableWidget>

#include "core/domain/model/TimelineEvent.h"
#include "ui/analysis_panel/EventTimelineView.h"

namespace {

using videoeye::ui::EventTimelineView;

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_event_timeline_view";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 页头那个唯一的聚合勾选框（objectName 由 AggregateFeatureToggle 统一设置）。
QCheckBox* HeaderToggle(EventTimelineView& view) {
    return view.findChild<QCheckBox*>(QStringLiteral("AnalysisFeatureToggle"));
}

// 按子页标题定位页面（三个子页的表格与摘要都各自嵌在自己那一页里）。
QWidget* FindTabPage(EventTimelineView& view, const QString& title) {
    auto* tabs = view.findChild<QTabWidget*>();
    if (!tabs)
        return nullptr;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == title)
            return tabs->widget(i);
    }
    return nullptr;
}

QTableWidget* TableInPage(QWidget* page) {
    return page ? page->findChild<QTableWidget*>() : nullptr;
}

QLabel* SummaryInPage(QWidget* page, const QString& prefix) {
    if (!page)
        return nullptr;
    for (QLabel* label : page->findChildren<QLabel*>()) {
        if (label->text().startsWith(prefix))
            return label;
    }
    return nullptr;
}

videoeye::model::AnalysisEvent MakeEvent(int index) {
    videoeye::model::AnalysisEvent event;
    event.index = index;
    event.severity = videoeye::model::AnalysisEventSeverity::Error;
    event.type = "时间戳回退";
    event.stream_index = 0;
    event.pts = index * 100;
    event.timestamp_seconds = index;
    event.summary = "summary";
    return event;
}

videoeye::model::SyncSample MakeSyncSample(int index, double diff_ms) {
    videoeye::model::SyncSample sample;
    sample.index = index;
    sample.audio_timestamp_seconds = index * 0.04;
    sample.video_timestamp_seconds = sample.audio_timestamp_seconds - diff_ms / 1000.0;
    sample.diff_ms = diff_ms;
    sample.audio_anchor = true;
    return sample;
}

videoeye::model::TimelineEvent MakeTimelineEvent(int index) {
    videoeye::model::TimelineEvent event;
    event.index = index;
    event.category = videoeye::model::TimelineEventCategory::VideoKeyframe;
    event.timestamp_seconds = index;
    event.label = "关键帧";
    return event;
}

} // namespace

TEST(EventTimelineViewToggleInit, HeaderToggleFollowsInjectedFeatureState) {
    EnsureApp();

    // AnalysisPanel 构造函数的默认值：Event / Sync / Timeline 三个都是关的。
    bool states[3] = {false, false, false};
    int set_enabled_calls = 0;

    EventTimelineView view;
    view.SetFeatureHooks([&states](int feature) { return states[feature]; },
                         [&states, &set_enabled_calls](int feature, bool enabled) {
                             states[feature] = enabled;
                             ++set_enabled_calls;
                         });

    QCheckBox* box = HeaderToggle(view);
    ASSERT_TRUE(box != nullptr) << "页头找不到「启用分析」聚合开关";

    // 1) 注入钩子后必须按真实状态回写 —— 默认全关时界面就应该是未勾选。
    EXPECT_EQ(box->checkState(), Qt::Unchecked);

    // 2) 回写只能改控件外观，不能反过来调用 set_enabled。
    EXPECT_EQ(set_enabled_calls, 0) << "初始化同步不该被当成用户操作回写开关状态";

    // 3) 子页里不该再有同名勾选框：一页一个分析入口。
    auto* tabs = view.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    for (int i = 0; i < tabs->count(); ++i) {
        for (QCheckBox* inner : tabs->widget(i)->findChildren<QCheckBox*>()) {
            EXPECT_NE(inner->text(), QStringLiteral("启用分析"))
                << "子页「" << tabs->tabText(i).toStdString() << "」里又出现了同名勾选框";
        }
    }
}

// 三态语义：全开勾选、全关未勾选、部分开启半选；点击按"还有开着的就全关"收口。
TEST(EventTimelineViewToggleInit, AggregateToggleShowsTriStateAndTogglesAllItems) {
    EnsureApp();

    bool states[3] = {true, false, false};
    int calls = 0;
    EventTimelineView view;
    view.SetFeatureHooks([&states](int feature) { return states[feature]; },
                         [&states, &calls](int feature, bool enabled) {
                             states[feature] = enabled;
                             ++calls;
                         });

    QCheckBox* box = HeaderToggle(view);
    ASSERT_TRUE(box != nullptr);
    // 只开了一项 -> 半选（界面要如实说明"正开着几项"）
    EXPECT_EQ(box->checkState(), Qt::PartiallyChecked);

    // 还有一项开着 -> 点击 = 全关
    box->click();
    EXPECT_FALSE(states[0]);
    EXPECT_FALSE(states[1]);
    EXPECT_FALSE(states[2]);
    EXPECT_EQ(calls, 3) << "聚合开关应把三项一起关掉";
    EXPECT_EQ(box->checkState(), Qt::Unchecked);

    // 全关 -> 点击 = 全开
    box->click();
    EXPECT_TRUE(states[0]);
    EXPECT_TRUE(states[1]);
    EXPECT_TRUE(states[2]);
    EXPECT_EQ(box->checkState(), Qt::Checked);

    // 全开 -> 点击 = 全关
    box->click();
    EXPECT_EQ(box->checkState(), Qt::Unchecked);
}

// 分类枚举驱动表格文本与摘要分组（对应评审 P1：时间轴分类改 enum）。
// 追加三类事件 -> FlushPendingUiUpdates -> 断言表格「类别」列文本与摘要计数。
// 修复前分类是 QString，摘要/曲线按 tr() 文本比较，翻译一变就错位；现在判断走枚举。
TEST(EventTimelineViewTimelineCategory, EnumCategoriesDriveTableAndSummary) {
    EnsureApp();

    bool all_on[3] = {true, true, true};
    EventTimelineView view;
    view.SetFeatureHooks([&all_on](int feature) { return all_on[feature]; }, [](int, bool) {});

    videoeye::model::TimelineEvent keyframe;
    keyframe.index = 0;
    keyframe.category = videoeye::model::TimelineEventCategory::VideoKeyframe;
    keyframe.timestamp_seconds = 1.0;
    keyframe.label = "关键帧 #0";
    view.AppendTimelineEvent(keyframe);

    videoeye::model::TimelineEvent audio;
    audio.index = 1;
    audio.category = videoeye::model::TimelineEventCategory::AudioSample;
    audio.timestamp_seconds = 2.0;
    audio.label = "音频帧 #0";
    view.AppendTimelineEvent(audio);

    videoeye::model::TimelineEvent event;
    event.index = 2;
    event.category = videoeye::model::TimelineEventCategory::Event;
    event.timestamp_seconds = 3.0;
    event.label = "同步异常";
    view.AppendTimelineEvent(event);

    view.FlushPendingUiUpdates();

    // 定位「统一时间轴」子页（表格与摘要都在这一页里）
    auto* tabs = view.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QWidget* timeline_page = nullptr;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == QStringLiteral("统一时间轴")) {
            timeline_page = tabs->widget(i);
            break;
        }
    }
    ASSERT_TRUE(timeline_page != nullptr) << "找不到「统一时间轴」子页";

    // 表格：类别列按枚举映射成显示文本
    auto* table = timeline_page->findChild<QTableWidget*>();
    ASSERT_TRUE(table != nullptr);
    ASSERT_EQ(table->rowCount(), 3);
    EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("视频关键帧"));
    EXPECT_EQ(table->item(1, 1)->text(), QStringLiteral("音频采样"));
    EXPECT_EQ(table->item(2, 1)->text(), QStringLiteral("事件"));

    // 摘要：按枚举分组计数，与表格显示文本解耦
    QLabel* summary = nullptr;
    for (QLabel* label : timeline_page->findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("总数"))) {
            summary = label;
            break;
        }
    }
    ASSERT_TRUE(summary != nullptr) << "找不到时间轴摘要标签";
    EXPECT_EQ(summary->text(), QStringLiteral("总数: 3 | 视频关键帧: 1 | 音频采样: 1 | 异常事件: 1"));
}

// 换媒体时的"清空列表"不受采集开关约束。
//
// 回归：Reset* 以前在开头按开关提前返回，而控制器换媒体时无条件发 Reset
// （见 test_realtime_analysis_controller 的同名契约用例）。于是
// "A 文件采集过 -> 关开关 -> 换 B -> 再开开关" 会把 B 的数据接在 A 的旧行后面，
// 序号也从 0 重新开始，界面上出现两段 0..N。
TEST(EventTimelineViewReset, ResetClearsListsEvenWhenFeaturesAreOff) {
    EnsureApp();

    bool states[3] = {true, true, true};
    EventTimelineView view;
    view.SetFeatureHooks([&states](int feature) { return states[feature]; }, [](int, bool) {});

    QWidget* event_page = FindTabPage(view, QStringLiteral("异常事件"));
    QWidget* sync_page = FindTabPage(view, QStringLiteral("同步分析"));
    QWidget* timeline_page = FindTabPage(view, QStringLiteral("统一时间轴"));
    ASSERT_TRUE(event_page != nullptr);
    ASSERT_TRUE(sync_page != nullptr);
    ASSERT_TRUE(timeline_page != nullptr);

    // 文件 A：开关开启时采集各一条。
    view.AppendAnalysisEvent(MakeEvent(0));
    view.AppendSyncSample(MakeSyncSample(0, 5.0));
    view.AppendTimelineEvent(MakeTimelineEvent(0));
    view.FlushPendingUiUpdates();
    ASSERT_EQ(TableInPage(event_page)->rowCount(), 1);
    ASSERT_EQ(TableInPage(sync_page)->rowCount(), 1);
    ASSERT_EQ(TableInPage(timeline_page)->rowCount(), 1);

    // 关闭开关后换媒体：Reset 必须照常清空。
    states[0] = states[1] = states[2] = false;
    view.ResetAnalysisEventList();
    view.ResetSyncSampleList();
    view.ResetTimelineEventList();
    view.FlushPendingUiUpdates();
    EXPECT_EQ(TableInPage(event_page)->rowCount(), 0) << "关开关时换文件，旧事件仍残留在表里";
    EXPECT_EQ(TableInPage(sync_page)->rowCount(), 0);
    EXPECT_EQ(TableInPage(timeline_page)->rowCount(), 0);
    EXPECT_TRUE(
        SummaryInPage(event_page, QStringLiteral("总事件数"))->text().startsWith(QStringLiteral("总事件数: 0")));
    EXPECT_TRUE(SummaryInPage(sync_page, QStringLiteral("样本数"))->text().startsWith(QStringLiteral("样本数: 0")));
    EXPECT_TRUE(SummaryInPage(timeline_page, QStringLiteral("总数"))->text().startsWith(QStringLiteral("总数: 0")));

    // 开关仍关闭时 Append 照旧被过滤（只放开清空，不放开采集）。
    view.AppendAnalysisEvent(MakeEvent(7));
    view.AppendSyncSample(MakeSyncSample(7, 1.0));
    view.AppendTimelineEvent(MakeTimelineEvent(7));
    view.FlushPendingUiUpdates();
    EXPECT_EQ(TableInPage(event_page)->rowCount(), 0);
    EXPECT_EQ(TableInPage(sync_page)->rowCount(), 0);
    EXPECT_EQ(TableInPage(timeline_page)->rowCount(), 0);

    // 重新开启后采集 B：序号从 7 开始，不能和 A 的旧行拼在一起。
    states[0] = states[1] = states[2] = true;
    view.AppendAnalysisEvent(MakeEvent(7));
    view.AppendSyncSample(MakeSyncSample(7, 1.0));
    view.AppendTimelineEvent(MakeTimelineEvent(7));
    view.FlushPendingUiUpdates();
    ASSERT_EQ(TableInPage(event_page)->rowCount(), 1);
    ASSERT_EQ(TableInPage(sync_page)->rowCount(), 1);
    ASSERT_EQ(TableInPage(timeline_page)->rowCount(), 1);
    EXPECT_EQ(TableInPage(event_page)->item(0, 0)->text(), QStringLiteral("7"));
    EXPECT_EQ(TableInPage(sync_page)->item(0, 0)->text(), QStringLiteral("7"));
    EXPECT_EQ(TableInPage(timeline_page)->item(0, 0)->text(), QStringLiteral("7"));
}

// 到达本地保留上限后只增量移除最旧的行；摘要按"全时段"统计并提示截断。
//
// 回归 1：TrimRecords 到上限后每次 Append 都 setRowCount(0)，下一次刷新重填
// 5000 行 —— 同步样本约 70 条/秒，长视频里会持续整表重建。
// 回归 2：摘要只遍历本地窗口，"平均/最大偏移"会静默退化成"最近一分钟"。
TEST(EventTimelineViewTrim, IncrementalTrimKeepsAllTimeSyncStats) {
    EnsureApp();

    bool all_on[3] = {true, true, true};
    EventTimelineView view;
    view.SetFeatureHooks([&all_on](int feature) { return all_on[feature]; }, [](int, bool) {});

    // 先灌满窗口并刷新一次，让表格行数与记录同步（之后才会真正走到增量 removeRow）。
    for (int i = 0; i < 5000; ++i) {
        view.AppendSyncSample(MakeSyncSample(i, i == 0 ? 500.0 : 0.0));
    }
    view.FlushPendingUiUpdates();

    QWidget* sync_page = FindTabPage(view, QStringLiteral("同步分析"));
    ASSERT_TRUE(sync_page != nullptr);
    QTableWidget* table = TableInPage(sync_page);
    ASSERT_TRUE(table != nullptr);
    ASSERT_EQ(table->rowCount(), 5000);

    // 再追加两条：最旧的两条（含 500ms 大偏移）被增量移出本地窗口。
    view.AppendSyncSample(MakeSyncSample(5000, 0.0));
    view.AppendSyncSample(MakeSyncSample(5001, 0.0));
    EXPECT_EQ(table->rowCount(), 4998) << "裁剪应当立即从表头逐行移除旧行，而不是清空整表";
    view.FlushPendingUiUpdates();

    EXPECT_EQ(table->rowCount(), 5000) << "本地保留窗口应固定在上限";
    ASSERT_TRUE(table->item(0, 0) != nullptr);
    EXPECT_EQ(table->item(0, 0)->text(), QStringLiteral("2")) << "表头应是第 3 条记录，行与记录不能错位";

    QLabel* summary = SummaryInPage(sync_page, QStringLiteral("样本数"));
    ASSERT_TRUE(summary != nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("样本数: 5002"))) << summary->text().toStdString();
    EXPECT_TRUE(summary->text().contains(QStringLiteral("本地保留 5000 条")));
    // 500ms 那条已经被移出本地窗口，但全时段最大偏移必须仍然记得它。
    EXPECT_TRUE(summary->text().contains(QStringLiteral("全时段最大偏移: 500.00 ms"))) << summary->text().toStdString();
    EXPECT_TRUE(summary->text().contains(QStringLiteral("本地列表已达上限"))) << summary->text().toStdString();
}

// 级别列显示走枚举映射（不比较本地化文本）；双击行请求跳到该事件的时间点。
TEST(EventTimelineViewInteraction, SeverityColumnAndDoubleClickSeek) {
    EnsureApp();

    bool all_on[3] = {true, true, true};
    EventTimelineView view;
    view.SetFeatureHooks([&all_on](int feature) { return all_on[feature]; }, [](int, bool) {});

    QWidget* event_page = FindTabPage(view, QStringLiteral("异常事件"));
    ASSERT_TRUE(event_page != nullptr);
    QTableWidget* table = TableInPage(event_page);
    ASSERT_TRUE(table != nullptr);

    // MakeEvent(3) 的时间戳为 3.0，级别为 Error。
    view.AppendAnalysisEvent(MakeEvent(3));
    view.FlushPendingUiUpdates();
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("错误")) << "级别列应由枚举映射成显示文本";

    double seek_seconds = -1.0;
    QObject::connect(&view, &EventTimelineView::SeekRequested,
                     [&seek_seconds](double seconds) { seek_seconds = seconds; });
    // 信号在 Qt5+ 是 public 函数，直接触发等价于用户双击该行。
    table->cellDoubleClicked(0, 1);
    EXPECT_DOUBLE_EQ(seek_seconds, 3.0) << "双击应跳到该事件的时间戳";
}

// 页头「清空」只清本地已采集列表，不受采集开关约束（与帧表 / 包表同口径）。
TEST(EventTimelineViewInteraction, HeaderClearButtonClearsAllThreeLists) {
    EnsureApp();

    bool all_on[3] = {true, true, true};
    EventTimelineView view;
    view.SetFeatureHooks([&all_on](int feature) { return all_on[feature]; }, [](int, bool) {});

    view.AppendAnalysisEvent(MakeEvent(0));
    view.AppendSyncSample(MakeSyncSample(0, 5.0));
    view.AppendTimelineEvent(MakeTimelineEvent(0));
    view.FlushPendingUiUpdates();

    QPushButton* clear = nullptr;
    for (QPushButton* button : view.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("清空")) {
            clear = button;
            break;
        }
    }
    ASSERT_TRUE(clear != nullptr) << "页头找不到「清空」按钮";
    clear->click();

    QWidget* event_page = FindTabPage(view, QStringLiteral("异常事件"));
    QWidget* sync_page = FindTabPage(view, QStringLiteral("同步分析"));
    QWidget* timeline_page = FindTabPage(view, QStringLiteral("统一时间轴"));
    ASSERT_TRUE(event_page != nullptr);
    ASSERT_TRUE(sync_page != nullptr);
    ASSERT_TRUE(timeline_page != nullptr);
    EXPECT_EQ(TableInPage(event_page)->rowCount(), 0);
    EXPECT_EQ(TableInPage(sync_page)->rowCount(), 0);
    EXPECT_EQ(TableInPage(timeline_page)->rowCount(), 0);
}
