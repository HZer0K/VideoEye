#include "ui/analysis_panel/ColorHdrPage.h"

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

#include "core/domain/model/ColorHdrResult.h"
#include "core/domain/model/ColorInfo.h"
#include "core/domain/model/HdrMetadataInfo.h"
#include "core/domain/model/QcReport.h"

#include <QColor>

namespace videoeye {
namespace ui {

ColorHdrPage::ColorHdrPage(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void ColorHdrPage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 第一行: 标题 + 开始/取消/导出
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("色彩与 HDR 元数据"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        start_button_ = new QPushButton(tr("开始分析"), row);
        start_button_->setToolTip(
            tr("读取视频流的 primaries / transfer / matrix / range / bit depth / 色度采样，"
               "并汇总 HDR10 静态元数据（母版显示、MaxCLL/MaxFALL）与 Dolby Vision 配置记录。"
               "与「码率与 GOP」「音频 QC」「诊断与报告」共用同一次扫描。"));
        connect(start_button_, &QPushButton::clicked,
                this, &ColorHdrPage::OnStartAnalysis);
        rl->addWidget(start_button_);

        cancel_button_ = new QPushButton(tr("取消"), row);
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked,
                this, &ColorHdrPage::OnCancelAnalysis);
        rl->addWidget(cancel_button_);

        QPushButton* export_btn = new QPushButton(tr("导出 CSV"), row);
        connect(export_btn, &QPushButton::clicked, this, &ColorHdrPage::OnExportCsv);
        rl->addWidget(export_btn);
        layout->addWidget(row);
    }

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(progress_bar_);

    // 第二行: 选项
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        probe_frame_check_ = new QCheckBox(tr("解码首帧读取动态元数据"), row);
        probe_frame_check_->setChecked(options_.probe_decoded_frame);
        probe_frame_check_->setToolTip(
            tr("容器/码流层没有 HDR 静态元数据时，解码首帧读取 AVFrame side data "
               "（HDR10+ / DV RPU / HDR Vivid）。关闭后只依赖容器标注，速度最快。"));
        connect(probe_frame_check_, &QCheckBox::toggled,
                this, &ColorHdrPage::OnOptionChanged);
        rl->addWidget(probe_frame_check_);
        rl->addStretch();
        layout->addWidget(row);
    }

    summary_label_ = new QLabel(
        tr("点击「开始分析」读取当前视频流的色彩与 HDR 元数据。"
           "正确的组合应为：BT.709 + BT.709 + BT.709（SDR）或 BT.2020 + PQ/HLG + BT.2020 NCL（HDR）。"),
        this);
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setMinimumHeight(340);

    auto make_info_table = [](QWidget* parent) {
        QTableWidget* table = new QTableWidget(0, 3, parent);
        table->setHorizontalHeaderLabels({tr("项目"), tr("值"), tr("说明")});
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setMinimumHeight(260);
        return table;
    };

    // ---- 子页 0: 色彩信息 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        color_info_table_ = make_info_table(page);
        pl->addWidget(color_info_table_);
        sub_tabs_->addTab(page, tr("色彩信息"));
    }

    // ---- 子页 1: HDR 元数据 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        hdr_info_table_ = make_info_table(page);
        pl->addWidget(hdr_info_table_);
        sub_tabs_->addTab(page, tr("HDR 元数据"));
    }

    // ---- 子页 2: 异常组合 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        QLabel* hint = new QLabel(
            tr("下面列出的是由色彩/HDR 规则判定的异常组合，判定阈值取自"
               "「诊断与报告 → 规则与阈值」，这些项目同样计入诊断报告评分。"), page);
        hint->setWordWrap(true);
        pl->addWidget(hint);

        issue_table_ = new QTableWidget(0, 4, page);
        issue_table_->setHorizontalHeaderLabels(
            {tr("严重度"), tr("规则"), tr("说明"), tr("建议")});
        issue_table_->verticalHeader()->setVisible(false);
        issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        issue_table_->horizontalHeader()->setStretchLastSection(true);
        issue_table_->setMinimumHeight(240);
        pl->addWidget(issue_table_);

        sub_tabs_->addTab(page, tr("异常组合"));
    }

    layout->addWidget(sub_tabs_);
}

void ColorHdrPage::ApplyOptionsFromUi() {
    options_.probe_decoded_frame = probe_frame_check_ &&
                                             probe_frame_check_->isChecked();
}

void ColorHdrPage::OnStartAnalysis() {
    ApplyOptionsFromUi();
    emit ScanRequested();
}

void ColorHdrPage::OnCancelAnalysis() { emit CancelRequested(); }

void ColorHdrPage::OnOptionChanged() {
    if (!probe_frame_check_) return;
    ApplyOptionsFromUi();
}

void ColorHdrPage::Refresh() {
    UpdateSummary();
    RebuildTables();
    RebuildIssueTable();
}

void ColorHdrPage::UpdateSummary() {
    if (!summary_label_) return;
    const auto& analysis = result_.color_hdr;
    if (!has_result_ || !analysis.analyzed) {
        summary_label_->setText(
            tr("暂无色彩/HDR 结果。点击「开始分析」扫描当前文件（需要有视频流）。"));
        return;
    }

    const model::ColorInfo& color = analysis.color;
    const model::HdrMetadataInfo& hdr = analysis.hdr;

    auto piece = [](const std::string& text) {
        return text.empty() ? QObject::tr("未标注") : QString::fromStdString(text);
    };

    QColor verdict_color = QColor("#43a047");   // 绿
    QString verdict = tr("色彩标注完整");
    int missing_count = 0;
    if (!color.PrimariesSpecified()) ++missing_count;
    if (!color.TransferSpecified()) ++missing_count;
    if (!color.MatrixSpecified()) ++missing_count;
    if (!color.RangeSpecified()) ++missing_count;
    if (missing_count > 0 || (hdr.hdr && !hdr.mastering_display.Complete() &&
                              color.transfer == model::TransferKind::Pq)) {
        verdict_color = QColor("#fb8c00");      // 橙
        verdict = tr("存在待核查项");
    }

    QString text;
    text += QStringLiteral("<b>%1</b>: %2 ｜ ").arg(tr("HDR 格式"), piece(hdr.format_name));
    text += QStringLiteral("%1 ｜ %2 ｜ %3 ｜ %4<br>")
                .arg(piece(color.primaries_name), piece(color.transfer_name),
                     piece(color.matrix_name), piece(color.range_name));
    text += QStringLiteral("%1: %2 ｜ %3 ｜ ")
                .arg(tr("像素格式"), piece(color.pixel_format.name),
                     color.EffectiveBitDepth() > 0
                         ? (QString::number(color.EffectiveBitDepth()) + QLatin1String(" bit"))
                         : tr("未标注"));
    text += QStringLiteral("%1<br>").arg(
        color.pixel_format.chroma_subsampling.empty()
            ? QString::fromStdString(color.codec_name + " / " + color.profile_name)
            : piece(color.pixel_format.chroma_subsampling));
    text += QStringLiteral("<font color='%1'><b>%2</b></font>")
                .arg(verdict_color.name(), verdict);

    if (hdr.mastering_display.has_luminance) {
        text += QStringLiteral(" ｜ MaxCLL %1 / MaxFALL %2 cd/m²")
                    .arg(hdr.content_light.max_cll)
                    .arg(hdr.content_light.max_fall);
    }
    if (hdr.dolby_vision.present) {
        text += QStringLiteral(" ｜ DV %1").arg(
            QString::fromStdString(hdr.dolby_vision.ProfileText()));
    }
    for (const auto& note : analysis.notes) {
        text += QStringLiteral("<br><font color='#e53935'>%1</font>")
                    .arg(QString::fromStdString(note).toHtmlEscaped());
    }
    summary_label_->setText(text);
}

void ColorHdrPage::RebuildTables() {
    if (!color_info_table_ || !hdr_info_table_) return;

    const auto fill = [this](QTableWidget* table,
                             const std::vector<model::ColorKeyValueRow>& rows) {
        table->setRowCount(0);
        table->setRowCount(static_cast<int>(rows.size()));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& row = rows[static_cast<size_t>(i)];
            table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(row.key)));
            QTableWidgetItem* value_item = new QTableWidgetItem(QString::fromStdString(row.value));
            // 缺失/异常项用醒目颜色标记
            const QString lower = QString::fromStdString(row.value).toLower();
            if (row.value == "缺失" || row.value == "未标注" || lower.startsWith("unknown")) {
                value_item->setForeground(QColor("#fb8c00"));
                value_item->setFont([value_item] {
                    QFont f = value_item->font();
                    f.setBold(true);
                    return f;
                }());
            }
            table->setItem(i, 1, value_item);
            table->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(row.note)));
        }
        table->resizeColumnsToContents();
    };

    const auto& analysis = result_.color_hdr;
    if (!has_result_ || !analysis.analyzed) {
        color_info_table_->setRowCount(0);
        hdr_info_table_->setRowCount(0);
        return;
    }
    fill(color_info_table_, model::BuildColorRows(analysis));
    fill(hdr_info_table_, model::BuildHdrRows(analysis));
}

void ColorHdrPage::RebuildIssueTable() {
    if (!issue_table_) return;
    issue_table_->setRowCount(0);
    if (!has_result_) return;

    auto severity_color = [](model::IssueSeverity severity) -> QColor {
        switch (severity) {
            case model::IssueSeverity::Critical: return QColor("#c62828");
            case model::IssueSeverity::Error:    return QColor("#e53935");
            case model::IssueSeverity::Warning:  return QColor("#fb8c00");
            case model::IssueSeverity::Info:     return QColor("#1e88e5");
        }
        return QColor("#888888");
    };

    int row = 0;
    for (const auto& issue : qc_report_.issues) {
        if (issue.category != model::IssueCategory::ColorHdr) continue;
        issue_table_->insertRow(row);
        QTableWidgetItem* severity_item =
            new QTableWidgetItem(QString::fromStdString(issue.SeverityText()));
        severity_item->setForeground(severity_color(issue.severity));
        {
            QFont f = severity_item->font();
            f.setBold(true);
            severity_item->setFont(f);
        }
        issue_table_->setItem(row, 0, severity_item);
        issue_table_->setItem(row, 1, new QTableWidgetItem(
                                                QString::fromStdString(issue.rule_id)));
        issue_table_->setItem(row, 2, new QTableWidgetItem(
                                                QString::fromStdString(issue.detail)));
        issue_table_->setItem(row, 3, new QTableWidgetItem(
                                                QString::fromStdString(issue.suggestion)));
        ++row;
    }
    if (row == 0) {
        issue_table_->insertRow(0);
        issue_table_->setItem(0, 0, new QTableWidgetItem(tr("无")));
        issue_table_->setItem(0, 1,
                                    new QTableWidgetItem(tr("未发现异常的色彩/HDR 组合")));
        issue_table_->setItem(0, 2, new QTableWidgetItem(tr("")));
        issue_table_->setItem(0, 3, new QTableWidgetItem(tr("")));
    }
    issue_table_->resizeColumnsToContents();
}

void ColorHdrPage::OnExportCsv() {
    if (!has_result_ || !result_.color_hdr.analyzed) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次色彩/HDR 分析。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出色彩与 HDR 信息 CSV"),
        source_path_ + QStringLiteral("_colorhdr.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件: %1").arg(path));
        return;
    }

    auto csv_field = [](const QString& text) {
        QString out = text;
        out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + out + QLatin1Char('"');
    };

    const auto& analysis = result_.color_hdr;
    QTextStream out(&file);
    out << "\xEF\xBB\xBF";  // UTF-8 BOM for Excel
    out << "分组,项目,值,说明\n";
    auto write_rows = [&](const char* /*unused*/, const QString& group,
                          const std::vector<model::ColorKeyValueRow>& rows) {
        for (const auto& row : rows) {
            out << csv_field(group) << ','
                << csv_field(QString::fromStdString(row.key)) << ','
                << csv_field(QString::fromStdString(row.value)) << ','
                << csv_field(QString::fromStdString(row.note)) << '\n';
        }
    };
    write_rows(nullptr, tr("色彩信息"), model::BuildColorRows(analysis));
    write_rows(nullptr, tr("HDR 元数据"), model::BuildHdrRows(analysis));
    file.close();

    const int rows = static_cast<int>(model::BuildColorRows(analysis).size() +
                                      model::BuildHdrRows(analysis).size());
    QMessageBox::information(this, tr("导出完成"), tr("已导出 %1 行。").arg(rows));
}

void ColorHdrPage::SetResult(const model::AnalysisResult& result,
                             const model::QcReport& qc_report) {
    result_ = result;
    qc_report_ = qc_report;
    has_result_ = true;
    Refresh();
}

void ColorHdrPage::SetQcReport(const model::QcReport& qc_report) {
    qc_report_ = qc_report;
    Refresh();
}

void ColorHdrPage::FillScanOptions(analyzer::AnalysisOptions& options) {
    ApplyOptionsFromUi();
    options.color_hdr_options = options_;
    options.analyze_color_hdr = true;
}

void ColorHdrPage::SetScanActive(bool active) {
    scan_active_ = active;
    if (start_button_) start_button_->setEnabled(!active);
    if (cancel_button_) cancel_button_->setEnabled(active);
}

void ColorHdrPage::SetProgress(int percent) {
    if (progress_bar_) progress_bar_->setValue(percent);
}

void ColorHdrPage::SetProgressFormat(const QString& format) {
    if (progress_bar_) progress_bar_->setFormat(format);
}

} // namespace ui
} // namespace videoeye
