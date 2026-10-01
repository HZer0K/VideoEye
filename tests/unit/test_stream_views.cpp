// 拆出来的三个页面组件（StreamOverviewView / FramePacketView / MacroblockView）
// 的「启用分析」开关同步，以及 FramePacketView → StreamOverviewView 的 GOP 桥接。
//
// 为什么值得单独测：
//   1. 和 EventTimelineView（评审 P1-4）同一类坑 —— 「启用分析」QCheckBox 是在组件
//      构造函数里建的，而面板注入 feature 钩子发生在那之后。若 SetFeatureHooks 只记下
//      钩子、不回写控件，界面会显示「已启用」而面板里 feature_enabled_ 其实是关的，
//      结果是数据被静默丢弃、界面毫无提示。这里守住三条：
//        a. 注入钩子后控件状态立刻等于钩子返回的真实状态；
//        b. 回写过程不发 toggled（否则会被钩子当成用户点击，把状态再翻回去）；
//        c. 点控件时钩子收到的是本组件自己的编号（0/1/2），不是面板的枚举值。
//   2. GOP 摘要在 FramePacketView 产出、StreamOverviewView 消费，两者不互相认识，
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
        if (id < 0 || id >= static_cast<int>(states.size())) return fallback;
        return states[static_cast<std::size_t>(id)];
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamOverviewView（流统计，组件内编号 0）
// ---------------------------------------------------------------------------

TEST(StreamViewsToggleTests, StreamOverviewToggleFollowsInjectedFeatureState) {
    EnsureApp();
    StreamOverviewView view;
    ToggleRecorder rec;

    QCheckBox* toggle = view.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle != nullptr);

    // 面板里 StreamStats 默认是启用的；先按「关」注入，验证控件被回写
    view.SetFeatureHooks(StatesHook({false}), rec.Sink());
    EXPECT_FALSE(toggle->isChecked());
    // 回写不应该被当成用户操作
    EXPECT_TRUE(rec.calls.empty());

    // 反向再验一次：注入「开」
    StreamOverviewView view2;
    ToggleRecorder rec2;
    view2.SetFeatureHooks(StatesHook({true}), rec2.Sink());
    QCheckBox* toggle2 = view2.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle2 != nullptr);
    EXPECT_TRUE(toggle2->isChecked());
    EXPECT_TRUE(rec2.calls.empty());

    // 用户点击 -> 钩子收到本组件的编号 0
    toggle->setChecked(true);
    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].first, 0);
    EXPECT_TRUE(rec.calls[0].second);
}

// ---------------------------------------------------------------------------
// FramePacketView（视频帧 0 / 音频帧 1 / 数据包 2）
// ---------------------------------------------------------------------------

TEST(StreamViewsToggleTests, FramePacketTogglesFollowInjectedFeatureState) {
    EnsureApp();
    FramePacketView view;
    ToggleRecorder rec;

    QTabWidget* tabs = view.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    ASSERT_EQ(tabs->count(), 4);   // 视频帧 / 包 / GOP 摘要 / 音频帧
    // 子页顺序与 SetupUi 的 addTab 顺序一致
    QCheckBox* video = tabs->widget(0)->findChild<QCheckBox*>();
    QCheckBox* packet = tabs->widget(1)->findChild<QCheckBox*>();
    QCheckBox* audio = tabs->widget(3)->findChild<QCheckBox*>();
    ASSERT_TRUE(video != nullptr);
    ASSERT_TRUE(packet != nullptr);
    ASSERT_TRUE(audio != nullptr);

    // 面板默认: VideoFrame=开, AudioFrame=关, Packet=关。
    // 用 (false, false, true) 这种非默认组合注入 —— 若编号映射错位，断言会立刻失败。
    view.SetFeatureHooks(StatesHook({false, false, true}, /*fallback=*/false), rec.Sink());

    EXPECT_FALSE(video->isChecked());
    EXPECT_FALSE(audio->isChecked());
    EXPECT_TRUE(packet->isChecked());
    EXPECT_TRUE(rec.calls.empty());   // 回写不触发 toggled

    // 逐个点击，验证钩子收到的编号与本组件的内部编号一致
    video->setChecked(true);
    audio->setChecked(true);
    packet->setChecked(false);
    ASSERT_EQ(rec.calls.size(), 3u);
    EXPECT_EQ(rec.calls[0].first, 0);  // 视频帧
    EXPECT_EQ(rec.calls[1].first, 1);  // 音频帧
    EXPECT_EQ(rec.calls[2].first, 2);  // 数据包
    EXPECT_TRUE(rec.calls[0].second);
    EXPECT_TRUE(rec.calls[1].second);
    EXPECT_FALSE(rec.calls[2].second);
}

// GOP 摘要从 FramePacketView 产出，经 GopSummariesChanged 交给面板再转给流概览区。
// 这里只验产出侧：有变化就发一次、无变化不发、内容能被取到。
TEST(StreamViewsToggleTests, FramePacketEmitsGopSummariesOnlyWhenChanged) {
    EnsureApp();
    FramePacketView view;

    int gop_signals = 0;
    QObject::connect(&view, &FramePacketView::GopSummariesChanged,
                     [&gop_signals]() { ++gop_signals; });

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
    sums[0].gop_index = 1; sums[0].total_frames = 30;
    sums[1].gop_index = 2; sums[1].total_frames = 45;
    sums[2].gop_index = 3; sums[2].total_frames = 30;
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
