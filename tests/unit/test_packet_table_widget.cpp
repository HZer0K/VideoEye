// PacketTableWidget（码流分析「包」子页）的接缝测试。
//
// 为什么值得单独测：它是帧表↔包表 PTS 互跳的记录侧，也是四个子页里最后抽出的一个。
// 它内部有两条出账路径 —— 切筛选时的「全量重建」与常规的「增量补差」—— 两者必须
// 遵循同一份筛选语义、共用同一个同步游标，错位只会静默少行/错行，界面不崩。另外
// 汇总行的包计数与字节聚合（视频/音频/其他三分 + 平均/最大/最小）是用户判断流结构
// 的直接依据，错了同样只会静默显示旧数字。
//
// 具体盯五件事：
//   1. 表结构：包表 7 列、列名、筛选下拉 4 项、开关默认未勾选。
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

PacketInfo MakePacket(int index, int stream_index, int stream_type, int64_t pts,
                      int64_t dts, int64_t duration, int size, double timestamp_seconds) {
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

}  // namespace

// ---------------------------------------------------------------------------
// 1. 表结构：7 列 + 列名 + 筛选下拉 4 项 + 开关默认未勾选
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, SetupCreatesSevenColumnTableAndFilterOptions) {
    EnsureApp();

    PacketTableWidget widget;
    QTableWidget* table = widget.packetTable();
    ASSERT_TRUE(table != nullptr);

    EXPECT_EQ(7, table->columnCount());
    EXPECT_EQ(QStringLiteral("流"), table->horizontalHeaderItem(1)->text());
    EXPECT_EQ(QStringLiteral("显示时间(PTS)"), table->horizontalHeaderItem(3)->text());
    EXPECT_EQ(QStringLiteral("包大小"), table->horizontalHeaderItem(6)->text());
    EXPECT_EQ(0, table->rowCount());

    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(filter != nullptr);
    ASSERT_EQ(4, filter->count());
    EXPECT_EQ(QStringLiteral("全部流"), filter->itemText(0));
    EXPECT_EQ(QStringLiteral("视频流"), filter->itemText(1));
    EXPECT_EQ(QStringLiteral("音频流"), filter->itemText(2));
    EXPECT_EQ(QStringLiteral("其他流"), filter->itemText(3));
    EXPECT_EQ(0, filter->currentIndex());

    // 构造后未刷新的初始文案（短文案；刷过一次后变为完整文案）
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(summary != nullptr);
    EXPECT_EQ(QStringLiteral("总包数: 0 | 视频包: 0 | 音频包: 0 | 其他包: 0"), summary->text());

    ASSERT_TRUE(widget.toggle() != nullptr);
    EXPECT_FALSE(widget.toggle()->isChecked());
}

// ---------------------------------------------------------------------------
// 2. 脏标志 + 增量刷新：Append 只置脏不落表，Flush 后才补差量并清脏
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, AppendMarksPendingAndFlushFillsCells) {
    EnsureApp();

    PacketTableWidget widget;
    QTableWidget* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1234, 0.5));
    EXPECT_TRUE(widget.HasPending());
    EXPECT_EQ(0, table->rowCount());  // 增量提交前不落到表上

    widget.FlushPending();
    EXPECT_FALSE(widget.HasPending());
    ASSERT_EQ(1, table->rowCount());

    ASSERT_TRUE(table->item(0, 0) != nullptr);
    ASSERT_TRUE(table->item(0, 6) != nullptr);
    EXPECT_EQ(QStringLiteral("1"), table->item(0, 0)->text());          // #
    EXPECT_EQ(QStringLiteral("#0 视频"), table->item(0, 1)->text());     // 流（索引+类型合并列）
    EXPECT_EQ(QStringLiteral("0.500"), table->item(0, 2)->text());      // 播放时间(s)
    EXPECT_EQ(QStringLiteral("1000"), table->item(0, 3)->text());       // 显示时间(PTS)
    EXPECT_EQ(QStringLiteral("900"), table->item(0, 4)->text());        // 解码时间(DTS)
    EXPECT_EQ(QStringLiteral("40"), table->item(0, 5)->text());         // 时长
    EXPECT_EQ(QStringLiteral("1234"), table->item(0, 6)->text());       // 包大小

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
    QTableWidget* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5));
    widget.AppendPacket(MakePacket(2, 1, AVMEDIA_TYPE_AUDIO, 2000, 1900, 20, 500, 1.0));
    widget.FlushPending();

    ASSERT_EQ(2, table->rowCount());
    EXPECT_EQ(QStringLiteral("#1 音频"), table->item(1, 1)->text());

    // 再追加一行（数据流）：只补这一行，已有行的内容不能被重排/丢失
    widget.AppendPacket(MakePacket(3, 2, AVMEDIA_TYPE_DATA, 3000, 2900, 10, 400, 1.5));
    EXPECT_TRUE(widget.HasPending());
    widget.FlushPending();

    ASSERT_EQ(3, table->rowCount());
    EXPECT_EQ(QStringLiteral("1"), table->item(0, 0)->text());
    EXPECT_EQ(QStringLiteral("2"), table->item(1, 0)->text());
    EXPECT_EQ(QStringLiteral("3"), table->item(2, 0)->text());
    EXPECT_EQ(QStringLiteral("#2 数据"), table->item(2, 1)->text());

    // 总字节 1900 -> 1.9 KB；平均 633；最大 1000；最小 400
    EXPECT_EQ(QStringLiteral("总包数: 3 | 总字节数: 1.9 KB | 视频包: 1 | 音频包: 1 | 其他包: 1 | "
                             "平均包大小: 633 B | 最大包大小: 1000 B | 最小包大小: 400 B"),
              summary->text());

    // 无新数据时再刷一次是 no-op，行数不翻倍
    widget.FlushPending();
    EXPECT_EQ(3, table->rowCount());
}

// ---------------------------------------------------------------------------
// 4. 筛选：全量重建 + 增量追加都遵循同一份筛选语义
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, FilterRebuildsTableAndAlsoAppliesToIncrementalRows) {
    EnsureApp();

    PacketTableWidget widget;
    QTableWidget* table = widget.packetTable();
    QComboBox* filter = widget.findChild<QComboBox*>();
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(filter != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5));
    widget.AppendPacket(MakePacket(2, 1, AVMEDIA_TYPE_AUDIO, 2000, 1900, 20, 500, 1.0));
    widget.AppendPacket(MakePacket(3, 2, AVMEDIA_TYPE_DATA, 3000, 2900, 10, 400, 1.5));
    widget.FlushPending();
    ASSERT_EQ(3, table->rowCount());

    // 切「视频流」：立即整表重建，只留视频包
    filter->setCurrentIndex(1);
    ASSERT_EQ(1, table->rowCount());
    EXPECT_EQ(QStringLiteral("#0 视频"), table->item(0, 1)->text());

    // 之后新数据走增量路径：不匹配筛选的行不能出现
    widget.AppendPacket(MakePacket(4, 1, AVMEDIA_TYPE_AUDIO, 4000, 3900, 20, 600, 2.0));
    widget.FlushPending();
    EXPECT_EQ(1, table->rowCount());

    // 匹配的仍要补进来
    widget.AppendPacket(MakePacket(5, 0, AVMEDIA_TYPE_VIDEO, 5000, 4900, 40, 1100, 2.5));
    widget.FlushPending();
    ASSERT_EQ(2, table->rowCount());
    EXPECT_EQ(QStringLiteral("5"), table->item(1, 0)->text());

    // 切回「全部流」：重建后恢复全量（含前两步被挡掉的记录）
    filter->setCurrentIndex(0);
    EXPECT_EQ(5, table->rowCount());

    // 切「其他流」：只留非视频非音频
    filter->setCurrentIndex(3);
    ASSERT_EQ(1, table->rowCount());
    EXPECT_EQ(QStringLiteral("#2 数据"), table->item(0, 1)->text());
}

// ---------------------------------------------------------------------------
// 5. ResetPackets 清空缓存与表格，汇总归零（完整文案）
// ---------------------------------------------------------------------------

TEST(PacketTableWidgetTests, ResetClearsTableAndSummary) {
    EnsureApp();

    PacketTableWidget widget;
    QTableWidget* table = widget.packetTable();
    QLabel* summary = SummaryLabel(widget);
    ASSERT_TRUE(table != nullptr);
    ASSERT_TRUE(summary != nullptr);

    widget.AppendPacket(MakePacket(1, 0, AVMEDIA_TYPE_VIDEO, 1000, 900, 40, 1000, 0.5));
    widget.AppendPacket(MakePacket(2, 1, AVMEDIA_TYPE_AUDIO, 2000, 1900, 20, 500, 1.0));
    widget.FlushPending();
    ASSERT_EQ(2, table->rowCount());

    widget.ResetPackets();
    EXPECT_EQ(0, table->rowCount());
    EXPECT_EQ(QStringLiteral("总包数: 0 | 总字节数: 0 B | 视频包: 0 | 音频包: 0 | 其他包: 0 | "
                             "平均包大小: 0 B | 最大包大小: 0 B | 最小包大小: 0 B"),
              summary->text());

    // Reset 会把汇总置脏一次（与原 FramePacketView 行为一致：下一次节拍多刷一次
    // no-op 汇总），显式刷掉后应恢复干净状态
    widget.FlushPending();
    EXPECT_FALSE(widget.HasPending());
    EXPECT_EQ(0, table->rowCount());
}