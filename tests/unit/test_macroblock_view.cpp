// MacroblockView 的页面级行为测试（评审「页面组件补测」）。
//
// 这一页承担三件事：运动矢量表按「像素精度」换算、术语随编码自适应（HEVC→CTU）、
// 以及可视化预览的 I 帧文案。三者写错都不崩：MV 表会显示 4 倍或 1/4 倍的假像素值，
// HEVC 素材会把 CTU 叫成宏块，I 帧预览停在「等待视频播放...」。全是手动点测
// 难以发现的静默回归。
//
// 只构造控件、不渲染（offscreen 平台）；导出按钮会弹 QFileDialog 模态框，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>

#include <cstddef>
#include <cstdint>

#include "core/domain/model/MacroblockInfo.h"
#include "ui/analysis_panel/MacroblockView.h"

using videoeye::model::MacroblockFrameAnalysis;
using videoeye::model::MotionVectorInfo;
using videoeye::ui::MacroblockView;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_macroblock_view";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 三张表的表头首列文本各不相同（序号 / 块大小 / 幅度范围(px)），按它精确定位。
QTableWidget* FindTableByFirstHeader(MacroblockView* view, const QString& header) {
    const auto tables = view->findChildren<QTableWidget*>();
    for (QTableWidget* table : tables) {
        if (table->columnCount() > 0 && table->horizontalHeaderItem(0) != nullptr &&
            table->horizontalHeaderItem(0)->text() == header) {
            return table;
        }
    }
    return nullptr;
}

QLabel* FindLabelContaining(MacroblockView* view, const QString& needle) {
    const auto labels = view->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text().contains(needle))
            return label;
    }
    return nullptr;
}

MotionVectorInfo MakeMv(int block_x, int block_y, int32_t motion_x, int32_t motion_y, uint16_t motion_scale,
                        int32_t source) {
    MotionVectorInfo mv;
    mv.block_x = block_x;
    mv.block_y = block_y;
    mv.motion_x = motion_x;
    mv.motion_y = motion_y;
    mv.motion_scale = motion_scale;
    mv.source = source;
    return mv;
}

} // namespace

// H.264 帧：MV 按 motion_scale 换算成像素、scale=0 按 1 兜底、参考方向三态齐全，
// 且未 Flush 前不画行。
TEST(MacroblockViewTests, FlushRendersMotionVectorRowsWithPixelScale) {
    EnsureApp();
    MacroblockView view;

    MacroblockFrameAnalysis analysis;
    analysis.frame_index = 12;
    analysis.timestamp = 1.5;
    analysis.frame_type = 2; // P 帧
    analysis.codec_id = 27;  // AV_CODEC_ID_H264
    analysis.frame_width = 1280;
    analysis.frame_height = 720;
    analysis.has_motion_vectors = true;

    MotionVectorInfo forward = MakeMv(0, 0, 8, -4, 4, -1); // 1/4 像素精度
    forward.block_w = 16;
    forward.block_h = 16;
    MotionVectorInfo backward = MakeMv(64, 0, 3, -2, 0, 1); // scale=0 -> 分母按 1
    backward.block_w = 8;
    backward.block_h = 8;
    MotionVectorInfo neutral = MakeMv(128, 0, 0, 0, 4, 0); // 无参考方向
    analysis.motion_vectors = {forward, backward, neutral};

    analysis.stats.total_blocks = 3;
    analysis.stats.forward_count = 1;
    analysis.stats.backward_count = 1;
    analysis.stats.intra_count = 1;
    analysis.stats.avg_motion_magnitude = 1.5;
    analysis.stats.max_motion_magnitude = 2.5;

    view.SetAnalysis(analysis);
    EXPECT_TRUE(view.HasPending());

    QTableWidget* mv_table = FindTableByFirstHeader(&view, QStringLiteral("序号"));
    ASSERT_TRUE(mv_table != nullptr);
    EXPECT_EQ(mv_table->rowCount(), 0); // 未 Flush 不画

    view.FlushPending();
    EXPECT_FALSE(view.HasPending());
    ASSERT_EQ(mv_table->rowCount(), 3);
    EXPECT_EQ(mv_table->item(0, 3)->text(), QStringLiteral("16x16"));
    EXPECT_EQ(mv_table->item(0, 4)->text(), QStringLiteral("2.00"));  // 8 / 4
    EXPECT_EQ(mv_table->item(0, 5)->text(), QStringLiteral("-1.00")); // -4 / 4
    EXPECT_EQ(mv_table->item(0, 8)->text(), QStringLiteral("前向"));
    EXPECT_EQ(mv_table->item(1, 4)->text(), QStringLiteral("3.00")); // scale=0 -> 分母 1
    EXPECT_EQ(mv_table->item(1, 5)->text(), QStringLiteral("-2.00"));
    EXPECT_EQ(mv_table->item(1, 8)->text(), QStringLiteral("后向"));
    EXPECT_EQ(mv_table->item(2, 8)->text(), QStringLiteral("—"));

    QLabel* summary = FindLabelContaining(&view, QStringLiteral("总数"));
    ASSERT_TRUE(summary != nullptr);
    const QString text = summary->text();
    EXPECT_TRUE(text.contains(QStringLiteral("帧 #12")));
    EXPECT_TRUE(text.contains(QStringLiteral("[P]")));
    EXPECT_TRUE(text.contains(QStringLiteral("宏块总数: 3")));
    EXPECT_TRUE(text.contains(QStringLiteral("前向: 1")));
    EXPECT_TRUE(text.contains(QStringLiteral("后向: 1")));
    EXPECT_TRUE(text.contains(QStringLiteral("帧内: 1")));

    // H.264 块大小表：8 行、首行 16x16
    QTableWidget* bs_table = FindTableByFirstHeader(&view, QStringLiteral("块大小"));
    ASSERT_TRUE(bs_table != nullptr);
    EXPECT_EQ(bs_table->rowCount(), 8);
    EXPECT_EQ(bs_table->item(0, 0)->text(), QStringLiteral("16x16"));

    // 幅度分布表恒 5 档
    QTableWidget* mag_table = FindTableByFirstHeader(&view, QStringLiteral("幅度范围(px)"));
    ASSERT_TRUE(mag_table != nullptr);
    EXPECT_EQ(mag_table->rowCount(), 5);
}

// HEVC 帧：术语切到 CTU、块大小表切到 14 行大尺寸分区、I 帧预览走「无运动矢量」文案。
TEST(MacroblockViewTests, HevcUsesCtuTermAndLargeBlockSizes) {
    EnsureApp();
    MacroblockView view;

    MacroblockFrameAnalysis analysis;
    analysis.frame_index = 5;
    analysis.frame_type = 1; // I 帧
    analysis.codec_id = 173; // AV_CODEC_ID_HEVC
    analysis.frame_width = 1920;
    analysis.frame_height = 1080;
    analysis.has_motion_vectors = false;
    analysis.stats.total_blocks = 10;
    analysis.stats.intra_count = 10;
    analysis.stats.count_64x64 = 4;
    analysis.stats.count_16x16 = 6;

    view.SetAnalysis(analysis);
    view.FlushPending();

    QLabel* summary = FindLabelContaining(&view, QStringLiteral("总数"));
    ASSERT_TRUE(summary != nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("CTU总数: 10")));

    QTableWidget* bs_table = FindTableByFirstHeader(&view, QStringLiteral("块大小"));
    ASSERT_TRUE(bs_table != nullptr);
    EXPECT_EQ(bs_table->rowCount(), 14); // HEVC 分区表
    EXPECT_EQ(bs_table->item(0, 0)->text(), QStringLiteral("64x64"));
    EXPECT_EQ(bs_table->item(0, 1)->text(), QStringLiteral("4"));
    EXPECT_EQ(bs_table->item(0, 2)->text(), QStringLiteral("40.0%")); // 4 / (4 + 6)
    EXPECT_EQ(bs_table->item(6, 0)->text(), QStringLiteral("16x16"));
    EXPECT_EQ(bs_table->item(6, 1)->text(), QStringLiteral("6"));
    EXPECT_EQ(bs_table->item(6, 2)->text(), QStringLiteral("60.0%"));

    // I 帧预览：无运动矢量文案 + 帧内块数
    QLabel* viz = FindLabelContaining(&view, QStringLiteral("I 帧"));
    ASSERT_TRUE(viz != nullptr);
    EXPECT_TRUE(viz->text().contains(QStringLiteral("帧内块数: 10")));
}

// 「启用分析」勾选框与协调层的同步语义：
//   - SetFeatureHooks 注入后立即按真实状态回写勾选框（否则界面显示"已启用"而面板为关）；
//   - SyncToggleFromHooks 把勾选框同步到钩子状态，但不得回触发 set_enabled_ 回调（无循环）；
//   - 用户手动勾选才回调 set_enabled_，供协调层记录"用户意图"。
TEST(MacroblockViewTests, ToggleFollowsHooksWithoutFeedbackLoop) {
    EnsureApp();
    MacroblockView view;

    QCheckBox* toggle = view.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle != nullptr);

    bool state = false;
    int set_call_count = 0;
    bool last_set_value = false;
    view.SetFeatureHooks([&state](int) { return state; },
                         [&set_call_count, &last_set_value](int, bool enabled) {
                             ++set_call_count;
                             last_set_value = enabled;
                         });

    // SetFeatureHooks 立即同步：state=false -> 勾选框未选中，且不会触发回调
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_EQ(set_call_count, 0);

    // 钩子状态变化后同步勾选框，但仍不触发回调（QSignalBlocker 生效）
    state = true;
    view.SyncToggleFromHooks();
    EXPECT_TRUE(toggle->isChecked());
    EXPECT_EQ(set_call_count, 0);

    state = false;
    view.SyncToggleFromHooks();
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_EQ(set_call_count, 0);

    // 用户手动勾选：回调一次，值为 true（协调层据此更新"用户启用分析"）
    toggle->setChecked(true);
    EXPECT_EQ(set_call_count, 1);
    EXPECT_TRUE(last_set_value);
}

// 无钩子时（组件脱离面板单独构造）同步不改变勾选框，也不崩。
TEST(MacroblockViewTests, SyncToggleWithoutHooksKeepsCurrentState) {
    EnsureApp();
    MacroblockView view;

    QCheckBox* toggle = view.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle != nullptr);
    EXPECT_FALSE(toggle->isChecked());

    toggle->setChecked(true);
    view.SyncToggleFromHooks();
    EXPECT_TRUE(toggle->isChecked());
}

// 运动矢量表的行数上限：第 501 条起不显示（保主线程流畅的硬约束）。
TEST(MacroblockViewTests, MotionVectorTableCapsAt500Rows) {
    EnsureApp();
    MacroblockView view;

    MacroblockFrameAnalysis analysis;
    analysis.codec_id = 27;
    analysis.frame_width = 640;
    analysis.frame_height = 360;
    analysis.has_motion_vectors = true;
    analysis.motion_vectors.resize(520);
    for (std::size_t i = 0; i < analysis.motion_vectors.size(); ++i) {
        analysis.motion_vectors[i] = MakeMv(static_cast<int>(i) * 16, 0, 4, 4, 4, -1);
    }

    view.SetAnalysis(analysis);
    view.FlushPending();

    QTableWidget* mv_table = FindTableByFirstHeader(&view, QStringLiteral("序号"));
    ASSERT_TRUE(mv_table != nullptr);
    ASSERT_EQ(mv_table->rowCount(), 500); // 行数上限
    EXPECT_EQ(mv_table->item(0, 0)->text(), QStringLiteral("0"));
    EXPECT_EQ(mv_table->item(499, 0)->text(), QStringLiteral("499")); // 保留的是前 500 条
}