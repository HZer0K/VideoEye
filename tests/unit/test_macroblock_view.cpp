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
#include <QMouseEvent>
#include <QTableWidget>

#include <cstddef>
#include <cstdint>
#include <vector>

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

// 运动强度曲线是唯一带「点击曲线可跳转」提示的标签，按提示文本定位，
// 避免依赖文本（画完后文本会被 pixmap 顶掉）。
QLabel* FindChartLabel(MacroblockView* view) {
    for (QLabel* label : view->findChildren<QLabel*>()) {
        if (label->toolTip().contains(QStringLiteral("点击曲线可跳转")))
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

// 一条带运动矢量的 P 帧，时间戳与平均/最大幅度可控，供曲线测试使用。
MacroblockFrameAnalysis MakeMvFrame(double timestamp, double avg_magnitude, double max_magnitude) {
    MacroblockFrameAnalysis analysis;
    analysis.codec_id = 27;  // AV_CODEC_ID_H264
    analysis.frame_type = 2; // P 帧
    analysis.frame_width = 640;
    analysis.frame_height = 360;
    analysis.timestamp = timestamp;
    analysis.has_motion_vectors = true;
    analysis.motion_vectors = {MakeMv(16, 16, 8, 8, 4, -1)};
    analysis.stats.total_blocks = 90;
    analysis.stats.distinct_block_count = 1;
    analysis.stats.avg_motion_magnitude = avg_magnitude;
    analysis.stats.max_motion_magnitude = max_magnitude;
    return analysis;
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
    analysis.stats.distinct_block_count = 3;
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
    EXPECT_TRUE(text.contains(QStringLiteral("去重块: 3")));
    EXPECT_TRUE(text.contains(QStringLiteral("前向: 1")));
    EXPECT_TRUE(text.contains(QStringLiteral("后向: 1")));
    EXPECT_TRUE(text.contains(QStringLiteral("帧内: 不可用")));

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
    analysis.stats.block_counts_estimated = true;
    analysis.stats.intra_count_available = true;
    analysis.stats.count_64x64 = 4;
    analysis.stats.count_16x16 = 6;

    view.SetAnalysis(analysis);
    view.FlushPending();

    QLabel* summary = FindLabelContaining(&view, QStringLiteral("总数"));
    ASSERT_TRUE(summary != nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("CTU总数: 10（估算）")));

    QTableWidget* bs_table = FindTableByFirstHeader(&view, QStringLiteral("块大小"));
    ASSERT_TRUE(bs_table != nullptr);
    EXPECT_EQ(bs_table->rowCount(), 14); // HEVC 分区表
    EXPECT_EQ(bs_table->item(0, 0)->text(), QStringLiteral("64x64"));
    EXPECT_EQ(bs_table->item(0, 1)->text(), QStringLiteral("4"));
    EXPECT_EQ(bs_table->item(0, 2)->text(), QStringLiteral("40.0%")); // 4 / (4 + 6)
    EXPECT_EQ(bs_table->item(6, 0)->text(), QStringLiteral("16x16"));
    EXPECT_EQ(bs_table->item(6, 1)->text(), QStringLiteral("6"));
    EXPECT_EQ(bs_table->item(6, 2)->text(), QStringLiteral("60.0%"));

    // I 帧预览：无运动矢量文案 + 帧内 CTU 数
    QLabel* viz = FindLabelContaining(&view, QStringLiteral("I 帧"));
    ASSERT_TRUE(viz != nullptr);
    EXPECT_TRUE(viz->text().contains(QStringLiteral("帧内CTU数: 10（估算）")));
}

TEST(MacroblockViewTests, PredictedFrameWithoutMotionVectorsIsNotLabeledIntra) {
    EnsureApp();
    MacroblockView view;
    MacroblockFrameAnalysis analysis;
    analysis.frame_type = 2;
    analysis.codec_id = 27; // H.264: 解码器本身支持导出 MV，只是这一帧没有
    analysis.frame_width = 1920;
    analysis.frame_height = 1080;
    view.SetAnalysis(analysis);
    view.FlushPending();
    QLabel* preview = FindLabelContaining(&view, QStringLiteral("未提供运动矢量数据"));
    ASSERT_NE(preview, nullptr);
    EXPECT_TRUE(preview->text().startsWith(QStringLiteral("P 帧")));
    EXPECT_FALSE(preview->text().contains(QStringLiteral("I 帧")));
    EXPECT_TRUE(preview->text().contains(QStringLiteral("帧内宏块数: 不可用")));

    // 没有运动矢量时 ComputeStats 直接早退、网格从没铺过，total_blocks 还停在 0 ——
    // 这时候把 0 当"0 个块"显示就是撒谎，必须跟帧内块数一样报不可用。
    QLabel* summary = FindLabelContaining(&view, QStringLiteral("总数"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("宏块总数: 不可用")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("去重块: 不可用")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("帧内: 不可用")));
}

// HEVC / VP9 / AV1 的 FFmpeg 解码器不导出运动矢量。这是解码器能力边界，
// 不能显示成"未提供运动矢量数据"（像是分析没生效），要直接说明原因。
TEST(MacroblockViewTests, CodecWithoutMvExportExplainsDecoderCapability) {
    EnsureApp();
    MacroblockView view;
    MacroblockFrameAnalysis analysis;
    analysis.frame_type = 2;
    analysis.codec_id = 173; // AV_CODEC_ID_HEVC
    analysis.frame_width = 1920;
    analysis.frame_height = 1080;
    view.SetAnalysis(analysis);
    view.FlushPending();

    QLabel* preview = FindLabelContaining(&view, QStringLiteral("HEVC"));
    ASSERT_NE(preview, nullptr);
    EXPECT_TRUE(preview->text().contains(QStringLiteral("解码器不导出运动矢量")));
    EXPECT_FALSE(preview->text().contains(QStringLiteral("未提供运动矢量数据")));
    EXPECT_TRUE(preview->text().contains(QStringLiteral("帧内CTU数: 不可用")));

    // AV1 同样没有 MV side data，但块术语仍是宏块
    MacroblockFrameAnalysis av1 = analysis;
    av1.codec_id = 225; // AV_CODEC_ID_AV1
    view.SetAnalysis(av1);
    view.FlushPending();
    QLabel* av1_preview = FindLabelContaining(&view, QStringLiteral("AV1"));
    ASSERT_NE(av1_preview, nullptr);
    EXPECT_TRUE(av1_preview->text().contains(QStringLiteral("解码器不导出运动矢量")));
    EXPECT_TRUE(av1_preview->text().contains(QStringLiteral("帧内宏块数: 不可用")));
}

// 换文件 / 关闭分析时必须清空：否则上一份素材的表格和预览会被当成当前文件的结果。
TEST(MacroblockViewTests, ResetClearsTableAndPreview) {
    EnsureApp();
    MacroblockView view;

    MacroblockFrameAnalysis analysis;
    analysis.frame_index = 7;
    analysis.codec_id = 27;
    analysis.frame_width = 640;
    analysis.frame_height = 360;
    analysis.has_motion_vectors = true;
    analysis.stats.total_blocks = 40;
    analysis.motion_vectors.push_back(MakeMv(16, 16, 16, 16, 8, -1));
    view.SetAnalysis(analysis);
    view.FlushPending();

    QTableWidget* mv_table = FindTableByFirstHeader(&view, QStringLiteral("序号"));
    ASSERT_TRUE(mv_table != nullptr);
    ASSERT_EQ(mv_table->rowCount(), 1);

    view.Reset();

    EXPECT_EQ(mv_table->rowCount(), 0);
    QLabel* summary = FindLabelContaining(&view, QStringLiteral("分析帧"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("分析帧: --")));
    EXPECT_FALSE(summary->text().contains(QStringLiteral("分析帧 #7")));
    QLabel* viz = FindLabelContaining(&view, QStringLiteral("等待视频播放"));
    ASSERT_NE(viz, nullptr);
    EXPECT_TRUE(viz->pixmap().isNull());
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

// 表格选中行 -> 预览联动高亮：选中后预览应重绘（pixmap 变化），换帧后选中被清空。
TEST(MacroblockViewTests, SelectingRowRedrawsPreviewHighlight) {
    EnsureApp();
    MacroblockView view;

    MacroblockFrameAnalysis analysis;
    analysis.codec_id = 27; // AV_CODEC_ID_H264
    analysis.frame_width = 640;
    analysis.frame_height = 360;
    analysis.has_motion_vectors = true;
    analysis.motion_vectors = {MakeMv(16, 16, 16, 16, 8, -1), MakeMv(80, 40, -16, 8, 4, 1)};
    analysis.stats.total_blocks = 90;
    analysis.stats.distinct_block_count = 2;
    view.SetAnalysis(analysis);
    view.FlushPending();

    QTableWidget* mv_table = FindTableByFirstHeader(&view, QStringLiteral("序号"));
    ASSERT_TRUE(mv_table != nullptr);
    ASSERT_EQ(mv_table->rowCount(), 2);

    // 可视化预览是唯一持有 pixmap 的 QLabel
    QLabel* viz = nullptr;
    for (QLabel* label : view.findChildren<QLabel*>()) {
        if (!label->pixmap().isNull()) {
            viz = label;
            break;
        }
    }
    ASSERT_NE(viz, nullptr);
    const qint64 base_key = viz->pixmap().cacheKey();

    // 选中第 1 行：预览重绘出高亮（新 pixmap）
    mv_table->selectRow(1);
    EXPECT_EQ(mv_table->currentRow(), 1);
    EXPECT_NE(base_key, viz->pixmap().cacheKey());

    // 新一帧数据到达：旧选中索引已经指向另一个矢量，必须清空选中并退出高亮
    view.SetAnalysis(analysis);
    view.FlushPending();
    EXPECT_TRUE(mv_table->selectedItems().isEmpty());

    // 复位后即便之前有选中也不影响
    view.Reset();
    EXPECT_TRUE(viz->pixmap().isNull());
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

    // 截断必须显式告知，不能让用户把表格行数当成整帧的运动矢量条数。
    QLabel* summary = FindLabelContaining(&view, QStringLiteral("列表仅显示前 500/520 条"));
    ASSERT_NE(summary, nullptr);
}

// 运动强度曲线：两个带运动矢量的帧之后应画出曲线（持有 pixmap），
// 单帧（I 帧被跳过）时仍是占位文案，Reset 后回到占位。
TEST(MacroblockViewTests, MotionChartDrawsAfterTwoSamplesAndResetsToPlaceholder) {
    EnsureApp();
    MacroblockView view;

    QLabel* chart = FindChartLabel(&view);
    ASSERT_NE(chart, nullptr);
    EXPECT_TRUE(chart->pixmap().isNull());
    EXPECT_TRUE(chart->text().contains(QStringLiteral("运动强度曲线")));

    view.SetAnalysis(MakeMvFrame(1.0, 2.0, 6.0));
    view.FlushPending();
    EXPECT_TRUE(chart->pixmap().isNull()); // 单点无法成线

    view.SetAnalysis(MakeMvFrame(1.1, 3.0, 9.0));
    view.FlushPending();
    EXPECT_FALSE(chart->pixmap().isNull());

    view.Reset();
    EXPECT_TRUE(chart->pixmap().isNull());
    EXPECT_TRUE(chart->text().contains(QStringLiteral("运动强度曲线")));
}

// 曲线点击定位：把横轴像素位置换算成时间并发 SeekRequested。
// 两个采样点跨度 < 1s 时横轴被撑到 1s（避免两点挤成一条垂线），
// 因此 [1.0, 1.1] 的曲线范围是 [1.0, 2.0]。
TEST(MacroblockViewTests, ClickingChartEmitsSeekWithinRange) {
    EnsureApp();
    MacroblockView view;
    view.SetAnalysis(MakeMvFrame(1.0, 2.0, 6.0));
    view.FlushPending();
    view.SetAnalysis(MakeMvFrame(1.1, 3.0, 9.0));
    view.FlushPending();

    QLabel* chart = FindChartLabel(&view);
    ASSERT_NE(chart, nullptr);
    ASSERT_FALSE(chart->pixmap().isNull());

    std::vector<double> seeks;
    QObject::connect(&view, &MacroblockView::SeekRequested, [&seeks](double seconds) { seeks.push_back(seconds); });

    // 点击换算与绘制共用边距：左端 = t0，右端 = t1，图外不触发。
    const int width = chart->pixmap().width();
    const int left = 34;
    const int right = width - 8;

    auto click = [chart](double x) {
        QMouseEvent event(QEvent::MouseButtonRelease, QPointF(x, 10.0), QPointF(x, 10.0), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(chart, &event);
    };

    click(0.0);                  // 左端 = t0
    click(width);                // 右端 = t1
    click((left + right) / 2.0); // 绘图区中点
    click(width + 20.0);         // 图外：不触发

    ASSERT_EQ(seeks.size(), 3u);
    EXPECT_NEAR(seeks[0], 1.0, 1e-6);
    EXPECT_NEAR(seeks[1], 2.0, 1e-6);
    EXPECT_NEAR(seeks[2], 1.5, 1e-6);
}

// 曲线宽度跟随标签实际宽度，而不是固定 360px：宽窗口下曲线应铺满，
// 点击右侧空白区才能按真实横轴换算。
TEST(MacroblockViewTests, MotionChartFollowsLabelWidth) {
    EnsureApp();
    MacroblockView view;
    view.SetAnalysis(MakeMvFrame(1.0, 2.0, 6.0));
    view.FlushPending();
    view.SetAnalysis(MakeMvFrame(1.1, 3.0, 9.0));
    view.FlushPending();

    QLabel* chart = FindChartLabel(&view);
    ASSERT_NE(chart, nullptr);
    ASSERT_FALSE(chart->pixmap().isNull());

    chart->resize(700, 110);
    view.SetAnalysis(MakeMvFrame(1.2, 3.5, 10.0));
    view.FlushPending();

    EXPECT_EQ(chart->pixmap().width(), chart->contentsRect().width());
    EXPECT_GT(chart->pixmap().width(), 360);
}

// 时间戳跳变（Seek）后旧曲线不再对应当前位置：历史清空、重新累积。
TEST(MacroblockViewTests, TimeJumpClearsMotionHistory) {
    EnsureApp();
    MacroblockView view;
    view.SetAnalysis(MakeMvFrame(1.0, 2.0, 6.0));
    view.FlushPending();
    view.SetAnalysis(MakeMvFrame(1.1, 3.0, 9.0));
    view.FlushPending();

    QLabel* chart = FindChartLabel(&view);
    ASSERT_NE(chart, nullptr);
    ASSERT_FALSE(chart->pixmap().isNull());

    // 跳到 30s：历史清空，只有这一个新采样点 -> 回到占位
    view.SetAnalysis(MakeMvFrame(30.0, 4.0, 12.0));
    view.FlushPending();
    EXPECT_TRUE(chart->pixmap().isNull());

    // 继续在 30s 附近累积 -> 重新画出曲线
    view.SetAnalysis(MakeMvFrame(30.1, 5.0, 15.0));
    view.FlushPending();
    EXPECT_FALSE(chart->pixmap().isNull());
}

// 2000 条运动矢量（超过预览上限 1200）：按顺序等距抽样绘制，不崩且仍有输出。
TEST(MacroblockViewTests, LargeMotionVectorCountIsSubsampledInPreview) {
    EnsureApp();
    MacroblockView view;

    MacroblockFrameAnalysis analysis;
    analysis.codec_id = 27;
    analysis.frame_width = 1920;
    analysis.frame_height = 1080;
    analysis.has_motion_vectors = true;
    analysis.motion_vectors.resize(2000);
    for (std::size_t i = 0; i < analysis.motion_vectors.size(); ++i) {
        analysis.motion_vectors[i] =
            MakeMv(static_cast<int>(i % 120) * 16, static_cast<int>(i / 120) * 16, 8, 8, 4, -1);
    }
    analysis.stats.total_blocks = 4000;
    analysis.stats.distinct_block_count = 2000;
    analysis.stats.avg_motion_magnitude = 2.0;
    analysis.stats.max_motion_magnitude = 4.0;

    view.SetAnalysis(analysis);
    view.FlushPending();

    // 表格仍只填前 500 行；预览必须持有 pixmap 且不崩溃
    QTableWidget* mv_table = FindTableByFirstHeader(&view, QStringLiteral("序号"));
    ASSERT_NE(mv_table, nullptr);
    EXPECT_EQ(mv_table->rowCount(), 500);

    QLabel* chart = FindChartLabel(&view);
    bool found_viz = false;
    for (QLabel* label : view.findChildren<QLabel*>()) {
        if (!label->pixmap().isNull() && label != chart) {
            found_viz = true;
            break;
        }
    }
    EXPECT_TRUE(found_viz);
}
