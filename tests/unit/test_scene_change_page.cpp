// SceneChangePage 的页面级行为测试（评审「页面组件补测」）。
//
// 这一页的接缝是「播放期逐帧 AppendResult -> 面板批量节拍 FlushPending」：
// AppendResult 只入队置脏、FlushPending 只按同步游标补增量行。游标一旦算错
// （重复补行 / 漏行）不会崩，只会静默多行少行；Reset 忘了归零游标，换文件后的
// 第一批切换点会永远不显示 —— 这两类都属于手动点测几乎发现不了的回归。
//
// 只构造控件、不渲染（offscreen 平台），导出按钮会弹 QFileDialog 模态框，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>

#include <cstddef>

#include "core/domain/model/SceneChangeResult.h"
#include "ui/analysis_panel/SceneChangePage.h"

using videoeye::model::SceneChangeResult;
using videoeye::ui::SceneChangePage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_scene_change_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

SceneChangeResult MakeRecord(int frame_index, double timestamp, double score) {
    SceneChangeResult record;
    record.frame_index = frame_index;
    record.timestamp = timestamp;
    record.score = score;
    return record;
}

// 汇总标签是全页唯一含「切换点」文案的 QLabel（标题只有「场景切换检测」）。
QLabel* FindSummaryLabel(SceneChangePage* page) {
    const auto labels = page->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text().contains(QStringLiteral("切换点")))
            return label;
    }
    return nullptr;
}

} // namespace

// 「启用检测」开关：初值跟随构造参数，用户拨动双向同步 feature_checked_ 并发信号。
TEST(SceneChangePageTests, FeatureToggleFollowsCtorAndEmits) {
    EnsureApp();

    // 初值 true：构造时就体现在勾选框上
    SceneChangePage on_page(/*feature_checked=*/true);
    QCheckBox* on_toggle = on_page.findChild<QCheckBox*>();
    ASSERT_TRUE(on_toggle != nullptr);
    EXPECT_TRUE(on_toggle->isChecked());
    EXPECT_TRUE(on_page.feature_checked());

    SceneChangePage page(/*feature_checked=*/false);
    QCheckBox* toggle = page.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle != nullptr);
    EXPECT_FALSE(toggle->isChecked());
    EXPECT_FALSE(page.feature_checked());

    int emit_count = 0;
    bool last_value = true;
    QObject::connect(&page, &SceneChangePage::FeatureToggled, [&emit_count, &last_value](bool enabled) {
        ++emit_count;
        last_value = enabled;
    });

    toggle->setChecked(true);
    EXPECT_EQ(emit_count, 1);
    EXPECT_TRUE(last_value);
    EXPECT_TRUE(page.feature_checked());

    toggle->setChecked(false);
    EXPECT_EQ(emit_count, 2);
    EXPECT_FALSE(last_value);
    EXPECT_FALSE(page.feature_checked());
}

// AppendResult 只入队置脏：未到批量节拍前表格必须保持空。
TEST(SceneChangePageTests, AppendOnlyQueuesUntilFlush) {
    EnsureApp();
    SceneChangePage page(/*feature_checked=*/false);

    QTableWidget* table = page.findChild<QTableWidget*>();
    ASSERT_TRUE(table != nullptr);
    ASSERT_EQ(table->columnCount(), 3);

    page.AppendResult(MakeRecord(3, 65.0, 0.9));
    page.AppendResult(MakeRecord(4, 66.0, 0.5));

    EXPECT_TRUE(page.HasPending());
    EXPECT_EQ(page.records().size(), std::size_t{2});
    EXPECT_EQ(table->rowCount(), 0); // 未到节拍，一行都不许画
}

// FlushPending 只补增量行且幂等：再刷一次不重复补，再追加一条只多一行。
TEST(SceneChangePageTests, FlushAppendsIncrementalRowsIdempotently) {
    EnsureApp();
    SceneChangePage page(/*feature_checked=*/false);
    QTableWidget* table = page.findChild<QTableWidget*>();
    ASSERT_TRUE(table != nullptr);

    page.AppendResult(MakeRecord(3, 65.0, 0.9));
    page.FlushPending();

    EXPECT_FALSE(page.HasPending());
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 0)->text(), QStringLiteral("3"));
    EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("00:01:05")); // 65 s
    EXPECT_EQ(table->item(0, 2)->text(), QStringLiteral("0.900"));

    // 同一批数据再刷一次：幂等，不许重复补行
    page.FlushPending();
    EXPECT_EQ(table->rowCount(), 1);

    page.AppendResult(MakeRecord(7, 125.0, 0.4));
    page.FlushPending();
    ASSERT_EQ(table->rowCount(), 2); // 只补增量，不整表重建
    EXPECT_EQ(table->item(1, 0)->text(), QStringLiteral("7"));
    EXPECT_EQ(table->item(1, 1)->text(), QStringLiteral("00:02:05")); // 125 s
}

// 汇总行聚合：个数 / 平均强度 / 最大强度，全部 3 位小数。
TEST(SceneChangePageTests, SummaryAggregatesCountAverageAndMax) {
    EnsureApp();
    SceneChangePage page(/*feature_checked=*/false);

    page.AppendResult(MakeRecord(1, 1.0, 0.9));
    page.AppendResult(MakeRecord(2, 2.0, 0.4));
    page.AppendResult(MakeRecord(3, 3.0, 0.5));
    page.FlushPending();

    QLabel* summary = FindSummaryLabel(&page);
    ASSERT_TRUE(summary != nullptr);
    const QString text = summary->text();
    EXPECT_TRUE(text.contains(QStringLiteral("共检测到 3 个切换点")));
    EXPECT_TRUE(text.contains(QStringLiteral("0.600"))); // (0.9 + 0.4 + 0.5) / 3
    EXPECT_TRUE(text.contains(QStringLiteral("0.900")));
}

// Reset 必须同时归零同步游标：否则清空后新来的切换点永远补不进行。
TEST(SceneChangePageTests, ResetAllowsRefillingTable) {
    EnsureApp();
    SceneChangePage page(/*feature_checked=*/false);
    QTableWidget* table = page.findChild<QTableWidget*>();
    ASSERT_TRUE(table != nullptr);
    QLabel* summary = FindSummaryLabel(&page);
    ASSERT_TRUE(summary != nullptr);

    page.AppendResult(MakeRecord(3, 65.0, 0.9));
    page.FlushPending();
    ASSERT_EQ(table->rowCount(), 1);

    page.Reset();
    EXPECT_TRUE(page.records().empty());
    EXPECT_FALSE(page.HasPending());
    EXPECT_EQ(table->rowCount(), 0);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("镜头切换点"))); // 回到引导文案

    page.AppendResult(MakeRecord(9, 9.0, 0.3));
    page.FlushPending();
    ASSERT_EQ(table->rowCount(), 1); // 游标归零的守卫：不清零这里会是 0 行
    EXPECT_EQ(table->item(0, 0)->text(), QStringLiteral("9"));
}