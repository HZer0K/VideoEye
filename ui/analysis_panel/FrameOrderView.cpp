#include "ui/analysis_panel/FrameOrderView.h"

#include <QHeaderView>
#include <QVBoxLayout>

#include <vector>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

FrameOrderView::FrameOrderView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void FrameOrderView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    hint_ = new QLabel(tr("按解码顺序列出每一帧的 POC（显示顺序值）。POC 比上一行小的行标红，"
                          "那是显示顺序要往回插的帧 —— 重排的存在与深度因此一目了然。"
                          "支持范围：仅 H.264；pic_order_cnt_type=1 与 HEVC/AV1/VP9/VVC 不支持；"
                          "不重建参考帧列表。"),
                       this);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    table_ = new QTableWidget(0, 6, this);
    table_->setHorizontalHeaderLabels(
        {tr("流"), tr("解码序"), tr("类型"), tr("frame_num"), tr("POC"), tr("包号 / 时间")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
}

void FrameOrderView::SetResult(const model::AnalysisResult* result) {
    result_ = result;
    RebuildTable();
}

void FrameOrderView::RebuildTable() {
    if (!table_)
        return;
    table_->setRowCount(0);
    if (result_ == nullptr) {
        if (hint_)
            hint_->setText(tr("尚无扫描结果。"));
        return;
    }
    if (result_->frame_order_timelines.empty()) {
        if (hint_)
            hint_->setText(tr("本次未追踪帧顺序（没有 H.264 视频轨，或「帧级顺序追踪」选项已关闭）。"));
        return;
    }

    // 与参数变化表同理：每条轨至少留一行，不可用的轨留「不可用 + 原因」，
    // 否则"没有 H.264 轨"和"有轨但算不出 POC"在界面上长得一样。
    struct Row {
        int stream_index = -1;
        QString decode_index;
        QString type;
        QString frame_num;
        QString poc;
        QString position;
        bool reorder = false;
        bool abnormal = false;
    };
    std::vector<Row> rows;
    int tracked = 0;
    int unavailable = 0;
    for (const model::FrameOrderTimeline& timeline : result_->frame_order_timelines) {
        ++tracked;
        if (!timeline.available) {
            ++unavailable;
            Row row;
            row.stream_index = timeline.stream_index;
            row.decode_index = QStringLiteral("-");
            row.type = tr("不可用");
            row.frame_num = QStringLiteral("-");
            row.poc = QString::fromStdString(timeline.unavailable_reason);
            row.position = QString::fromStdString(timeline.Summary());
            row.abnormal = true;
            rows.push_back(row);
            continue;
        }
        for (std::size_t i = 0; i < timeline.frames.size(); ++i) {
            const model::FrameOrderEntry& entry = timeline.frames[i];
            Row row;
            row.stream_index = timeline.stream_index;
            row.decode_index = QString::number(static_cast<qulonglong>(i));
            const char type_char = entry.TypeChar();
            row.type = QString::fromLatin1(&type_char, 1) + (entry.idr ? QStringLiteral("(IDR)") : QString());
            row.frame_num = QString::number(entry.frame_num);
            row.poc = QString::number(static_cast<qlonglong>(entry.poc));
            row.position = tr("包 %1").arg(static_cast<qlonglong>(entry.packet_index));
            if (entry.has_time)
                row.position += QStringLiteral(" / ") + QString::number(entry.time_seconds, 'f', 3) + " s";
            row.reorder = i > 0 && entry.poc < timeline.frames[i - 1].poc;
            rows.push_back(row);
        }
    }

    QString hint = tr("追踪 %1 条视频轨，共列出 %2 帧（解码顺序）。").arg(tracked).arg(static_cast<int>(rows.size()));
    if (unavailable > 0)
        hint += tr(" 其中 %1 条轨不可用，原因见表内。").arg(unavailable);
    if (hint_)
        hint_->setText(hint);

    table_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Row& row = rows[i];
        SetTableItemText(table_, i, 0, QString::number(row.stream_index));
        SetTableItemText(table_, i, 1, row.decode_index);
        SetTableItemText(table_, i, 2, row.type);
        SetTableItemText(table_, i, 3, row.frame_num);
        SetTableItemText(table_, i, 4, row.poc);
        SetTableItemText(table_, i, 5, row.position);
        // 重排（POC 回退）是正常编码结构，用橙色提示而非红色；
        // 红色留给"这条轨算不出 POC"这种真的没结论的情况。
        if (row.abnormal) {
            for (int c = 0; c < table_->columnCount(); ++c) {
                if (QTableWidgetItem* cell = table_->item(i, c))
                    cell->setForeground(QColor("#c62828"));
            }
        } else if (row.reorder) {
            for (int c = 0; c < table_->columnCount(); ++c) {
                if (QTableWidgetItem* cell = table_->item(i, c))
                    cell->setForeground(QColor("#ef6c00"));
            }
        }
    }
    table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
