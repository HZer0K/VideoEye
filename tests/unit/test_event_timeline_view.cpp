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

} // namespace

TEST(EventTimelineViewToggleInit, HeaderToggleFollowsInjectedFeatureState) {
    EnsureApp();

    // AnalysisPanel 构造函数的默认值：Event / Sync / Timeline 三个都是关的。
    bool states[3] = {false, false, false};
    int set_enabled_calls = 0;

    EventTimelineView view;
    int toggled_signals = 0;
    QObject::connect(&view, &EventTimelineView::FeatureToggled, [&toggled_signals](int, bool) { ++toggled_signals; });

    view.SetFeatureHooks([&states](int feature) { return states[feature]; },
                         [&states, &set_enabled_calls](int feature, bool enabled) {
                             states[feature] = enabled;
                             ++set_enabled_calls;
                         });

    QCheckBox* box = HeaderToggle(view);
    ASSERT_TRUE(box != nullptr) << "页头找不到「启用分析」聚合开关";

    // 1) 注入钩子后必须按真实状态回写 —— 默认全关时界面就应该是未勾选。
    EXPECT_EQ(box->checkState(), Qt::Unchecked);

    // 2) 回写只能改控件外观，不能反过来调用 set_enabled 或发 FeatureToggled。
    EXPECT_EQ(set_enabled_calls, 0) << "初始化同步不该被当成用户操作回写开关状态";
    EXPECT_EQ(toggled_signals, 0) << "初始化同步不该触发 FeatureToggled";

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
        if (label->text().contains(QStringLiteral("事件数"))) {
            summary = label;
            break;
        }
    }
    ASSERT_TRUE(summary != nullptr) << "找不到时间轴摘要标签";
    EXPECT_EQ(summary->text(), QStringLiteral("事件数: 3 | 视频关键帧: 1 | 音频采样: 1 | 异常事件: 1"));
}
