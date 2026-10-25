#include "ui/analysis_panel/ParameterTimelineView.h"

#include <QHeaderView>
#include <QVBoxLayout>

#include <cstdint>
#include <vector>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

ParameterTimelineView::ParameterTimelineView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void ParameterTimelineView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    hint_ =
        new QLabel(tr("按时间顺序列出 SPS/PPS/VPS/AV1 序列头的配置变化。"
                      "同一参数集 id 的内容被替换（标红）意味着前后帧不是同一套配置，是拼接或编码器重启的直接证据。"),
                   this);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    table_ = new QTableWidget(0, 7, this);
    table_->setHorizontalHeaderLabels(
        {tr("流"), tr("时间"), tr("参数集"), tr("变化前"), tr("变化后"), tr("包号 / 偏移"), tr("说明")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_);
}

void ParameterTimelineView::SetResult(const model::AnalysisResult* result) {
    result_ = result;
    RebuildTable();
}

void ParameterTimelineView::RebuildTable() {
    if (!table_)
        return;
    table_->setRowCount(0);
    if (result_ == nullptr) {
        if (hint_)
            hint_->setText(tr("尚无扫描结果。"));
        return;
    }
    if (result_->parameter_timelines.empty()) {
        if (hint_)
            hint_->setText(tr("本次未追踪参数变化（没有 H.264/HEVC/AV1 视频轨，或「参数变化追踪」选项已关闭）。"));
        return;
    }

    // 先把所有轨的变化事件摊平，再一次性填表 —— 一条轨没有变化也要留一行
    // 「未检测到配置变化」，否则"这条轨没变"和"这条轨没扫"在界面上分不出来。
    struct Row {
        int stream_index = -1;
        QString time;
        QString source;
        QString old_config;
        QString new_config;
        QString position;
        QString note;
        bool replaced_same_id = false;
    };
    std::vector<Row> rows;
    int tracked_streams = 0;
    int truncated_streams = 0;
    int64_t missing_refs = 0;
    for (const model::ParameterTimeline& timeline : result_->parameter_timelines) {
        ++tracked_streams;
        if (timeline.truncated)
            ++truncated_streams;
        missing_refs += timeline.missing_reference_count;
        if (timeline.changes.empty()) {
            Row row;
            row.stream_index = timeline.stream_index;
            row.time = QStringLiteral("-");
            row.source = QString::fromStdString(timeline.codec_name);
            row.note = tr("未检测到配置变化（当前配置：%1）").arg(QString::fromStdString(timeline.ActiveSummary()));
            rows.push_back(row);
            continue;
        }
        for (const model::ParameterChangeEvent& event : timeline.changes) {
            Row row;
            row.stream_index = event.stream_index >= 0 ? event.stream_index : timeline.stream_index;
            // 无 PTS 时不能写 0.000 s —— 那会被读成"在文件开头"
            row.time = event.has_time ? QString::number(event.time_seconds, 'f', 3) + " s" : tr("无 PTS");
            row.source = QString::fromStdString(event.source);
            row.old_config = QString::fromStdString(event.old_config);
            row.new_config = QString::fromStdString(event.new_config);
            QString position = tr("包 %1").arg(static_cast<qlonglong>(event.packet_index));
            if (event.file_offset >= 0)
                position +=
                    QStringLiteral(" / 0x") + QString::number(static_cast<qlonglong>(event.file_offset), 16).toUpper();
            row.position = position;
            row.note = QString::fromStdString(event.changes);
            row.replaced_same_id = event.replaced_same_id;
            rows.push_back(row);
        }
    }

    QString hint = tr("追踪 %1 条视频轨，共 %2 次配置变化。").arg(tracked_streams).arg(static_cast<int>(rows.size()));
    if (missing_refs > 0)
        hint += tr(" 有 %1 处引用了从未传输的参数集（解码必然失败）。").arg(static_cast<qlonglong>(missing_refs));
    if (truncated_streams > 0)
        hint += tr(" 有 %1 条轨已达追踪上限，后面的变化未记录。").arg(truncated_streams);
    if (hint_)
        hint_->setText(hint);

    table_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const Row& row = rows[i];
        SetTableItemText(table_, i, 0, QString::number(row.stream_index));
        SetTableItemText(table_, i, 1, row.time);
        SetTableItemText(table_, i, 2, row.source);
        SetTableItemText(table_, i, 3, row.old_config.isEmpty() ? QStringLiteral("-") : row.old_config);
        SetTableItemText(table_, i, 4, row.new_config.isEmpty() ? QStringLiteral("-") : row.new_config);
        SetTableItemText(table_, i, 5, row.position);
        SetTableItemText(table_, i, 6,
                         row.replaced_same_id ? tr("%1 ｜ 同一 id 内容被替换：前后帧不是同一套配置").arg(row.note)
                                              : row.note);
        if (row.replaced_same_id) {
            for (int c = 0; c < table_->columnCount(); ++c) {
                if (QTableWidgetItem* cell = table_->item(i, c))
                    cell->setForeground(QColor("#c62828"));
            }
        }
    }
    table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
