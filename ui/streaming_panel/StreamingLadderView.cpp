#include "ui/streaming_panel/StreamingLadderView.h"

#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ui/streaming_panel/StreamingFormat.h"

namespace videoeye {
namespace ui {

StreamingLadderView::StreamingLadderView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    table_ = new QTableWidget(this);
    table_->setAlternatingRowColors(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
}

void StreamingLadderView::Clear() {
    table_->setRowCount(0);
    table_->setColumnCount(0);
}

void StreamingLadderView::SetResult(const model::StreamingPackageResult& result) {
    table_->setRowCount(0);
    if (!result.valid)
        return;

    const QStringList headers = QStringList() << tr("码率层") << tr("分辨率") << tr("码率(kbps)") << tr("编码")
                                              << tr("容器") << tr("分片数") << tr("平均时长(s)") << tr("最长时长(s)")
                                              << tr("初始化段") << tr("关键帧数");
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->setRowCount(static_cast<int>(result.ladder.size()));

    for (int row = 0; row < static_cast<int>(result.ladder.size()); ++row) {
        const model::StreamingLadderEntry& e = result.ladder[row];
        int col = 0;
        auto set = [&](const QString& text) { table_->setItem(row, col++, new QTableWidgetItem(text)); };
        set(QString::fromStdString(e.label));
        set(StreamingResolution(e.width, e.height));
        set(StreamingKbps(e.bandwidth_bps));
        set(QString::fromStdString(e.video_codec.empty() ? e.audio_codec : e.video_codec));
        set(QString::fromStdString(e.container_hint));
        set(QString::number(e.segment_count));
        set(StreamingSeconds(e.avg_segment_duration_s));
        set(StreamingSeconds(e.max_segment_duration_s));
        set(e.has_init_section ? tr("有") : tr("无"));
        set(QString::number(e.keyframe_times.size()));
    }
    table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
