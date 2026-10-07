// AudioQcPage 的页面级行为测试（评审「页面组件补测」）。
//
// 这一页的静默回归点：
//   1) 削波事件峰值换算：线性 peak -> dBFS（20*log10），peak=0 回落到 -120 哨兵；
//      声道号 -1 -> 「全部」，其余按声道名展示；
//   2) 判定表：11 条固定规则 id 的命中等级（失败 / 警告 / 提示 / 通过）与配色，
//      阈值文案取自「规则与阈值」快照；改规则后 SetQcReport 必须立即重算；
//   3) 汇总的响度三档判定（|偏差| <= 1 达标 / <= 3 偏离 / 其余超标）与 -∞ 兜底；
//   4) 参数往返（FillScanOptions）+ 扫描态互斥（开始 / 取消按钮 + IsScanActive）。
//
// 只构造控件、不渲染（offscreen 平台）；导出按钮会弹 QFileDialog 模态框，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/AudioQcResult.h"
#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/LoudnessPoint.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "ui/analysis_panel/AudioQcPage.h"

using videoeye::model::AnalysisResult;
using videoeye::model::AudioChannelStat;
using videoeye::model::AudioClipEvent;
using videoeye::model::AudioQcResult;
using videoeye::model::AudioSilenceRange;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueSeverity;
using videoeye::model::QcReport;
using videoeye::model::QcRule;
using videoeye::ui::AudioQcPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_audio_qc_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 页内唯一 QTabWidget 的第 tab_index 页里，按表头首列文案（可选列数）找表
QTableWidget* TableInTab(QWidget* root, int tab_index, const QString& first_header, int columns = -1) {
    auto* tabs = root->findChild<QTabWidget*>();
    if (tabs == nullptr)
        return nullptr;
    QWidget* page = tabs->widget(tab_index);
    if (page == nullptr)
        return nullptr;
    for (QTableWidget* table : page->findChildren<QTableWidget*>()) {
        if (table->horizontalHeaderItem(0) == nullptr || table->horizontalHeaderItem(0)->text() != first_header) {
            continue;
        }
        if (columns >= 0 && table->columnCount() != columns)
            continue;
        return table;
    }
    return nullptr;
}

QLabel* FindLabelContaining(QWidget* root, const QString& needle) {
    for (QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text().contains(needle))
            return label;
    }
    return nullptr;
}

QDoubleSpinBox* FindSpinBySuffix(QWidget* root, const QString& suffix) {
    for (QDoubleSpinBox* spin : root->findChildren<QDoubleSpinBox*>()) {
        if (spin->suffix() == suffix)
            return spin;
    }
    return nullptr;
}

// 削波阈值框是唯一没有 suffix 的 4 位小数输入框
QDoubleSpinBox* FindClipSpin(QWidget* root) {
    for (QDoubleSpinBox* spin : root->findChildren<QDoubleSpinBox*>()) {
        if (spin->suffix().isEmpty() && spin->decimals() == 4)
            return spin;
    }
    return nullptr;
}

QCheckBox* FindCheckBoxByText(QWidget* root, const QString& text) {
    for (QCheckBox* box : root->findChildren<QCheckBox*>()) {
        if (box->text() == text)
            return box;
    }
    return nullptr;
}

QPushButton* FindButton(QWidget* root, const QString& text) {
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

QString Cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

// 通过 meta-object 发射 cellClicked 信号，走与真实点击同一根接线
void ClickCell(QTableWidget* table, int row, int column) {
    QMetaObject::invokeMethod(table, "cellClicked", Qt::DirectConnection, Q_ARG(int, row), Q_ARG(int, column));
}

// 「一切正常」的音频 QC 结果：2 声道、2 段削波（全声道 + 单声道）、2 段静音
AudioQcResult MakeAudioQc() {
    AudioQcResult qc;
    qc.analyzed = true;
    qc.has_audio = true;
    qc.metadata.channel_layout = "立体声 (2.0)";
    qc.metadata.channels = 2;
    qc.metadata.sample_rate = 48000;
    qc.metadata.sample_format = "fltp";
    qc.metadata.bits_per_sample = 32;
    qc.metadata.stream_duration_seconds = 60.0;
    qc.metadata.container_duration_seconds = 60.5;
    qc.metadata.video_duration_seconds = 60.2;
    qc.metadata.has_video = true;
    qc.metadata.container_delta_seconds = -0.5;
    qc.metadata.video_delta_seconds = -0.2;

    qc.duration_seconds = 60.0;
    qc.integrated_lufs = -23.5;
    qc.short_term_max_lufs = -20.0;
    qc.momentary_max_lufs = -18.0;
    qc.loudness_range_lu = 5.5;
    qc.true_peak_dbtp = -1.2;
    qc.sample_peak_dbfs = -1.5;
    qc.rms_dbfs = -20.0;
    qc.max_dc_offset = 0.0001;
    qc.clipping_sample_count = 49;
    qc.clipping_event_count = 2;
    qc.silence_ratio = 0.25;
    qc.longest_silence_seconds = 2.5;
    qc.correlation_available = true;
    qc.correlation_min = -0.1;
    qc.correlation_mean = 0.9;

    AudioChannelStat fl;
    fl.index = 0;
    fl.name = "FL";
    fl.rms_dbfs = -20.1;
    fl.peak_dbfs = -1.5;
    AudioChannelStat fr;
    fr.index = 1;
    fr.name = "FR";
    fr.rms_dbfs = -21.2;
    fr.peak_dbfs = -2.0;
    qc.channels = {fl, fr};

    AudioClipEvent all_ch;
    all_ch.start_seconds = 1.5;
    all_ch.end_seconds = 2.0;
    all_ch.channel = -1;
    all_ch.sample_count = 42;
    all_ch.peak = 0.5;
    AudioClipEvent fl_ch;
    fl_ch.start_seconds = 20.0;
    fl_ch.end_seconds = 20.25;
    fl_ch.channel = 0;
    fl_ch.sample_count = 7;
    fl_ch.peak = 0.0;
    qc.clipping_events = {all_ch, fl_ch};

    AudioSilenceRange s1;
    s1.start_seconds = 10.0;
    s1.end_seconds = 12.5;
    s1.duration_seconds = 2.5;
    s1.rms_dbfs = -50.123;
    AudioSilenceRange s2;
    s2.start_seconds = 30.25;
    s2.end_seconds = 31.0;
    s2.duration_seconds = 0.75;
    s2.rms_dbfs = -70.0;
    qc.silence_ranges = {s1, s2};
    return qc;
}

DiagnosticIssue MakeIssue(const char* rule_id, IssueSeverity severity, const char* detail) {
    DiagnosticIssue issue;
    issue.rule_id = rule_id;
    issue.severity = severity;
    issue.detail = detail;
    return issue;
}

} // namespace

// 构造后未分析 -> 引导文案 + 四个空表；喂入「扫过但没有音频」的结果 -> 空态文案
TEST(AudioQcPageTests, UnanalyzedShowsHintAndEmptyTables) {
    EnsureApp();
    AudioQcPage page;

    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("点击「开始分析」")), nullptr);
    auto* tabs = page.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 4);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("响度与电平"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("静音与削波"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("声道与相位"));
    EXPECT_EQ(tabs->tabText(3), QStringLiteral("规则结果"));

    QTableWidget* clip = TableInTab(&page, 1, QStringLiteral("起始"), 5);
    QTableWidget* silence = TableInTab(&page, 1, QStringLiteral("起始"), 4);
    QTableWidget* metadata = TableInTab(&page, 2, QStringLiteral("项目"));
    QTableWidget* verdict = TableInTab(&page, 3, QStringLiteral("规则"));
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(silence, nullptr);
    ASSERT_NE(metadata, nullptr);
    ASSERT_NE(verdict, nullptr);
    EXPECT_EQ(clip->rowCount(), 0);
    EXPECT_EQ(silence->rowCount(), 0);
    EXPECT_EQ(metadata->rowCount(), 0);
    EXPECT_EQ(verdict->rowCount(), 0);

    AnalysisResult result;
    page.SetResult(result, QcReport{});
    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("暂无音频 QC 结果")), nullptr);
    EXPECT_EQ(clip->rowCount(), 0);
    EXPECT_EQ(silence->rowCount(), 0);
    // metadata 固定 10 行：无视频流时不出现「与视频时差」行
    ASSERT_EQ(metadata->rowCount(), 10);
    EXPECT_EQ(Cell(metadata, 0, 0), QStringLiteral("声道布局"));
    EXPECT_EQ(Cell(metadata, 7, 1), QStringLiteral("无视频流"));
    // 判定表固定 11 条规则；无 issue 全部「通过」并配绿色
    ASSERT_EQ(verdict->rowCount(), 11);
    EXPECT_EQ(Cell(verdict, 0, 0), QStringLiteral("audio.loudness.target_high"));
    EXPECT_EQ(Cell(verdict, 0, 1), QStringLiteral("通过"));
    EXPECT_EQ(verdict->item(0, 1)->foreground().color(), QColor("#43a047"));
}

// 削波 / 静音 / metadata 三张表的取值与格式化
TEST(AudioQcPageTests, ResultFillsClipSilenceAndMetadataTables) {
    EnsureApp();
    AudioQcPage page;
    AnalysisResult result;
    result.audio_qc = MakeAudioQc();
    page.SetResult(result, QcReport{});

    QTableWidget* clip = TableInTab(&page, 1, QStringLiteral("起始"), 5);
    QTableWidget* silence = TableInTab(&page, 1, QStringLiteral("起始"), 4);
    QTableWidget* metadata = TableInTab(&page, 2, QStringLiteral("项目"));
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(silence, nullptr);
    ASSERT_NE(metadata, nullptr);

    // 全声道事件 channel=-1 -> 「全部」；peak=0.5 -> 20*log10(0.5) = -6.02
    ASSERT_EQ(clip->rowCount(), 2);
    EXPECT_EQ(Cell(clip, 0, 0), QStringLiteral("1.500"));
    EXPECT_EQ(Cell(clip, 0, 1), QStringLiteral("2.000"));
    EXPECT_EQ(Cell(clip, 0, 2), QStringLiteral("全部"));
    EXPECT_EQ(Cell(clip, 0, 3), QStringLiteral("42"));
    EXPECT_EQ(Cell(clip, 0, 4), QStringLiteral("-6.02"));
    // 单声道事件按声道名展示；peak=0 -> -120 哨兵值
    EXPECT_EQ(Cell(clip, 1, 2), QStringLiteral("FL"));
    EXPECT_EQ(Cell(clip, 1, 4), QStringLiteral("-120.00"));

    ASSERT_EQ(silence->rowCount(), 2);
    EXPECT_EQ(Cell(silence, 0, 0), QStringLiteral("10.000"));
    EXPECT_EQ(Cell(silence, 0, 1), QStringLiteral("12.500"));
    EXPECT_EQ(Cell(silence, 0, 2), QStringLiteral("2.500"));
    EXPECT_EQ(Cell(silence, 0, 3), QStringLiteral("-50.1"));
    EXPECT_EQ(Cell(silence, 1, 0), QStringLiteral("30.250"));

    // 有视频流时多出「与视频时差」行（11 行）
    ASSERT_EQ(metadata->rowCount(), 11);
    EXPECT_EQ(Cell(metadata, 0, 1), QStringLiteral("立体声 (2.0)"));
    EXPECT_EQ(Cell(metadata, 1, 1), QStringLiteral("2"));
    EXPECT_EQ(Cell(metadata, 2, 1), QStringLiteral("48000 Hz"));
    EXPECT_EQ(Cell(metadata, 3, 1), QStringLiteral("fltp"));
    EXPECT_EQ(Cell(metadata, 4, 1), QStringLiteral("32 bit"));
    EXPECT_EQ(Cell(metadata, 5, 1), QStringLiteral("60.000 s"));
    EXPECT_EQ(Cell(metadata, 9, 0), QStringLiteral("与视频时差"));
    EXPECT_EQ(Cell(metadata, 9, 1), QStringLiteral("-0.200 s"));
    EXPECT_EQ(Cell(metadata, 10, 1), QStringLiteral("无"));
}

// 削波表上限 2000 行：超出的记录只影响表格，不影响其它列
TEST(AudioQcPageTests, ClipTableCapsAt2000Rows) {
    EnsureApp();
    AudioQcPage page;
    AnalysisResult result;
    result.audio_qc.analyzed = true;
    for (int i = 0; i < 2005; ++i) {
        AudioClipEvent event;
        event.start_seconds = i * 0.1;
        event.end_seconds = i * 0.1 + 0.05;
        event.sample_count = 1;
        event.peak = 1.0;
        result.audio_qc.clipping_events.push_back(event);
    }
    page.SetResult(result, QcReport{});

    QTableWidget* clip = TableInTab(&page, 1, QStringLiteral("起始"), 5);
    ASSERT_NE(clip, nullptr);
    ASSERT_EQ(clip->rowCount(), 2000);
    EXPECT_EQ(Cell(clip, 1999, 0), QStringLiteral("199.900"));
    EXPECT_EQ(Cell(clip, 1999, 3), QStringLiteral("1"));
}

// 汇总的响度三档判定：±1 LU 达标 / ±3 LU 偏离 / 其余超标，静音值兜底 -∞
TEST(AudioQcPageTests, SummaryGradesLoudnessDeviationWithBoundaries) {
    EnsureApp();
    AudioQcPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」"));
    ASSERT_NE(summary, nullptr);

    AnalysisResult result;
    result.audio_qc = MakeAudioQc();
    QcReport report;

    result.audio_qc.integrated_lufs = -23.5; // 偏差 -0.50 -> 达标
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度达标")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("#43a047")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("Integrated -23.50 LUFS")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("偏差 -0.50 LU")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("立体声 (2.0)")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("48000 Hz")));

    result.audio_qc.integrated_lufs = -25.0; // 偏差 -2.00 -> 偏离
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度偏离")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("#fb8c00")));

    result.audio_qc.integrated_lufs = -30.0; // 偏差 -7.00 -> 超标
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度超标")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("#e53935")));

    result.audio_qc.integrated_lufs = -120.0; // 静音 -> -∞ + 超标
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("-∞")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度超标")));

    // 边界改由「目标响度」输入框驱动：+1.00 LU 达标 / +3.00 LU 偏离
    QDoubleSpinBox* target = FindSpinBySuffix(&page, QStringLiteral(" LUFS"));
    ASSERT_NE(target, nullptr);
    result.audio_qc.integrated_lufs = -24.0;
    target->setValue(-25.0);
    page.SetResult(result, report); // 重新灌入让 summary 以新的目标值重算
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度达标")));
    target->setValue(-27.0);
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度偏离")));
    target->setValue(-33.0);
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("响度超标")));

    // notes 与 metadata 不一致也要进入汇总
    result.audio_qc.notes.push_back("真峰值检测已关闭，dBTP 回落为采样峰值");
    result.audio_qc.metadata.inconsistencies.push_back("音频流 60.000 s 与容器 65.000 s 偏差过大");
    page.SetResult(result, report);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("真峰值检测已关闭")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("metadata 不一致")));
}

// 判定表：11 条固定规则 id 的等级文案 + 配色 + 阈值文案；SetQcReport 立即重算
TEST(AudioQcPageTests, VerdictTableMapsSeverityColorAndRuleThreshold) {
    EnsureApp();
    AudioQcPage page;
    AnalysisResult result;
    result.audio_qc = MakeAudioQc();

    QcReport report;
    QcRule true_peak_rule;
    true_peak_rule.id = "audio.true_peak";
    true_peak_rule.name = "真峰值上限";
    true_peak_rule.threshold = -1.0;
    true_peak_rule.unit = "dBTP";
    QcRule clip_rule;
    clip_rule.id = "audio.clipping";
    clip_rule.name = "削波段数";
    clip_rule.threshold = 3.0; // 无单位：阈值只显示数字
    clip_rule.unit = "";
    report.rules = {true_peak_rule, clip_rule};
    report.issues = {
        MakeIssue("audio.loudness.target_high", IssueSeverity::Critical, "Integrated -30.0 LUFS 高于上限"),
        MakeIssue("audio.clipping", IssueSeverity::Warning, "2 处削波"),
        MakeIssue("audio.dc_offset", IssueSeverity::Error, "DC 偏移 0.010"),
        MakeIssue("audio.silence.longest", IssueSeverity::Info, "最长静音 2.5 s"),
    };
    page.SetResult(result, report);

    QTableWidget* verdict = TableInTab(&page, 3, QStringLiteral("规则"));
    ASSERT_NE(verdict, nullptr);
    ASSERT_EQ(verdict->rowCount(), 11);

    // 行序固定：0 target_high / 3 true_peak / 4 clipping / 5 silence.longest
    // 7 dc_offset / 8 phase_correlation
    EXPECT_EQ(Cell(verdict, 0, 0), QStringLiteral("audio.loudness.target_high"));
    EXPECT_EQ(Cell(verdict, 0, 1), QStringLiteral("失败"));
    EXPECT_EQ(Cell(verdict, 0, 3), QStringLiteral("Integrated -30.0 LUFS 高于上限"));
    EXPECT_EQ(verdict->item(0, 1)->foreground().color(), QColor("#e53935"));

    EXPECT_EQ(Cell(verdict, 3, 0), QStringLiteral("真峰值上限"));
    EXPECT_EQ(Cell(verdict, 3, 1), QStringLiteral("通过"));
    EXPECT_EQ(Cell(verdict, 3, 2), QStringLiteral("-1.00dBTP"));
    EXPECT_EQ(verdict->item(3, 1)->foreground().color(), QColor("#43a047"));

    EXPECT_EQ(Cell(verdict, 4, 0), QStringLiteral("削波段数"));
    EXPECT_EQ(Cell(verdict, 4, 1), QStringLiteral("警告"));
    EXPECT_EQ(Cell(verdict, 4, 2), QStringLiteral("3.00"));
    EXPECT_EQ(verdict->item(4, 1)->foreground().color(), QColor("#fb8c00"));

    EXPECT_EQ(Cell(verdict, 5, 1), QStringLiteral("提示"));
    EXPECT_EQ(verdict->item(5, 1)->foreground().color(), QColor("#1e88e5"));

    EXPECT_EQ(Cell(verdict, 7, 1), QStringLiteral("失败")); // Error 也映射「失败」
    EXPECT_EQ(verdict->item(7, 1)->foreground().color(), QColor("#e53935"));
    EXPECT_EQ(Cell(verdict, 8, 1), QStringLiteral("通过"));

    // 用户在「规则与阈值」页降级后 SetQcReport：不重扫也要立即变色
    report.issues[2].severity = IssueSeverity::Info;
    page.SetQcReport(report);
    EXPECT_EQ(Cell(verdict, 7, 1), QStringLiteral("提示"));
    EXPECT_EQ(verdict->item(7, 1)->foreground().color(), QColor("#1e88e5"));
}

// 参数默认值 + FillScanOptions 往返 + 扫描态互斥 + 进度条
TEST(AudioQcPageTests, OptionsRoundTripAndScanLifecycle) {
    EnsureApp();
    AudioQcPage page;

    QDoubleSpinBox* target = FindSpinBySuffix(&page, QStringLiteral(" LUFS"));
    QDoubleSpinBox* silence = FindSpinBySuffix(&page, QStringLiteral(" dBFS"));
    QDoubleSpinBox* min_silence = FindSpinBySuffix(&page, QStringLiteral(" s"));
    QDoubleSpinBox* clip = FindClipSpin(&page);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(silence, nullptr);
    ASSERT_NE(min_silence, nullptr);
    ASSERT_NE(clip, nullptr);
    EXPECT_DOUBLE_EQ(target->value(), -23.0);
    EXPECT_DOUBLE_EQ(silence->value(), -60.0);
    EXPECT_DOUBLE_EQ(min_silence->value(), 0.5);
    EXPECT_DOUBLE_EQ(clip->value(), 0.999);

    QCheckBox* loudness = FindCheckBoxByText(&page, QStringLiteral("响度(BS.1770)"));
    QCheckBox* true_peak = FindCheckBoxByText(&page, QStringLiteral("真峰值(4×)"));
    QCheckBox* correlation = FindCheckBoxByText(&page, QStringLiteral("声道相关性"));
    ASSERT_NE(loudness, nullptr);
    ASSERT_NE(true_peak, nullptr);
    ASSERT_NE(correlation, nullptr);
    EXPECT_TRUE(loudness->isChecked());
    EXPECT_TRUE(true_peak->isChecked());
    EXPECT_TRUE(correlation->isChecked());

    target->setValue(-20.0);
    silence->setValue(-50.0);
    min_silence->setValue(3.5);
    clip->setValue(0.95);
    true_peak->setChecked(false);
    videoeye::AnalysisOptions options;
    page.FillScanOptions(options);
    EXPECT_TRUE(options.analyze_audio_qc);
    EXPECT_DOUBLE_EQ(options.audio_qc_options.silence_threshold_dbfs, -50.0);
    EXPECT_DOUBLE_EQ(options.audio_qc_options.min_silence_seconds, 3.5);
    EXPECT_DOUBLE_EQ(options.audio_qc_options.clip_threshold, 0.95);
    EXPECT_TRUE(options.audio_qc_options.enable_loudness);
    EXPECT_FALSE(options.audio_qc_options.enable_true_peak);
    EXPECT_TRUE(options.audio_qc_options.enable_correlation);

    int scan_requests = 0;
    int cancel_requests = 0;
    QObject::connect(&page, &AudioQcPage::ScanRequested, [&scan_requests] { ++scan_requests; });
    QObject::connect(&page, &AudioQcPage::CancelRequested, [&cancel_requests] { ++cancel_requests; });
    QPushButton* start_button = FindButton(&page, QStringLiteral("开始分析"));
    QPushButton* cancel_button = FindButton(&page, QStringLiteral("取消"));
    ASSERT_NE(start_button, nullptr);
    ASSERT_NE(cancel_button, nullptr);

    EXPECT_FALSE(page.IsScanActive());
    EXPECT_TRUE(start_button->isEnabled());
    EXPECT_FALSE(cancel_button->isEnabled());

    start_button->click();
    EXPECT_EQ(scan_requests, 1);
    cancel_button->click(); // 禁用态点不动
    EXPECT_EQ(cancel_requests, 0);

    page.SetScanActive(true);
    EXPECT_TRUE(page.IsScanActive());
    EXPECT_FALSE(start_button->isEnabled());
    EXPECT_TRUE(cancel_button->isEnabled());
    cancel_button->click();
    EXPECT_EQ(cancel_requests, 1);

    page.SetScanActive(false);
    EXPECT_FALSE(page.IsScanActive());
    EXPECT_TRUE(start_button->isEnabled());
    EXPECT_FALSE(cancel_button->isEnabled());

    auto* progress = page.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    page.SetProgress(42);
    EXPECT_EQ(progress->value(), 42);
    page.SetProgressFormat(QStringLiteral("扫描中 %p%"));
    EXPECT_EQ(progress->format(), QStringLiteral("扫描中 %p%"));
}

// 点削波 / 静音行 -> SeekRequested；越界行不跳
TEST(AudioQcPageTests, SeekRequestedOnClipAndSilenceRowClick) {
    EnsureApp();
    AudioQcPage page;
    std::vector<double> seeks;
    QObject::connect(&page, &AudioQcPage::SeekRequested, [&seeks](double seconds) { seeks.push_back(seconds); });

    AnalysisResult result;
    result.audio_qc = MakeAudioQc();
    page.SetResult(result, QcReport{});

    QTableWidget* clip = TableInTab(&page, 1, QStringLiteral("起始"), 5);
    QTableWidget* silence = TableInTab(&page, 1, QStringLiteral("起始"), 4);
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(silence, nullptr);

    ClickCell(clip, 1, 0);
    ASSERT_EQ(seeks.size(), 1u);
    EXPECT_DOUBLE_EQ(seeks[0], 20.0);

    ClickCell(silence, 1, 0);
    ASSERT_EQ(seeks.size(), 2u);
    EXPECT_DOUBLE_EQ(seeks[1], 30.25);

    // 越界行与负行号都不应该跳
    ClickCell(clip, 9, 0);
    ClickCell(silence, -1, 0);
    EXPECT_EQ(seeks.size(), 2u);
}