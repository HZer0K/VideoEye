// ContainerStructurePage 的页面级行为测试（评审「页面组件补测」优先级最高的一项）。
//
// 为什么值得单独测：这个页是「打开文件即自动填充」的页面里逻辑最密的一页 —— 同时接
// 通用结构树、MP4 box 表、MP4 样本表、EBML 详情表四种数据源，还要按 format 在
// QStackedWidget 上切到正确的详情页。这几条路由只要写错一个分支，界面就停在旧页 /
// 显示空表，且不会崩，属于最难在手动点测里发现的那类回归。
//
// 这里只构造控件、不渲染（offscreen 平台），通过公开 API（SetResult / SetError /
// ApplySampleTable / result / feature_checked）加 findChild 按类型取控件来断言：
//   - 不同 format 要切到正确的详情页（MP4→1 / MKV→2 / 无效→0）；
//   - 结构树 / 样本轨下拉按数据量填充；
//   - 「启用分析」开关初始态跟随构造参数，且用户点击会发 FeatureToggled。
// 不触发导出按钮（那会弹 QFileDialog 模态框）。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QString>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#include <string>

#include "core/domain/model/ContainerStructureInfo.h"
#include "ui/analysis_panel/ContainerStructurePage.h"

using videoeye::model::ContainerElement;
using videoeye::model::ContainerFormat;
using videoeye::model::ContainerStructureResult;
using videoeye::model::EbmlAnalysisResult;
using videoeye::model::EbmlTrackInfo;
using videoeye::model::Mp4BoxAnalysisResult;
using videoeye::model::Mp4Sample;
using videoeye::model::Mp4SampleTableResult;
using videoeye::model::Mp4TrackSampleTable;
using videoeye::model::TrackBoxTables;
using videoeye::ui::ContainerStructurePage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_container_structure_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 最小可填充的 MP4 结果：通用树 1 个 moov(含 trak) + 1 路流 + 1 路 MP4 box 表
// （stts/stco/stsc/stsz/stss 各 1 条）+ 1 路样本表（2 个样本）。足以驱动
// PopulateMp4BoxTablesInContainer / RebuildMp4SampleTable 两条填充链。
ContainerStructureResult MakeMp4Result() {
    ContainerStructureResult r;
    r.format = ContainerFormat::MP4;
    r.format_name = "MP4";
    r.valid = true;
    r.summary = "summary-ok";

    ContainerElement moov;
    moov.name = "moov"; moov.type = "Box"; moov.size = 100; moov.offset = 0; moov.depth = 0;
    ContainerElement trak;
    trak.name = "trak"; trak.type = "Box"; trak.size = 50; trak.offset = 8; trak.depth = 1;
    trak.value = "video";
    moov.children.push_back(trak);
    r.element_tree.push_back(moov);

    r.streams.push_back({0, "video", "avc1", "1920x1080"});
    r.metadata["title"] = "demo";

    r.mp4_detail.valid = true;
    TrackBoxTables tb;
    tb.track_id = 1; tb.track_type = "video";
    tb.stts_entries.push_back({2, 1024});
    tb.stco_entries.push_back({0x1000});
    tb.stsc_entries.push_back({1, 1, 1});
    tb.stsz_entries.push_back({100});
    tb.stss_entries.push_back({0});
    r.mp4_detail.track_tables.push_back(tb);

    r.mp4_samples.valid = true;
    r.mp4_samples.file_size = 10000;
    r.mp4_samples.top_level_order = {"ftyp", "moov", "mdat"};
    r.mp4_samples.moov_offset = 16;
    r.mp4_samples.moov_size = 100;
    r.mp4_samples.moov_before_mdat = true;
    Mp4TrackSampleTable track;
    track.track_id = 1; track.type = "video"; track.codec = "avc1";
    track.media_timescale = 1024; track.stsz_sample_count = 2;
    Mp4Sample a;
    a.index = 0; a.chunk_index = 0; a.offset = 0x1000; a.size = 100;
    a.dts = 0; a.cts = 0; a.duration = 1024; a.keyframe = true;
    track.samples.push_back(a);
    Mp4Sample b;
    b.index = 1; b.chunk_index = 0; b.offset = 0x1100; b.size = 100;
    b.dts = 1024; b.cts = 1024; b.duration = 1024; b.keyframe = false;
    track.samples.push_back(b);
    r.mp4_samples.tracks.push_back(track);
    return r;
}

// 最小可填充的 MKV 结果：只填 EBML 详情表（1 路视频轨），不填通用树/流。
ContainerStructureResult MakeMkvResult() {
    ContainerStructureResult r;
    r.format = ContainerFormat::MKV;
    r.format_name = "Matroska";
    r.valid = true;
    r.summary = "ok";

    r.ebml_detail.valid = true;
    EbmlTrackInfo t;
    t.track_number = 1; t.track_type = 1; t.track_type_name = "视频";
    t.codec_id = "V_VP9"; t.codec_name = "VP9";
    t.pixel_width = 1920; t.pixel_height = 1080; t.language = "eng";
    t.default_track = true;
    r.ebml_detail.tracks.push_back(t);
    return r;
}

// 无效结果：解析失败，页面应当停在通用信息页、清空结构树。
ContainerStructureResult MakeInvalidResult() {
    ContainerStructureResult r;
    r.format = ContainerFormat::MP4;
    r.format_name = "MP4";
    r.valid = false;
    r.error_message = "unsupported container";
    return r;
}

// 单独一份样本表，给 ApplySampleTable 用（不依赖完整结构结果）。
Mp4SampleTableResult MakeSampleTable() {
    Mp4SampleTableResult samples;
    samples.valid = true;
    samples.file_size = 5000;
    Mp4TrackSampleTable tr;
    tr.track_id = 5; tr.type = "audio"; tr.codec = "mp4a";
    tr.media_timescale = 44100; tr.stsz_sample_count = 1;
    Mp4Sample s;
    s.index = 0; s.chunk_index = 0; s.offset = 0x200; s.size = 200;
    s.dts = 0; s.cts = 0; s.duration = 1024; s.keyframe = true;
    tr.samples.push_back(s);
    samples.tracks.push_back(tr);
    return samples;
}

bool AnyTableHasRows(ContainerStructurePage* page) {
    const auto tables = page->findChildren<QTableWidget*>();
    for (const auto* t : tables) {
        if (t->rowCount() > 0) return true;
    }
    return false;
}

}  // namespace

// MP4 结果要切到详情页 1，并填充结构树 / 样本轨下拉 / 至少一张表。
TEST(ContainerStructurePageTests, Mp4ResultShowsMp4DetailPage) {
    EnsureApp();
    ContainerStructurePage page(/*feature_checked=*/true);
    page.SetResult(MakeMp4Result());

    QStackedWidget* stack = page.findChild<QStackedWidget*>();
    ASSERT_TRUE(stack != nullptr);
    EXPECT_EQ(stack->currentIndex(), 1);  // MP4 详情页

    QTreeWidget* tree = page.findChild<QTreeWidget*>();
    ASSERT_TRUE(tree != nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 1);  // moov

    QComboBox* combo = page.findChild<QComboBox*>();
    ASSERT_TRUE(combo != nullptr);
    EXPECT_EQ(combo->count(), 1);  // 1 路样本轨

    EXPECT_TRUE(AnyTableHasRows(&page));
    EXPECT_TRUE(page.result().valid);
}

// MKV 结果要切到详情页 2，EBML 轨道表有内容；通用树没填，应为空。
TEST(ContainerStructurePageTests, MkvResultShowsEbmlDetailPage) {
    EnsureApp();
    ContainerStructurePage page(/*feature_checked=*/true);
    page.SetResult(MakeMkvResult());

    QStackedWidget* stack = page.findChild<QStackedWidget*>();
    ASSERT_TRUE(stack != nullptr);
    EXPECT_EQ(stack->currentIndex(), 2);  // EBML 详情页

    QTreeWidget* tree = page.findChild<QTreeWidget*>();
    ASSERT_TRUE(tree != nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0);  // 没填通用树

    EXPECT_TRUE(AnyTableHasRows(&page));  // EBML 轨道表
    EXPECT_TRUE(page.result().valid);
}

// 无效结果：停在通用信息页(0)，结构树清空。
TEST(ContainerStructurePageTests, InvalidResultStaysOnGenericPage) {
    EnsureApp();
    ContainerStructurePage page(/*feature_checked=*/true);
    page.SetResult(MakeInvalidResult());

    QStackedWidget* stack = page.findChild<QStackedWidget*>();
    ASSERT_TRUE(stack != nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);

    QTreeWidget* tree = page.findChild<QTreeWidget*>();
    ASSERT_TRUE(tree != nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    EXPECT_FALSE(page.result().valid);
}

// SetError 与「无效结果」同一套空页面表现：停在通用页、清结构树。
TEST(ContainerStructurePageTests, SetErrorClearsPage) {
    EnsureApp();
    ContainerStructurePage page(/*feature_checked=*/true);
    page.SetResult(MakeMp4Result());
    page.SetError(QString::fromUtf8("parse failed"));

    QStackedWidget* stack = page.findChild<QStackedWidget*>();
    ASSERT_TRUE(stack != nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);

    QTreeWidget* tree = page.findChild<QTreeWidget*>();
    ASSERT_TRUE(tree != nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    EXPECT_FALSE(page.result().valid);
}

// ApplySampleTable 只刷新样本相关表，不改变详情页路由；下拉按轨道数填充。
TEST(ContainerStructurePageTests, ApplySampleTableRefreshesSampleTables) {
    EnsureApp();
    ContainerStructurePage page(/*feature_checked=*/true);
    // 先放一个无效结果（停在通用页），再单独喂样本表
    page.SetResult(MakeInvalidResult());
    page.ApplySampleTable(MakeSampleTable());

    QComboBox* combo = page.findChild<QComboBox*>();
    ASSERT_TRUE(combo != nullptr);
    EXPECT_EQ(combo->count(), 1);  // 1 路样本轨
}

// 「启用分析」开关初始态跟随构造参数，点击发出 FeatureToggled(true)。
TEST(ContainerStructurePageTests, FeatureToggleFollowsCtorAndEmits) {
    EnsureApp();
    ContainerStructurePage page(/*feature_checked=*/false);
    EXPECT_FALSE(page.feature_checked());

    QCheckBox* toggle = page.findChild<QCheckBox*>();
    ASSERT_TRUE(toggle != nullptr);
    EXPECT_FALSE(toggle->isChecked());

    bool emitted = false;
    QObject::connect(&page, &ContainerStructurePage::FeatureToggled,
                     [&emitted](bool on) { emitted = on; });

    toggle->setChecked(true);
    EXPECT_TRUE(emitted);
    EXPECT_TRUE(page.feature_checked());
}
