// PacketTableWidget（码流分析「包」子页）的接缝测试。
//
// 为什么值得单独测：它是帧表↔包表 PTS 互跳的记录侧，也是四个子页里最后抽出的一个。
// 它内部有两条出账路径 —— 切筛选时的「全量重建」与常规的「增量补差」—— 两者必须
// 遵循同一份筛选语义、共用同一个同步游标，错位只会静默少行/错行，界面不崩。另外
// 汇总行的包计数与字节聚合（视频/音频/其他三分 + 平均/最大/最小）是用户判断流结构
// 的直接依据，错了同样只会静默显示旧数字。
//
// 具体盯五件事：
//   1. 表结构：包表 9 列、列名、筛选下拉 5 项、开关默认未勾选。
//   2. 脏标志 + 增量刷新：Append 只入队置脏不落表，Flush 才按游标补差量。
//   3. 单元格内容与汇总聚合：流列 "#索引 类型"、时间/PTS/DTS/时长/大小，汇总计数与聚合。
//   4. 筛选：切「视频流」整表重建只留视频包；之后新数据仍按筛选增量追加；切回恢复全量。
//   5. ResetPackets：清空缓存与表格，汇总归零（完整文案，非构造时的短文案）。
//
// 用 offscreen 平台跑，不需要显示器；只构造控件与提交数据，不渲染。
// CSV 导出是模态对话框分支，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QScrollBar>
#include <QTableView>
#include <QTableWidget>

#include <libavcodec/avcodec.h>

#include "core/domain/model/PacketInfo.h"
#include "ui/analysis_panel/PacketTableWidget.h"

using videoeye::model::PacketInfo;
using videoeye::ui::PacketTableWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_packet_table_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QLabel* SummaryLabel(PacketTableWidget& widget) {
    // 组件内有两个 QLabel（汇总行 + "筛选:"），按文案找汇总行
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("总包数"))) {
            return label;
        }
    }
    return nullptr;
}

PacketInfo MakePacket(int index, int stream_index, int stream_type, int64_t pts, int64_t dts, int64_t duration,
                      int size, double timestamp_seconds) {
    PacketInfo info;
    info.index = index;
    info.stream_index = stream_index;
    info.stream_type = stream_type;
    info.pts = pts;
    info.dts = dts;
    info.duration = duration;
    info.size = size;
    info.flags = AV_PKT_FLAG_KEY;
    info.pos = static_cast<int64_t>(index) * 1024;
    info.timestamp_seconds = timestamp_seconds;
    return info;
}

int RowCount(QTableView* table) {
    return table->model()->rowCount();
}

int ColumnCount(QTableView* table) {
    return table->model()->columnCount();
}

QString Header(QTableView* table, int column) {
    return table->model()->headerData(column, Qt::Horizontal).toString();
}

QString Cell(QTableView* table, int row, int column) {
    return table->model()->index(row, column).data(Qt::DisplayRole).toString();
}

bool HasCell(QTableView* table, int row, int column) {
    return table->model()->index(row, column).isValid();
}
} // namespace

// ---------------------------------------------------------------------------
// 1. 表结构：9 列 + 列名 + 筛选下拉 5 项 + 开关默认未勾选
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, SetupCreatesNineColumnTableAndFilterOptions) {
    EnsureApp();

    PacketTableWidget widget;
    QTableView* table = widget.packetTable();
    ASSERT_TRUE(table != nullptr);

    EXPECT_EQ(9, ColumnCount(table));
    EXPECT_EQ(QStringLiteral("流"), Header(table, 1));
    EXPECT_EQ(QStringLiteral("显示时间(PTS)"), Header(table, 3));
    EXPECT_EQ(QStringLiteral("包大小"), Header(table, 6));
    EXPECT_EQ(QStringLiteral("标志"), Header(table, 7));
    EXPECT_EQ(QStringLiteral("文件偏移"), Header(table, 8));
    EXPECT_EQ(0, RowCount(table));

    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(filter != nullptr);
    ASSERT_EQ(5, filter->count());
    EXPECT_EQ(QStringLiteral("全部流"), filter->itemText(0));
    EXPECT_EQ(QStringLiteral("视频流"), filter->itemText(1));
    EXPECT_EQ(QStringLiteral("音频流"), filter->itemText(2));
    EXPECT_EQ(QStringLiteral("其他流"), filter->itemText(3));
    EXPECT_EQ(QStringLiteral("视频关键帧"), filter->itemText(4));
    EXPECT_EQ(0, filter->currentIndex());

    // 构造后未刷新的初始文案（短文案；刷过一次后变为完整文案）
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(summary != nullptr);
    EXPECT_EQ(QStringLiteral("总包数: 0 | 视频包: 0 | 音频包: 0 | 其他包: 0"), summary->text());

    // 采集开关已上提到「播放统计与帧包」页头（整页一个聚合开关），子页不再有同名开关。
    for (QCheckBox* box : widget.findChildren<QCheckBox*>())
        EXPECT_NE(box->text(), QStringLiteral("启用分析"));
}

// ---------------------------------------------------------------------------
// 2. 脏标志 + 增量刷新：Append 只置脏不落表，Flush 后才补差量并清脏
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, AppendMarksPendingAndFlushFillsCells) {
    EnsureApp();

    PacketTableWidget widget;
    QTableView* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1234, 0.5));
    EXPECT_TRUE(widget.HasPending());
    EXPECT_EQ(0, RowCount(table)); // 增量提交前不落到表上

    widget.FlushPending();
    EXPECT_FALSE(widget.HasPending());
    ASSERT_EQ(1, RowCount(table));

    ASSERT_TRUE(HasCell(table, 0, 0));
    ASSERT_TRUE(HasCell(table, 0, 6));
    EXPECT_EQ(QStringLiteral("1"), Cell(table, 0, 0));       // #
    EXPECT_EQ(QStringLiteral("#0 视频"), Cell(table, 0, 1)); // 流（索引+类型合并列）
    EXPECT_EQ(QStringLiteral("0.500"), Cell(table, 0, 2));   // 播放时间(s)
    EXPECT_EQ(QStringLiteral("1000"), Cell(table, 0, 3));    // 显示时间(PTS)
    EXPECT_EQ(QStringLiteral("900"), Cell(table, 0, 4));     // 解码时间(DTS)
    EXPECT_EQ(QStringLiteral("40"), Cell(table, 0, 5));      // 时长
    EXPECT_EQ(QStringLiteral("1234"), Cell(table, 0, 6));    // 包大小
    EXPECT_EQ(QStringLiteral("KEY"), Cell(table, 0, 7));     // 标志
    EXPECT_EQ(QStringLiteral("1024"), Cell(table, 0, 8));    // 文件偏移

    EXPECT_EQ(QStringLiteral("总包数: 1 | 总字节数: 1.2 KB | 视频包: 1 | 音频包: 0 | 其他包: 0 | "
                             "平均包大小: 1234 B | 最大包大小: 1234 B | 最小包大小: 1234 B"),
              summary->text());
}

// ---------------------------------------------------------------------------
// 3. 增量刷新 + 汇总聚合：新行只补差量，已有行不重排；三类包计数与字节聚合正确
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, FlushIsIncrementalAndSummaryAggregates) {
    EnsureApp();

    PacketTableWidget widget;
    QTableView* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5));
    widget.AppendPacket(MakePacket(2, 1, AVMEDIA_TYPE_AUDIO, 2000, 1900, 20, 500, 1.0));
    widget.FlushPending();

    ASSERT_EQ(2, RowCount(table));
    EXPECT_EQ(QStringLiteral("#1 音频"), Cell(table, 1, 1));

    // 再追加一行（数据流）：只补这一行，已有行的内容不能被重排/丢失
    widget.AppendPacket(MakePacket(3, 2, AVMEDIA_TYPE_DATA, 3000, 2900, 10, 400, 1.5));
    EXPECT_TRUE(widget.HasPending());
    widget.FlushPending();

    ASSERT_EQ(3, RowCount(table));
    EXPECT_EQ(QStringLiteral("1"), Cell(table, 0, 0));
    EXPECT_EQ(QStringLiteral("2"), Cell(table, 1, 0));
    EXPECT_EQ(QStringLiteral("3"), Cell(table, 2, 0));
    EXPECT_EQ(QStringLiteral("#2 数据"), Cell(table, 2, 1));

    // 总字节 1900 -> 1.9 KB；平均 633；最大 1000；最小 400
    EXPECT_EQ(QStringLiteral("总包数: 3 | 总字节数: 1.9 KB | 视频包: 1 | 音频包: 1 | 其他包: 1 | "
                             "平均包大小: 633 B | 最大包大小: 1000 B | 最小包大小: 400 B"),
              summary->text());

    // 无新数据时再刷一次是 no-op，行数不翻倍
    widget.FlushPending();
    EXPECT_EQ(3, RowCount(table));
}

// ---------------------------------------------------------------------------
// 4. 筛选：全量重建 + 增量追加都遵循同一份筛选语义
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, FilterRebuildsTableAndAlsoAppliesToIncrementalRows) {
    EnsureApp();

    PacketTableWidget widget;
    QTableView* table = widget.packetTable();
    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(filter != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5));
    widget.AppendPacket(MakePacket(2, 1, AVMEDIA_TYPE_AUDIO, 2000, 1900, 20, 500, 1.0));
    widget.AppendPacket(MakePacket(3, 2, AVMEDIA_TYPE_DATA, 3000, 2900, 10, 400, 1.5));
    widget.FlushPending();
    ASSERT_EQ(3, RowCount(table));

    // 切「视频流」：立即整表重建，只留视频包
    filter->setCurrentIndex(1);
    ASSERT_EQ(1, RowCount(table));
    EXPECT_EQ(QStringLiteral("#0 视频"), Cell(table, 0, 1));

    // 之后新数据走增量路径：不匹配筛选的行不能出现
    widget.AppendPacket(MakePacket(4, 1, AVMEDIA_TYPE_AUDIO, 4000, 3900, 20, 600, 2.0));
    widget.FlushPending();
    EXPECT_EQ(1, RowCount(table));

    // 匹配的仍要补进来
    widget.AppendPacket(MakePacket(5, 0, AVMEDIA_TYPE_VIDEO, 5000, 4900, 40, 1100, 2.5));
    widget.FlushPending();
    ASSERT_EQ(2, RowCount(table));
    EXPECT_EQ(QStringLiteral("5"), Cell(table, 1, 0));

    // 切回「全部流」：重建后恢复全量（含前两步被挡掉的记录）
    filter->setCurrentIndex(0);
    EXPECT_EQ(5, RowCount(table));

    // 切「其他流」：只留非视频非音频
    filter->setCurrentIndex(3);
    ASSERT_EQ(1, RowCount(table));
    EXPECT_EQ(QStringLiteral("#2 数据"), Cell(table, 0, 1));
}

// ---------------------------------------------------------------------------
// 5. ResetPackets 清空缓存与表格，汇总归零（完整文案）
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, ResetClearsTableAndSummary) {
    EnsureApp();

    PacketTableWidget widget;
    QTableView* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5));
    widget.AppendPacket(MakePacket(2, 1, AVMEDIA_TYPE_AUDIO, 2000, 1900, 20, 500, 1.0));
    widget.FlushPending();
    ASSERT_EQ(2, RowCount(table));

    widget.ResetPackets();
    EXPECT_EQ(0, RowCount(table));
    EXPECT_EQ(QStringLiteral("总包数: 0 | 总字节数: 0 B | 视频包: 0 | 音频包: 0 | 其他包: 0 | "
                             "平均包大小: 0 B | 最大包大小: 0 B | 最小包大小: 0 B"),
              summary->text());

    // Reset 会把汇总置脏一次（与原 FramePacketView 行为一致：下一次节拍多刷一次
    // no-op 汇总），显式刷掉后应恢复干净状态
    widget.FlushPending();
    EXPECT_FALSE(widget.HasPending());
    EXPECT_EQ(0, RowCount(table));
}

// ---------------------------------------------------------------------------
// 6. 增量刷新不能把正在往回翻看的用户拽回底部
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, FlushKeepsUserScrollPositionWhenNotPinned) {
    EnsureApp();

    PacketTableWidget widget;
    widget.resize(640, 200);
    widget.show();
    QCoreApplication::processEvents();

    QTableView* table = widget.packetTable();
    ASSERT_TRUE(table != nullptr);

    for (int i = 0; i < 120; ++i) {
        widget.AppendPacket(MakePacket(i, 0, AVMEDIA_TYPE_VIDEO, i, i, 1, 1000, i * 0.04));
    }
    widget.FlushPending();
    QCoreApplication::processEvents();

    QScrollBar* bar = table->verticalScrollBar();
    ASSERT_GT(bar->maximum(), 0) << "表格没有产生可滚动高度，测试前提不成立";
    EXPECT_EQ(bar->value(), bar->maximum()) << "跟随模式下应停在最新一条";

    // 用户上翻查看历史：后续入库不得改变滚动位置
    bar->setValue(0);
    for (int i = 120; i < 160; ++i) {
        widget.AppendPacket(MakePacket(i, 0, AVMEDIA_TYPE_VIDEO, i, i, 1, 1000, i * 0.04));
    }
    widget.FlushPending();
    QCoreApplication::processEvents();
    EXPECT_EQ(bar->value(), 0) << "用户上翻时不应被拽回底部";

    // 用户自己滑回底部：恢复跟随
    bar->setValue(bar->maximum());
    widget.AppendPacket(MakePacket(160, 0, AVMEDIA_TYPE_VIDEO, 160, 160, 1, 1000, 6.4));
    widget.FlushPending();
    QCoreApplication::processEvents();
    EXPECT_EQ(bar->value(), bar->maximum());
}

// ---------------------------------------------------------------------------
// 7. 「视频关键帧」筛选：只看带 AV_PKT_FLAG_KEY 的视频包，音频包不能混进来
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, KeyFrameFilterKeepsOnlyKeyVideoPackets) {
    EnsureApp();

    PacketTableWidget widget;
    QTableView* table = widget.packetTable();
    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(filter != nullptr);

    PacketInfo key_video = MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5);
    PacketInfo delta_video = MakePacket(2, 0, AVMEDIA_TYPE_VIDEO, 2000, 1900, 40, 800, 1.0);
    delta_video.flags = 0;
    PacketInfo key_audio = MakePacket(3, 1, AVMEDIA_TYPE_AUDIO, 3000, 2900, 20, 500, 1.5); // 带 KEY 但不是视频

    widget.AppendPacket(key_video);
    widget.AppendPacket(delta_video);
    widget.AppendPacket(key_audio);
    widget.FlushPending();

    filter->setCurrentIndex(4); // 视频关键帧
    ASSERT_EQ(1, RowCount(table));
    EXPECT_EQ(QStringLiteral("#0 视频"), Cell(table, 0, 1));
    EXPECT_EQ(QStringLiteral("KEY"), Cell(table, 0, 7));

    // 增量路径同样只收关键帧视频包
    widget.AppendPacket(key_audio);
    widget.AppendPacket(delta_video);
    widget.FlushPending();
    EXPECT_EQ(1, RowCount(table));

    widget.AppendPacket(key_video);
    widget.FlushPending();
    ASSERT_EQ(2, RowCount(table));
    EXPECT_EQ(QStringLiteral("1"), Cell(table, 1, 0));
}

// ---------------------------------------------------------------------------
// 8. 裁剪后汇总仍然正确（增量计数器扣减 + min/max 失效重算）
//
// 记录上限可注入，用 3 条记录把裁剪路径走一遍；否则要灌十万条才触发淘汰。
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, TrimKeepsSummaryConsistent) {
    EnsureApp();

    PacketTableWidget widget(nullptr, 3);
    QTableView* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    // 第一批: 500(最大) / 100(最小) / 300
    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 0, 0, 1, 500, 0.0));
    widget.AppendPacket(MakePacket(2, 0, AVMEDIA_TYPE_VIDEO, 1, 1, 1, 100, 0.1));
    widget.AppendPacket(MakePacket(3, 0, AVMEDIA_TYPE_VIDEO, 2, 2, 1, 300, 0.2));
    widget.FlushPending();
    ASSERT_EQ(3, RowCount(table));

    // 第二批把最早三条挤掉: 剩下 200 / 400(新最大) / 50(新最小)
    widget.AppendPacket(MakePacket(4, 0, AVMEDIA_TYPE_VIDEO, 3, 3, 1, 200, 0.3));
    widget.AppendPacket(MakePacket(5, 0, AVMEDIA_TYPE_VIDEO, 4, 4, 1, 400, 0.4));
    widget.AppendPacket(MakePacket(6, 0, AVMEDIA_TYPE_VIDEO, 5, 5, 1, 50, 0.5));
    widget.FlushPending();

    ASSERT_EQ(3, RowCount(table));
    EXPECT_EQ(QStringLiteral("4"), Cell(table, 0, 0)); // 头部三条已被淘汰
    EXPECT_EQ(QStringLiteral("总包数: 3 | 总字节数: 650 B | 视频包: 3 | 音频包: 0 | 其他包: 0 | "
                             "平均包大小: 216 B | 最大包大小: 400 B | 最小包大小: 50 B"),
              summary->text());
}