#include "ui/analysis_panel/AudioFrameTableWidget.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QTextStream>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

namespace {
// 记录上限统一取 10 万（见 RecordTableModel.h）。
// 以前本页是 3 万 —— 一小时视频的音频帧就有十几万，3 万意味着后半段被悄悄丢掉，
// 而界面上只说"总音频帧数: 30000"，看不出是被截的。
constexpr std::size_t kMaxAudioFrameRecords = kDefaultMaxRecordCount;

std::vector<RecordTableModel<AudioFrameRecord>::ColumnSpec> AudioFrameColumns() {
    using Spec = RecordTableModel<AudioFrameRecord>::ColumnSpec;
    return {
        Spec{QStringLiteral("#"), [](const AudioFrameRecord& r) { return QString::number(r.index); }, {}},
        Spec{QStringLiteral("播放时间(s)"),
             [](const AudioFrameRecord& r) { return QString::number(r.timestamp_seconds, 'f', 3); },
             {}},
        Spec{QStringLiteral("原始 PTS"), [](const AudioFrameRecord& r) { return QString::number(r.pts); }, {}},
        Spec{QStringLiteral("样本数"), [](const AudioFrameRecord& r) { return QString::number(r.sample_count); }, {}},
        Spec{QStringLiteral("采样率"), [](const AudioFrameRecord& r) { return QString::number(r.sample_rate); }, {}},
        Spec{QStringLiteral("声道"), [](const AudioFrameRecord& r) { return QString::number(r.channels); }, {}},
        Spec{QStringLiteral("字节"), [](const AudioFrameRecord& r) { return QString::number(r.byte_count); }, {}},
    };
}
} // namespace

AudioFrameTableWidget::AudioFrameTableWidget(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

AudioFrameTableWidget::~AudioFrameTableWidget() = default;

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
    audio_toggle_->setToolTip(tr("播放时实时采集，需要额外的数据留存，默认关闭，按需开启。"));
    a_toolbar->addWidget(audio_toggle_);
    a_layout->addLayout(a_toolbar);

    QGroupBox* a_table_group = new QGroupBox(tr("音频帧信息"), this);
    QVBoxLayout* a_table_layout = new QVBoxLayout(a_table_group);

    // 模型不挂 Qt 父对象、由本组件在析构里 delete：QTableView 不会接管模型的
    // 所有权（setModel 只是引用），挂父对象反而会与"组件析构时手动删"双重释放。
    audio_frame_model_ = new RecordTableModel<AudioFrameRecord>(AudioFrameColumns(), this);

    audio_frame_table_ = new QTableView(a_table_group);
    audio_frame_table_->setModel(audio_frame_model_);
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
    connect(export_audio_frame_csv_button_, &QPushButton::clicked, this, &AudioFrameTableWidget::OnExportAudioFrameCsv);
}

const std::vector<AudioFrameRecord>& AudioFrameTableWidget::AudioFrameRecords() const {
    return audio_frame_model_->records();
}

void AudioFrameTableWidget::ResetAudioFrames() {
    pending_audio_frames_.clear();
    audio_frame_table_dirty_ = false;
    audio_frame_summary_dirty_ = true;
    if (audio_frame_model_) {
        audio_frame_model_->Clear();
    }
    UpdateAudioFrameSummary();
}

void AudioFrameTableWidget::AppendAudioFrame(int index, qint64 pts, double timestamp_seconds, int sample_count,
                                             int sample_rate, int channels, int byte_count) {
    if (!audio_frame_model_) {
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

    // 只入队 + 置脏：真正交给模型在 FlushPending，一次 rowsInserted 通知一批。
    pending_audio_frames_.push_back(record);
    audio_frame_table_dirty_ = true;
    audio_frame_summary_dirty_ = true;
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
    if (!audio_frame_summary_label_ || !audio_frame_model_) {
        return;
    }

    const auto& records = audio_frame_model_->records();
    long long total_samples = 0;
    long long total_bytes = 0;
    for (const auto& record : records) {
        total_samples += record.sample_count;
        total_bytes += record.byte_count;
    }

    audio_frame_summary_label_->setText(
        tr("总音频帧数: %1 | 总样本数: %2 | 总字节数: %3").arg(records.size()).arg(total_samples).arg(total_bytes));
}

void AudioFrameTableWidget::FlushPendingAudioFrameTableUpdates() {
    if (!audio_frame_model_ || !audio_frame_table_) {
        return;
    }

    if (!pending_audio_frames_.empty()) {
        audio_frame_model_->AppendRecords(std::move(pending_audio_frames_));
        pending_audio_frames_.clear();
    }

    // 超限淘汰最早的：模型自己维护记录，裁剪走 beginRemoveRows，
    // 不再是"整表重建"（旧实现的 TrimRecords + setRowCount(0)）。
    const std::size_t count = audio_frame_model_->record_count();
    if (count > kMaxAudioFrameRecords) {
        audio_frame_model_->TrimFront(count - kMaxAudioFrameRecords);
    }

    if (audio_frame_model_->rowCount() > 0) {
        audio_frame_table_->scrollToBottom();
    }
}

void AudioFrameTableWidget::OnExportAudioFrameCsv() {
    if (!audio_frame_model_ || audio_frame_model_->record_count() == 0) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的音频帧数据。"));
        return;
    }

    const int rows = static_cast<int>(audio_frame_model_->record_count());
    ExportCsvStream(
        this, tr("导出音频帧 CSV"),
        QString("videoeye_audio_frames_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,timestamp_seconds,pts,sample_count,sample_rate,channels,byte_count\n";
            for (const auto& record : audio_frame_model_->records()) {
                out << record.index << ',' << QString::number(record.timestamp_seconds, 'f', 6) << ',' << record.pts
                    << ',' << record.sample_count << ',' << record.sample_rate << ',' << record.channels << ','
                    << record.byte_count << '\n';
            }
        },
        rows);
}

} // namespace ui
} // namespace videoeye
