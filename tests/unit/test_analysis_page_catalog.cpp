// 分析页目录（ui/analysis_panel/AnalysisPageCatalog.h）的顺序契约测试。
//
// 根因: 页面在外部 QStackedWidget 里的**下标**是硬契约 —— ctest 里有按 stack 下标
// 取控件的回归（tabs->widget(0/1/3)：流概览、包、音频帧三张表各按子页下标定位）。
// 以前这个顺序没有任何一处显式声明，它只是 AnalysisPanel::SetupUI 里十几个
// SetupXxx() 的**调用次序**的副产品：调序一下标全错位，但改的人看不见这条依赖，
// 测试先红在一个完全不相干的用例上，排查成本极高。
//
// 现在顺序是一张数据表（AnalysisPageCatalog），建页方按它循环。这里钉住四类
// "错了就是静默错位"的不变量：
//   1. 页序列本身（顺序 == stack 页序号）；
//   2. id 唯一（侧栏徽标按 id 归集，重名会让两页共用一个徽标数）；
//   3. 每个条目的标题 / 分组非空（空标题在侧栏上是一条点不动的空白行）；
//   4. (分组, 组内序号) 唯一（侧栏按它排序，撞键会让两页的相对次序变成
//      QMap 的插入次序，即"看起来对但不可靠"）。
//
// 目录是 header-only 且不碰任何页面组件，所以这里不需要 offscreen，也不必为了测
// 顺序去链 14 个页面的 .cpp 与它们的依赖图 —— 只链 Qt6::Core。

#include <gtest/gtest.h>

#include <QString>

#include <algorithm>
#include <set>
#include <vector>

#include "ui/analysis_panel/AnalysisPageCatalog.h"

namespace {

using videoeye::ui::AnalysisPageCatalog;
using videoeye::ui::AnalysisPageDescriptor;

std::vector<QString> Ids() {
    std::vector<QString> out;
    for (const AnalysisPageDescriptor& entry : AnalysisPageCatalog()) {
        out.push_back(QString::fromUtf8(entry.id));
    }
    return out;
}

} // namespace

TEST(AnalysisPageCatalogTest, PageOrderMatchesStackContract) {
    // 顺序 == 外部 QStackedWidget 的页序号。改这张表之前先想清楚：
    // tests/unit/test_stream_views.cpp 等用例按 stack 下标取控件。
    const std::vector<QString> expected = {
        QStringLiteral("playback_stats_packets"),
        QStringLiteral("event_timeline"),
        QStringLiteral("file_structure"),
        QStringLiteral("motion_vectors"),
        QStringLiteral("scene_change"),
        QStringLiteral("visual_quality"),
        QStringLiteral("bitrate_gop"),
        QStringLiteral("audio_qc"),
        QStringLiteral("color_hdr"),
        QStringLiteral("codec_parameters"),
        QStringLiteral("streaming_packets"),
        QStringLiteral("subtitle_aux"),
        QStringLiteral("quality_diagnostics"),
        QStringLiteral("report_batch_qc"),
    };
    EXPECT_EQ(expected, Ids());
}

TEST(AnalysisPageCatalogTest, IdsAreUnique) {
    const std::vector<QString> ids = Ids();
    const std::set<QString> unique(ids.begin(), ids.end());
    EXPECT_EQ(unique.size(), ids.size()) << "pageId 重名会让两页共用同一个侧栏徽标";
}

TEST(AnalysisPageCatalogTest, EveryEntryHasTitleAndGroup) {
    for (const AnalysisPageDescriptor& entry : AnalysisPageCatalog()) {
        const QString id = QString::fromUtf8(entry.id);
        EXPECT_TRUE(QString::fromUtf8(entry.title).trimmed().isEmpty() == false)
            << "缺标题的页面在侧栏上是一条点不动的空白行: " << id.toStdString();
        EXPECT_TRUE(QString::fromUtf8(entry.group).trimmed().isEmpty() == false)
            << "缺分组的页面在侧栏上无处安放: " << id.toStdString();
    }
}

TEST(AnalysisPageCatalogTest, GroupOrderKeysAreUnique) {
    // 侧栏按 (分组, 组内序号) 排序。同一分组里两个页面撞序号时，相对次序会退化成
    // 表格的插入次序 —— 表面上还排得对，但改一个不相关的页就可能翻掉。
    std::set<QString> seen;
    for (const AnalysisPageDescriptor& entry : AnalysisPageCatalog()) {
        const QString key = QString::fromUtf8(entry.group) + QStringLiteral("/") + QString::number(entry.order);
        EXPECT_TRUE(seen.insert(key).second) << "同一分组里组内序号撞车: " << key.toStdString();
    }
}

TEST(AnalysisPageCatalogTest, QualityDiagnosticsLeadsItsGroup) {
    // 「全片质量」组的结论页必须排第一：这一组里码率 / 音频 / 色彩都是分项，
    // 用户进来先看"这次扫描的结论"，再按需下钻。组内次序由 (group, order) 决定，
    // 与 stack 顺序无关 —— 所以这里断言 order 而不是表格行序。
    int diagnostics_order = -1;
    int smallest_in_group = 1000;
    for (const AnalysisPageDescriptor& entry : AnalysisPageCatalog()) {
        if (QString::fromUtf8(entry.group) != QStringLiteral("full_quality"))
            continue;
        smallest_in_group = std::min(smallest_in_group, entry.order);
        if (QString::fromUtf8(entry.id) == QStringLiteral("quality_diagnostics"))
            diagnostics_order = entry.order;
    }
    EXPECT_GE(diagnostics_order, 0) << "quality_diagnostics 不在 full_quality 组";
    EXPECT_EQ(diagnostics_order, smallest_in_group) << "质量诊断不是「全片质量」组的首项";
}

TEST(AnalysisPageCatalogTest, BadgePagesExistInCatalog) {
    // CollectPageBadges 按 pageId 归集徽标（见 AnalysisPageFactory.cpp）。
    // 那边写死的 id 必须在目录里 —— 否则徽标永远画不出来，且不报错。
    const std::vector<QString> ids = Ids();
    const std::vector<QString> badge_pages = {
        QStringLiteral("quality_diagnostics"), QStringLiteral("bitrate_gop"),       QStringLiteral("audio_qc"),
        QStringLiteral("color_hdr"),           QStringLiteral("streaming_packets"), QStringLiteral("codec_parameters"),
        QStringLiteral("subtitle_aux"),        QStringLiteral("visual_quality"),
    };
    for (const QString& id : badge_pages) {
        EXPECT_NE(std::find(ids.begin(), ids.end(), id), ids.end())
            << "徽标口径里的 pageId 不在目录中: " << id.toStdString();
    }
}
