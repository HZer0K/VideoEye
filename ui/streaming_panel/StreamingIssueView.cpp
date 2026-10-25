#include "ui/streaming_panel/StreamingIssueView.h"

#include <QColor>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace videoeye {
namespace ui {
namespace {

// Critical / Error 同色：报告里两者都是"要修"，靠文案区分就够了
QColor SeverityColor(model::IssueSeverity severity) {
    switch (severity) {
    case model::IssueSeverity::Critical:
        return QColor("#F85149");
    case model::IssueSeverity::Error:
        return QColor("#F85149");
    case model::IssueSeverity::Warning:
        return QColor("#D29922");
    default:
        return QColor("#8B949E");
    }
}

} // namespace

StreamingIssueView::StreamingIssueView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    table_ = new QTableWidget(this);
    table_->setAlternatingRowColors(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
}

void StreamingIssueView::Clear() {
    table_->setRowCount(0);
    table_->setColumnCount(0);
}

int StreamingIssueView::IssueCount() const {
    return table_ ? table_->rowCount() : 0;
}

void StreamingIssueView::SetResult(const model::StreamingPackageResult& result) {
    table_->setRowCount(0);
    if (!result.valid)
        return;

    const QStringList headers = QStringList() << tr("级别") << tr("问题") << tr("说明") << tr("建议") << tr("次数");
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->setRowCount(static_cast<int>(result.issues.size()));

    // 严重度高的排前面
    std::vector<const model::StreamingIssue*> ordered;
    for (const model::StreamingIssue& issue : result.issues)
        ordered.push_back(&issue);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const model::StreamingIssue* a, const model::StreamingIssue* b) {
                         return static_cast<int>(a->severity) > static_cast<int>(b->severity);
                     });

    for (int row = 0; row < static_cast<int>(ordered.size()); ++row) {
        const model::StreamingIssue& issue = *ordered[row];
        const QColor color = SeverityColor(issue.severity);
        int col = 0;
        auto set = [&](const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setForeground(color);
            table_->setItem(row, col++, item);
        };
        set(QString::fromStdString(model::ToString(issue.severity)));
        set(QString::fromStdString(issue.title));
        set(QString::fromStdString(issue.detail));
        set(QString::fromStdString(issue.suggestion));
        set(QString::number(issue.occurrence_count));
    }
    table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
