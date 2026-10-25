#include "ui/analysis_panel/EvidenceCoverageView.h"

#include <QHeaderView>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

EvidenceCoverageView::EvidenceCoverageView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void EvidenceCoverageView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    hint_ = new QLabel(tr("「结论可用」是唯一权威口径：只有该维度确实执行、能力完整、无失败且覆盖达标时才为「是」。"
                          "未执行的行会写明原因，据此判断报告里的「未发现问题」是「真的没问题」还是「没证据」。"),
                       this);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    table_ = new QTableWidget(0, 8, this);
    table_->setHorizontalHeaderLabels({tr("维度"), tr("状态"), tr("能力"), tr("结论可用"), tr("处理对象"),
                                       tr("扫描 / 总量"), tr("覆盖率"), tr("说明")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
}

void EvidenceCoverageView::SetResult(const model::AnalysisResult* result) {
    result_ = result;
    RebuildTable();
}

void EvidenceCoverageView::RebuildTable() {
    if (!table_)
        return;
    table_->setRowCount(0);
    if (result_ == nullptr) {
        if (hint_)
            hint_->setText(tr("尚无扫描结果。点击「开始分析」后可查看每个维度的覆盖情况。"));
        return;
    }

    if (hint_)
        hint_->setText(tr("「结论可用」是唯一权威口径：只有该维度确实执行、能力完整、无失败且覆盖达标时才为「是」。"
                          "未执行的行会写明原因，据此判断报告里的「未发现问题」是「真的没问题」还是「没证据」。"));
    const auto& dimensions = result_->dimensions;
    table_->setRowCount(static_cast<int>(dimensions.size()));
    for (int i = 0; i < static_cast<int>(dimensions.size()); ++i) {
        const model::DimensionStatus& status = dimensions[i];
        SetTableItemText(table_, i, 0, QString::fromStdString(model::DimensionName(status.dimension)));
        SetTableItemText(table_, i, 1, status.executed ? tr("已执行") : tr("未执行"));
        SetTableItemText(table_, i, 2, QString::fromStdString(model::CapabilityToString(status.capability)));
        SetTableItemText(table_, i, 3, status.conclusive ? tr("是") : tr("否"));
        SetTableItemText(table_, i, 4,
                         status.scope_unit.empty() ? QStringLiteral("-") : QString::fromStdString(status.scope_unit));
        // 总量未知时写"未知"而不是"0"：0 会被读成"一个都没扫到"
        const QString units = QString::number(static_cast<qlonglong>(status.units_scanned)) + " / " +
                              (status.has_coverage ? QString::number(static_cast<qlonglong>(status.units_total))
                                                   : QStringLiteral("未知"));
        SetTableItemText(table_, i, 5, units);
        SetTableItemText(table_, i, 6,
                         status.has_coverage ? QString::number(status.CoveragePercent(), 'f', 1) + " %"
                                             : QStringLiteral("-"));
        QString remark = QString::fromStdString(status.failure_reason);
        for (const std::string& note : status.notes) {
            if (!remark.isEmpty())
                remark += QStringLiteral("；");
            remark += QString::fromStdString(note);
        }
        if (status.failures > 0) {
            if (!remark.isEmpty())
                remark += QStringLiteral("；");
            remark += tr("失败 %1").arg(static_cast<qlonglong>(status.failures));
        }
        SetTableItemText(table_, i, 7, remark);

        // 未执行 / 失败的行整行标色：这两类最容易被误读成"通过"
        QColor color;
        if (!status.executed)
            color = QColor("#9e9e9e");
        else if (status.capability == model::AnalysisCapability::Failed)
            color = QColor("#c62828");
        else if (status.capability == model::AnalysisCapability::Partial)
            color = QColor("#ef6c00");
        if (color.isValid()) {
            for (int c = 0; c < table_->columnCount(); ++c) {
                if (QTableWidgetItem* cell = table_->item(i, c))
                    cell->setForeground(color);
            }
        }
    }
    table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
