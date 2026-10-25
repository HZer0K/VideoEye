#include "ui/analysis_panel/AudioQcPage.h"

#include <QColor>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include <utility>

#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/analysis_panel/AudioChannelView.h"
#include "ui/analysis_panel/AudioEventView.h"
#include "ui/analysis_panel/AudioLoudnessView.h"
#include "ui/analysis_panel/AudioTrackView.h"
#include "ui/analysis_panel/AudioVerdictView.h"

#include "core/domain/model/AudioQcResult.h"
#include "core/domain/model/LoudnessPoint.h"
#include "core/domain/model/QcReport.h"

namespace videoeye {
namespace ui {
namespace {

QString AudioFormatDb(double value, double silence_floor) {
    if (value <= silence_floor + 1.0)
        return QStringLiteral("-∞");
    return QString::number(value, 'f', 2);
}

} // namespace

AudioQcPage::AudioQcPage(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

// ===========================================================================
// 音频 QC 页（响度 / 真峰值 / 削波 / 静音 / 声道相位 / metadata 一致性）
// ===========================================================================

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
        connect(start_button_, &QPushButton::clicked, this, &AudioQcPage::OnStartAnalysis);
        rl->addWidget(start_button_);

        cancel_button_ = new QPushButton(tr("取消"), row);
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked, this, &AudioQcPage::OnCancelAnalysis);
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
        connect(target_lufs_spin_, &QDoubleSpinBox::valueChanged, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(target_lufs_spin_);

        rl->addWidget(new QLabel(tr("静音阈值"), row));
        silence_spin_ = new QDoubleSpinBox(row);
        silence_spin_->setRange(-120.0, 0.0);
        silence_spin_->setDecimals(0);
        silence_spin_->setSingleStep(5.0);
        silence_spin_->setValue(options_.silence_threshold_dbfs);
        silence_spin_->setSuffix(tr(" dBFS"));
        connect(silence_spin_, &QDoubleSpinBox::valueChanged, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(silence_spin_);

        rl->addWidget(new QLabel(tr("最短静音"), row));
        min_silence_spin_ = new QDoubleSpinBox(row);
        min_silence_spin_->setRange(0.0, 60.0);
        min_silence_spin_->setDecimals(2);
        min_silence_spin_->setSingleStep(0.1);
        min_silence_spin_->setValue(options_.min_silence_seconds);
        min_silence_spin_->setSuffix(tr(" s"));
        connect(min_silence_spin_, &QDoubleSpinBox::valueChanged, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(min_silence_spin_);

        rl->addWidget(new QLabel(tr("削波阈值"), row));
        clip_spin_ = new QDoubleSpinBox(row);
        clip_spin_->setRange(0.5, 1.0);
        clip_spin_->setDecimals(4);
        clip_spin_->setSingleStep(0.0005);
        clip_spin_->setValue(options_.clip_threshold);
        connect(clip_spin_, &QDoubleSpinBox::valueChanged, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(clip_spin_);

        loudness_check_ = new QCheckBox(tr("响度(BS.1770)"), row);
        loudness_check_->setChecked(options_.enable_loudness);
        connect(loudness_check_, &QCheckBox::toggled, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(loudness_check_);

        true_peak_check_ = new QCheckBox(tr("真峰值(4×)"), row);
        true_peak_check_->setChecked(options_.enable_true_peak);
        true_peak_check_->setToolTip(tr("4× 过采样检测，最耗时的一项；关闭后 dBTP 回落为采样峰值"));
        connect(true_peak_check_, &QCheckBox::toggled, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(true_peak_check_);

        correlation_check_ = new QCheckBox(tr("声道相关性"), row);
        correlation_check_->setChecked(options_.enable_correlation);
        connect(correlation_check_, &QCheckBox::toggled, this, &AudioQcPage::OnOptionChanged);
        rl->addWidget(correlation_check_);

        rl->addStretch();
        layout->addWidget(row);
    }

    summary_label_ =
        new QLabel(tr("点击「开始分析」对音频流做一次完整解码体检：Integrated / Short-term / Momentary LUFS、"
                      "LRA、真峰值、削波、静音段、声道相位与 metadata 一致性。"),
                   this);
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setMinimumHeight(340);

    loudness_view_ = new AudioLoudnessView(sub_tabs_);
    sub_tabs_->addTab(loudness_view_, tr("响度与电平"));

    event_view_ = new AudioEventView(sub_tabs_);
    connect(event_view_, &AudioEventView::SeekRequested, this, &AudioQcPage::SeekRequested);
    sub_tabs_->addTab(event_view_, tr("静音与削波"));

    channel_view_ = new AudioChannelView(sub_tabs_);
    sub_tabs_->addTab(channel_view_, tr("声道与相位"));

    verdict_view_ = new AudioVerdictView(sub_tabs_);
    sub_tabs_->addTab(verdict_view_, tr("规则结果"));

    track_view_ = new AudioTrackView(sub_tabs_);
    sub_tabs_->addTab(track_view_, tr("音轨对照"));

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

void AudioQcPage::OnCancelAnalysis() {
    emit CancelRequested();
}

void AudioQcPage::OnOptionChanged() {
    if (!silence_spin_)
        return;
    ApplyOptionsFromUi();
    if (target_lufs_spin_ && has_result_)
        loudness_view_->SetResult(result_.get(), target_lufs_spin_->value());
}

void AudioQcPage::Refresh() {
    UpdateSummary();
    loudness_view_->SetResult(has_result_ ? result_.get() : nullptr,
                              target_lufs_spin_ ? target_lufs_spin_->value() : -23.0);
    event_view_->SetResult(has_result_ ? result_.get() : nullptr);
    channel_view_->SetResult(has_result_ ? result_.get() : nullptr);
    if (has_result_)
        verdict_view_->SetQcReport(qc_report_);
    track_view_->SetResult(has_result_ ? result_.get() : nullptr);
}

void AudioQcPage::UpdateSummary() {
    if (!summary_label_)
        return;
    const auto& qc = result_->audio_qc;
    if (!has_result_ || !qc.analyzed) {
        summary_label_->setText(tr("暂无音频 QC 结果。点击「开始分析」扫描当前文件（需要有音频流且能解码）。"));
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
        text +=
            tr(" ｜ 相关性 min %1 / mean %2").arg(qc.correlation_min, 0, 'f', 3).arg(qc.correlation_mean, 0, 'f', 3);
    }
    for (const auto& note : qc.notes) {
        text += QStringLiteral("<br><font color='#888888'>%1</font>").arg(QString::fromStdString(note).toHtmlEscaped());
    }
    if (!qc.metadata.inconsistencies.empty()) {
        text += QStringLiteral("<br><font color='#e53935'>%1: %2</font>")
                    .arg(tr("metadata 不一致"),
                         QString::fromStdString(qc.metadata.inconsistencies.front()).toHtmlEscaped());
    }
    summary_label_->setText(text);
}

void AudioQcPage::OnExportCsv() {
    if (!has_result_ || !result_->audio_qc.analyzed) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次音频 QC 分析。"));
        return;
    }
    ExportCsvStream(
        this, tr("导出响度曲线 CSV"), source_path_ + QStringLiteral("_audioqc.csv"),
        [this](QTextStream& out) {
            out << "time_s,momentary_lufs,short_term_lufs,integrated_lufs,rms_dbfs,"
                   "sample_peak_dbfs,true_peak_dbtp,correlation,silent\n";
            for (const auto& p : result_->audio_qc.loudness_points) {
                out << QString::number(p.timestamp_seconds, 'f', 3) << ',' << QString::number(p.momentary_lufs, 'f', 2)
                    << ',' << QString::number(p.short_term_lufs, 'f', 2) << ','
                    << QString::number(p.integrated_lufs, 'f', 2) << ',' << QString::number(p.rms_dbfs, 'f', 2) << ','
                    << QString::number(p.sample_peak_dbfs, 'f', 2) << ',' << QString::number(p.true_peak_dbtp, 'f', 2)
                    << ',' << QString::number(p.correlation, 'f', 4) << ',' << (p.silent ? 1 : 0) << '\n';
            }
        },
        static_cast<int>(result_->audio_qc.loudness_points.size()));
}

void AudioQcPage::SetResult(std::shared_ptr<const model::AnalysisResult> result, const model::QcReport& qc_report) {
    result_ = std::move(result);
    qc_report_ = qc_report;
    has_result_ = true;
    Refresh();
}

void AudioQcPage::SetQcReport(const model::QcReport& qc_report) {
    qc_report_ = qc_report;
    Refresh();
}

int AudioQcPage::VerdictRuleCount() {
    return AudioVerdictView::RuleCount();
}

void AudioQcPage::FillScanOptions(videoeye::AnalysisOptions& options) {
    ApplyOptionsFromUi();
    options.audio_qc_options = options_;
    options.analyze_audio_qc = true;
    // 位流参数与解码无关，用户在这页点"开始分析"时一并发起，避免两套入口。
    options.analyze_audio_bitstream = true;
}

int AudioQcPage::IssueCount() const {
    return event_view_ ? event_view_->ClipRows() + event_view_->SilenceRows() : 0;
}

void AudioQcPage::SetScanActive(bool active) {
    scan_active_ = active;
    if (start_button_)
        start_button_->setEnabled(!active);
    if (cancel_button_)
        cancel_button_->setEnabled(active);
}

void AudioQcPage::SetProgress(int percent) {
    if (progress_bar_)
        progress_bar_->setValue(percent);
}

void AudioQcPage::SetProgressFormat(const QString& format) {
    if (progress_bar_)
        progress_bar_->setFormat(format);
}

} // namespace ui
} // namespace videoeye
