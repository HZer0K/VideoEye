#include "ui/analysis_panel/AudioTrackView.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHeaderView>
#include <QVBoxLayout>

#include "core/domain/model/AudioQcResult.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

AudioTrackView::AudioTrackView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void AudioTrackView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    hint_ = new QLabel(tr("前面几页的响度/静音/规则均为**主音轨**口径；本页给出全部参与分析的音轨，"
                          "用于多语言、评论轨与不同混音版本之间的横向对照。"),
                       this);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    table_ = new QTableWidget(0, 10, this);
    table_->setHorizontalHeaderLabels({tr("流"), tr("声道布局"), tr("采样率"), tr("积分响度"), tr("真峰值"), tr("LRA"),
                                       tr("静音占比"), tr("覆盖率"), tr("与视频时长差"), tr("失败/丢帧")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setMinimumHeight(220);
    layout->addWidget(table_);
}

void AudioTrackView::SetResult(const model::AnalysisResult* result) {
    result_ = result;
    RebuildTable();
}

void AudioTrackView::RebuildTable() {
    if (!table_)
        return;
    table_->setRowCount(0);
    if (result_ == nullptr) {
        if (hint_)
            hint_->setText(tr("尚无结果。点击「开始分析」后会列出全部音轨的横向对照。"));
        return;
    }

    const auto& result = *result_;
    const auto& tracks = result.audio_qc_streams;
    if (tracks.empty()) {
        // 空表要说明"为什么空"：没音轨 / 选项关闭 / 全部未解出码 是三种不同的事
        if (hint_)
            hint_->setText(tr("本次没有音轨参与分析（可能是文件无音频流、选项已关闭，"
                              "或全部音轨都未能打开解码器）。"));
        return;
    }

    if (hint_)
        hint_->setText(tr("共 %1 / %2 条音轨参与分析。前面几页的响度/静音/规则均为主音轨口径；"
                          "本页用于多语言、评论轨与不同混音版本之间的横向对照。")
                           .arg(result.audio_qc_tracks_analyzed)
                           .arg(result.audio_qc_tracks_total));

    table_->setRowCount(static_cast<int>(tracks.size()));
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        const model::AudioQcResult& qc = tracks[static_cast<size_t>(i)];
        const int stream_index = (static_cast<size_t>(i) < result.audio_qc_stream_indices.size())
                                     ? result.audio_qc_stream_indices[static_cast<size_t>(i)]
                                     : -1;
        SetTableItemText(table_, i, 0, QString::number(stream_index));

        if (!qc.analyzed) {
            // 未分析的轨不要把默认值（-70 LUFS 之类）当成实测值填进去
            SetTableItemText(table_, i, 1, qc.notes.empty() ? tr("未分析") : QString::fromStdString(qc.notes.front()));
            for (int c = 2; c < table_->columnCount(); ++c)
                SetTableItemText(table_, i, c, QStringLiteral("-"));
            for (int c = 0; c < table_->columnCount(); ++c) {
                if (QTableWidgetItem* cell = table_->item(i, c))
                    cell->setForeground(QColor("#9e9e9e"));
            }
            continue;
        }

        SetTableItemText(table_, i, 1, QString::fromStdString(qc.metadata.channel_layout));
        SetTableItemText(table_, i, 2, tr("%1 Hz").arg(qc.metadata.sample_rate));
        SetTableItemText(table_, i, 3, QString::number(qc.integrated_lufs, 'f', 2) + QStringLiteral(" LUFS"));
        SetTableItemText(table_, i, 4, QString::number(qc.true_peak_dbtp, 'f', 2) + QStringLiteral(" dBTP"));
        SetTableItemText(table_, i, 5, QString::number(qc.loudness_range_lu, 'f', 2) + QStringLiteral(" LU"));
        SetTableItemText(table_, i, 6, QString::number(qc.silence_ratio * 100.0, 'f', 1) + QStringLiteral(" %"));
        SetTableItemText(table_, i, 7, QString::number(qc.coverage_ratio * 100.0, 'f', 1) + QStringLiteral(" %"));
        // 音轨与视频流的时长差：音画不同步最直接的一个数（正值 = 音频更长）
        const double delta = qc.metadata.video_delta_seconds;
        SetTableItemText(table_, i, 8,
                         qc.metadata.has_video ? QString::number(delta, 'f', 3) + QStringLiteral(" s")
                                               : QStringLiteral("-（无视频流）"));
        const qlonglong errors =
            static_cast<qlonglong>(qc.decode_error_packets + qc.decode_error_frames + qc.dropped_frames);
        SetTableItemText(table_, i, 9, QString::number(errors));

        // 覆盖率不足的轨标色：这是"分析跑过但没跑满"，最容易被误读成已完成
        if (qc.coverage_ratio > 0.0 && qc.coverage_ratio < 0.99) {
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
