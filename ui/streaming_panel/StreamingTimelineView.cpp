#include "ui/streaming_panel/StreamingTimelineView.h"

#include <QColor>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ui/streaming_panel/StreamingFormat.h"

namespace videoeye {
namespace ui {
namespace {

constexpr int kMaxTimelineRows = 200; // 时间轴对齐表最多画多少行
constexpr int kMaxTimelineCols = 8;   // 时间轴对齐表最多画多少条码率层
constexpr double kAlignTolerance = 0.05;

} // namespace

StreamingTimelineView::StreamingTimelineView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    table_ = new QTableWidget(this);
    table_->setAlternatingRowColors(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
}

void StreamingTimelineView::Clear() {
    table_->setRowCount(0);
    table_->setColumnCount(0);
}

void StreamingTimelineView::SetResult(const model::StreamingPackageResult& result) {
    table_->setRowCount(0);
    if (!result.valid || result.ladder.empty())
        return;

    // 参考轨：关键帧时间点最多的那条（ABR 通常以它为对齐基准）
    int reference = -1;
    for (int i = 0; i < static_cast<int>(result.ladder.size()); ++i) {
        if (result.ladder[i].keyframe_times.empty())
            continue;
        if (reference < 0 || result.ladder[i].keyframe_times.size() > result.ladder[reference].keyframe_times.size()) {
            reference = i;
        }
    }

    const int cols = std::min<int>(static_cast<int>(result.ladder.size()), kMaxTimelineCols);
    QStringList headers;
    headers << tr("分片 #");
    for (int c = 0; c < cols; ++c) {
        headers << QString::fromStdString(result.ladder[c].label);
    }
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);

    int rows = 0;
    for (int c = 0; c < cols; ++c) {
        rows = std::max(rows, static_cast<int>(result.ladder[c].keyframe_times.size()));
    }
    rows = std::min(rows, kMaxTimelineRows);
    table_->setRowCount(rows);

    const QColor kAlignBg("#5A2B2B");
    const QColor kAlignFg("#F85149");
    const QColor kMissingFg("#6E7681");

    for (int r = 0; r < rows; ++r) {
        auto* index_item = new QTableWidgetItem(QString::number(r));
        table_->setItem(r, 0, index_item);

        double ref_time = 0.0;
        bool has_ref = false;
        if (reference >= 0 && reference < cols &&
            r < static_cast<int>(result.ladder[reference].keyframe_times.size())) {
            ref_time = result.ladder[reference].keyframe_times[r];
            has_ref = true;
        }

        for (int c = 0; c < cols; ++c) {
            const model::StreamingLadderEntry& e = result.ladder[c];
            auto* item = new QTableWidgetItem();
            if (r < static_cast<int>(e.keyframe_times.size())) {
                const double t = e.keyframe_times[r];
                item->setText(StreamingSeconds(t));
                if (has_ref && std::fabs(t - ref_time) > kAlignTolerance) {
                    item->setBackground(kAlignBg);
                    item->setForeground(kAlignFg);
                    item->setToolTip(tr("与参考层 %1 的起点偏差 %2 秒")
                                         .arg(QString::fromStdString(result.ladder[reference].label))
                                         .arg(StreamingSeconds(t - ref_time)));
                }
                if (c == reference) {
                    QFont font = item->font();
                    font.setBold(true);
                    item->setFont(font);
                }
            } else {
                item->setText(QStringLiteral("-"));
                item->setForeground(kMissingFg);
            }
            table_->setItem(r, c + 1, item);
        }
    }
    table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
