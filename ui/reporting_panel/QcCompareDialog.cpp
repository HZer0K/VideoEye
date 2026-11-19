#include "ui/reporting_panel/QcCompareDialog.h"

#include <utility>

#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "core/reporting/QcReportExporter.h"

namespace videoeye {
namespace ui {

namespace {

QString ToQString(const std::string& text) {
    return QString::fromStdString(text);
}

QString FormatDelta(const qc::QcCompareRow& row) {
    if (row.diff == qc::QcFieldDiff::OnlyLeft || row.diff == qc::QcFieldDiff::OnlyRight ||
        row.diff == qc::QcFieldDiff::Unavailable)
        return QStringLiteral("—");
    if (!row.numeric)
        return row.diff == qc::QcFieldDiff::Same ? QStringLiteral("—") : QStringLiteral("不同");
    const QString sign = row.delta > 0.0 ? QStringLiteral("+") : QString();
    return sign + QString::number(row.delta, 'f', 3) + ToQString(row.unit);
}

} // namespace

QcCompareDialog::QcCompareDialog(qc::QcRunResult left, qc::QcRunResult right, QWidget* parent)
    : QDialog(parent), left_(std::move(left)), right_(std::move(right)) {
    comparison_ = qc::CompareRuns(left_, right_, tolerance_);
    BuildUi();
    FillTable();
    UpdateSummary();
}

void QcCompareDialog::BuildUi() {
    setWindowTitle(tr("双文件 QC 对比"));
    resize(1000, 640);

    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("双文件对比"), this);
    title->setObjectName(QStringLiteral("PageTitle"));
    layout->addWidget(title);

    auto* paths = new QLabel(
        tr("左侧：%1\n右侧：%2").arg(ToQString(comparison_.left_path), ToQString(comparison_.right_path)), this);
    paths->setObjectName(QStringLiteral("HintLabel"));
    paths->setWordWrap(true);
    paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(paths);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("SectionLabel"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    // 容忍度 + 筛选：都只影响展示口径，不触发重新分析。
    auto* tools = new QHBoxLayout();
    tools->addWidget(new QLabel(tr("数值容差:"), this));
    tolerance_spin_ = new QDoubleSpinBox(this);
    tolerance_spin_->setRange(0.0, 50.0);
    tolerance_spin_->setDecimals(2);
    tolerance_spin_->setSingleStep(0.05);
    tolerance_spin_->setValue(0.0);
    tolerance_spin_->setSuffix(tr(" %"));
    tolerance_spin_->setToolTip(tr("相对容差：差值不超过该百分比时视为一致（默认 0%，按严格比较）"));
    connect(tolerance_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &QcCompareDialog::OnToleranceChanged);
    tools->addWidget(tolerance_spin_);

    only_diff_check_ = new QCheckBox(tr("仅显示差异"), this);
    only_diff_check_->setToolTip(tr("隐藏两侧一致与两侧都缺失的字段，只看差异 / 单侧缺失"));
    connect(only_diff_check_, &QCheckBox::toggled, this, &QcCompareDialog::OnFilterChanged);
    tools->addWidget(only_diff_check_);
    tools->addStretch();
    layout->addLayout(tools);

    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({tr("分组"), tr("字段"), tr("左侧"), tr("右侧"), tr("差值")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(table_, 1);

    auto* buttons = new QDialogButtonBox(this);
    export_button_ = buttons->addButton(tr("导出对比报告…"), QDialogButtonBox::ActionRole);
    export_button_->setToolTip(tr("按扩展名导出 HTML / JSON / CSV / TXT / PDF"));
    buttons->addButton(QDialogButtonBox::Close);
    connect(export_button_, &QPushButton::clicked, this, &QcCompareDialog::OnExport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void QcCompareDialog::RebuildComparison() {
    comparison_ = qc::CompareRuns(left_, right_, tolerance_);
    FillTable();
    UpdateSummary();
}

void QcCompareDialog::FillTable() {
    const QColor kWarn(0xD3, 0x54, 0x00);
    const QColor kMuted(0x7F, 0x8C, 0x8D);

    table_->setRowCount(static_cast<int>(comparison_.rows.size()));
    for (int i = 0; i < static_cast<int>(comparison_.rows.size()); ++i) {
        const qc::QcCompareRow& row = comparison_.rows[static_cast<size_t>(i)];
        const QString values[5] = {ToQString(row.group), ToQString(row.field), ToQString(row.left),
                                   ToQString(row.right), FormatDelta(row)};
        for (int col = 0; col < 5; ++col) {
            auto* item = new QTableWidgetItem(values[col]);
            if (col >= 2)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (row.diff == qc::QcFieldDiff::Different)
                item->setForeground(kWarn);
            else if (row.diff != qc::QcFieldDiff::Same)
                item->setForeground(kMuted);
            table_->setItem(i, col, item);
        }
        if (row.group.empty()) {
            // 没有分组的行不显示占位，避免视觉噪音。
            table_->item(i, 0)->setText(QString());
        }
    }
    // 重算后行数/内容都变了，筛选口径要重新套一遍。
    ApplyFilter();
}

void QcCompareDialog::UpdateSummary() {
    if (!summary_)
        return;
    summary_->setText(tr("左侧 %1（%2 分） · 右侧 %3（%4 分） · 差异 %5 项 / 可比 %6 项")
                          .arg(ToQString(comparison_.left_verdict))
                          .arg(comparison_.left_score, 0, 'f', 1)
                          .arg(ToQString(comparison_.right_verdict))
                          .arg(comparison_.right_score, 0, 'f', 1)
                          .arg(comparison_.DifferentCount())
                          .arg(comparison_.AvailableRowCount()));
}

void QcCompareDialog::ApplyFilter() {
    if (!table_ || !only_diff_check_)
        return;
    const bool only_diff = only_diff_check_->isChecked();
    for (int i = 0; i < table_->rowCount(); ++i) {
        if (!only_diff) {
            table_->setRowHidden(i, false);
            continue;
        }
        const qc::QcCompareRow& row = comparison_.rows[static_cast<size_t>(i)];
        // 两侧一致、或两侧都没采集到，都不算"差异"。
        const bool hide = row.diff == qc::QcFieldDiff::Same || row.diff == qc::QcFieldDiff::Unavailable;
        table_->setRowHidden(i, hide);
    }
}

int QcCompareDialog::RowCount() const {
    return table_ ? table_->rowCount() : 0;
}

void QcCompareDialog::OnToleranceChanged() {
    tolerance_.relative = tolerance_spin_ ? tolerance_spin_->value() / 100.0 : 0.0;
    RebuildComparison();
}

void QcCompareDialog::OnFilterChanged() {
    ApplyFilter();
}

void QcCompareDialog::OnExport() {
    QString default_path = QStringLiteral("qc-compare.html");
    if (!comparison_.right_path.empty()) {
        const QFileInfo info(ToQString(comparison_.right_path));
        if (!info.absolutePath().isEmpty())
            default_path =
                info.absolutePath() + QStringLiteral("/") + info.completeBaseName() + QStringLiteral("-compare.html");
    }
    const QString path =
        QFileDialog::getSaveFileName(this, tr("导出对比报告"), default_path,
                                     tr("HTML (*.html);;JSON (*.json);;CSV (*.csv);;文本 (*.txt);;PDF (*.pdf)"));
    if (path.isEmpty())
        return;

    if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        const reporting::QcPdfExportResult result =
            reporting::QcReportExporter::ExportComparisonPdf(path.toStdString(), comparison_);
        if (!result.ok) {
            QMessageBox::warning(this, tr("导出失败"), tr("无法写入 %1").arg(path));
            return;
        }
        if (result.text_loss)
            QMessageBox::information(this, tr("导出完成"), tr("已写入 %1（部分超范围字符被替换）").arg(path));
        else
            QMessageBox::information(this, tr("导出完成"), tr("已写入 %1").arg(path));
        return;
    }

    if (!reporting::QcReportExporter::ExportComparisonAuto(path.toStdString(), comparison_)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法识别扩展名或写入失败：%1").arg(path));
        return;
    }
    QMessageBox::information(this, tr("导出完成"), tr("已写入 %1").arg(path));
}

} // namespace ui
} // namespace videoeye
