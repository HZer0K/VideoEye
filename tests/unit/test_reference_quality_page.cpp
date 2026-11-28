// 「参考帧质量对比」页的页面级行为。
//
// 这一页的坑都是"不崩、不报，只是显示错"：
//   1) 换文件后旧结果还留在页面上 —— 用户会把它读成新文件的结论；
//   2) 无损帧（PSNR = +inf）显示成空白 —— 与"没算出来"混淆，一个真结论被抹掉；
//   3) 逐帧明细几千行时表格被撑爆 —— 必须有上限，同时把"总共多少帧"说清楚；
//   4) 失败时页面显示一排 0 / 空白 —— 必须显示错误原因，而不是看起来"全 0 dB"。
//
// 只构造控件、不渲染（offscreen 平台）。导出按钮会弹 QFileDialog 模态框，
// 导出本身在 test_reference_quality_exporter.cpp 里覆盖，这里不进导出路径。

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <cmath>
#include <limits>
#include <string>

#include "core/domain/model/ReferenceQualityResult.h"
#include "ui/analysis_panel/ReferenceQualityPage.h"
#include "ui/charts/MetricChartWidget.h"

using videoeye::model::ReferenceFrameMetric;
using videoeye::model::ReferenceQualityResult;
using videoeye::model::ReferenceQualitySummary;
using videoeye::ui::MetricChartWidget;
using videoeye::ui::ReferenceQualityPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_reference_quality_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QLabel* FindLabelContaining(QWidget* root, const QString& text) {
    for (QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text().contains(text))
            return label;
    }
    return nullptr;
}

QTableWidget* FindFrameTable(QWidget* page) {
    return page->findChild<QTableWidget*>();
}

ReferenceFrameMetric MakeFrame(int index, double seconds, double psnr, double ssim) {
    ReferenceFrameMetric frame;
    frame.index = index;
    frame.timestamp_seconds = seconds;
    frame.reference_timestamp_seconds = seconds;
    frame.distorted_timestamp_seconds = seconds;
    frame.reference_frame_index = index;
    frame.distorted_frame_index = index;
    frame.width = 320;
    frame.height = 180;
    frame.psnr_db = psnr;
    frame.ssim = ssim;
    frame.valid = true;
    return frame;
}

// 一次典型的成功对比：两帧正常 + 一帧无损。
ReferenceQualityResult MakeResult() {
    ReferenceQualityResult result;
    result.valid = true;
    ReferenceQualitySummary& summary = result.summary;
    summary.target_count = 3;
    summary.compared_frames = 3;
    summary.lossless_frames = 1;
    summary.coverage = 1.0;
    summary.mean_psnr_db = 36.5;
    summary.mean_ssim = 0.981;
    summary.min_psnr_db = 33.0;
    summary.min_psnr_frame_index = 0;
    summary.min_ssim = 0.96;
    summary.min_ssim_frame_index = 0;
    summary.reference_duration_seconds = 2.0;
    summary.distorted_duration_seconds = 2.0;

    result.frames.push_back(MakeFrame(0, 0.0, 33.0, 0.96));
    result.frames.push_back(MakeFrame(1, 0.5, 40.0, 0.99));
    result.frames.push_back(MakeFrame(2, 1.0, std::numeric_limits<double>::infinity(), 1.0));
    return result;
}

} // namespace

TEST(ReferenceQualityPageTest, StartsEmptyWithHintAndNoRows) {
    EnsureApp();
    ReferenceQualityPage page;

    EXPECT_FALSE(page.HasResult());
    EXPECT_FALSE(page.IsRunning());
    EXPECT_TRUE(page.ReferencePath().isEmpty());
    ASSERT_NE(FindFrameTable(&page), nullptr);
    EXPECT_EQ(FindFrameTable(&page)->rowCount(), 0);
    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("选择参考视频与待测视频")), nullptr);
    ASSERT_NE(page.findChild<QProgressBar*>(), nullptr);
    EXPECT_EQ(page.findChild<QProgressBar*>()->value(), 0);
}

TEST(ReferenceQualityPageTest, CurrentFileBecomesDistortedAndClearsStaleResult) {
    EnsureApp();
    ReferenceQualityPage page;
    page.SetResult(MakeResult());
    ASSERT_TRUE(page.HasResult());

    page.SetSourcePath(QStringLiteral("D:/media/after.mp4"));
    // 当前打开的文件默认当「待测视频」；旧结果必须同时作废，否则会挂在新文件上。
    EXPECT_EQ(page.DistortedPath(), QStringLiteral("D:/media/after.mp4"));
    EXPECT_FALSE(page.HasResult());
    EXPECT_EQ(FindFrameTable(&page)->rowCount(), 0);
    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("选择参考视频与待测视频")), nullptr);
}

TEST(ReferenceQualityPageTest, ResultFillsSummaryAndFrameTable) {
    EnsureApp();
    ReferenceQualityPage page;
    page.SetResult(MakeResult());

    EXPECT_TRUE(page.HasResult());
    QTableWidget* table = FindFrameTable(&page);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 3);
    // 尺寸列与状态列是"看一眼就知道对不对齐"的两列
    EXPECT_EQ(table->item(0, 3)->text(), QStringLiteral("320×180"));
    EXPECT_EQ(table->item(0, 6)->text(), QStringLiteral("已比较"));

    QLabel* summary = FindLabelContaining(&page, QStringLiteral("已比较 3 / 3 帧"));
    ASSERT_NE(summary, nullptr) << "汇总行应报出已比较 / 目标帧数与覆盖率";
    EXPECT_TRUE(summary->text().contains(QStringLiteral("平均 PSNR 36.50 dB")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("平均 SSIM 0.9810")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("最低 PSNR 33.00 dB（第 0 帧）")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("无损 1")));
    EXPECT_NE(page.findChild<MetricChartWidget*>(), nullptr);
}

TEST(ReferenceQualityPageTest, LosslessFrameShowsInfinityRatherThanBlank) {
    EnsureApp();
    ReferenceQualityPage page;
    page.SetResult(MakeResult());

    QTableWidget* table = FindFrameTable(&page);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 3);
    // PSNR 是 +inf 表示两帧逐像素一致 —— 这是"完全一样"的真结论，不能显示成空白或 0
    EXPECT_EQ(table->item(2, 4)->text(), QStringLiteral("∞（无损）"));
}

TEST(ReferenceQualityPageTest, InvalidFrameShowsItsErrorInsteadOfADash) {
    EnsureApp();
    ReferenceQualityPage page;
    ReferenceQualityResult result;
    result.valid = true;
    result.summary.target_count = 1;
    result.summary.failed_frames = 1;
    ReferenceFrameMetric failed = MakeFrame(0, 0.0, videoeye::model::kQualityNoValue, videoeye::model::kQualityNoValue);
    failed.valid = false;
    failed.error_message = "尺寸不一致: 320x180 vs 320x240";
    result.frames.push_back(failed);
    page.SetResult(result);

    QTableWidget* table = FindFrameTable(&page);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 4)->text(), QStringLiteral("—"));
    EXPECT_EQ(table->item(0, 6)->text(), QStringLiteral("尺寸不一致: 320x180 vs 320x240"));
}

TEST(ReferenceQualityPageTest, FailedRunShowsErrorReasonNotZeroMetrics) {
    EnsureApp();
    ReferenceQualityPage page;
    ReferenceQualityResult result;
    result.valid = false;
    result.error_message = "参考视频没有可解码的视频帧";
    page.SetResult(result);

    EXPECT_TRUE(page.HasResult());
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("参考视频没有可解码的视频帧"));
    ASSERT_NE(summary, nullptr) << "失败时必须显示原因，而不是一排空白";
    EXPECT_EQ(FindFrameTable(&page)->rowCount(), 0);
}

TEST(ReferenceQualityPageTest, TableIsCappedButHintReportsTheTotal) {
    EnsureApp();
    ReferenceQualityPage page;
    ReferenceQualityResult result;
    result.valid = true;
    result.summary.target_count = 2005;
    result.summary.compared_frames = 2005;
    result.summary.coverage = 1.0;
    for (int i = 0; i < 2005; ++i)
        result.frames.push_back(MakeFrame(i, i * 0.04, 30.0, 0.95));
    page.SetResult(result);

    QTableWidget* table = FindFrameTable(&page);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowCount(), 2000) << "界面表格必须有上限，否则几千行 QTableWidgetItem 会拖住 UI";
    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("共 2005 帧")), nullptr)
        << "被截断时必须报出总帧数，否则用户会以为总共就这么多";
}

TEST(ReferenceQualityPageTest, ClearResultRestoresTheEmptyState) {
    EnsureApp();
    ReferenceQualityPage page;
    page.SetResult(MakeResult());
    ASSERT_TRUE(page.HasResult());

    page.ClearResult();
    EXPECT_FALSE(page.HasResult());
    EXPECT_EQ(FindFrameTable(&page)->rowCount(), 0);
    EXPECT_EQ(page.findChild<QProgressBar*>()->value(), 0);
    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("选择参考视频与待测视频")), nullptr);
}
