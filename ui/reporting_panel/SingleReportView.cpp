#include "ui/reporting_panel/SingleReportView.h"

#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "core/domain/model/QcReport.h"

namespace videoeye {
namespace ui {

SingleReportView::SingleReportView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void SingleReportView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    QGroupBox* single_box = new QGroupBox(tr("单文件报告"), this);
    QVBoxLayout* single_layout = new QVBoxLayout(single_box);

    QHBoxLayout* file_row = new QHBoxLayout();
    file_row->addWidget(new QLabel(tr("当前文件:"), single_box));
    current_file_label_ = new QLabel(tr("未选择"), single_box);
    current_file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file_row->addWidget(current_file_label_, 1);

    analyze_button_ = new QPushButton(tr("按模板重新分析"), single_box);
    connect(analyze_button_, &QPushButton::clicked, this, &SingleReportView::AnalyzeRequested);
    file_row->addWidget(analyze_button_);

    export_button_ = new QPushButton(tr("导出报告…"), single_box);
    connect(export_button_, &QPushButton::clicked, this, &SingleReportView::ExportRequested);
    file_row->addWidget(export_button_);
    single_layout->addLayout(file_row);

    QHBoxLayout* format_row = new QHBoxLayout();
    format_row->addWidget(new QLabel(tr("导出格式:"), single_box));
    format_json_ = new QCheckBox(tr("JSON"), single_box);
    format_csv_ = new QCheckBox(tr("CSV"), single_box);
    format_html_ = new QCheckBox(tr("HTML"), single_box);
    format_txt_ = new QCheckBox(tr("TXT"), single_box);
    format_pdf_ = new QCheckBox(tr("PDF"), single_box);
    format_json_->setChecked(true);
    for (auto* check : {format_json_, format_csv_, format_html_, format_txt_, format_pdf_}) {
        format_row->addWidget(check);
    }
    format_pdf_->setToolTip(tr("PDF 不嵌入中文字体，中文会显示成 '?'，需要完整中文请用 HTML"));
    format_row->addStretch();
    single_layout->addLayout(format_row);

    QHBoxLayout* verdict_row = new QHBoxLayout();
    verdict_label_ = new QLabel(tr("尚无结果"), single_box);
    QFont verdict_font = verdict_label_->font();
    verdict_font.setPointSize(verdict_font.pointSize() + 6);
    verdict_font.setBold(true);
    verdict_label_->setFont(verdict_font);
    verdict_row->addWidget(verdict_label_);
    issue_count_label_ = new QLabel(QString(), single_box);
    verdict_row->addWidget(issue_count_label_);
    verdict_row->addStretch();
    single_layout->addLayout(verdict_row);

    layout->addWidget(single_box);
}

void SingleReportView::SetCurrentFile(const QString& path) {
    if (current_file_label_)
        current_file_label_->setText(path.isEmpty() ? tr("未选择") : path);
}

void SingleReportView::ShowResult(const qc::QcRunResult& result) {
    if (!result.ok) {
        verdict_label_->setText(tr("分析未完成"));
        verdict_label_->setStyleSheet("color: #b3261e;");
        issue_count_label_->setText(QString::fromStdString(result.error));
        return;
    }
    verdict_label_->setText(QString::number(result.report.score, 'f', 1) + " / 100  " +
                            QString::fromStdString(result.report.verdict));
    const QString color = (result.report.score >= 90.0)   ? "#1b7f3b"
                          : (result.report.score >= 70.0) ? "#b26a00"
                                                          : "#b3261e";
    verdict_label_->setStyleSheet("color: " + color + ";");
    issue_count_label_->setText(tr("致命 %1 ｜ 错误 %2 ｜ 警告 %3 ｜ 提示 %4")
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Critical))
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Error))
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Warning))
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Info)));
}

void SingleReportView::ShowAnalyzing() {
    verdict_label_->setText(tr("分析中…"));
    issue_count_label_->setText(QString());
}

void SingleReportView::ShowFailure(const QString& text) {
    verdict_label_->setText(text);
}

void SingleReportView::ShowInvalidated(bool has_file) {
    verdict_label_->setText(has_file ? tr("结果已过期，请重新分析") : tr("尚无结果"));
    verdict_label_->setStyleSheet(QString());
    issue_count_label_->setText(QString());
}

void SingleReportView::SetBusy(bool busy) {
    if (analyze_button_)
        analyze_button_->setEnabled(!busy);
}

void SingleReportView::SetExportEnabled(bool enabled) {
    if (export_button_)
        export_button_->setEnabled(enabled);
}

bool SingleReportView::IsExportEnabled() const {
    return export_button_ && export_button_->isEnabled();
}

std::vector<qc::QcReportFormat> SingleReportView::SelectedFormats() const {
    std::vector<qc::QcReportFormat> formats;
    if (format_json_->isChecked())
        formats.push_back(qc::QcReportFormat::Json);
    if (format_csv_->isChecked())
        formats.push_back(qc::QcReportFormat::Csv);
    if (format_html_->isChecked())
        formats.push_back(qc::QcReportFormat::Html);
    if (format_txt_->isChecked())
        formats.push_back(qc::QcReportFormat::Text);
    if (format_pdf_->isChecked())
        formats.push_back(qc::QcReportFormat::Pdf);
    if (formats.empty())
        formats.push_back(qc::QcReportFormat::Json); // 一个都不勾退回 JSON
    return formats;
}

} // namespace ui
} // namespace videoeye
