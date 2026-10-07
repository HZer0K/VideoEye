// VideoFrameTableWidget（码流分析「视频帧」+「GOP 摘要」两个子页）的接缝测试。
//
// 为什么值得单独测：它从 FramePacketView 抽出，一个组件同时持两张表 —— 帧明细表与
// 由帧记录 pict_type / is_key_frame **派生**出来的 GOP 分段表。派生逻辑、筛选器的
// 可见行重算、以及"清空后仍要通知流概览区把最大 GOP/分布曲线归零"这三条链断了都不崩，
// 只会让界面停在旧数据，是最难手动点测发现的回归。
//
// 具体盯四件事：
//   1. 表结构：帧表 6 列、GOP 表 9 列、筛选下拉两项、组件自身不可见（宿主型）。
//   2. GOP 派生：I 帧开新段、P/B 续段，段的起止帧号 / 计数必须正确。
//   3. 脏标志 + FlushPending：只在 flush 时落表，并恰好发一次 GopSummariesChanged。
//   4. 筛选与 Reset：切"仅 I 帧"重算可见行；Reset 清空缓存与两张表，且 flush 时仍发
//      GopSummariesChanged（让流概览区一并归零）。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QTableWidget>

#include <libavcodec/avcodec.h>

#include "ui/analysis_panel/VideoFrameTableWidget.h"

using videoeye::ui::VideoFrameTableWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_video_frame_table_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 组件内有两张 QTableWidget；frameTable() 外露了帧表，另一张就是 GOP 表。
QTableWidget* GopTable(VideoFrameTableWidget& widget) {
    for (QTableWidget* table : widget.findChildren<QTableWidget*>()) {
        if (table != widget.frameTable()) {
            return table;
        }
    }
    return nullptr;
}

QLabel* SummaryLabel(VideoFrameTableWidget& widget) {
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("总帧数"))) {
            return label;
        }
    }
    return nullptr;
}

void AppendStandardFrames(VideoFrameTableWidget& widget) {
    // I(key) P P I(key) —— 两段 GOP
    widget.AppendVideoFrame(1, AV_PICTURE_TYPE_I, true, 0, 0.0);
    widget.AppendVideoFrame(2, AV_PICTURE_TYPE_P, false, 1000, 1.0);
    widget.AppendVideoFrame(3, AV_PICTURE_TYPE_P, false, 2000, 2.0);
    widget.AppendVideoFrame(4, AV_PICTURE_TYPE_I, true, 3000, 3.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 表结构：帧表 6 列、GOP 表 9 列、筛选下拉两项、宿主自身不可见
// ---------------------------------------------------------------------------

TEST(VideoFrameTableWidgetTests, SetupCreatesFrameAndGopTables) {
    EnsureApp();

    VideoFrameTableWidget widget;
    QTableWidget* frame_table = widget.frameTable();
    ASSERT_TRUE(frame_table != nullptr);

    EXPECT_EQ(6, frame_table->columnCount());
    EXPECT_EQ(QStringLiteral("帧类型"), frame_table->horizontalHeaderItem(1)->text());
    EXPECT_EQ(QStringLiteral("GOP #"), frame_table->horizontalHeaderItem(4)->text());

    QTableWidget* gop_table = GopTable(widget);
    ASSERT_TRUE(gop_table != nullptr);
    EXPECT_EQ(9, gop_table->columnCount());
    EXPECT_EQ(QStringLiteral("总帧数"), gop_table->horizontalHeaderItem(5)->text());

    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(filter != nullptr);
    ASSERT_EQ(2, filter->count());
    EXPECT_EQ(QStringLiteral("全部帧"), filter->itemText(0));
    EXPECT_EQ(QStringLiteral("仅 I 帧"), filter->itemText(1));

    ASSERT_TRUE(widget.videoFramePage() != nullptr);
    ASSERT_TRUE(widget.gopPage() != nullptr);

    // 宿主组件自身不可见：它只是两个子页的容器，由父页 addTab
    EXPECT_TRUE(widget.isHidden());

    ASSERT_TRUE(widget.toggle() != nullptr);
    EXPECT_FALSE(widget.toggle()->isChecked());
}

// ---------------------------------------------------------------------------
// 2. GOP 派生：I 帧开新段、P/B 续段，段的起止帧号与计数正确
// ---------------------------------------------------------------------------

TEST(VideoFrameTableWidgetTests, AppendDerivesGopBoundariesFromFrameTypes) {
    EnsureApp();

    VideoFrameTableWidget widget;
    AppendStandardFrames(widget);

    ASSERT_EQ(4u, widget.FrameRecords().size());
    ASSERT_EQ(2u, widget.GopSummaries().size());

    const auto& gop0 = widget.GopSummaries()[0];
    EXPECT_EQ(1, gop0.gop_index);
    EXPECT_EQ(1, gop0.start_frame);
    EXPECT_EQ(3, gop0.end_frame);
    EXPECT_EQ(3, gop0.total_frames);
    EXPECT_EQ(1, gop0.i_count);
    EXPECT_EQ(2, gop0.p_count);
    EXPECT_EQ(0, gop0.b_count);
    EXPECT_EQ(1, gop0.key_count);

    const auto& gop1 = widget.GopSummaries()[1];
    EXPECT_EQ(2, gop1.gop_index);
    EXPECT_EQ(4, gop1.start_frame);
    EXPECT_EQ(4, gop1.end_frame);
    EXPECT_EQ(1, gop1.total_frames);
    EXPECT_EQ(1, gop1.i_count);
}

// ---------------------------------------------------------------------------
// 3. 脏标志 + FlushPending：只在 flush 落表，并恰好发一次 GopSummariesChanged
// ---------------------------------------------------------------------------

TEST(VideoFrameTableWidgetTests, FlushFillsBothTablesAndEmitsGopChangedOnce) {
    EnsureApp();

    VideoFrameTableWidget widget;
    QTableWidget* frame_table = widget.frameTable();
    QTableWidget* gop_table = GopTable(widget);
    ASSERT_TRUE(frame_table != nullptr);
    ASSERT_TRUE(gop_table != nullptr);

    int gop_changed = 0;
    QObject::connect(&widget, &VideoFrameTableWidget::GopSummariesChanged,
                     [&gop_changed]() { ++gop_changed; });

    AppendStandardFrames(widget);
    EXPECT_TRUE(widget.HasPending());
    EXPECT_EQ(0, frame_table->rowCount());  // 增量提交前不落表
    EXPECT_EQ(0, gop_table->rowCount());

    widget.FlushPending();
    EXPECT_FALSE(widget.HasPending());

    ASSERT_EQ(4, frame_table->rowCount());
    ASSERT_EQ(2, gop_table->rowCount());
    // GOP 摘要只在这一处统一通知，避免每个 GOP 边界都拷一遍向量
    EXPECT_EQ(1, gop_changed);

    ASSERT_TRUE(frame_table->item(0, 1) != nullptr);
    EXPECT_EQ(QStringLiteral("I"), frame_table->item(0, 1)->text());
    EXPECT_EQ(QStringLiteral("P"), frame_table->item(1, 1)->text());
    EXPECT_EQ(QStringLiteral("1"), frame_table->item(1, 4)->text());  // GOP #
    EXPECT_EQ(QStringLiteral("2"), frame_table->item(1, 5)->text());  // GOP 内
    EXPECT_EQ(QStringLiteral("2"), frame_table->item(3, 4)->text());  // 第 4 帧属于第 2 段

    ASSERT_TRUE(gop_table->item(0, 6) != nullptr);
    EXPECT_EQ(QStringLiteral("3"), gop_table->item(0, 5)->text());  // 总帧数
    EXPECT_EQ(QStringLiteral("1"), gop_table->item(0, 6)->text());  // I
    EXPECT_EQ(QStringLiteral("2"), gop_table->item(0, 7)->text());  // P
}

// ---------------------------------------------------------------------------
// 4. 筛选「仅 I 帧」重算可见行
// ---------------------------------------------------------------------------

TEST(VideoFrameTableWidgetTests, IFrameFilterRebuildsVisibleRows) {
    EnsureApp();

    VideoFrameTableWidget widget;
    QTableWidget* frame_table = widget.frameTable();
    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(frame_table != nullptr);
    ASSERT_TRUE(filter != nullptr);

    AppendStandardFrames(widget);
    widget.FlushPending();
    ASSERT_EQ(4, frame_table->rowCount());

    filter->setCurrentIndex(1);  // 仅 I 帧
    EXPECT_EQ(2, frame_table->rowCount());
    EXPECT_EQ(QStringLiteral("I"), frame_table->item(0, 1)->text());

    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(summary != nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("显示: 2")));

    filter->setCurrentIndex(0);  // 全部帧
    EXPECT_EQ(4, frame_table->rowCount());
}

// ---------------------------------------------------------------------------
// 5. 未知帧类型渲染为 "?"（派生表可读性）
// ---------------------------------------------------------------------------

TEST(VideoFrameTableWidgetTests, UnknownFrameTypeRendersQuestionMark) {
    EnsureApp();

    VideoFrameTableWidget widget;
    QTableWidget* frame_table = widget.frameTable();
    ASSERT_TRUE(frame_table != nullptr);

    widget.AppendVideoFrame(1, AV_PICTURE_TYPE_NONE, false, 0, 0.0);
    widget.FlushPending();

    ASSERT_EQ(1, frame_table->rowCount());
    ASSERT_TRUE(frame_table->item(0, 1) != nullptr);
    EXPECT_EQ(QStringLiteral("?"), frame_table->item(0, 1)->text());
}

// ---------------------------------------------------------------------------
// 6. Reset 清空缓存与两张表，且 flush 仍发 GopSummariesChanged 以便概览区归零
// ---------------------------------------------------------------------------

TEST(VideoFrameTableWidgetTests, ResetClearsTablesAndNotifiesGopChanged) {
    EnsureApp();

    VideoFrameTableWidget widget;
    QTableWidget* frame_table = widget.frameTable();
    QTableWidget* gop_table = GopTable(widget);
    ASSERT_TRUE(frame_table != nullptr);
    ASSERT_TRUE(gop_table != nullptr);

    AppendStandardFrames(widget);
    widget.FlushPending();
    ASSERT_EQ(4, frame_table->rowCount());

    int gop_changed = 0;
    QObject::connect(&widget, &VideoFrameTableWidget::GopSummariesChanged,
                     [&gop_changed]() { ++gop_changed; });

    widget.ResetVideoFrames();
    EXPECT_TRUE(widget.FrameRecords().empty());
    EXPECT_TRUE(widget.GopSummaries().empty());
    EXPECT_EQ(0, frame_table->rowCount());
    EXPECT_EQ(0, gop_table->rowCount());
    EXPECT_TRUE(widget.HasPending());

    widget.FlushPending();
    EXPECT_EQ(1, gop_changed);
}