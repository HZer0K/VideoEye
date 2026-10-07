// SubtitleAuxPage 的页面级行为测试（评审「页面组件补测」）。
//
// 静默回归点：
//   1) 字幕流下拉 + cue 表的流过滤 / 「只看有问题的」过滤：过滤条件抄错只会
//      静默少行多行，肉眼扫不出来；
//   2) cue / 章节的问题列来自 issues 关联（同一条 cue 命中多条规则要累加），
//      有问题的行必须整行标红；
//   3) 时码主信息的展示 + StartTimecodeReady 只在拿到主时码时冒泡一次；
//   4) SCTE-35 表：Event ID 十六进制补零、OUT/IN 语义、CRC 三态；
//   5) 规则阈值同步（ApplyRuleThresholds 只认正阈值，不覆盖默认值）。
//
// 只构造控件、不渲染（offscreen 平台）；四个导出按钮会弹 QFileDialog 模态框，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/AuxiliaryDataInfo.h"
#include "core/domain/model/QcRule.h"
#include "core/domain/model/SubtitleCueInfo.h"
#include "core/domain/model/TimecodeInfo.h"
#include "ui/analysis_panel/SubtitleAuxPage.h"

using videoeye::model::AnalysisResult;
using videoeye::model::AuxDataStream;
using videoeye::model::AuxStreamKind;
using videoeye::model::ChapterInfo;
using videoeye::model::ChapterIssue;
using videoeye::model::ChapterIssueType;
using videoeye::model::MetadataTagEntry;
using videoeye::model::QcRule;
using videoeye::model::Scte35Command;
using videoeye::model::Scte35Cue;
using videoeye::model::Scte35Segmentation;
using videoeye::model::SubtitleCue;
using videoeye::model::SubtitleFormat;
using videoeye::model::SubtitleIssue;
using videoeye::model::SubtitleIssueType;
using videoeye::model::SubtitleKind;
using videoeye::model::SubtitleStreamInfo;
using videoeye::model::Timecode;
using videoeye::model::TimecodeSource;
using videoeye::model::TimecodeTrack;
using videoeye::ui::SubtitleAuxPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_subtitle_aux_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QTableWidget* TableInTab(QWidget* root, int tab_index, const QString& first_header) {
    auto* tabs = root->findChild<QTabWidget*>();
    if (tabs == nullptr) return nullptr;
    QWidget* page = tabs->widget(tab_index);
    if (page == nullptr) return nullptr;
    for (QTableWidget* table : page->findChildren<QTableWidget*>()) {
        if (table->horizontalHeaderItem(0) != nullptr &&
            table->horizontalHeaderItem(0)->text() == first_header) {
            return table;
        }
    }
    return nullptr;
}

QLabel* FindLabelContaining(QWidget* root, const QString& needle) {
    for (QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text().contains(needle)) return label;
    }
    return nullptr;
}

QPushButton* FindButton(QWidget* root, const QString& text) {
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

QString Cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

void ClickCell(QTableWidget* table, int row, int column) {
    QMetaObject::invokeMethod(table, "cellClicked", Qt::DirectConnection,
                              Q_ARG(int, row), Q_ARG(int, column));
}

// 字幕夹具：2 条流（文本 + 图形）、3 条 cue、2 个问题（1 个 cue 级 + 1 个流级）
void FillSubtitle(AnalysisResult& result) {
    auto& sub = result.subtitle;
    SubtitleStreamInfo s0;
    s0.stream_index = 2;
    s0.format = SubtitleFormat::Srt;
    s0.kind = SubtitleKind::Text;
    s0.codec_name = "subrip";
    s0.language = "zh";
    s0.handler_name = "SubRip";
    s0.cue_count = 2;
    s0.packet_count = 100;
    SubtitleStreamInfo s1;
    s1.stream_index = 3;
    s1.format = SubtitleFormat::DvbSubtitle;
    s1.kind = SubtitleKind::Bitmap;
    s1.codec_name = "dvbsub";
    s1.cue_count = 0;
    s1.packet_count = 50;
    s1.note = "图形字幕仅输出 metadata";
    sub.streams = {s0, s1};

    SubtitleCue c0;
    c0.index = 0;
    c0.stream_index = 2;
    c0.start_seconds = 1.25;
    c0.end_seconds = 3.75;
    c0.duration_seconds = 2.5;
    c0.char_count = 12;
    c0.text = "你好世界";
    c0.language = "zh";
    c0.has_issue = true;
    SubtitleCue c1;
    c1.index = 1;
    c1.stream_index = 2;
    c1.start_seconds = 5.0;
    c1.end_seconds = 6.5;
    c1.duration_seconds = 1.5;
    c1.char_count = 6;
    c1.text = "第二句";
    SubtitleCue c2;
    c2.index = 0;
    c2.stream_index = 3;
    c2.start_seconds = 10.0;
    c2.end_seconds = 12.0;
    c2.duration_seconds = 2.0;
    c2.char_count = 4;
    c2.text = "(图形)";
    sub.cues = {c0, c1, c2};

    SubtitleIssue i0;
    i0.type = SubtitleIssueType::TooShort;
    i0.severity = videoeye::model::IssueSeverity::Warning;
    i0.stream_index = 2;
    i0.cue_index = 0;
    i0.detail = "停留 2.500 s 短于下限 3.000 s";
    SubtitleIssue i1;
    i1.type = SubtitleIssueType::MissingLanguage;
    i1.severity = videoeye::model::IssueSeverity::Warning;
    i1.stream_index = 3;
    i1.cue_index = -1;   // 流级问题
    i1.detail = "流 3 缺少 language tag";
    sub.issues = {i0, i1};
    sub.text_stream_count = 1;
    sub.bitmap_stream_count = 1;
    sub.total_cue_count = 3;
    sub.analyzed = true;
    result.subtitle_analyzed = true;
}

// 时码夹具：10:00:00:00 @ 25 fps + 一条 tmcd 轨 + 2 个章节（第 2 个重叠）
void FillTimecode(AnalysisResult& result) {
    auto& tc = result.timecode;
    tc.primary.valid = true;
    tc.primary.hours = 10;
    tc.primary.minutes = 0;
    tc.primary.seconds = 0;
    tc.primary.frames = 0;
    tc.has_primary = true;
    tc.primary_frame_rate = 25.0;
    TimecodeTrack track;
    track.stream_index = 2;
    track.source = TimecodeSource::TimecodeTrack;
    track.codec_name = "tmcd";
    track.frame_rate = 25.0;
    track.first_timecode = tc.primary;
    tc.tracks = {track};

    ChapterInfo ch0;
    ch0.index = 0;
    ch0.start_seconds = 0.0;
    ch0.end_seconds = 30.0;
    ch0.title = "开场";
    ch0.language = "zh";
    ChapterInfo ch1;
    ch1.index = 1;
    ch1.start_seconds = 30.0;
    ch1.end_seconds = 60.0;
    ch1.title = "正片";
    tc.chapters = {ch0, ch1};
    ChapterIssue issue;
    issue.type = ChapterIssueType::Overlap;
    issue.chapter_index = 1;
    issue.start_seconds = 25.0;
    issue.end_seconds = 35.0;
    issue.detail = "章节 2 与上一章节重叠 5.000 s";
    tc.chapter_issues = {issue};
    tc.analyzed = true;
    result.timecode_analyzed = true;
}

// 辅助数据夹具：1 条 SCTE-35 data 流、2 条 cue（OUT/IN）、2 条 metadata
void FillAux(AnalysisResult& result) {
    auto& aux = result.aux_data;
    AuxDataStream s;
    s.stream_index = 1;
    s.kind = AuxStreamKind::Scte35;
    s.codec_name = "scte35";
    s.codec_tag = "0x86";
    s.handler_name = "SCTE-35";
    s.packet_count = 2;
    s.byte_count = 1024;
    aux.streams = {s};

    Scte35Cue out;
    out.index = 0;
    out.stream_index = 1;
    out.packet_pts_seconds = 100.0;
    out.command = Scte35Command::SpliceInsert;
    out.event_id = 0xABCDEF12u;
    out.has_event_id = true;
    out.out_of_network = true;
    out.valid = true;
    out.splice_time_seconds = 100.0;
    out.has_splice_time = true;
    out.break_duration_seconds = 30.0;
    out.has_duration = true;
    out.crc_checked = true;
    out.crc_valid = true;
    Scte35Segmentation seg;
    seg.type_id = 0x30;
    seg.type_name = "广告开始";
    seg.has_duration = true;
    seg.duration_seconds = 30.0;
    seg.upid_summary = "0xABCD";
    out.segmentation = {seg};
    Scte35Cue in;
    in.index = 1;
    in.stream_index = 1;
    in.packet_pts_seconds = 130.0;
    in.command = Scte35Command::SpliceInsert;
    in.out_of_network = false;
    in.valid = true;
    in.crc_checked = false;
    aux.cues = {out, in};
    aux.scte35_cue_count = 2;
    aux.scte35_out_count = 1;
    aux.scte35_in_count = 1;

    MetadataTagEntry tag0;
    tag0.scope = "容器";
    tag0.key = "title";
    tag0.value = "测试片";
    MetadataTagEntry tag1;
    tag1.scope = "流 1";
    tag1.stream_index = 1;
    tag1.key = "language";
    tag1.value = "zh";
    aux.metadata = {tag0, tag1};
    aux.analyzed = true;
    result.aux_data_analyzed = true;
}

}  // namespace

// 构造后未分析 -> 提示文案 + 四个空表；点「重新扫描」发 ScanRequested
TEST(SubtitleAuxPageTests, UnanalyzedShowsHintAndEmptyTables) {
    EnsureApp();
    SubtitleAuxPage page;

    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("未分析：点「重新扫描」")), nullptr);
    auto* tabs = page.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 4);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("字幕"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("时码与章节"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("辅助数据与 SCTE-35"));
    EXPECT_EQ(tabs->tabText(3), QStringLiteral("metadata"));

    QTableWidget* cue = TableInTab(&page, 0, QStringLiteral("#"));
    QTableWidget* scte35 = TableInTab(&page, 2, QStringLiteral("#"));
    QTableWidget* metadata = TableInTab(&page, 3, QStringLiteral("作用域"));
    ASSERT_NE(cue, nullptr);
    ASSERT_NE(scte35, nullptr);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(cue->rowCount(), 0);
    EXPECT_EQ(scte35->rowCount(), 0);
    EXPECT_EQ(metadata->rowCount(), 0);

    int scan_requests = 0;
    QObject::connect(&page, &SubtitleAuxPage::ScanRequested, [&scan_requests] { ++scan_requests; });
    QPushButton* rescan = FindButton(&page, QStringLiteral("重新扫描"));
    ASSERT_NE(rescan, nullptr);
    rescan->click();
    EXPECT_EQ(scan_requests, 1);
}

// 字幕流表 / cue 表 + 两个过滤器 + 问题行整行标红
TEST(SubtitleAuxPageTests, SubtitleTablesAndFilters) {
    EnsureApp();
    SubtitleAuxPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("未分析：点「重新扫描」"));
    ASSERT_NE(summary, nullptr);

    AnalysisResult result;
    FillSubtitle(result);
    int timecode_emits = 0;
    QObject::connect(&page, &SubtitleAuxPage::StartTimecodeReady,
                     [&timecode_emits](const QString&, double) { ++timecode_emits; });
    page.SetResult(result);

    EXPECT_TRUE(summary->text().contains(
        QStringLiteral("字幕流 2 条（文本 1 / 图形 1），cue 3 条，问题 2 处")));
    // 只有字幕数据时不应冒泡播放器时码
    EXPECT_EQ(timecode_emits, 0);

    QTableWidget* streams = TableInTab(&page, 0, QStringLiteral("流"));
    QTableWidget* cue = TableInTab(&page, 0, QStringLiteral("#"));
    ASSERT_NE(streams, nullptr);
    ASSERT_NE(cue, nullptr);

    // 流表：编码 / 格式 / 承载 / 语言 / handler / 默认与强制 / cue 与包数
    ASSERT_EQ(streams->rowCount(), 2);
    EXPECT_EQ(Cell(streams, 0, 0), QStringLiteral("2"));
    EXPECT_EQ(Cell(streams, 0, 1), QStringLiteral("subrip"));
    EXPECT_EQ(Cell(streams, 0, 2),
              QString::fromStdString(videoeye::model::ToString(SubtitleFormat::Srt)));
    EXPECT_EQ(Cell(streams, 0, 3),
              QString::fromStdString(videoeye::model::ToString(SubtitleKind::Text)));
    EXPECT_EQ(Cell(streams, 0, 4), QStringLiteral("zh"));
    EXPECT_EQ(Cell(streams, 0, 5), QStringLiteral("SubRip"));
    EXPECT_EQ(Cell(streams, 0, 6), QStringLiteral("否"));
    EXPECT_EQ(Cell(streams, 0, 7), QStringLiteral("否"));
    EXPECT_EQ(Cell(streams, 0, 8), QStringLiteral("2"));
    EXPECT_EQ(Cell(streams, 0, 9), QStringLiteral("100"));
    EXPECT_EQ(Cell(streams, 1, 3),
              QString::fromStdString(videoeye::model::ToString(SubtitleKind::Bitmap)));
    EXPECT_EQ(Cell(streams, 1, 10), QStringLiteral("图形字幕仅输出 metadata"));

    // 流下拉：全部 + 两条流，data 为流号
    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 3);
    EXPECT_EQ(combo->itemData(0).toInt(), -1);
    EXPECT_EQ(combo->itemData(1).toInt(), 2);
    EXPECT_EQ(combo->itemData(2).toInt(), 3);
    EXPECT_TRUE(combo->itemText(1).contains(QStringLiteral("流 2")));

    // cue 表：序号从 1 起、时间 3 位小数、问题列取 issue 文案、问题行整行标红
    ASSERT_EQ(cue->rowCount(), 3);
    EXPECT_EQ(Cell(cue, 0, 0), QStringLiteral("1"));
    EXPECT_EQ(Cell(cue, 0, 1), QStringLiteral("1.250"));
    EXPECT_EQ(Cell(cue, 0, 2), QStringLiteral("3.750"));
    EXPECT_EQ(Cell(cue, 0, 3), QStringLiteral("2.500"));
    EXPECT_EQ(Cell(cue, 0, 4), QStringLiteral("12"));
    EXPECT_EQ(Cell(cue, 0, 5), QStringLiteral("zh"));
    EXPECT_EQ(Cell(cue, 0, 6),
              QString::fromStdString(videoeye::model::ToString(SubtitleIssueType::TooShort)));
    EXPECT_EQ(Cell(cue, 0, 7), QStringLiteral("你好世界"));
    EXPECT_EQ(cue->item(0, 0)->background().color(), QColor("#5a1f1f"));
    EXPECT_EQ(Cell(cue, 1, 6), QString());
    EXPECT_EQ(Cell(cue, 1, 7), QStringLiteral("第二句"));
    EXPECT_NE(cue->item(1, 0)->background().color(), QColor("#5a1f1f"));

    // 流过滤：只看流 3 -> 只剩图形字幕那条
    combo->setCurrentIndex(2);
    ASSERT_EQ(cue->rowCount(), 1);
    EXPECT_EQ(Cell(cue, 0, 7), QStringLiteral("(图形)"));
    combo->setCurrentIndex(0);
    EXPECT_EQ(cue->rowCount(), 3);

    // 「只看有问题的」：只有 has_issue 的 cue 留下
    auto* issues_only = page.findChild<QCheckBox*>();
    ASSERT_NE(issues_only, nullptr);
    issues_only->setChecked(true);
    ASSERT_EQ(cue->rowCount(), 1);
    EXPECT_EQ(Cell(cue, 0, 7), QStringLiteral("你好世界"));

    // 两个过滤叠加：流 3 + 只看有问题的 -> 空
    combo->setCurrentIndex(2);
    EXPECT_EQ(cue->rowCount(), 0);
    combo->setCurrentIndex(0);
    issues_only->setChecked(false);
    EXPECT_EQ(cue->rowCount(), 3);
}

// 时码表 / 章节表 + StartTimecodeReady 冒泡规则
TEST(SubtitleAuxPageTests, TimecodeChapterTablesAndStartTimecodeSignal) {
    EnsureApp();
    SubtitleAuxPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("未分析：点「重新扫描」"));
    ASSERT_NE(summary, nullptr);

    AnalysisResult result;
    FillSubtitle(result);
    FillTimecode(result);

    int timecode_emits = 0;
    QString tc_text;
    double tc_fps = 0.0;
    QObject::connect(&page, &SubtitleAuxPage::StartTimecodeReady,
                     [&](const QString& timecode, double fps) {
                         ++timecode_emits;
                         tc_text = timecode;
                         tc_fps = fps;
                     });
    page.SetResult(result);
    ASSERT_EQ(timecode_emits, 1);
    EXPECT_EQ(tc_text, QStringLiteral("10:00:00:00"));
    EXPECT_DOUBLE_EQ(tc_fps, 25.0);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("首帧时码 10:00:00:00 @ 25.000 fps")));

    QTableWidget* timecode = TableInTab(&page, 1, QStringLiteral("项目"));
    QTableWidget* chapter = TableInTab(&page, 1, QStringLiteral("#"));
    ASSERT_NE(timecode, nullptr);
    ASSERT_NE(chapter, nullptr);

    // 时码表：固定 3 行 + 1 条时码源
    ASSERT_EQ(timecode->rowCount(), 4);
    EXPECT_EQ(Cell(timecode, 0, 0), QStringLiteral("首帧时码"));
    EXPECT_EQ(Cell(timecode, 0, 1), QStringLiteral("10:00:00:00"));
    EXPECT_EQ(Cell(timecode, 1, 0), QStringLiteral("帧率"));
    EXPECT_EQ(Cell(timecode, 1, 1), QStringLiteral("25.000"));
    EXPECT_EQ(Cell(timecode, 2, 1), QStringLiteral("否"));
    EXPECT_EQ(Cell(timecode, 3, 0), QStringLiteral("时码源 2"));
    EXPECT_EQ(Cell(timecode, 3, 1), QStringLiteral("10:00:00:00"));

    // 章节表：时长与问题列，问题章节标红
    ASSERT_EQ(chapter->rowCount(), 2);
    EXPECT_EQ(Cell(chapter, 0, 0), QStringLiteral("1"));
    EXPECT_EQ(Cell(chapter, 0, 1), QStringLiteral("0.000"));
    EXPECT_EQ(Cell(chapter, 0, 2), QStringLiteral("30.000"));
    EXPECT_EQ(Cell(chapter, 0, 3), QStringLiteral("30.000"));
    EXPECT_EQ(Cell(chapter, 0, 4), QStringLiteral("开场"));
    EXPECT_EQ(Cell(chapter, 0, 5), QStringLiteral("zh"));
    EXPECT_EQ(Cell(chapter, 0, 6), QString());
    EXPECT_EQ(Cell(chapter, 1, 6),
              QString::fromStdString(videoeye::model::ToString(ChapterIssueType::Overlap)));
    EXPECT_EQ(chapter->item(1, 0)->background().color(), QColor("#5a1f1f"));

    // drop-frame 时码用 ';' 分隔，并在汇总里标注
    // （信号文本来自 primary.ToString()，读的是 primary.drop_frame；
    //   primary_drop_frame 只驱动汇总文案与时码表的「drop-frame」行）
    result.timecode.primary.drop_frame = true;
    result.timecode.primary_drop_frame = true;
    page.SetResult(result);
    EXPECT_EQ(timecode_emits, 2);
    EXPECT_EQ(tc_text, QStringLiteral("10:00:00;00"));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("（drop-frame）")));

    // 没有主时码：不再冒泡，表里显示「无」
    result.timecode.has_primary = false;
    page.SetResult(result);
    EXPECT_EQ(timecode_emits, 2);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("未找到时码")));
    EXPECT_EQ(Cell(timecode, 0, 1), QStringLiteral("无"));
}

// 辅助数据流 / SCTE-35 / metadata 三张表
TEST(SubtitleAuxPageTests, AuxStreamScte35AndMetadataTables) {
    EnsureApp();
    SubtitleAuxPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("未分析：点「重新扫描」"));
    ASSERT_NE(summary, nullptr);

    AnalysisResult result;
    FillAux(result);
    page.SetResult(result);

    EXPECT_TRUE(summary->text().contains(QStringLiteral("SCTE-35 cue 2 条（OUT 1 / IN 1）")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("metadata 2 条")));

    QTableWidget* streams = TableInTab(&page, 2, QStringLiteral("流"));
    QTableWidget* scte35 = TableInTab(&page, 2, QStringLiteral("#"));
    QTableWidget* metadata = TableInTab(&page, 3, QStringLiteral("作用域"));
    ASSERT_NE(streams, nullptr);
    ASSERT_NE(scte35, nullptr);
    ASSERT_NE(metadata, nullptr);

    ASSERT_EQ(streams->rowCount(), 1);
    EXPECT_EQ(Cell(streams, 0, 0), QStringLiteral("1"));
    EXPECT_EQ(Cell(streams, 0, 1),
              QString::fromStdString(videoeye::model::ToString(AuxStreamKind::Scte35)));
    EXPECT_EQ(Cell(streams, 0, 2), QStringLiteral("scte35"));
    EXPECT_EQ(Cell(streams, 0, 3), QStringLiteral("0x86"));
    EXPECT_EQ(Cell(streams, 0, 7), QStringLiteral("1024"));

    // OUT cue：十六进制补零 Event ID、OUT 语义、splice / 分段 / CRC 齐全
    ASSERT_EQ(scte35->rowCount(), 2);
    EXPECT_EQ(Cell(scte35, 0, 0), QStringLiteral("1"));
    EXPECT_EQ(Cell(scte35, 0, 1), QStringLiteral("100.000"));
    EXPECT_EQ(Cell(scte35, 0, 2),
              QString::fromStdString(videoeye::model::ToString(Scte35Command::SpliceInsert)));
    EXPECT_EQ(Cell(scte35, 0, 3), QStringLiteral("0xabcdef12"));
    EXPECT_EQ(Cell(scte35, 0, 4), QStringLiteral("OUT"));
    EXPECT_EQ(Cell(scte35, 0, 5), QStringLiteral("100.000"));
    EXPECT_EQ(Cell(scte35, 0, 6), QStringLiteral("30.000"));
    EXPECT_EQ(Cell(scte35, 0, 7), QStringLiteral("广告开始"));
    EXPECT_EQ(Cell(scte35, 0, 8), QStringLiteral("30.000"));
    EXPECT_EQ(Cell(scte35, 0, 9), QStringLiteral("0xABCD"));
    EXPECT_EQ(Cell(scte35, 0, 10), QStringLiteral("通过"));

    // IN cue：缺 Event ID / splice / 分段 / CRC 时全部回落「-」与「未校验」
    EXPECT_EQ(Cell(scte35, 1, 3), QStringLiteral("-"));
    EXPECT_EQ(Cell(scte35, 1, 4), QStringLiteral("IN"));
    EXPECT_EQ(Cell(scte35, 1, 5), QStringLiteral("-"));
    EXPECT_EQ(Cell(scte35, 1, 6), QStringLiteral("-"));
    EXPECT_EQ(Cell(scte35, 1, 7), QString());
    EXPECT_EQ(Cell(scte35, 1, 10), QStringLiteral("未校验"));

    ASSERT_EQ(metadata->rowCount(), 2);
    EXPECT_EQ(Cell(metadata, 0, 0), QStringLiteral("容器"));
    EXPECT_EQ(Cell(metadata, 0, 1), QStringLiteral("title"));
    EXPECT_EQ(Cell(metadata, 0, 2), QStringLiteral("测试片"));
    EXPECT_EQ(Cell(metadata, 1, 0), QStringLiteral("流 1"));
    EXPECT_EQ(Cell(metadata, 1, 1), QStringLiteral("language"));
    EXPECT_EQ(Cell(metadata, 1, 2), QStringLiteral("zh"));
}

// 规则阈值同步：只认正阈值；缺失的规则保持默认
TEST(SubtitleAuxPageTests, ApplyRuleThresholdsUsesRulesWithPositiveGuard) {
    EnsureApp();
    SubtitleAuxPage page;

    std::vector<QcRule> rules;
    QcRule short_rule;
    short_rule.id = "subtitle.cue_too_short";
    short_rule.threshold = 1.2;
    QcRule long_rule;
    long_rule.id = "subtitle.cue_too_long";
    long_rule.threshold = 0.0;   // 0 不应覆盖默认值
    QcRule fast_rule;
    fast_rule.id = "subtitle.cue_too_fast";
    fast_rule.threshold = 15.0;
    rules = {short_rule, long_rule, fast_rule};

    videoeye::SubtitleOptions options;
    page.ApplyRuleThresholds(rules, options);
    EXPECT_DOUBLE_EQ(options.min_cue_duration_seconds, 1.2);
    EXPECT_DOUBLE_EQ(options.max_cue_duration_seconds, 8.0);
    EXPECT_DOUBLE_EQ(options.max_chars_per_second, 15.0);

    videoeye::SubtitleOptions defaults;
    page.ApplyRuleThresholds({}, defaults);
    EXPECT_DOUBLE_EQ(defaults.min_cue_duration_seconds, 0.5);
    EXPECT_DOUBLE_EQ(defaults.max_cue_duration_seconds, 8.0);
    EXPECT_DOUBLE_EQ(defaults.max_chars_per_second, 21.0);
}

// 点 cue 行 -> SeekRequested（时间取自「开始」列文本）；越界行不跳
TEST(SubtitleAuxPageTests, SeekRequestedOnCueRowClick) {
    EnsureApp();
    SubtitleAuxPage page;
    std::vector<double> seeks;
    QObject::connect(&page, &SubtitleAuxPage::SeekRequested,
                     [&seeks](double seconds) { seeks.push_back(seconds); });

    AnalysisResult result;
    FillSubtitle(result);
    page.SetResult(result);

    QTableWidget* cue = TableInTab(&page, 0, QStringLiteral("#"));
    ASSERT_NE(cue, nullptr);

    ClickCell(cue, 1, 0);
    ASSERT_EQ(seeks.size(), 1u);
    EXPECT_DOUBLE_EQ(seeks[0], 5.0);

    // 过滤后点第一行（流 3 的 cue，开始 10.0 s）
    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(2);
    ASSERT_EQ(cue->rowCount(), 1);
    ClickCell(cue, 0, 0);
    ASSERT_EQ(seeks.size(), 2u);
    EXPECT_DOUBLE_EQ(seeks[1], 10.0);

    combo->setCurrentIndex(0);
    ClickCell(cue, 9, 0);   // 越界行
    ClickCell(cue, -1, 0);  // 负行号
    EXPECT_EQ(seeks.size(), 2u);
}