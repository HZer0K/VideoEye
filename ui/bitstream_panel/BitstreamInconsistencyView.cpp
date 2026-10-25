#include "ui/bitstream_panel/BitstreamInconsistencyView.h"

#include <QColor>
#include <QHeaderView>
#include <QVBoxLayout>

namespace videoeye {
namespace ui {

BitstreamInconsistencyView::BitstreamInconsistencyView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void BitstreamInconsistencyView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    title_ = new QLabel(tr("容器与码流不一致项:"), this);
    layout->addWidget(title_);

    inconsistency_table_ = new QTableWidget(this);
    inconsistency_table_->setColumnCount(5);
    inconsistency_table_->setHorizontalHeaderLabels({tr("级别"), tr("字段"), tr("容器值"), tr("码流值"), tr("说明")});
    inconsistency_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    inconsistency_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    inconsistency_table_->setAlternatingRowColors(true);
    inconsistency_table_->setMinimumHeight(120);
    if (inconsistency_table_->horizontalHeader()) {
        inconsistency_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    }
    layout->addWidget(inconsistency_table_);
}

void BitstreamInconsistencyView::Clear() {
    inconsistency_table_->setRowCount(0);
}

int BitstreamInconsistencyView::RowCount() const {
    return inconsistency_table_ ? inconsistency_table_->rowCount() : 0;
}

void BitstreamInconsistencyView::SetResult(const model::BitstreamAnalysisResult& result) {
    inconsistency_table_->setRowCount(0);
    if (!result.analyzed || result.inconsistencies.empty())
        return;
    BuildTable(result);
}

void BitstreamInconsistencyView::BuildTable(const model::BitstreamAnalysisResult& result) {
    for (const auto& item : result.inconsistencies) {
        const int row = inconsistency_table_->rowCount();
        inconsistency_table_->insertRow(row);

        const bool is_info = item.severity == "info";
        QTableWidgetItem* severity =
            new QTableWidgetItem(is_info ? tr("提示") : (item.severity == "error" ? tr("错误") : tr("警告")));
        severity->setForeground(QColor(is_info ? "#616161" : "#d32f2f"));

        inconsistency_table_->setItem(row, 0, severity);
        inconsistency_table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(item.field)));
        inconsistency_table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(item.container_value)));
        inconsistency_table_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(item.bitstream_value)));

        QString description = QString::fromStdString(item.description);
        if (!item.suggestion.empty()) {
            description += tr("（建议：%1）").arg(QString::fromStdString(item.suggestion));
        }
        inconsistency_table_->setItem(row, 4, new QTableWidgetItem(description));
    }

    inconsistency_table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
