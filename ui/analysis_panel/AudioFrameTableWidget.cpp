#include "ui/analysis_panel/AudioFrameTableWidget.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QTextStream>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

namespace {
constexpr std::size_t kMaxAudioFrameRecords = 30000;
}  // namespace

AudioFrameTableWidget::AudioFrameTableWidget(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

void AudioFrameTableWidget::SetupUi() {
    QVBoxLayout* a_layout = new QVBoxLayout(this);
    a_layout->setContentsMargins(4, 4, 4, 4);
    a_layout->setSpacing(4);

    QHBoxLayout* a_toolbar = new QHBoxLayout();
    audio_frame_summary_label_ = new QLabel(tr("总音频帧数: 0 | 总样本数: 0 | 总字节数: 0"), this);
    a_toolbar->addWidget(audio_frame_summary_label_, 1);

    export_audio_frame_csv_button_ = new QPushButton(tr("导出 CSV"), this);
    a_toolbar->addWidget(export_audio_frame_csv_button_);

    audio_toggle_ = new QCheckBox(tr("启用分析"), this);
    a_toolbar->addWidget(audio_toggle_);
    a_layout->addLayout(a_toolbar);

    QGroupBox* a_table_group = new QGroupBox(tr("音频帧信息"), this);
    QVBoxLayout* a_table_layout = new QVBoxLayout(a_table_group);
    // 音频帧列名升级 (列数保持 7 列, PTS/Hz 等协议术语括注用途, 字面更紧凑)
    audio_frame_table_ = new QTableWidget(0, 7, a_table_group);
    audio_frame_table_->setHorizontalHeaderLabels({
        "#", "播放时间(s)", "原始 PTS", "样本数", "采样率", "声道", "字节"
    });
    audio_frame_table_->verticalHeader()->setVisible(false);
    audio_frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    audio_frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    audio_frame_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    audio_frame_table_->setSortingEnabled(false);
    audio_frame_table_->horizontalHeader()->setStretchLastSection(true);
    audio_frame_table_->horizontalHeader()->setMinimumSectionSize(50);
    audio_frame_table_->setColumnWidth(0, 60);
    audio_frame_table_->setColumnWidth(1, 100);
    audio_frame_table_->setColumnWidth(2, 100);
    audio_frame_table_->setColumnWidth(3, 100);
    audio_frame_table_->setColumnWidth(4, 120);
    audio_frame_table_->setColumnWidth(5, 90);
    audio_frame_table_->setMinimumWidth(550);
    audio_frame_table_->setMinimumHeight(120);
    a_table_layout->addWidget(audio_frame_table_);
    a_layout->addWidget(a_table_group);

    // 导出按钮是本子页内部的事（只依赖自己的记录缓存），就地连回。
    connect(export_audio_frame_csv_button_, &QPushButton::clicked,
            this, &AudioFrameTableWidget::OnExportAudioFrameCsv);
}

void AudioFrameTableWidget::ResetAudioFrames() {
    audio_frame_records_.clear();
    audio_frame_table_synced_record_count_ = 0;
    audio_frame_table_dirty_ = false;
    audio_frame_summary_dirty_ = true;
    if (audio_frame_table_) {
        audio_frame_table_->setRowCount(0);
    }
    UpdateAudioFrameSummary();
}

void AudioFrameTableWidget::AppendAudioFrame(int index, qint64 pts, double timestamp_seconds,
                                             int sample_count, int sample_rate, int channels,
                                             int byte_count) {
    if (!audio_frame_table_) {
        return;
    }

    AudioFrameRecord record;
    record.index = index;
    record.pts = pts;
    record.timestamp_seconds = timestamp_seconds;
    record.sample_count = sample_count;
    record.sample_rate = sample_rate;
    record.channels = channels;
    record.byte_count = byte_count;

    audio_frame_records_.push_back(record);
    audio_frame_table_dirty_ = true;
    audio_frame_summary_dirty_ = true;
    TrimRecords(audio_frame_records_, audio_frame_table_synced_record_count_, audio_frame_table_,
                audio_frame_table_dirty_, kMaxAudioFrameRecords);
}

bool AudioFrameTableWidget::HasPending() const {
    return audio_frame_table_dirty_ || audio_frame_summary_dirty_;
}

void AudioFrameTableWidget::FlushPending() {
    if (audio_frame_table_dirty_) {
        FlushPendingAudioFrameTableUpdates();
        audio_frame_table_dirty_ = false;
    }
    if (audio_frame_summary_dirty_) {
        UpdateAudioFrameSummary();
        audio_frame_summary_dirty_ = false;
    }
}

void AudioFrameTableWidget::UpdateAudioFrameSummary() {
    if (!audio_frame_summary_label_) {
        return;
    }

    long long total_samples = 0;
    long long total_bytes = 0;
    for (const auto& record : audio_frame_records_) {
        total_samples += record.sample_count;
        total_bytes += record.byte_count;
    }

    audio_frame_summary_label_->setText(
        tr("总音频帧数: %1 | 总样本数: %2 | 总字节数: %3")
            .arg(audio_frame_records_.size())
            .arg(total_samples)
            .arg(total_bytes));
}

void AudioFrameTableWidget::FlushPendingAudioFrameTableUpdates() {
    if (!audio_frame_table_) {
        return;
    }

    audio_frame_table_->setUpdatesEnabled(false);
    for (std::size_t i = audio_frame_table_synced_record_count_; i < audio_frame_records_.size(); ++i) {
        AppendAudioFrameRowToTable(audio_frame_records_[i]);
    }
    audio_frame_table_->setUpdatesEnabled(true);
    audio_frame_table_synced_record_count_ = audio_frame_records_.size();

    if (audio_frame_table_->rowCount() > 0) {
        audio_frame_table_->scrollToBottom();
    }
}

void AudioFrameTableWidget::AppendAudioFrameRowToTable(const AudioFrameRecord& record) {
    const int row = audio_frame_table_->rowCount();
    audio_frame_table_->insertRow(row);
    SetTableItemText(audio_frame_table_, row, 0, QString::number(record.index));
    SetTableItemText(audio_frame_table_, row, 1, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(audio_frame_table_, row, 2, QString::number(record.pts));
    SetTableItemText(audio_frame_table_, row, 3, QString::number(record.sample_count));
    SetTableItemText(audio_frame_table_, row, 4, QString::number(record.sample_rate));
    SetTableItemText(audio_frame_table_, row, 5, QString::number(record.channels));
    SetTableItemText(audio_frame_table_, row, 6, QString::number(record.byte_count));
}

void AudioFrameTableWidget::OnExportAudioFrameCsv() {
    if (audio_frame_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的音频帧数据。"));
        return;
    }

    ExportCsvStream(this,
        tr("导出音频帧 CSV"),
        QString("videoeye_audio_frames_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,timestamp_seconds,pts,sample_count,sample_rate,channels,byte_count\n";
            for (const auto& record : audio_frame_records_) {
                out << record.index << ','
                    << QString::number(record.timestamp_seconds, 'f', 6) << ','
                    << record.pts << ','
                    << record.sample_count << ','
                    << record.sample_rate << ','
                    << record.channels << ','
                    << record.byte_count << '\n';
            }
        },
        static_cast<int>(audio_frame_records_.size()));
}

}  // namespace ui
}  // namespace videoeye
