// StreamingPanel 的页面级行为测试（评审「页面组件补测」，P2 拆分的回归防线）。
//
// 这一页把 HLS / DASH 的清单结构、码率阶梯、分片时间轴与问题表拼在一起，
// 四条链都是"不崩只静默出错"：
//   1) 结构树层级：DASH 的 Period - AdaptationSet - Representation - Segment
//      与 HLS 的 variant / rendition / playlist / DATERANGE 少了任何一层，
//      用户只看得到根节点；
//   2) 码率阶梯格式化：分辨率缺失写 "-"、kbps 取整、编码回落到音频码；
//   3) 时间轴对齐：参考层取关键帧最多的那条，偏差超过 0.05 s 必须高亮；
//   4) 问题表：按严重度倒序排列并整行着色，IssueCount 的口径就是行数。
//
// 另加 Clear / 解析失败 / 扫描态三处状态复位。只构造控件、不渲染（offscreen）；
// 「导出 JSON」会弹 QFileDialog，不点它，复制 JSON 也不走剪贴板 —— 只测
// ExportToJson() 的序列化结果。

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <string>
#include <vector>

#include "core/domain/model/StreamPackageInfo.h"
#include "ui/streaming_panel/StreamingPanel.h"

using videoeye::model::SegmentInfo;
using videoeye::model::StreamingIssue;
using videoeye::model::StreamingKind;
using videoeye::model::StreamingLadderEntry;
using videoeye::model::StreamingPackageResult;
using videoeye::ui::StreamingPanel;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_streaming_panel";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QLabel* FindLabelContaining(QWidget* root, const QString& needle) {
    const QList<QLabel*> labels = root->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text().contains(needle))
            return label;
    }
    return nullptr;
}

QPushButton* FindButton(QWidget* root, const QString& text) {
    const QList<QPushButton*> buttons = root->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

// 三张表的表头首列互不相同，用它定位不受子视图嵌套层数影响
QTableWidget* TableByHeader(QWidget* root, const QString& header) {
    const QList<QTableWidget*> tables = root->findChildren<QTableWidget*>();
    for (QTableWidget* table : tables) {
        if (table->horizontalHeaderItem(0) != nullptr && table->horizontalHeaderItem(0)->text() == header)
            return table;
    }
    return nullptr;
}

QString Cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

QTreeWidgetItem* FindChildItem(QTreeWidgetItem* parent, const QString& name) {
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem* child = parent->child(i);
        if (child->text(0) == name)
            return child;
        if (QTreeWidgetItem* found = FindChildItem(child, name))
            return found;
    }
    return nullptr;
}

QTreeWidgetItem* FindItem(QTreeWidget* tree, const QString& name) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (item->text(0) == name)
            return item;
        if (QTreeWidgetItem* found = FindChildItem(item, name))
            return found;
    }
    return nullptr;
}

SegmentInfo MakeSegment(uint64_t sequence, double start, double duration, bool probed) {
    SegmentInfo segment;
    segment.sequence = sequence;
    segment.start_seconds = start;
    segment.duration_seconds = duration;
    segment.has_duration = true;
    segment.exists = true;
    segment.probed = probed;
    return segment;
}

// 一份典型 DASH 结果：1 个 Period / 1 个 AdaptationSet / 1 条 1080p 轨，
// ladder 里另有 1 条 720p（时间轴第 2 个关键帧偏 0.2 s，必须高亮）。
StreamingPackageResult MakeDashResult() {
    StreamingPackageResult result;
    result.manifest_path = "C:/media/manifest.mpd";
    result.kind = StreamingKind::Dash;
    result.valid = true;
    result.mpd_type = "static";
    result.media_presentation_duration_s = 60.0;
    result.has_availability_start_time = true;
    result.availability_start_time = "2026-10-25T00:00:00Z";

    videoeye::model::DashRepresentationInfo rep;
    rep.index = 0;
    rep.period_index = 0;
    rep.adaptation_index = 0;
    rep.id = "r0";
    rep.bandwidth_bps = 5000000;
    rep.width = 1920;
    rep.height = 1080;
    rep.video_codec = "avc1";
    rep.content_type = "video";
    rep.mime_type = "video/mp4";
    rep.segments = {MakeSegment(0, 0.0, 6.0, false), MakeSegment(1, 6.0, 6.0, false)};
    result.representations = {rep};

    videoeye::model::DashDescriptor role;
    role.scheme_id_uri = "urn:mpeg:dash:role:2011";
    role.value = "main";

    videoeye::model::DashAdaptationSetInfo adaptation;
    adaptation.index = 0;
    adaptation.content_type = "video";
    adaptation.mime_type = "video/mp4";
    adaptation.representation_indices = {0};
    adaptation.roles = {role};

    videoeye::model::DashPeriodInfo period;
    period.index = 0;
    period.duration_seconds = 60.0;
    period.has_duration = true;
    period.adaptation_sets = {adaptation};
    result.periods = {period};

    StreamingLadderEntry high;
    high.label = "Representation r0";
    high.bandwidth_bps = 5000000;
    high.width = 1920;
    high.height = 1080;
    high.video_codec = "avc1";
    high.container_hint = "fMP4/CMAF";
    high.segment_count = 2;
    high.avg_segment_duration_s = 6.0;
    high.max_segment_duration_s = 6.0;
    high.has_init_section = true;
    high.keyframe_times = {0.0, 6.0, 12.0};

    StreamingLadderEntry low;
    low.label = "Representation r1";
    low.bandwidth_bps = 2500000;
    low.width = 1280;
    low.height = 720;
    low.video_codec = "avc1";
    low.container_hint = "fMP4/CMAF";
    low.segment_count = 2;
    low.avg_segment_duration_s = 6.0;
    low.max_segment_duration_s = 6.0;
    low.has_init_section = false;
    low.keyframe_times = {0.0, 6.2};
    result.ladder = {high, low};

    StreamingIssue info;
    info.code = "container.streaming.segment_independence_undeclared";
    info.title = "未声明独立分片";
    info.severity = videoeye::model::IssueSeverity::Info;
    info.occurrence_count = 1;
    StreamingIssue critical;
    critical.code = "container.streaming.segment_missing_file";
    critical.title = "分片缺失";
    critical.severity = videoeye::model::IssueSeverity::Critical;
    critical.occurrence_count = 3;
    StreamingIssue warning;
    warning.code = "container.dash.live_missing_availability_start_time";
    warning.title = "缺 availabilityStartTime";
    warning.severity = videoeye::model::IssueSeverity::Warning;
    warning.occurrence_count = 2;
    result.issues = {info, critical, warning};
    return result;
}

// 一份直播型 HLS 主播放列表：含 I-FRAME 档 / 备选音轨 / 广告标记 / 加密 /
// LL-HLS / SKIP / 墙钟解析失败，摘要与结构树各有一条对应文案。
StreamingPackageResult MakeHlsResult() {
    StreamingPackageResult result;
    result.manifest_path = "C:/media/master.m3u8";
    result.kind = StreamingKind::HlsMaster;
    result.valid = true;
    result.remote = true;

    videoeye::model::HlsVariantInfo variant;
    variant.index = 0;
    variant.uri = "v0.m3u8";
    variant.bandwidth_bps = 800000;
    variant.width = 640;
    variant.height = 360;
    variant.video_codec = "avc1";
    videoeye::model::HlsVariantInfo iframe;
    iframe.index = 1;
    iframe.uri = "iframe.m3u8";
    iframe.iframe_only = true;
    iframe.bandwidth_bps = 400000;
    result.variants = {variant, iframe};

    videoeye::model::HlsRenditionInfo rendition;
    rendition.type = "AUDIO";
    rendition.group_id = "aud";
    rendition.name = "en";
    result.renditions = {rendition};

    videoeye::model::MediaPlaylistInfo playlist;
    playlist.index = 0;
    playlist.role = "video";
    playlist.target_duration_s = 6;
    playlist.has_target_duration = true;
    playlist.has_endlist = false;
    playlist.playlist_type = "event";
    playlist.has_media_sequence = false;
    playlist.encrypted = true;
    playlist.key_method = "AES-128";
    playlist.has_init_section = true;
    playlist.init_uri = "init.mp4";
    playlist.low_latency = true;
    playlist.partial_count = 3;
    playlist.has_program_date_time = true;
    playlist.program_date_time = "2026-10-25T00:00:00Z";
    playlist.program_date_time_valid = false;
    playlist.has_start = true;
    playlist.start_time_offset_s = 1.5;
    playlist.start_precise = true;
    playlist.has_skip = true;
    playlist.skipped_segments = 2;
    playlist.total_duration_seconds = 12.0;
    playlist.segments = {MakeSegment(0, 0.0, 6.0, true), MakeSegment(1, 6.0, 6.0, false)};

    videoeye::model::HlsDateRangeInfo date_range;
    date_range.id = "ad1";
    date_range.at_seconds = 2.0;
    date_range.has_duration = true;
    date_range.duration_seconds = 4.0;
    date_range.has_scte35_out = true;
    playlist.date_ranges = {date_range};
    result.playlists = {playlist};

    StreamingLadderEntry entry;
    entry.label = "variant #0";
    entry.bandwidth_bps = 800000;
    entry.width = 640;
    entry.height = 360;
    entry.video_codec = "avc1";
    entry.container_hint = "MPEG-TS";
    entry.segment_count = 2;
    entry.avg_segment_duration_s = 6.0;
    entry.max_segment_duration_s = 6.0;
    entry.keyframe_times = {0.0, 6.0};
    result.ladder = {entry};
    return result;
}

} // namespace

// 未分析：引导文案 + 空树 + 三张空表 + 徽标 0
TEST(StreamingPanelTests, UnanalyzedShowsInitialStateAndEmptyViews) {
    EnsureApp();
    StreamingPanel panel;

    EXPECT_NE(FindLabelContaining(&panel, QStringLiteral("尚未分析流媒体包")), nullptr);
    EXPECT_FALSE(panel.HasResult());
    EXPECT_EQ(panel.IssueCount(), 0);

    QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    EXPECT_EQ(tree->headerItem()->text(0), QStringLiteral("结构"));

    const auto tables = panel.findChildren<QTableWidget*>();
    ASSERT_EQ(tables.size(), 3);
    for (QTableWidget* table : tables) {
        EXPECT_EQ(table->rowCount(), 0);
        EXPECT_EQ(table->columnCount(), 0);
    }

    auto* tabs = panel.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 2);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("分片时间轴对齐"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("问题"));
}

// DASH：结构树层级 + 阶梯表格式化 + 时间轴对齐高亮 + 问题表严重度排序
TEST(StreamingPanelTests, DashResultFillsStructureLadderTimelineAndIssues) {
    EnsureApp();
    StreamingPanel panel;
    panel.SetResult(MakeDashResult());

    EXPECT_TRUE(panel.HasResult());
    EXPECT_EQ(panel.IssueCount(), 3);

    // ---- 摘要 ----
    QLabel* summary = FindLabelContaining(&panel, QStringLiteral("DASH MPD"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("2 个码率层")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("2 个分片")));
    // 分片探测是抽样：0 个探过的必须如实写出来
    EXPECT_TRUE(summary->text().contains(QStringLiteral("分片抽查 0/2，其余未探测")));

    // ---- 结构树：根 -> Period -> AdaptationSet -> Representation -> Segment ----
    QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    QTreeWidgetItem* root = tree->topLevelItem(0);
    EXPECT_EQ(root->text(0), QString::fromUtf8(videoeye::model::ToString(StreamingKind::Dash)));
    EXPECT_EQ(root->text(1), QStringLiteral("C:/media/manifest.mpd"));

    QTreeWidgetItem* mpd_type = FindChildItem(root, QStringLiteral("MPD 类型"));
    ASSERT_NE(mpd_type, nullptr);
    EXPECT_EQ(mpd_type->text(1), QStringLiteral("static"));
    QTreeWidgetItem* duration = FindChildItem(root, QStringLiteral("总时长"));
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(duration->text(1), QStringLiteral("60.000 s"));

    QTreeWidgetItem* period = FindChildItem(root, QStringLiteral("Period 0"));
    ASSERT_NE(period, nullptr);
    EXPECT_EQ(period->text(1), QStringLiteral("duration=60.000 s"));
    QTreeWidgetItem* adaptation = FindChildItem(period, QStringLiteral("AdaptationSet 0"));
    ASSERT_NE(adaptation, nullptr);
    QTreeWidgetItem* role = FindChildItem(adaptation, QStringLiteral("Role"));
    ASSERT_NE(role, nullptr);
    EXPECT_EQ(role->text(1), QStringLiteral("main"));
    QTreeWidgetItem* representation = FindChildItem(adaptation, QStringLiteral("Representation r0"));
    ASSERT_NE(representation, nullptr);
    EXPECT_EQ(representation->text(1), QStringLiteral("1920x1080 5000 kbps"));
    QTreeWidgetItem* segment = FindChildItem(representation, QStringLiteral("Segment 1"));
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(segment->text(1), QStringLiteral("t=6.000 d=6.000 ok"));

    // ---- 码率阶梯 ----
    QTableWidget* ladder = TableByHeader(&panel, QStringLiteral("码率层"));
    ASSERT_NE(ladder, nullptr);
    ASSERT_EQ(ladder->rowCount(), 2);
    EXPECT_EQ(ladder->columnCount(), 10);
    EXPECT_EQ(Cell(ladder, 0, 0), QStringLiteral("Representation r0"));
    EXPECT_EQ(Cell(ladder, 0, 1), QStringLiteral("1920x1080"));
    EXPECT_EQ(Cell(ladder, 0, 2), QStringLiteral("5000"));
    EXPECT_EQ(Cell(ladder, 0, 3), QStringLiteral("avc1"));
    EXPECT_EQ(Cell(ladder, 0, 4), QStringLiteral("fMP4/CMAF"));
    EXPECT_EQ(Cell(ladder, 0, 5), QStringLiteral("2"));
    EXPECT_EQ(Cell(ladder, 0, 6), QStringLiteral("6.000"));
    EXPECT_EQ(Cell(ladder, 0, 7), QStringLiteral("6.000"));
    EXPECT_EQ(Cell(ladder, 0, 8), QStringLiteral("有"));
    EXPECT_EQ(Cell(ladder, 0, 9), QStringLiteral("3"));
    EXPECT_EQ(Cell(ladder, 1, 1), QStringLiteral("1280x720"));
    EXPECT_EQ(Cell(ladder, 1, 8), QStringLiteral("无"));

    // ---- 时间轴：参考层取关键帧最多的 1080p（粗体），720p 偏 0.2 s 高亮 ----
    QTableWidget* timeline = TableByHeader(&panel, QStringLiteral("分片 #"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->columnCount(), 3); // 序号列 + 2 条码率层
    EXPECT_EQ(timeline->rowCount(), 3);
    ASSERT_NE(timeline->item(0, 1), nullptr);
    EXPECT_TRUE(timeline->item(0, 1)->font().bold());
    EXPECT_EQ(Cell(timeline, 1, 1), QStringLiteral("6.000"));
    EXPECT_EQ(Cell(timeline, 1, 2), QStringLiteral("6.200"));
    ASSERT_NE(timeline->item(1, 2), nullptr);
    EXPECT_EQ(timeline->item(1, 2)->background().color(), QColor("#5A2B2B"));
    EXPECT_EQ(timeline->item(1, 2)->foreground().color(), QColor("#F85149"));
    EXPECT_TRUE(timeline->item(1, 2)->toolTip().contains(QStringLiteral("起点偏差 0.200")));
    EXPECT_EQ(Cell(timeline, 2, 2), QStringLiteral("-")); // 720p 少一个关键帧

    // ---- 问题表：致命 -> 警告 -> 提示，整行按级别着色 ----
    QTableWidget* issues = TableByHeader(&panel, QStringLiteral("级别"));
    ASSERT_NE(issues, nullptr);
    ASSERT_EQ(issues->rowCount(), 3);
    EXPECT_EQ(Cell(issues, 0, 0), QStringLiteral("致命"));
    EXPECT_EQ(Cell(issues, 0, 1), QStringLiteral("分片缺失"));
    EXPECT_EQ(Cell(issues, 0, 4), QStringLiteral("3"));
    EXPECT_EQ(issues->item(0, 0)->foreground().color(), QColor("#F85149"));
    EXPECT_EQ(Cell(issues, 1, 0), QStringLiteral("警告"));
    EXPECT_EQ(issues->item(1, 1)->foreground().color(), QColor("#D29922"));
    EXPECT_EQ(Cell(issues, 2, 0), QStringLiteral("提示"));
    EXPECT_EQ(issues->item(2, 3)->foreground().color(), QColor("#8B949E"));

    // ---- JSON 导出：能力边界与关键字段必须跟着数据走 ----
    const QJsonDocument doc = QJsonDocument::fromJson(panel.ExportToJson().toUtf8());
    ASSERT_FALSE(doc.isNull());
    const QJsonObject root_obj = doc.object();
    EXPECT_EQ(root_obj.value("kind").toString(), QString::fromUtf8(videoeye::model::ToString(StreamingKind::Dash)));
    EXPECT_TRUE(root_obj.value("valid").toBool());
    EXPECT_FALSE(root_obj.value("live").toBool());

    const QJsonObject scope = root_obj.value("check_scope").toObject();
    EXPECT_TRUE(scope.value("snapshot_only").toBool());
    EXPECT_FALSE(scope.value("remote_fetch").toBool());
    EXPECT_EQ(scope.value("segments_total").toInt(), 2);
    EXPECT_EQ(scope.value("segments_probed").toInt(), 0);
    EXPECT_FALSE(scope.value("segments_probe_complete").toBool());

    EXPECT_EQ(root_obj.value("dash_live").toObject().value("type").toString(), QStringLiteral("static"));
    EXPECT_FALSE(root_obj.value("dash_descriptors").toObject().value("encrypted").toBool());

    const QJsonArray ladder_json = root_obj.value("ladder").toArray();
    ASSERT_EQ(ladder_json.size(), 2);
    EXPECT_EQ(ladder_json.at(1).toObject().value("label").toString(), QStringLiteral("Representation r1"));

    // JSON 按原顺序导出（不排序）：提示 / 致命 / 警告
    const QJsonArray issues_json = root_obj.value("issues").toArray();
    ASSERT_EQ(issues_json.size(), 3);
    EXPECT_EQ(issues_json.at(1).toObject().value("severity").toString(), QStringLiteral("致命"));
}

// HLS：直播清单的结构树细节 + 摘要里的业务标记
TEST(StreamingPanelTests, HlsResultShowsLivePlaylistDetailsAndSummary) {
    EnsureApp();
    StreamingPanel panel;
    panel.SetResult(MakeHlsResult());

    EXPECT_TRUE(panel.HasResult());
    EXPECT_EQ(panel.IssueCount(), 0);

    QLabel* summary = FindLabelContaining(&panel, QStringLiteral("HLS 主播放列表"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("直播")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("1 个码率层")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("2 个分片")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("含远端 URI")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("分片抽查 1/2")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("已加密")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("1 个广告/章节标记")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("1 档快进(I-FRAME)")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("单次快照（不持续轮询）")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("1 条直播清单，窗口=12.000 s")));

    QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    QTreeWidgetItem* root = tree->topLevelItem(0);
    EXPECT_EQ(root->text(0), QString::fromUtf8(videoeye::model::ToString(StreamingKind::HlsMaster)));

    QTreeWidgetItem* variant = FindChildItem(root, QStringLiteral("variant #0"));
    ASSERT_NE(variant, nullptr);
    EXPECT_EQ(variant->text(1), QStringLiteral("640x360 800 kbps"));
    // I-FRAME 档没有分辨率 -> "-"，不能写成 0x0
    QTreeWidgetItem* iframe = FindChildItem(root, QStringLiteral("I-FRAME #1"));
    ASSERT_NE(iframe, nullptr);
    EXPECT_EQ(iframe->text(1), QStringLiteral("- 400 kbps"));
    QTreeWidgetItem* rendition = FindChildItem(root, QStringLiteral("AUDIO en"));
    ASSERT_NE(rendition, nullptr);
    EXPECT_EQ(rendition->text(1), QStringLiteral("group=aud"));

    QTreeWidgetItem* playlist = FindChildItem(root, QStringLiteral("video #0"));
    ASSERT_NE(playlist, nullptr);
    EXPECT_EQ(playlist->text(1), QStringLiteral("2 分片 target=6 s"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("清单类型"))->text(1), QStringLiteral("直播 EVENT（只增不减）"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("序号基准"))->text(1), QStringLiteral("未声明"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("加密"))->text(1), QStringLiteral("AES-128"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("初始化段"))->text(1), QStringLiteral("init.mp4"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("LL-HLS"))->text(1), QStringLiteral("3 个部分分片"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("推荐起点 (EXT-X-START)"))->text(1),
              QStringLiteral("1.500 s，精确"));
    EXPECT_EQ(FindChildItem(playlist, QStringLiteral("delta update (EXT-X-SKIP)"))->text(1),
              QStringLiteral("跳过 2 个分片"));
    // 墙钟写了但解析不出来，要明说，不能当成"有墙钟"
    EXPECT_TRUE(FindChildItem(playlist, QStringLiteral("墙钟起点 (PROGRAM-DATE-TIME)"))
                    ->text(1)
                    .contains(QStringLiteral("（无法解析）")));

    QTreeWidgetItem* markers = FindChildItem(playlist, QStringLiteral("广告/章节标记 (EXT-X-DATERANGE)"));
    ASSERT_NE(markers, nullptr);
    EXPECT_EQ(markers->text(1), QStringLiteral("1 条"));
    QTreeWidgetItem* marker = FindChildItem(markers, QStringLiteral("ad1"));
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->text(1), QStringLiteral("t=2.000 s d=4.000 s [SCTE-35]"));

    QTreeWidgetItem* segment = FindChildItem(playlist, QStringLiteral("Segment 0"));
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(segment->text(1), QStringLiteral("t=0.000 d=6.000 ok"));

    // HLS 分支的 JSON：playlists / iframe_variants / date_ranges
    const QJsonObject json = QJsonDocument::fromJson(panel.ExportToJson().toUtf8()).object();
    EXPECT_TRUE(json.value("live").toBool());
    const QJsonArray playlists = json.value("playlists").toArray();
    ASSERT_EQ(playlists.size(), 1);
    EXPECT_EQ(playlists.at(0).toObject().value("segment_count").toInt(), 2);
    EXPECT_TRUE(playlists.at(0).toObject().value("has_media_sequence").toBool() == false);
    ASSERT_EQ(json.value("iframe_variants").toArray().size(), 1);
    ASSERT_EQ(playlists.at(0).toObject().value("date_ranges").toArray().size(), 1);
    EXPECT_TRUE(
        playlists.at(0).toObject().value("date_ranges").toArray().at(0).toObject().value("has_scte35").toBool());
}

// 解析失败：摘要给出原因，四块内容全部清空
TEST(StreamingPanelTests, InvalidResultClearsViewsAndShowsError) {
    EnsureApp();
    StreamingPanel panel;
    panel.SetResult(MakeDashResult());
    ASSERT_TRUE(panel.HasResult());

    StreamingPackageResult invalid;
    invalid.kind = StreamingKind::HlsMedia;
    invalid.valid = false;
    invalid.error_message = "清单截断";
    panel.SetResult(invalid);

    EXPECT_FALSE(panel.HasResult());
    EXPECT_NE(FindLabelContaining(&panel, QStringLiteral("流媒体包解析失败：清单截断")), nullptr);
    QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    for (QTableWidget* table : panel.findChildren<QTableWidget*>()) {
        EXPECT_EQ(table->rowCount(), 0);
    }
    EXPECT_EQ(panel.IssueCount(), 0);
}

// Clear：摘要、结构树、三张表、徽标一起复位
TEST(StreamingPanelTests, ClearResetsEverything) {
    EnsureApp();
    StreamingPanel panel;
    panel.SetResult(MakeDashResult());
    ASSERT_TRUE(panel.HasResult());

    panel.Clear();

    EXPECT_FALSE(panel.HasResult());
    EXPECT_NE(FindLabelContaining(&panel, QStringLiteral("尚未分析流媒体包")), nullptr);
    QTreeWidget* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0);
    for (QTableWidget* table : panel.findChildren<QTableWidget*>()) {
        EXPECT_EQ(table->rowCount(), 0);
        EXPECT_EQ(table->columnCount(), 0);
    }
    EXPECT_EQ(panel.IssueCount(), 0);
    EXPECT_FALSE(QJsonDocument::fromJson(panel.ExportToJson().toUtf8()).object().value("valid").toBool());
}

// 扫描态：共用诊断页那一次扫描，「开始分析」必须跟着禁用并在终态复位
TEST(StreamingPanelTests, ScanActiveDisablesRefreshButton) {
    EnsureApp();
    StreamingPanel panel;
    QPushButton* refresh = FindButton(&panel, QStringLiteral("开始分析"));
    ASSERT_NE(refresh, nullptr);
    EXPECT_TRUE(refresh->isEnabled());

    panel.SetScanActive(true);
    EXPECT_TRUE(panel.IsScanActive());
    EXPECT_FALSE(refresh->isEnabled());

    panel.SetScanActive(false);
    EXPECT_FALSE(panel.IsScanActive());
    EXPECT_TRUE(refresh->isEnabled());
}
