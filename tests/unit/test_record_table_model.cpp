// RecordTableModel 的回归。
//
// 重点不是"model 会不会用"，而是守住迁到 model/view 想解决的两件事：
//   1. 十万级记录能装下、且插入/裁剪都不退化成 O(n²)；
//   2. 裁剪与筛选之后"行号 ↔ 记录索引"的映射仍然自洽 —— 这条错了的表现是
//      "点第 100 行跳到第 3 行的帧"，只有把映射单独测出来才发现得了。
//
// 本测试不需要 QApplication：QAbstractTableModel 不碰 GUI。

#include "ui/analysis_panel/RecordTableModel.h"

#include <chrono>

#include <gtest/gtest.h>

namespace {

using videoeye::ui::kDefaultMaxRecordCount;
using videoeye::ui::RecordTableModel;

struct Row {
    int id = 0;
    int value = 0;
};

std::vector<RecordTableModel<Row>::ColumnSpec> MakeColumns() {
    using Spec = RecordTableModel<Row>::ColumnSpec;
    return {
        Spec{QStringLiteral("id"), [](const Row& r) { return QString::number(r.id); }, {}},
        Spec{QStringLiteral("value"), [](const Row& r) { return QString::number(r.value); },
             // 负数染红：顺带验证 ForegroundRole 走列适配器
             [](const Row& r) { return r.value < 0 ? QColor("#e53935") : QColor(); }},
    };
}

std::vector<Row> MakeRows(int from, int count) {
    std::vector<Row> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        out.push_back(Row{from + i, (from + i) % 7 - 3});
    }
    return out;
}

} // namespace

TEST(RecordTableModelTest, EmptyModel) {
    RecordTableModel<Row> model(MakeColumns());
    EXPECT_EQ(0, model.rowCount());
    EXPECT_EQ(2, model.columnCount());
    EXPECT_TRUE(model.records().empty());
}

TEST(RecordTableModelTest, HeaderAndData) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 3));

    ASSERT_EQ(3, model.rowCount());
    EXPECT_EQ(QStringLiteral("id"), model.headerData(0, Qt::Horizontal).toString());
    EXPECT_EQ(QStringLiteral("value"), model.headerData(1, Qt::Horizontal).toString());

    EXPECT_EQ(QStringLiteral("0"), model.index(0, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(QStringLiteral("2"), model.index(2, 0).data(Qt::DisplayRole).toString());
}

// 越界/无效索引必须返回空 QVariant 而不是崩：视图在重排期间会拿旧索引来问。
TEST(RecordTableModelTest, OutOfRangeIndexReturnsEmpty) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 1));

    EXPECT_FALSE(model.index(99, 0).data(Qt::DisplayRole).isValid());
    EXPECT_FALSE(model.index(0, 99).data(Qt::DisplayRole).isValid());
    EXPECT_FALSE(model.index(-1, 0).data(Qt::DisplayRole).isValid());
    // 父节点非根 -> 0 行 0 列（表格模型没有子树）
    EXPECT_EQ(0, model.rowCount(model.index(0, 0)));
    EXPECT_EQ(0, model.columnCount(model.index(0, 0)));
}

TEST(RecordTableModelTest, ForegroundComesFromColumnSpec) {
    RecordTableModel<Row> model(MakeColumns());
    // value = id % 7 - 3 -> id 0 => -3（负），id 3 => 0（非负）
    model.AppendRecords(MakeRows(0, 4));

    const QVariant fg0 = model.index(0, 1).data(Qt::ForegroundRole);
    ASSERT_TRUE(fg0.isValid());
    EXPECT_EQ(QColor("#e53935"), fg0.value<QColor>());

    // 非负不染色：返回无效 QVariant，让视图用默认色
    EXPECT_FALSE(model.index(3, 1).data(Qt::ForegroundRole).isValid());
}

// ---- 裁剪 ----

TEST(RecordTableModelTest, TrimFrontKeepsTailAndReindexes) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 10));
    ASSERT_EQ(10, model.rowCount());

    const std::size_t removed = model.TrimFront(4);
    EXPECT_EQ(4u, removed);
    ASSERT_EQ(6, model.rowCount());
    EXPECT_EQ(6u, model.record_count());

    // 剩下的必须是原来第 4~9 条
    EXPECT_EQ(QStringLiteral("4"), model.index(0, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(QStringLiteral("9"), model.index(5, 0).data(Qt::DisplayRole).toString());
    // 裁剪是真的从 records_ 里 erase 掉了前 4 条，所以记录索引跟着从 0 重排 ——
    // 行号 ↔ 记录索引仍然是恒等映射（无筛选时）。
    EXPECT_EQ(0u, model.RecordIndexOfRow(0));
    EXPECT_EQ(0, model.RowOfRecord(0u));
    EXPECT_EQ(5u, model.RecordIndexOfRow(5));
}

TEST(RecordTableModelTest, TrimFrontMoreThanSizeClampsToEmpty) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 3));
    model.TrimFront(100);
    EXPECT_EQ(0, model.rowCount());
    EXPECT_EQ(0u, model.record_count());
}

TEST(RecordTableModelTest, TrimZeroIsNoOp) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 3));
    EXPECT_EQ(0u, model.TrimFront(0));
    EXPECT_EQ(3, model.rowCount());
}

TEST(RecordTableModelTest, ClearEmptiesEverything) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 5));
    model.Clear();
    EXPECT_EQ(0, model.rowCount());
    EXPECT_TRUE(model.records().empty());
}

// ---- 筛选 ----

TEST(RecordTableModelTest, FilterShrinksVisibleRowsButKeepsRecords) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 10));

    model.SetFilter([](const Row& r) { return r.id % 2 == 0; });
    ASSERT_EQ(5, model.rowCount());
    EXPECT_EQ(10u, model.record_count()); // 记录还在，只是不显示

    EXPECT_EQ(QStringLiteral("0"), model.index(0, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(QStringLiteral("2"), model.index(1, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(QStringLiteral("8"), model.index(4, 0).data(Qt::DisplayRole).toString());
}

TEST(RecordTableModelTest, RowOfRecordAccountsForFilter) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 10));
    model.SetFilter([](const Row& r) { return r.id % 2 == 0; });

    EXPECT_EQ(0, model.RowOfRecord(0u));
    EXPECT_EQ(4, model.RowOfRecord(8u));
    EXPECT_EQ(-1, model.RowOfRecord(7u)) << "被筛掉的记录不该有行号";
    // 反过来：第 1 行是记录索引 2（可见的是 0,2,4,6,8）
    EXPECT_EQ(2u, model.RecordIndexOfRow(1));
    EXPECT_EQ(1, model.RowOfRecord(model.RecordIndexOfRow(1)));
}

// 筛选生效期间追加：新记录要按同一条件判断可见性，不能一律显示。
TEST(RecordTableModelTest, AppendRespectsActiveFilter) {
    RecordTableModel<Row> model(MakeColumns());
    model.SetFilter([](const Row& r) { return r.id % 2 == 0; });
    model.AppendRecords(MakeRows(0, 4)); // 0,2 可见
    ASSERT_EQ(2, model.rowCount());

    model.AppendRecords(MakeRows(4, 4)); // 4,6 可见
    ASSERT_EQ(4, model.rowCount());
    EXPECT_EQ(QStringLiteral("6"), model.index(3, 0).data(Qt::DisplayRole).toString());
    // 记录是全量保留的（0..7 共 8 条），筛选只影响行号
    EXPECT_EQ(8u, model.record_count());
}

// 筛选期间裁剪：可见索引要整体前移，不能出现"行号指向已被裁掉的记录"。
TEST(RecordTableModelTest, TrimFrontWithFilterReindexesVisible) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 10));
    model.SetFilter([](const Row& r) { return r.id % 2 == 0; });
    ASSERT_EQ(5, model.rowCount());

    model.TrimFront(4); // 记录 0..3 被裁，剩下 4..9；可见的应为 4,6,8
    ASSERT_EQ(3, model.rowCount());
    EXPECT_EQ(QStringLiteral("4"), model.index(0, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(QStringLiteral("6"), model.index(1, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(QStringLiteral("8"), model.index(2, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(0, model.RowOfRecord(0u)); // 记录索引 0 == 原 id 4
}

TEST(RecordTableModelTest, ResetFilterRestoresAllRows) {
    RecordTableModel<Row> model(MakeColumns());
    model.AppendRecords(MakeRows(0, 10));
    model.SetFilter([](const Row& r) { return r.id % 2 == 0; });
    ASSERT_EQ(5, model.rowCount());

    model.SetFilter(nullptr);
    EXPECT_EQ(10, model.rowCount());
    EXPECT_FALSE(model.HasFilter());
}

// ---- 十万级 ----
//
// 这条是迁到 model/view 的**目的**所在：以前行数 = QTableWidgetItem 数，
// 十万行 × 7 列是七十万个堆对象；现在只是十万条 POD 记录。

TEST(RecordTableModelTest, HandlesHundredThousandRecords) {
    RecordTableModel<Row> model(MakeColumns());

    const auto start = std::chrono::steady_clock::now();
    // 分 10 批插入，模拟播放期"每 120ms 交一批"的真实节奏
    for (int batch = 0; batch < 10; ++batch) {
        model.AppendRecords(MakeRows(batch * 10000, 10000));
    }
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    ASSERT_EQ(static_cast<int>(kDefaultMaxRecordCount), model.rowCount());
    EXPECT_EQ(kDefaultMaxRecordCount, model.record_count());

    // 20 万次 QString::number 级别的构造都不该发生（data() 是按需的），
    // 所以插入应当是毫秒级而不是秒级。给 2 秒的宽松上界，只为拦住 O(n²) 回归。
    EXPECT_LT(elapsed_ms, 2000.0) << "插入 10 万条耗时 " << elapsed_ms << " ms，疑似退化";

    // 末尾行的映射必须仍然正确：这是"裁过/追加过很多次之后索引还准不准"的取证点
    EXPECT_EQ(QStringLiteral("99999"), model.index(99999, 0).data(Qt::DisplayRole).toString());
    EXPECT_EQ(kDefaultMaxRecordCount - 1, model.RecordIndexOfRow(99999));
}

TEST(RecordTableModelTest, DefaultMaxRecordCountCoversOneHourVideo) {
    // 1 小时 25fps ≈ 9 万帧；10 万必须装得下，否则长视频后半段会被静默丢掉。
    EXPECT_GE(kDefaultMaxRecordCount, 90000u);
}

// 反复追加 + 裁剪（模拟长视频播到上限之后持续滚动），行号不能漂移。
TEST(RecordTableModelTest, RollingWindowStaysConsistent) {
    RecordTableModel<Row> model(MakeColumns());
    constexpr std::size_t kMax = 1000; // 用小窗口跑同样的逻辑

    int next_id = 0;
    for (int round = 0; round < 20; ++round) {
        model.AppendRecords(MakeRows(next_id, 300));
        next_id += 300;
        if (model.record_count() > kMax) {
            model.TrimFront(model.record_count() - kMax);
        }
    }

    ASSERT_EQ(static_cast<int>(kMax), model.rowCount());
    // 最后一行应当是最后插入的那条
    EXPECT_EQ(QString::number(next_id - 1), model.index(kMax - 1, 0).data().toString());
    // 行号 ↔ 记录索引在无筛选时是恒等映射
    for (int row : {0, 1, 500, 999}) {
        EXPECT_EQ(static_cast<std::size_t>(row), model.RecordIndexOfRow(row));
        EXPECT_EQ(row, model.RowOfRecord(model.RecordIndexOfRow(row)));
    }
}
