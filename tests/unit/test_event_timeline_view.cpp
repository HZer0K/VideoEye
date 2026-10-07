// EventTimelineView 开关初始化回归测试（对应评审 P1-4）
//
// 根因: 三个「启用分析」QCheckBox 在构造函数里就已建好，那时 AnalysisPanel 还没注入
// 功能开关钩子，控件只能按默认值创建；SetFeatureHooks() 注入之后又不回写，于是
// 界面显示「启用分析」，而面板 feature_enabled_ 里其实是关的 —— 数据处理被静默过滤，
// 用户看到的界面与实际行为不一致。
//
// 本测试直接用 QApplication(offscreen) 构造视图并注入钩子，断言：
//   1) 三个勾选框的勾选态 == 钩子里的真实状态（而不是构造函数留下的默认值）；
//   2) 注入时的回写属于"初始化同步"，不能伪造出 FeatureToggled / set_enabled 回调
//      （那是用户点开关才该有的语义）；
//   3) 真实状态为开时控件也跟着开 —— 反向验证，排除"恒为未勾选"的假通过。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QString>
#include <QTableWidget>
#include <QTabWidget>

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

// 按子页标题定位该页的「启用分析」勾选框。
// 之所以不按 findChildren 的顺序取：页签的创建顺序（异常事件 / 统一时间轴 / 同步分析）
// 与侧边栏期望顺序（Event / Sync / Timeline）并不一致，靠下标会写出一条脆弱的测试。
QCheckBox* ToggleOf(EventTimelineView& view, const QString& tab_text) {
    auto* tabs = view.findChild<QTabWidget*>();
    if (!tabs) return nullptr;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) != tab_text) continue;
        QWidget* page = tabs->widget(i);
        return page ? page->findChild<QCheckBox*>() : nullptr;
    }
    return nullptr;
}

} // namespace

TEST(EventTimelineViewToggleInit, TogglesFollowInjectedFeatureState) {
    EnsureApp();

    // AnalysisPanel 构造函数的默认值：Event / Sync / Timeline 三个都是关的。
    bool states[3] = {false, false, false};
    int set_enabled_calls = 0;

    EventTimelineView view;
    int toggled_signals = 0;
    QObject::connect(&view, &EventTimelineView::FeatureToggled,
                     [&toggled_signals](int, bool) { ++toggled_signals; });

    view.SetFeatureHooks([&states](int feature) { return states[feature]; },
                         [&states, &set_enabled_calls](int feature, bool enabled) {
                             states[feature] = enabled;
                             ++set_enabled_calls;
                         });

    QCheckBox* event_box = ToggleOf(view, QStringLiteral("异常事件"));
    QCheckBox* sync_box = ToggleOf(view, QStringLiteral("同步分析"));
    QCheckBox* timeline_box = ToggleOf(view, QStringLiteral("统一时间轴"));
    ASSERT_TRUE(event_box != nullptr) << "找不到「异常事件」页的启用开关";
    ASSERT_TRUE(sync_box != nullptr) << "找不到「同步分析」页的启用开关";
    ASSERT_TRUE(timeline_box != nullptr) << "找不到「统一时间轴」页的启用开关";

    // 1) 注入钩子后必须按真实状态回写 —— 默认全关时界面就应该是未勾选。
    //    修复前这里三条全是 checked（构造函数里 is_enabled_ 还是空的，落到默认 true）。
    EXPECT_FALSE(event_box->isChecked());
    EXPECT_FALSE(sync_box->isChecked());
    EXPECT_FALSE(timeline_box->isChecked());

    // 2) 回写只能改控件外观，不能反过来调用 set_enabled 或发 FeatureToggled。
    EXPECT_EQ(set_enabled_calls, 0) << "初始化同步不该被当成用户操作回写开关状态";
    EXPECT_EQ(toggled_signals, 0) << "初始化同步不该触发 FeatureToggled";

    // 3) 反向验证：真实状态为开时，控件必须跟着勾上。
    //    否则上面三条 EXPECT_FALSE 用"恒不勾选"也能过，测试就失去意义了。
    bool all_on[3] = {true, true, true};
    EventTimelineView view_on;
    view_on.SetFeatureHooks([&all_on](int feature) { return all_on[feature]; },
                            [](int, bool) {});

    QCheckBox* event_on = ToggleOf(view_on, QStringLiteral("异常事件"));
    QCheckBox* sync_on = ToggleOf(view_on, QStringLiteral("同步分析"));
    QCheckBox* timeline_on = ToggleOf(view_on, QStringLiteral("统一时间轴"));
    ASSERT_TRUE(event_on != nullptr);
    ASSERT_TRUE(sync_on != nullptr);
    ASSERT_TRUE(timeline_on != nullptr);
    EXPECT_TRUE(event_on->isChecked());
    EXPECT_TRUE(sync_on->isChecked());
    EXPECT_TRUE(timeline_on->isChecked());
}

// 分类枚举驱动表格文本与摘要分组（对应评审 P1：时间轴分类改 enum）。
// 追加三类事件 -> FlushPendingUiUpdates -> 断言表格「类别」列文本与摘要计数。
// 修复前分类是 QString，摘要/曲线按 tr() 文本比较，翻译一变就错位；现在判断走枚举。
TEST(EventTimelineViewTimelineCategory, EnumCategoriesDriveTableAndSummary) {
    EnsureApp();

    bool all_on[3] = {true, true, true};
    EventTimelineView view;
    view.SetFeatureHooks([&all_on](int feature) { return all_on[feature]; },
                         [](int, bool) {});

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
    EXPECT_EQ(summary->text(),
              QStringLiteral("事件数: 3 | 视频关键帧: 1 | 音频采样: 1 | 异常事件: 1"));
}
