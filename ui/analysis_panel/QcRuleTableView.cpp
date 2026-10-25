#include "ui/analysis_panel/QcRuleTableView.h"

#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

QcRuleTableView::QcRuleTableView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void QcRuleTableView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    QLabel* hint =
        new QLabel(tr("勾选启用列可开关规则，双击阈值可直接修改；修改后会立即用当前扫描结果重算报告。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    table_ = new QTableWidget(0, 6, this);
    table_->setHorizontalHeaderLabels({tr("启用"), tr("规则"), tr("类别"), tr("严重度"), tr("判定"), tr("阈值")});
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
    connect(table_, &QTableWidget::itemChanged, this, &QcRuleTableView::OnItemChanged);

    QPushButton* reset_btn = new QPushButton(tr("恢复默认规则"), this);
    connect(reset_btn, &QPushButton::clicked, this, &QcRuleTableView::ResetRequested);
    layout->addWidget(reset_btn, 0, Qt::AlignRight);
}

void QcRuleTableView::SetRulesProvider(RulesProvider provider) {
    provider_ = std::move(provider);
    Reload();
}

void QcRuleTableView::Reload() {
    if (!table_)
        return;
    updating_ = true;
    table_->setRowCount(0);

    const std::vector<model::QcRule> empty;
    const std::vector<model::QcRule>& rules = provider_ ? provider_() : empty;
    table_->setRowCount(static_cast<int>(rules.size()));
    for (int i = 0; i < static_cast<int>(rules.size()); ++i) {
        const auto& rule = rules[i];

        QTableWidgetItem* enable_item = new QTableWidgetItem();
        enable_item->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
        enable_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        table_->setItem(i, 0, enable_item);

        QTableWidgetItem* name_item = new QTableWidgetItem(QString::fromStdString(rule.name));
        name_item->setToolTip(QString::fromStdString(rule.description));
        name_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        table_->setItem(i, 1, name_item);

        SetTableItemText(table_, i, 2, QString::fromStdString(ToString(rule.category)));
        SetTableItemText(table_, i, 3, QString::fromStdString(ToString(rule.severity)));
        SetTableItemText(table_, i, 4, QString::fromStdString(ToString(rule.op)));

        QTableWidgetItem* threshold_item =
            new QTableWidgetItem(QString::number(rule.threshold, 'f', 2) + QString::fromStdString(rule.unit));
        threshold_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        threshold_item->setToolTip(tr("双击修改阈值（仅需填写数值部分）"));
        table_->setItem(i, 5, threshold_item);
    }
    table_->resizeColumnsToContents();
    updating_ = false;
}

void QcRuleTableView::OnItemChanged(QTableWidgetItem* item) {
    if (!item || updating_ || !provider_)
        return;
    const QString rule_id = item->data(Qt::UserRole).toString();
    if (rule_id.isEmpty())
        return;

    model::QcRule* rule = model::FindQcRule(provider_(), rule_id.toStdString());
    if (!rule)
        return;

    if (item->column() == 0) {
        rule->enabled = (item->checkState() == Qt::Checked);
    } else if (item->column() == 5) {
        // 允许 "12.5s" / "12.5" 这类输入，取前导数字部分
        const QString text = item->text().trimmed();
        int end = 0;
        while (end < text.size() && (text.at(end).isDigit() || text.at(end) == QLatin1Char('.') ||
                                     text.at(end) == QLatin1Char('-') || text.at(end) == QLatin1Char('+'))) {
            ++end;
        }
        bool ok = false;
        const double value = text.left(end).toDouble(&ok);
        if (!ok) {
            // 还原显示
            updating_ = true;
            item->setText(QString::number(rule->threshold, 'f', 2) + QString::fromStdString(rule->unit));
            updating_ = false;
            return;
        }
        rule->threshold = value;
    } else {
        return;
    }

    emit RulesChanged();
}

} // namespace ui
} // namespace videoeye
