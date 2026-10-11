// AudioFrameTableWidget（码流分析「音频帧」子页）的接缝测试。
//
// 为什么值得单独测：它从 FramePacketView 抽出，持有自己的记录缓存、脏标志与增量同步
// 游标。这三者一旦对不上，表现是"界面上少几行 / 汇总数字对不上 / 导出内容缺行"，
// 全是静默回归，只有单独驱动这个组件才能量到。
//
// 具体盯四件事：
//   1. 表结构：7 列、列名、初始汇总为 0、开关默认未勾选。
//   2. 脏标志与增量刷新：AppendAudioFrame 只入队置脏（不落表），FlushPending 才按游标
//      补差量；刷完脏标志必须清掉。
//   3. 单元格内容与汇总聚合：每一列取到正确的字段；汇总行的"帧数/样本数/字节数"求和。
//   4. ResetAudioFrames：清空缓存与表格，汇总归零。
//
// 表已从 QTableWidget 换成 QTableView + RecordTableModel（10 万级记录），
// 所以读单元格一律走 model() 而不是 item() —— QTableView 本身不存数据。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QTableView>

#include "ui/analysis_panel/AudioFrameTableWidget.h"

using videoeye::ui::AudioFrameTableWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_audio_frame_table_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QTableView* AudioTable(AudioFrameTableWidget& widget) {
    return widget.findChild<QTableView*>();
}

QLabel* SummaryLabel(AudioFrameTableWidget& widget) {
    // 组件内只有这一个 QLabel（汇总行）
    return widget.findChild<QLabel*>();
}

int RowCount(QTableView* table) { return table->model()->rowCount(); }

int ColumnCount(QTableView* table) { return table->model()->columnCount(); }

QString Header(QTableView* table, int column) {
    return table->model()->headerData(column, Qt::Horizontal).toString();
}

QString Cell(QTableView* table, int row, int column) {
    return table->model()->index(row, column).data(Qt::DisplayRole).toString();
}

bool HasCell(QTableView* table, int row, int column) {
    return table->model()->index(row, column).isValid();
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 表结构：7 列 + 列名 + 初始汇总 + 开关默认未勾选
// ---------------------------------------------------------------------------

TEST(AudioFrameTableWidgetTests, SetupCreatesSevenColumnTableAndZeroSummary) {
    EnsureApp();

    AudioFrameTableWidget widget;
    QTableView* table = AudioTable(widget);
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);
    ASSERT_TRUE(table->model() != nullptr);

    EXPECT_EQ(7, ColumnCount(table));
    EXPECT_EQ(QStringLiteral("样本数"), Header(table, 3));
    EXPECT_EQ(QStringLiteral("采样率"), Header(table, 4));
    EXPECT_EQ(QStringLiteral("声道"), Header(table, 5));
    EXPECT_EQ(0, RowCount(table));

    EXPECT_EQ(QStringLiteral("总音频帧数: 0 | 总样本数: 0 | 总字节数: 0"), summary->text());

    ASSERT_TRUE(widget.toggle() != nullptr);
    EXPECT_FALSE(widget.toggle()->isChecked());
}

// ---------------------------------------------------------------------------
// 2. Append 只置脏不落表；FlushPending 后才补差量并清脏标志
// ---------------------------------------------------------------------------

TEST(AudioFrameTableWidgetTests, AppendMarksPendingAndFlushFillsCells) {
    EnsureApp();

    AudioFrameTableWidget widget;
    QTableView* table = AudioTable(widget);
    ASSERT_TRUE(table != nullptr);

    widget.AppendAudioFrame(1, 1000, 0.5, 1024, 48000, 2, 4096);
    EXPECT_TRUE(widget.HasPending());
    EXPECT_EQ(0, RowCount(table));  // 增量提交前不落到表上

    widget.FlushPending();
    EXPECT_FALSE(widget.HasPending());
    ASSERT_EQ(1, RowCount(table));

    ASSERT_TRUE(HasCell(table, 0, 0));
    ASSERT_TRUE(HasCell(table, 0, 6));
    EXPECT_EQ(QStringLiteral("1"), Cell(table, 0, 0));        // #
    EXPECT_EQ(QStringLiteral("0.500"), Cell(table, 0, 1));    // 播放时间(s)
    EXPECT_EQ(QStringLiteral("1000"), Cell(table, 0, 2));     // 原始 PTS
    EXPECT_EQ(QStringLiteral("1024"), Cell(table, 0, 3));     // 样本数
    EXPECT_EQ(QStringLiteral("48000"), Cell(table, 0, 4));    // 采样率
    EXPECT_EQ(QStringLiteral("2"), Cell(table, 0, 5));        // 声道
    EXPECT_EQ(QStringLiteral("4096"), Cell(table, 0, 6));     // 字节
}

// ---------------------------------------------------------------------------
// 3. 增量刷新 + 汇总聚合
// ---------------------------------------------------------------------------

TEST(AudioFrameTableWidgetTests, FlushIsIncrementalAndSummaryAggregates) {
    EnsureApp();

    AudioFrameTableWidget widget;
    QTableView* table = AudioTable(widget);
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendAudioFrame(1, 1000, 0.5, 1024, 48000, 2, 4096);
    widget.AppendAudioFrame(2, 2000, 1.0, 512, 48000, 2, 2048);
    widget.FlushPending();

    ASSERT_EQ(2, RowCount(table));
    EXPECT_EQ(QStringLiteral("总音频帧数: 2 | 总样本数: 1536 | 总字节数: 6144"),
              summary->text());

    // 再追加一行：只补这一行，已有行的内容不能被重排/丢失
    widget.AppendAudioFrame(3, 3000, 1.5, 256, 44100, 1, 512);
    EXPECT_TRUE(widget.HasPending());
    widget.FlushPending();

    ASSERT_EQ(3, RowCount(table));
    EXPECT_EQ(QStringLiteral("1"), Cell(table, 0, 0));
    EXPECT_EQ(QStringLiteral("2"), Cell(table, 1, 0));
    EXPECT_EQ(QStringLiteral("3"), Cell(table, 2, 0));
    EXPECT_EQ(QStringLiteral("总音频帧数: 3 | 总样本数: 1792 | 总字节数: 6656"),
              summary->text());

    // 无新数据时再刷一次是 no-op，行数不翻倍
    widget.FlushPending();
    EXPECT_EQ(3, RowCount(table));
}

// ---------------------------------------------------------------------------
// 4. ResetAudioFrames 清空缓存与表格，汇总归零
// ---------------------------------------------------------------------------

TEST(AudioFrameTableWidgetTests, ResetClearsTableAndSummary) {
    EnsureApp();

    AudioFrameTableWidget widget;
    QTableView* table = AudioTable(widget);
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendAudioFrame(1, 1000, 0.5, 1024, 48000, 2, 4096);
    widget.FlushPending();
    ASSERT_EQ(1, RowCount(table));

    widget.ResetAudioFrames();
    EXPECT_EQ(0, RowCount(table));
    EXPECT_EQ(QStringLiteral("总音频帧数: 0 | 总样本数: 0 | 总字节数: 0"), summary->text());
}

// ---------------------------------------------------------------------------
// 5. 模型的记录缓存是唯一数据源
//
// 迁移到 model/view 之后，"记录"住在模型里而不是组件里。外部（CSV 导出、汇总）
// 拿的必须是同一份 —— 否则就会出现"表格显示 3 行、导出 5 行"这类对不上。
// ---------------------------------------------------------------------------

TEST(AudioFrameTableWidgetTests, RecordsLiveInModelAndMatchRowCount) {
    EnsureApp();

    AudioFrameTableWidget widget;
    ASSERT_TRUE(widget.audioFrameModel() != nullptr);

    widget.AppendAudioFrame(1, 1000, 0.5, 1024, 48000, 2, 4096);
    widget.AppendAudioFrame(2, 2000, 1.0, 512, 48000, 2, 2048);
    widget.FlushPending();

    EXPECT_EQ(2u, widget.AudioFrameRecords().size());
    EXPECT_EQ(2u, widget.audioFrameModel()->record_count());
    // 无筛选时可见行数 == 记录数
    EXPECT_EQ(2, widget.audioFrameModel()->rowCount());
}
