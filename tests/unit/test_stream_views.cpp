// 拆出来的页面组件（StreamOverviewView / FramePacketView / MacroblockView）的
// 「启用分析」开关同步，以及 FramePacketView → StreamOverviewView 的 GOP 桥接。
//
// 为什么值得单独测：
//   1. 和 EventTimelineView 同一类坑 —— 「启用分析」QCheckBox 是在组件构造函数里建的，
//      而面板注入 feature 钩子发生在那之后。若 SetFeatureHooks 只记下钩子、不回写控件，
//      界面会显示「已启用」而面板里 feature_enabled_ 其实是关的，结果是数据被静默丢弃、
//      界面毫无提示。这里守住三条：
//        a. 注入钩子后控件状态立刻等于钩子返回的真实状态；
//        b. 回写过程不发 clicked/toggled（否则会被钩子当成用户点击，把状态再翻回去）；
//        c. 点控件时钩子收到的是本组件自己的编号（0/1/2/3），不是面板的枚举值。
//   2. 「播放统计与帧包」页头只有一个聚合开关（流统计 / 视频帧 / 音频帧 / 数据包），
//      下方四张明细子表不再各挂一个同名勾选框 —— 一条断言守住"一页一个分析入口"。
//   3. GOP 摘要在 FramePacketView 产出、StreamOverviewView 消费，两者不互相认识，
//      全靠面板接信号。这条链断了不会崩，只会让「最大GOP大小」与 GOP 分布曲线永远
//      停在上一次的值 —— 属于最难在界面上发现的那类回归。
//
// 用 offscreen 平台跑，不需要显示器；只构造控件、不渲染，所以直接编这几个组件的源。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QString>
#include <QTabWidget>
#include <QWidget>

#include <libavcodec/avcodec.h>

#include <functional>
#include <utility>
#include <vector>

#include "ui/analysis_panel/FramePacketView.h"
#include "ui/analysis_panel/MacroblockView.h"
#include "ui/analysis_panel/StreamOverviewView.h"

using videoeye::ui::FramePacketView;
using videoeye::ui::GopSummary;
using videoeye::ui::MacroblockView;
using videoeye::ui::StreamOverviewView;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_stream_views";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 记录「开关被点」的回调，用来验证编号映射与是否误触发
struct ToggleRecorder {
    std::vector<std::pair<int, bool>> calls;

    std::function<void(int, bool)> Sink() {
        return [this](int id, bool enabled) { calls.emplace_back(id, enabled); };
    }
};

// 由一个「编号 -> 状态」表驱动的 is_enabled 钩子
std::function<bool(int)> StatesHook(const std::vector<bool>& states, bool fallback = true) {
    return [states, fallback](int id) {
        if (id < 0 || id >= static_cast<int>(states.size()))
            return fallback;
        return states[static_cast<std::size_t>(id)];
    };
}

} // namespace

// ---------------------------------------------------------------------------
// StreamOverviewView（页头聚合开关，覆盖 0..3 = 流统计 / 视频帧 / 音频帧 / 数据包）
// ---------------------------------------------------------------------------

TEST(StreamViewsToggleTests, PlaybackHeaderToggleAggregatesFourFeatures) {
    EnsureApp();
    StreamOverviewView view;
    ToggleRecorder rec;

    QCheckBox* toggle = view.findChild<QCheckBox*>(QStringLiteral("AnalysisFeatureToggle"));
    ASSERT_TRUE(toggle != nullptr) << "页头找不到「启用分析」聚合开关";

    // 面板默认: StreamStats=开, VideoFrame=开, AudioFrame=关, Packet=关 -> 半选
    bool states[4] = {true, true, false, false};
    view.SetFeatureHooks([&states](int id) { return states[id]; },
                         [&states, &rec](int id, bool enabled) {
                             states[id] = enabled;
                             rec.calls.emplace_back(id, enabled);
                         });
    EXPECT_EQ(toggle->checkState(), Qt::PartiallyChecked);
    // 回写不应该被当成用户操作
    EXPECT_TRUE(rec.calls.empty());

    // 全开 -> 勾选；全关 -> 未勾选（反向验证，排除"恒半选"的假通过）
    for (bool& s : states)
        s = true;
    view.SetFeatureHooks([&states](int id) { return states[id]; },
                         [&states, &rec](int id, bool enabled) {
                             states[id] = enabled;
                             rec.calls.emplace_back(id, enabled);
                         });
    EXPECT_EQ(toggle->checkState(), Qt::Checked);

    for (bool& s : states)
        s = false;
    view.SetFeatureHooks([&states](int id) { return states[id]; },
                         [&states, &rec](int id, bool enabled) {
                             states[id] = enabled;
                             rec.calls.emplace_back(id, enabled);
                         });
    EXPECT_EQ(toggle->checkState(), Qt::Unchecked);
    EXPECT_TRUE(rec.calls.empty()) << "初始化同步不该伪造用户操作";
}

// 点击聚合开关 = 一键全开 / 全关，四个采集项一起变。
TEST(StreamViewsToggleTests, PlaybackHeaderToggleFlipsAllItems) {
    EnsureApp();
    StreamOverviewView view;
    ToggleRecorder rec;

    bool states[4] = {true, true, false, false};
    view.SetFeatureHooks([&states](int id) { return states[id]; },
                         [&states, &rec](int id, bool enabled) {
                             states[id] = enabled;
                             rec.calls.emplace_back(id, enabled);
                         });

    QCheckBox* toggle = view.findChild<QCheckBox*>(QStringLiteral("AnalysisFeatureToggle"));
    ASSERT_TRUE(toggle != nullptr);

    // 当前有开着的项 -> 点击全关
    toggle->click();
    ASSERT_EQ(rec.calls.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(rec.calls[static_cast<std::size_t>(i)].first, i);
        EXPECT_FALSE(rec.calls[static_cast<std::size_t>(i)].second);
    }
    EXPECT_EQ(toggle->checkState(), Qt::Unchecked);

    // 全关 -> 点击全开
    rec.calls.clear();
    toggle->click();
    ASSERT_EQ(rec.calls.size(), 4u);
    for (const auto& call : rec.calls)
        EXPECT_TRUE(call.second);
    EXPECT_EQ(toggle->checkState(), Qt::Checked);
}

// ---------------------------------------------------------------------------
// FramePacketView：四张明细子表不再各挂一个「启用分析」（整页一个开关在页头）
// ---------------------------------------------------------------------------

TEST(StreamViewsToggleTests, FramePacketSubPagesHaveNoAnalysisToggle) {
    EnsureApp();
    FramePacketView view;

    QTabWidget* tabs = view.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    ASSERT_EQ(tabs->count(), 4); // 视频帧 / 包 / GOP 摘要 / 音频帧
    for (int i = 0; i < tabs->count(); ++i) {
        for (QCheckBox* box : tabs->widget(i)->findChildren<QCheckBox*>()) {
            EXPECT_NE(box->text(), QStringLiteral("启用分析"))
                << "子页「" << tabs->tabText(i).toStdString() << "」里又出现了同名勾选框，"
                << "与「一页一个分析入口」的规范冲突";
        }
    }
}

// GOP 摘要从 FramePacketView 产出，经 GopSummariesChanged 交给面板再转给流概览区。
// 这里只验产出侧：有变化就发一次、无变化不发、内容能被取到。
TEST(StreamViewsToggleTests, FramePacketEmitsGopSummariesOnlyWhenChanged) {
    EnsureApp();
    FramePacketView view;

    int gop_signals = 0;
    QObject::connect(&view, &FramePacketView::GopSummariesChanged, [&gop_signals]() { ++gop_signals; });

    // 第一帧是关键帧 -> 开一个新 GOP 段
    view.AppendVideoFrame(0, AV_PICTURE_TYPE_I, true, 0, 0.0);
    view.AppendVideoFrame(1, AV_PICTURE_TYPE_P, false, 40, 0.04);
    view.AppendVideoFrame(2, AV_PICTURE_TYPE_B, false, 80, 0.08);
    EXPECT_TRUE(view.HasPending());

    view.FlushPending();
    EXPECT_EQ(gop_signals, 1);
    ASSERT_EQ(view.GopSummaries().size(), 1u);
    EXPECT_EQ(view.GopSummaries()[0].total_frames, 3);
    EXPECT_EQ(view.GopSummaries()[0].i_count, 1);
    EXPECT_EQ(view.GopSummaries()[0].p_count, 1);
    EXPECT_EQ(view.GopSummaries()[0].b_count, 1);
    EXPECT_FALSE(view.HasPending());

    // 没有新数据时不该重复发信号
    view.FlushPending();
    EXPECT_EQ(gop_signals, 1);

    // 第二个关键帧 -> 开第二个 GOP 段，再发一次
    view.AppendVideoFrame(3, AV_PICTURE_TYPE_I, true, 120, 0.12);
    view.FlushPending();
    EXPECT_EQ(gop_signals, 2);
    EXPECT_EQ(view.GopSummaries().size(), 2u);

    // 换文件清空后同样要发信号，否则流概览区的「最大GOP大小」会一直停在旧值
    view.ResetVideoFrames();
    view.FlushPending();
    EXPECT_EQ(gop_signals, 3);
    EXPECT_TRUE(view.GopSummaries().empty());
}

// 消费侧：拿到 GOP 摘要后刷新，不应崩溃，且清空后再刷新也能回到空态
TEST(StreamViewsToggleTests, StreamOverviewAcceptsGopSummaries) {
    EnsureApp();
    StreamOverviewView view;

    std::vector<GopSummary> sums(3);
    sums[0].gop_index = 1;
    sums[0].total_frames = 30;
    sums[1].gop_index = 2;
    sums[1].total_frames = 45;
    sums[2].gop_index = 3;
    sums[2].total_frames = 30;
    view.SetGopSummaries(sums);
    EXPECT_TRUE(view.HasPending());
    view.FlushPending();
    EXPECT_FALSE(view.HasPending());

    view.SetGopSummaries({});
    view.FlushPending();
    EXPECT_FALSE(view.HasPending());

    // 换文件时曲线清零（面板在 ResetVideoFrameList 里调用）
    view.ResetCharts();
}

// ---------------------------------------------------------------------------
// MacroblockView（宏块分析，组件内编号 0）
// ---------------------------------------------------------------------------

TEST(StreamViewsToggleTests, MacroblockToggleFollowsInjectedFeatureState) {
    EnsureApp();
    MacroblockView view;
    ToggleRecorder rec;

    QCheckBox* toggle = view.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle != nullptr);

    // 面板里 Macroblock 默认是关的
    view.SetFeatureHooks(StatesHook({false}), rec.Sink());
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_TRUE(rec.calls.empty());

    toggle->setChecked(true);
    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].first, 0);
    EXPECT_TRUE(rec.calls[0].second);
}
