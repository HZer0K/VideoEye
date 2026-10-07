#include "ui/analysis_panel/AudioQcPage.h"

#include <QAbstractItemView>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"

#include "core/domain/model/AudioQcResult.h"
#include "core/domain/model/LoudnessPoint.h"
#include "core/domain/model/QcReport.h"

#include <QColor>

namespace videoeye {
namespace ui {

AudioQcPage::AudioQcPage(QWidget* parent) : QWidget(parent) {
    SetupUi();
}


// ===========================================================================
// 音频 QC 标签页（响度 / 真峰值 / 削波 / 静音 / 声道相位 / metadata 一致性）
// ===========================================================================
namespace {

// 参与「音频 QC → 规则结果」展示的规则 id（与 QcModels.cpp 的 audio.* 保持一致）
const char* const kAudioQcRuleIds[] = {
    "audio.loudness.target_high", "audio.loudness.target_low", "audio.loudness.range",
    "audio.true_peak",            "audio.clipping",            "audio.silence.longest",
    "audio.silence.ratio",        "audio.dc_offset",           "audio.phase_correlation",
    "audio.metadata.layout",      "audio.metadata.duration_mismatch",
};

QString AudioVerdictText(const model::QcReport& report, const QString& rule_id,
                         QString* detail_out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id.toStdString()) continue;
        if (detail_out) *detail_out = QString::fromStdString(issue.detail);
        switch (issue.severity) {
            case model::IssueSeverity::Critical:
            case model::IssueSeverity::Error:    return QStringLiteral("失败");
            case model::IssueSeverity::Warning:  return QStringLiteral("警告");
            default:                             return QStringLiteral("提示");
        }
    }
    return QStringLiteral("通过");
}

QString AudioFormatDb(double value, double silence_floor) {
    if (value <= silence_floor + 1.0) return QStringLiteral("-∞");
    return QString::number(value, 'f', 2);
}

// 抽稀（保留每组极值，避免丢掉峰值）
void AppendAudioPoints(ChartSeries* series, const std::vector<model::LoudnessPoint>& points,
                       int limit, double (model::LoudnessPoint::*member), double floor_value) {
    if (points.empty()) return;
    const double fallback = floor_value;
    auto get = [member, fallback](const model::LoudnessPoint& p) {
        const double v = p.*member;
        return (v <= fallback + 1.0) ? fallback : v;
    };
    const size_t n = points.size();
    const size_t step = (n <= static_cast<size_t>(limit)) ? 1 : (n + limit - 1) / limit;
    const size_t out_n = (n + step - 1) / step;
    SeriesBatch batch(series);
    batch.Reserve(static_cast<int>(out_n));
    for (size_t i = 0; i < n; i += step) {
        batch.Add(points[i].timestamp_seconds, get(points[i]));
    }
}

}  // namespace

void AudioQcPage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 开始/取消/导出
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("音频 QC（响度 / 真峰值 / 削波 / 静音 / 相位）"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        start_button_ = new QPushButton(tr("开始分析"), row);
        start_button_->setToolTip(
            tr("对当前文件解码音频流做一次完整体检：BS.1770 响度、4× 过采样真峰值、削波、"
               "静音段、声道相位与 metadata 一致性。与「码率与 GOP」「诊断与报告」共用同一次扫描。"));
        connect(start_button_, &QPushButton::clicked,
                this, &AudioQcPage::OnStartAnalysis);
        rl->addWidget(start_button_);

        cancel_button_ = new QPushButton(tr("取消"), row);
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked,
                this, &AudioQcPage::OnCancelAnalysis);
        rl->addWidget(cancel_button_);

        QPushButton* export_btn = new QPushButton(tr("导出响度 CSV"), row);
        connect(export_btn, &QPushButton::clicked, this, &AudioQcPage::OnExportCsv);
        rl->addWidget(export_btn);
        layout->addWidget(row);
    }

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(progress_bar_);

    // 参数行
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        rl->addWidget(new QLabel(tr("目标响度"), row));
        target_lufs_spin_ = new QDoubleSpinBox(row);
        target_lufs_spin_->setRange(-70.0, 0.0);
        target_lufs_spin_->setDecimals(1);
        target_lufs_spin_->setSingleStep(1.0);
        target_lufs_spin_->setValue(-23.0);
        target_lufs_spin_->setSuffix(tr(" LUFS"));
        target_lufs_spin_->setToolTip(tr("仅用于曲线上的参考线；合格判定阈值在「规则与阈值」页"));
        connect(target_lufs_spin_, &QDoubleSpinBox::valueChanged,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(target_lufs_spin_);

        rl->addWidget(new QLabel(tr("静音阈值"), row));
        silence_spin_ = new QDoubleSpinBox(row);
        silence_spin_->setRange(-120.0, 0.0);
        silence_spin_->setDecimals(0);
        silence_spin_->setSingleStep(5.0);
        silence_spin_->setValue(options_.silence_threshold_dbfs);
        silence_spin_->setSuffix(tr(" dBFS"));
        connect(silence_spin_, &QDoubleSpinBox::valueChanged,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(silence_spin_);

        rl->addWidget(new QLabel(tr("最短静音"), row));
        min_silence_spin_ = new QDoubleSpinBox(row);
        min_silence_spin_->setRange(0.0, 60.0);
        min_silence_spin_->setDecimals(2);
        min_silence_spin_->setSingleStep(0.1);
        min_silence_spin_->setValue(options_.min_silence_seconds);
        min_silence_spin_->setSuffix(tr(" s"));
        connect(min_silence_spin_, &QDoubleSpinBox::valueChanged,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(min_silence_spin_);

        rl->addWidget(new QLabel(tr("削波阈值"), row));
        clip_spin_ = new QDoubleSpinBox(row);
        clip_spin_->setRange(0.5, 1.0);
        clip_spin_->setDecimals(4);
        clip_spin_->setSingleStep(0.0005);
        clip_spin_->setValue(options_.clip_threshold);
        connect(clip_spin_, &QDoubleSpinBox::valueChanged,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(clip_spin_);

        loudness_check_ = new QCheckBox(tr("响度(BS.1770)"), row);
        loudness_check_->setChecked(options_.enable_loudness);
        connect(loudness_check_, &QCheckBox::toggled,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(loudness_check_);

        true_peak_check_ = new QCheckBox(tr("真峰值(4×)"), row);
        true_peak_check_->setChecked(options_.enable_true_peak);
        true_peak_check_->setToolTip(tr("4× 过采样检测，最耗时的一项；关闭后 dBTP 回落为采样峰值"));
        connect(true_peak_check_, &QCheckBox::toggled,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(true_peak_check_);

        correlation_check_ = new QCheckBox(tr("声道相关性"), row);
        correlation_check_->setChecked(options_.enable_correlation);
        connect(correlation_check_, &QCheckBox::toggled,
                this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(correlation_check_);

        rl->addStretch();
        layout->addWidget(row);
    }

    summary_label_ = new QLabel(
        tr("点击「开始分析」对音频流做一次完整解码体检：Integrated / Short-term / Momentary LUFS、"
           "LRA、真峰值、削波、静音段、声道相位与 metadata 一致性。"), this);
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setMinimumHeight(340);

    // ---- 子页 0: 响度与电平 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        lufs_chart_ = new MetricChartWidget(page);
        lufs_chart_->SetTitle(tr("响度曲线（M 400ms / S 3s / I 累计）"));
        momentary_series_ = lufs_chart_->AddLineSeries(tr("瞬时 M"), QColor("#42a5f5"));
        short_term_series_ = lufs_chart_->AddLineSeries(tr("短期 S"), QColor("#66bb6a"));
        integrated_series_ = lufs_chart_->AddLineSeries(tr("累计 I"), QColor("#8e24aa"));
        target_series_ = lufs_chart_->AddLineSeries(tr("目标"), QColor("#e53935"));
        lufs_axis_x_ = lufs_chart_->AxisX();
        lufs_axis_y_ = lufs_chart_->AxisY();
        lufs_axis_x_->SetTitleText(tr("时间 (s)"));
        lufs_axis_y_->SetTitleText(tr("LUFS"));
        lufs_chart_->setMinimumHeight(210);
        pl->addWidget(lufs_chart_);

        level_chart_ = new MetricChartWidget(page);
        level_chart_->SetTitle(tr("电平曲线（RMS / 采样峰值 / 真峰值）"));
        rms_series_ = level_chart_->AddLineSeries(tr("RMS dBFS"), QColor("#42a5f5"));
        peak_series_ = level_chart_->AddLineSeries(tr("峰值 dBFS"), QColor("#66bb6a"));
        true_peak_series_ = level_chart_->AddLineSeries(tr("真峰值 dBTP"), QColor("#fb8c00"));
        level_axis_x_ = level_chart_->AxisX();
        level_axis_y_ = level_chart_->AxisY();
        level_axis_x_->SetTitleText(tr("时间 (s)"));
        level_axis_y_->SetTitleText(tr("dB"));
        level_chart_->setMinimumHeight(210);
        pl->addWidget(level_chart_);

        sub_tabs_->addTab(page, tr("响度与电平"));
    }

    // ---- 子页 1: 静音与削波 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        event_chart_ = new MetricChartWidget(page);
        event_chart_->SetTitle(tr("静音段（方波）与削波点（三角）时间轴"));
        silence_series_ = event_chart_->AddLineSeries(tr("静音段"), QColor("#1e88e5"));
        clip_series_ = event_chart_->AddScatterSeries(tr("削波"), QColor("#e53935"));
        clip_series_->SetMarkerSize(9.0);
        clip_series_->SetMarkerShape(ChartMarkerShape::Triangle);
        clip_series_->SetBorderColor(QColor("#e53935"));
        event_axis_x_ = event_chart_->AxisX();
        event_axis_y_ = event_chart_->AxisY();
        event_axis_x_->SetTitleText(tr("时间 (s)"));
        event_axis_y_->SetTitleText(tr("静音 0/1 ｜ 削波 1.5"));
        event_axis_y_->SetRange(-0.2, 1.8);
        event_chart_->setMinimumHeight(180);
        pl->addWidget(event_chart_);

        QLabel* clip_hint = new QLabel(tr("点击任意一行跳转到该位置。"), page);
        pl->addWidget(clip_hint);
        clip_table_ = new QTableWidget(0, 5, page);
        clip_table_->setHorizontalHeaderLabels(
            {tr("起始"), tr("结束"), tr("声道"), tr("削波样本"), tr("峰值 dBFS")});
        clip_table_->verticalHeader()->setVisible(false);
        clip_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        clip_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        clip_table_->horizontalHeader()->setStretchLastSection(true);
        clip_table_->setMinimumHeight(120);
        connect(clip_table_, &QTableWidget::cellClicked,
                this, &AudioQcPage::OnClipCellClicked);
        pl->addWidget(clip_table_);

        silence_table_ = new QTableWidget(0, 4, page);
        silence_table_->setHorizontalHeaderLabels(
            {tr("起始"), tr("结束"), tr("时长(s)"), tr("平均 RMS dBFS")});
        silence_table_->verticalHeader()->setVisible(false);
        silence_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        silence_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        silence_table_->horizontalHeader()->setStretchLastSection(true);
        silence_table_->setMinimumHeight(120);
        connect(silence_table_, &QTableWidget::cellClicked,
                this, &AudioQcPage::OnSilenceCellClicked);
        pl->addWidget(silence_table_);

        sub_tabs_->addTab(page, tr("静音与削波"));
    }

    // ---- 子页 2: 声道与相位 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        channel_chart_ = new MetricChartWidget(page);
        channel_chart_->SetTitle(tr("声道能量（RMS / 峰值 dBFS）"));
        channel_series_ = channel_chart_->AddBarSeries(tr("RMS dBFS"), QColor("#42a5f5"));
        channel_peak_series_ = channel_chart_->AddBarSeries(tr("峰值 dBFS"), QColor("#66bb6a"));
        channel_axis_x_ = channel_chart_->AxisX();
        channel_axis_y_ = channel_chart_->AxisY();
        channel_axis_y_->SetTitleText(tr("dBFS"));
        channel_chart_->setMinimumHeight(200);
        pl->addWidget(channel_chart_);

        corr_chart_ = new MetricChartWidget(page);
        corr_chart_->SetTitle(tr("声道相关性（最差声道对，1=同相 / -1=反相）"));
        corr_series_ = corr_chart_->AddLineSeries(tr("相关性"), QColor("#42a5f5"));
        corr_axis_x_ = corr_chart_->AxisX();
        corr_axis_y_ = corr_chart_->AxisY();
        corr_axis_x_->SetTitleText(tr("时间 (s)"));
        corr_axis_y_->SetRange(-1.05, 1.05);
        corr_chart_->setMinimumHeight(200);
        pl->addWidget(corr_chart_);

        metadata_table_ = new QTableWidget(0, 2, page);
        metadata_table_->setHorizontalHeaderLabels({tr("项目"), tr("值")});
        metadata_table_->verticalHeader()->setVisible(false);
        metadata_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        metadata_table_->horizontalHeader()->setStretchLastSection(true);
        metadata_table_->setMinimumHeight(150);
        pl->addWidget(metadata_table_);

        sub_tabs_->addTab(page, tr("声道与相位"));
    }

    // ---- 子页 3: 规则结果 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        QLabel* hint = new QLabel(
            tr("判定阈值取自「诊断与报告 → 规则与阈值」，改完会立即重算；"
               "这些音频问题也会一并进入诊断报告的评分。"), page);
        hint->setWordWrap(true);
        pl->addWidget(hint);

        verdict_table_ = new QTableWidget(0, 4, page);
        verdict_table_->setHorizontalHeaderLabels(
            {tr("规则"), tr("判定"), tr("实测值"), tr("说明")});
        verdict_table_->verticalHeader()->setVisible(false);
        verdict_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        verdict_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        verdict_table_->horizontalHeader()->setStretchLastSection(true);
        verdict_table_->setMinimumHeight(220);
        pl->addWidget(verdict_table_);

        sub_tabs_->addTab(page, tr("规则结果"));
    }

    layout->addWidget(sub_tabs_);
}

void AudioQcPage::ApplyOptionsFromUi() {
    options_.silence_threshold_dbfs = silence_spin_->value();
    options_.min_silence_seconds = min_silence_spin_->value();
    options_.clip_threshold = clip_spin_->value();
    options_.enable_loudness = loudness_check_->isChecked();
    options_.enable_true_peak = true_peak_check_->isChecked();
    options_.enable_correlation = correlation_check_->isChecked();
}

void AudioQcPage::OnStartAnalysis() {
    ApplyOptionsFromUi();
    emit ScanRequested();
}

void AudioQcPage::OnCancelAnalysis() { emit CancelRequested(); }

void AudioQcPage::OnOptionChanged() {
    if (!silence_spin_) return;
    ApplyOptionsFromUi();
    if (target_lufs_spin_ && has_result_) UpdateCharts();
}

void AudioQcPage::OnClipCellClicked(int row, int) {
    if (!has_result_ || row < 0) return;
    const auto& events = result_.audio_qc.clipping_events;
    if (row >= static_cast<int>(events.size())) return;
    emit SeekRequested(events[static_cast<size_t>(row)].start_seconds);
}

void AudioQcPage::OnSilenceCellClicked(int row, int) {
    if (!has_result_ || row < 0) return;
    const auto& ranges = result_.audio_qc.silence_ranges;
    if (row >= static_cast<int>(ranges.size())) return;
    emit SeekRequested(ranges[static_cast<size_t>(row)].start_seconds);
}

void AudioQcPage::Refresh() {
    UpdateSummary();
    UpdateCharts();
    RebuildClipTable();
    RebuildSilenceTable();
    RebuildVerdictTable();
    RebuildMetadataTable();
}

void AudioQcPage::UpdateSummary() {
    if (!summary_label_) return;
    const auto& qc = result_.audio_qc;
    if (!has_result_ || !qc.analyzed) {
        summary_label_->setText(
            tr("暂无音频 QC 结果。点击「开始分析」扫描当前文件（需要有音频流且能解码）。"));
        return;
    }

    const double target = target_lufs_spin_ ? target_lufs_spin_->value() : -23.0;
    const double deviation = qc.integrated_lufs - target;
    QString verdict;
    QColor verdict_color;
    if (std::abs(deviation) <= 1.0) {
        verdict = tr("响度达标");
        verdict_color = QColor("#43a047");
    } else if (std::abs(deviation) <= 3.0) {
        verdict = tr("响度偏离");
        verdict_color = QColor("#fb8c00");
    } else {
        verdict = tr("响度超标");
        verdict_color = QColor("#e53935");
    }

    QString text;
    text += QStringLiteral("<b>%1</b>: %2 ｜ ").arg(tr("布局"), QString::fromStdString(qc.metadata.channel_layout));
    text += QStringLiteral("%1 Hz ｜ %2 s ｜ ").arg(qc.metadata.sample_rate).arg(qc.duration_seconds, 0, 'f', 2);
    text += QStringLiteral("<font color='%1'><b>%2</b></font><br>").arg(verdict_color.name(), verdict);
    text += tr("Integrated %1 LUFS（目标 %2，偏差 %3 LU）｜ Short-term 最大 %4 ｜ Momentary 最大 %5 ｜ LRA %6 LU<br>")
                .arg(AudioFormatDb(qc.integrated_lufs, model::kSilenceLufs))
                .arg(target, 0, 'f', 1)
                .arg(deviation, 0, 'f', 2)
                .arg(AudioFormatDb(qc.short_term_max_lufs, model::kSilenceLufs))
                .arg(AudioFormatDb(qc.momentary_max_lufs, model::kSilenceLufs))
                .arg(qc.loudness_range_lu, 0, 'f', 1);
    text += tr("真峰值 %1 dBTP ｜ 采样峰值 %2 dBFS ｜ RMS %3 dBFS ｜ DC %4<br>")
                .arg(AudioFormatDb(qc.true_peak_dbtp, model::kSilenceLevelDb))
                .arg(AudioFormatDb(qc.sample_peak_dbfs, model::kSilenceLevelDb))
                .arg(AudioFormatDb(qc.rms_dbfs, model::kSilenceLevelDb))
                .arg(qc.max_dc_offset, 0, 'f', 5);
    text += tr("削波 %1 样本 / %2 段 ｜ 静音 %3 段（占比 %4%，最长 %5 s）")
                .arg(qc.clipping_sample_count)
                .arg(qc.clipping_event_count)
                .arg(qc.silence_ranges.size())
                .arg(qc.silence_ratio * 100.0, 0, 'f', 1)
                .arg(qc.longest_silence_seconds, 0, 'f', 2);
    if (qc.correlation_available) {
        text += tr(" ｜ 相关性 min %1 / mean %2")
                    .arg(qc.correlation_min, 0, 'f', 3)
                    .arg(qc.correlation_mean, 0, 'f', 3);
    }
    for (const auto& note : qc.notes) {
        text += QStringLiteral("<br><font color='#888888'>%1</font>")
                    .arg(QString::fromStdString(note).toHtmlEscaped());
    }
    if (!qc.metadata.inconsistencies.empty()) {
        text += QStringLiteral("<br><font color='#e53935'>%1: %2</font>")
                    .arg(tr("metadata 不一致"),
                         QString::fromStdString(qc.metadata.inconsistencies.front()).toHtmlEscaped());
    }
    summary_label_->setText(text);
}

void AudioQcPage::UpdateCharts() {
    if (!lufs_chart_) return;
    momentary_series_->Clear();
    short_term_series_->Clear();
    integrated_series_->Clear();
    target_series_->Clear();
    rms_series_->Clear();
    peak_series_->Clear();
    true_peak_series_->Clear();
    silence_series_->Clear();
    clip_series_->Clear();
    corr_series_->Clear();

    const auto& qc = result_.audio_qc;
    if (!has_result_ || !qc.analyzed) return;

    constexpr int kMaxPoints = 4000;
    const auto& points = qc.loudness_points;
    AppendAudioPoints(momentary_series_, points, kMaxPoints,
                      &model::LoudnessPoint::momentary_lufs, model::kSilenceLufs);
    AppendAudioPoints(short_term_series_, points, kMaxPoints,
                      &model::LoudnessPoint::short_term_lufs, model::kSilenceLufs);
    AppendAudioPoints(integrated_series_, points, kMaxPoints,
                      &model::LoudnessPoint::integrated_lufs, model::kSilenceLufs);
    AppendAudioPoints(rms_series_, points, kMaxPoints,
                      &model::LoudnessPoint::rms_dbfs, model::kSilenceLevelDb);
    AppendAudioPoints(peak_series_, points, kMaxPoints,
                      &model::LoudnessPoint::sample_peak_dbfs, model::kSilenceLevelDb);
    AppendAudioPoints(true_peak_series_, points, kMaxPoints,
                      &model::LoudnessPoint::true_peak_dbtp, model::kSilenceLevelDb);
    AppendAudioPoints(corr_series_, points, kMaxPoints,
                      &model::LoudnessPoint::correlation, -2.0);

    // 目标响度参考线
    const double target = target_lufs_spin_ ? target_lufs_spin_->value() : -23.0;
    if (!points.empty()) {
        target_series_->Append(points.front().timestamp_seconds, target);
        target_series_->Append(points.back().timestamp_seconds, target);
    }

    // 静音段画成方波；削波点画在 y=1.5
    {
        SeriesBatch silence(silence_series_);
        silence.Reserve(static_cast<int>(qc.silence_ranges.size() * 4));
        for (const auto& range : qc.silence_ranges) {
            silence.Add(range.start_seconds, 0.0);
            silence.Add(range.start_seconds, 1.0);
            silence.Add(range.end_seconds, 1.0);
            silence.Add(range.end_seconds, 0.0);
        }
    }
    {
        SeriesBatch clip(clip_series_);
        clip.Reserve(static_cast<int>(qc.clipping_events.size()));
        for (const auto& event : qc.clipping_events) {
            clip.Add(event.start_seconds, 1.5);
        }
    }

    const double span = std::max(1.0, qc.duration_seconds);
    lufs_axis_x_->SetRange(0.0, span);
    level_axis_x_->SetRange(0.0, span);
    event_axis_x_->SetRange(0.0, span);
    corr_axis_x_->SetRange(0.0, span);
    lufs_axis_y_->SetRange(-60.0, 0.0);
    level_axis_y_->SetRange(-90.0, 6.0);

    // 声道能量柱状图（每个声道两根柱: RMS / 峰值）
    channel_series_->Clear();
    channel_peak_series_->Clear();
    if (!qc.channels.empty()) {
        QStringList categories;
        for (const auto& ch : qc.channels) {
            const QString name = ch.name.empty() ? QString("Ch%1").arg(ch.index + 1)
                                                 : QString::fromStdString(ch.name);
            categories << name;
            const double rms = (ch.rms_dbfs <= model::kSilenceLevelDb + 1.0) ? -120.0 : ch.rms_dbfs;
            const double peak = (ch.peak_dbfs <= model::kSilenceLevelDb + 1.0) ? -120.0 : ch.peak_dbfs;
            channel_series_->Append(static_cast<double>(categories.size() - 1), rms);
            channel_peak_series_->Append(static_cast<double>(categories.size() - 1), peak);
        }
        channel_chart_->SetCategories(categories);
        channel_axis_y_->SetRange(-90.0, 6.0);
    }
}

void AudioQcPage::RebuildClipTable() {
    if (!clip_table_) return;
    clip_table_->setRowCount(0);
    if (!has_result_) return;
    const auto& qc = result_.audio_qc;
    const int rows = std::min<int>(static_cast<int>(qc.clipping_events.size()), 2000);
    clip_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& e = qc.clipping_events[static_cast<size_t>(i)];
        SetTableItemText(clip_table_, i, 0, QString::number(e.start_seconds, 'f', 3));
        SetTableItemText(clip_table_, i, 1, QString::number(e.end_seconds, 'f', 3));
        QString channel = (e.channel < 0) ? tr("全部")
                                          : QString::fromStdString(
                                                e.channel < static_cast<int>(qc.channels.size())
                                                    ? qc.channels[static_cast<size_t>(e.channel)].name
                                                    : std::string());
        SetTableItemText(clip_table_, i, 2, channel);
        SetTableItemText(clip_table_, i, 3, QString::number(e.sample_count));
        SetTableItemText(clip_table_, i, 4, QString::number(
            e.peak > 0.0 ? 20.0 * std::log10(e.peak) : model::kSilenceLevelDb, 'f', 2));
    }
}

void AudioQcPage::RebuildSilenceTable() {
    if (!silence_table_) return;
    silence_table_->setRowCount(0);
    if (!has_result_) return;
    const auto& qc = result_.audio_qc;
    const int rows = std::min<int>(static_cast<int>(qc.silence_ranges.size()), 2000);
    silence_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& r = qc.silence_ranges[static_cast<size_t>(i)];
        SetTableItemText(silence_table_, i, 0, QString::number(r.start_seconds, 'f', 3));
        SetTableItemText(silence_table_, i, 1, QString::number(r.end_seconds, 'f', 3));
        SetTableItemText(silence_table_, i, 2, QString::number(r.duration_seconds, 'f', 3));
        SetTableItemText(silence_table_, i, 3, QString::number(r.rms_dbfs, 'f', 1));
    }
}

void AudioQcPage::RebuildVerdictTable() {
    if (!verdict_table_) return;
    verdict_table_->setRowCount(0);
    if (!has_result_) return;

    const size_t count = sizeof(kAudioQcRuleIds) / sizeof(kAudioQcRuleIds[0]);
    verdict_table_->setRowCount(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i) {
        const QString id = QString::fromUtf8(kAudioQcRuleIds[i]);
        QString name = id;
        QString threshold_text;
        for (const auto& rule : qc_report_.rules) {
            if (rule.id == id.toStdString()) {
                name = QString::fromStdString(rule.name);
                threshold_text = QString::fromStdString(
                    rule.unit.empty() ? QString::number(rule.threshold, 'f', 2).toStdString()
                                      : (QString::number(rule.threshold, 'f', 2) +
                                         QString::fromStdString(rule.unit)).toStdString());
                break;
            }
        }
        QString detail;
        const QString verdict = AudioVerdictText(qc_report_, id, &detail);
        SetTableItemText(verdict_table_, static_cast<int>(i), 0, name);
        SetTableItemText(verdict_table_, static_cast<int>(i), 1, verdict);
        SetTableItemText(verdict_table_, static_cast<int>(i), 2, threshold_text);
        SetTableItemText(verdict_table_, static_cast<int>(i), 3, detail);
        QTableWidgetItem* item = verdict_table_->item(static_cast<int>(i), 1);
        if (item != nullptr) {
            item->setForeground(verdict == tr("失败")   ? QColor("#e53935")
                                : verdict == tr("警告") ? QColor("#fb8c00")
                                : verdict == tr("提示") ? QColor("#1e88e5")
                                                        : QColor("#43a047"));
        }
    }
}

void AudioQcPage::RebuildMetadataTable() {
    if (!metadata_table_) return;
    metadata_table_->setRowCount(0);
    if (!has_result_) return;
    const auto& meta = result_.audio_qc.metadata;

    auto add_row = [this](const QString& key, const QString& value) {
        const int row = metadata_table_->rowCount();
        metadata_table_->insertRow(row);
        SetTableItemText(metadata_table_, row, 0, key);
        SetTableItemText(metadata_table_, row, 1, value);
    };

    add_row(tr("声道布局"), QString::fromStdString(meta.channel_layout));
    add_row(tr("声道数"), QString::number(meta.channels));
    add_row(tr("采样率"), QString::number(meta.sample_rate) + tr(" Hz"));
    add_row(tr("采样格式"), meta.sample_format.empty() ? tr("未知")
                                                       : QString::fromStdString(meta.sample_format));
    add_row(tr("位深"), meta.bits_per_sample > 0 ? QString::number(meta.bits_per_sample) + tr(" bit")
                                                 : tr("未知"));
    add_row(tr("音频时长"), QString::number(meta.stream_duration_seconds, 'f', 3) + tr(" s"));
    add_row(tr("容器时长"), QString::number(meta.container_duration_seconds, 'f', 3) + tr(" s"));
    add_row(tr("视频时长"), meta.has_video
                                ? QString::number(meta.video_duration_seconds, 'f', 3) + tr(" s")
                                : tr("无视频流"));
    add_row(tr("与容器时差"), QString::number(meta.container_delta_seconds, 'f', 3) + tr(" s"));
    if (meta.has_video) {
        add_row(tr("与视频时差"), QString::number(meta.video_delta_seconds, 'f', 3) + tr(" s"));
    }
    add_row(tr("一致性问题"), meta.inconsistencies.empty()
                                 ? tr("无")
                                 : QString::fromStdString(meta.inconsistencies.front()));
}

void AudioQcPage::OnExportCsv() {
    if (!has_result_ || !result_.audio_qc.analyzed) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次音频 QC 分析。"));
        return;
    }
    ExportCsvStream(this, tr("导出响度曲线 CSV"),
        source_path_ + QStringLiteral("_audioqc.csv"),
        [this](QTextStream& out) {
            out << "time_s,momentary_lufs,short_term_lufs,integrated_lufs,rms_dbfs,"
                   "sample_peak_dbfs,true_peak_dbtp,correlation,silent\n";
            for (const auto& p : result_.audio_qc.loudness_points) {
                out << QString::number(p.timestamp_seconds, 'f', 3) << ','
                    << QString::number(p.momentary_lufs, 'f', 2) << ','
                    << QString::number(p.short_term_lufs, 'f', 2) << ','
                    << QString::number(p.integrated_lufs, 'f', 2) << ','
                    << QString::number(p.rms_dbfs, 'f', 2) << ','
                    << QString::number(p.sample_peak_dbfs, 'f', 2) << ','
                    << QString::number(p.true_peak_dbtp, 'f', 2) << ','
                    << QString::number(p.correlation, 'f', 4) << ','
                    << (p.silent ? 1 : 0) << '\n';
            }
        },
        static_cast<int>(result_.audio_qc.loudness_points.size()));
}

void AudioQcPage::SetResult(const model::AnalysisResult& result,
                            const model::QcReport& qc_report) {
    result_ = result;
    qc_report_ = qc_report;
    has_result_ = true;
    Refresh();
}

void AudioQcPage::SetQcReport(const model::QcReport& qc_report) {
    qc_report_ = qc_report;
    Refresh();
}

void AudioQcPage::FillScanOptions(videoeye::AnalysisOptions& options) {
    ApplyOptionsFromUi();
    options.audio_qc_options = options_;
    options.analyze_audio_qc = true;
}

void AudioQcPage::SetScanActive(bool active) {
    scan_active_ = active;
    if (start_button_) start_button_->setEnabled(!active);
    if (cancel_button_) cancel_button_->setEnabled(active);
}

void AudioQcPage::SetProgress(int percent) {
    if (progress_bar_) progress_bar_->setValue(percent);
}

void AudioQcPage::SetProgressFormat(const QString& format) {
    if (progress_bar_) progress_bar_->setFormat(format);
}

} // namespace ui
} // namespace videoeye
