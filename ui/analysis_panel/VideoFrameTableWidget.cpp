#include "ui/analysis_panel/VideoFrameTableWidget.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

// 帧类型常量（AV_PICTURE_TYPE_*）。原先由被移除的 core/analysis 头文件间接带入；
// 现在 UI 直接依赖 FFmpeg 公共常量，显式 include（与"静态库 PRIVATE 不传 include
// 目录"的一致）。
#include <libavcodec/avcodec.h>

namespace videoeye {
namespace ui {

namespace {
constexpr std::size_t kMaxFrameRecords = 50000;
// GOP 段数远少于帧数（典型几百~几千），上限取帧上限的 1/10 足够。
constexpr std::size_t kMaxGopSummaries = kMaxFrameRecords / 10;
}  // namespace

VideoFrameTableWidget::VideoFrameTableWidget(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

void VideoFrameTableWidget::SetupUi() {
    // 本组件自身是 QWidget 但不可见：它只是两个子页 widget 的宿主，由父页把
    // videoFramePage() / gopPage() 加到自己的 QTabWidget 上。
    setVisible(false);

    // ---- 子页 0: 视频帧 ----
    video_sub_ = new QWidget(this);
    QVBoxLayout* v_layout = new QVBoxLayout(video_sub_);
    v_layout->setContentsMargins(4, 4, 4, 4);
    v_layout->setSpacing(4);

    QHBoxLayout* v_toolbar = new QHBoxLayout();
    v_toolbar->addWidget(new QLabel(tr("筛选:"), video_sub_));
    frame_filter_combo_ = new QComboBox(video_sub_);
    frame_filter_combo_->addItems({tr("全部帧"), tr("仅 I 帧")});
    v_toolbar->addWidget(frame_filter_combo_);

    frame_summary_label_ = new QLabel(tr("总帧数: 0 | 显示: 0 | GOP: 0"), video_sub_);
    v_toolbar->addWidget(frame_summary_label_, 1);

    export_frame_csv_button_ = new QPushButton(tr("导出 CSV"), video_sub_);
    v_toolbar->addWidget(export_frame_csv_button_);

    video_toggle_ = new QCheckBox(tr("启用分析"), video_sub_);
    video_toggle_->setToolTip(tr("播放时实时采集，复用播放所需数据，几乎无额外开销，默认开启。"));
    v_toolbar->addWidget(video_toggle_);
    v_layout->addLayout(v_toolbar);

    QGroupBox* table_group = new QGroupBox(tr("视频帧信息"), video_sub_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);
    // 视频帧列名升级 + 删除冗余列 (原 7 列 → 6 列, 去掉了「关键帧」列, 该信息
    // 已由「帧类型」(I) 覆盖; 同时将协议术语 PTS 改为更易懂的「原始 PTS」)
    frame_table_ = new QTableWidget(0, 6, table_group);
    frame_table_->setHorizontalHeaderLabels({
        "#", "帧类型", "播放时间(s)", "原始 PTS", "GOP #", "GOP 内"
    });
    frame_table_->verticalHeader()->setVisible(false);
    frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    frame_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    frame_table_->setSortingEnabled(false);
    frame_table_->horizontalHeader()->setStretchLastSection(true);
    frame_table_->horizontalHeader()->setMinimumSectionSize(40);
    frame_table_->setColumnWidth(0, 60);   // #
    frame_table_->setColumnWidth(1, 70);   // 帧类型
    frame_table_->setColumnWidth(2, 110);  // 播放时间(s)
    frame_table_->setColumnWidth(3, 110);  // 原始 PTS
    frame_table_->setColumnWidth(4, 70);   // GOP #
    frame_table_->setMinimumWidth(400);
    frame_table_->setMinimumHeight(120);
    table_layout->addWidget(frame_table_);
    v_layout->addWidget(table_group);

    // ---- 子页 2: GOP 摘要 ----
    gop_sub_ = new QWidget(this);
    QVBoxLayout* g_layout = new QVBoxLayout(gop_sub_);
    g_layout->setContentsMargins(4, 4, 4, 4);
    g_layout->setSpacing(4);

    QHBoxLayout* g_toolbar = new QHBoxLayout();
    g_toolbar->addWidget(new QLabel(tr("按解码帧 pict_type 统计的 GOP 摘要"), gop_sub_), 1);
    QPushButton* g_export_btn = new QPushButton(tr("导出 CSV"), gop_sub_);
    g_toolbar->addWidget(g_export_btn);
    g_layout->addLayout(g_toolbar);

    QGroupBox* g_table_group = new QGroupBox(tr("GOP 分段统计"), gop_sub_);
    QVBoxLayout* g_table_layout = new QVBoxLayout(g_table_group);
    // GOP 列名升级 (列数保持 9 列, 起止类列改为「帧号」「时间(s)」表述, 与其它表统一)
    gop_table_ = new QTableWidget(0, 9, g_table_group);
    gop_table_->setHorizontalHeaderLabels({
        "GOP #", "起始帧号", "结束帧号", "起始(s)", "结束(s)", "总帧数", "I", "P", "B"
    });
    gop_table_->verticalHeader()->setVisible(false);
    gop_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gop_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gop_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    gop_table_->setSortingEnabled(false);
    gop_table_->horizontalHeader()->setStretchLastSection(true);
    gop_table_->setMinimumWidth(500);
    gop_table_->setMinimumHeight(120);
    g_table_layout->addWidget(gop_table_);
    g_layout->addWidget(g_table_group);

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) { OnFrameFilterChanged(); });
    connect(export_frame_csv_button_, &QPushButton::clicked,
            this, &VideoFrameTableWidget::OnExportFrameCsv);
    connect(g_export_btn, &QPushButton::clicked,
            this, &VideoFrameTableWidget::OnExportGopCsv);
    // 帧表的 itemSelectionChanged 不在这里连：帧表 ↔ 包表按 PTS 互跳要同时碰两张表，
    // 连线留在 FramePacketView（它持有两侧）。
}

bool VideoFrameTableWidget::HasPending() const {
    return frame_table_dirty_ || gop_table_dirty_ || frame_summary_dirty_;
}

void VideoFrameTableWidget::FlushPending() {
    if (frame_table_dirty_) {
        FlushPendingFrameTableUpdates();
        frame_table_dirty_ = false;
    }
    if (gop_table_dirty_) {
        FlushPendingGopTableUpdates();
        gop_table_dirty_ = false;
        // GOP 摘要变化要通知「流概览」区（最大 GOP 指标 + GOP 分布曲线），
        // 统一在这里发一次，避免每个 GOP 边界都拷一遍向量。
        emit GopSummariesChanged();
    }
    if (frame_summary_dirty_) {
        UpdateFrameSummary();
        frame_summary_dirty_ = false;
    }
}

void VideoFrameTableWidget::ResetVideoFrames() {
    frame_records_.clear();
    gop_summaries_.clear();
    frame_table_synced_record_count_ = 0;
    gop_table_synced_count_ = 0;
    frame_table_dirty_ = false;
    // 清空 GOP 摘要后仍要走一次刷新：FlushPending 会据此发出 GopSummariesChanged()，
    // 让「流概览」区的最大 GOP 指标与分布曲线一并归零（两份缓存必须一起失效）。
    gop_table_dirty_ = true;
    frame_summary_dirty_ = true;
    if (frame_table_) {
        frame_table_->setRowCount(0);
    }
    if (gop_table_) {
        gop_table_->setRowCount(0);
    }
    UpdateFrameSummary();
}

void VideoFrameTableWidget::AppendVideoFrame(int index, int frame_type, bool is_key_frame,
                                             qint64 pts, double timestamp_seconds) {
    if (!frame_table_ || !gop_table_) {
        return;
    }

    VideoFrameRecord record;
    record.index = index;
    record.frame_type = frame_type;
    record.is_key_frame = is_key_frame;
    record.pts = pts;
    record.timestamp_seconds = timestamp_seconds;

    if (frame_records_.empty()) {
        record.gop_index = 1;
        record.gop_position = 1;
    } else {
        const VideoFrameRecord& last_record = frame_records_.back();
        if (is_key_frame) {
            record.gop_index = last_record.gop_index + 1;
            record.gop_position = 1;
        } else {
            record.gop_index = last_record.gop_index;
            record.gop_position = last_record.gop_position + 1;
        }
    }

    frame_records_.push_back(record);
    frame_table_dirty_ = true;
    frame_summary_dirty_ = true;
    {
        const std::size_t old_size = frame_records_.size();
        TrimRecords(frame_records_, frame_table_synced_record_count_, frame_table_,
                    frame_table_dirty_, kMaxFrameRecords);
        if (frame_records_.size() != old_size) {
            // 帧记录被裁剪时同步清理GOP数据
            gop_summaries_.clear();
            gop_table_synced_count_ = 0;
            if (gop_table_) gop_table_->setRowCount(0);
            gop_table_dirty_ = true;
        }
    }

    if (gop_summaries_.empty() || record.gop_position == 1) {
        GopSummary summary;
        summary.gop_index = record.gop_index;
        summary.start_frame = record.index;
        summary.end_frame = record.index;
        summary.start_ts = record.timestamp_seconds;
        summary.end_ts = record.timestamp_seconds;
        summary.total_frames = 1;
        summary.key_count = record.is_key_frame ? 1 : 0;
        if (record.frame_type == AV_PICTURE_TYPE_I) {
            summary.i_count = 1;
        } else if (record.frame_type == AV_PICTURE_TYPE_P) {
            summary.p_count = 1;
        } else if (record.frame_type == AV_PICTURE_TYPE_B) {
            summary.b_count = 1;
        }
        gop_summaries_.push_back(summary);
    } else {
        GopSummary& summary = gop_summaries_.back();
        summary.end_frame = record.index;
        summary.end_ts = record.timestamp_seconds;
        summary.total_frames++;
        if (record.is_key_frame) {
            summary.key_count++;
        }
        if (record.frame_type == AV_PICTURE_TYPE_I) {
            summary.i_count++;
        } else if (record.frame_type == AV_PICTURE_TYPE_P) {
            summary.p_count++;
        } else if (record.frame_type == AV_PICTURE_TYPE_B) {
            summary.b_count++;
        }
    }
    gop_table_dirty_ = true;
    TrimRecords(gop_summaries_, gop_table_synced_count_, gop_table_,
                gop_table_dirty_, kMaxGopSummaries);
}

QString VideoFrameTableWidget::FrameTypeToString(int frame_type) const {
    if (frame_type == AV_PICTURE_TYPE_I) {
        return "I";
    }
    if (frame_type == AV_PICTURE_TYPE_P) {
        return "P";
    }
    if (frame_type == AV_PICTURE_TYPE_B) {
        return "B";
    }
    return "?";
}

bool VideoFrameTableWidget::MatchesFrameFilter(const VideoFrameRecord& record) const {
    if (!frame_filter_combo_) {
        return true;
    }

    switch (frame_filter_combo_->currentIndex()) {
    case 1:
        return record.frame_type == AV_PICTURE_TYPE_I;
    default:
        return true;
    }
}

void VideoFrameTableWidget::RebuildFrameTable() {
    if (!frame_table_) {
        return;
    }

    frame_table_->setUpdatesEnabled(false);
    frame_table_->setRowCount(0);
    for (const auto& record : frame_records_) {
        if (!MatchesFrameFilter(record)) {
            continue;
        }
        AppendFrameRowToTable(record);
    }
    frame_table_->setUpdatesEnabled(true);
    frame_table_synced_record_count_ = frame_records_.size();
}

void VideoFrameTableWidget::UpdateFrameSummary() {
    if (!frame_summary_label_) {
        return;
    }

    int visible_count = 0;
    for (const auto& record : frame_records_) {
        if (MatchesFrameFilter(record)) {
            visible_count++;
        }
    }

    int key_count = 0;
    for (const auto& record : frame_records_) {
        if (record.is_key_frame) {
            key_count++;
        }
    }

    frame_summary_label_->setText(
        tr("总帧数: %1 | 显示: %2 | 关键帧: %3 | GOP: %4")
            .arg(frame_records_.size())
            .arg(visible_count)
            .arg(key_count)
            .arg(gop_summaries_.size()));
}

void VideoFrameTableWidget::OnFrameFilterChanged() {
    RebuildFrameTable();
    UpdateFrameSummary();
}

void VideoFrameTableWidget::FlushPendingFrameTableUpdates() {
    if (!frame_table_) {
        return;
    }

    frame_table_->setUpdatesEnabled(false);
    for (std::size_t i = frame_table_synced_record_count_; i < frame_records_.size(); ++i) {
        if (!MatchesFrameFilter(frame_records_[i])) {
            continue;
        }
        AppendFrameRowToTable(frame_records_[i]);
    }
    frame_table_->setUpdatesEnabled(true);
    frame_table_synced_record_count_ = frame_records_.size();

    if (frame_table_->rowCount() > 0) {
        frame_table_->scrollToBottom();
    }
}

void VideoFrameTableWidget::FlushPendingGopTableUpdates() {
    if (!gop_table_ || gop_summaries_.empty()) {
        return;
    }

    gop_table_->setUpdatesEnabled(false);
    while (gop_table_synced_count_ < gop_summaries_.size()) {
        gop_table_->insertRow(static_cast<int>(gop_table_synced_count_));
        UpdateGopRowInTable(static_cast<int>(gop_table_synced_count_),
                            gop_summaries_[gop_table_synced_count_]);
        ++gop_table_synced_count_;
    }

    const int last_row = static_cast<int>(gop_summaries_.size()) - 1;
    UpdateGopRowInTable(last_row, gop_summaries_.back());
    gop_table_->setUpdatesEnabled(true);
}

// 与 SetupUi 帧表表头一一对应 (6 列): #/帧类型/播放时间(s)/原始 PTS/GOP #/GOP 内
void VideoFrameTableWidget::AppendFrameRowToTable(const VideoFrameRecord& record) {
    const int row = frame_table_->rowCount();
    frame_table_->insertRow(row);
    SetTableItemText(frame_table_, row, 0, QString::number(record.index));
    SetTableItemText(frame_table_, row, 1, FrameTypeToString(record.frame_type));
    SetTableItemText(frame_table_, row, 2, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(frame_table_, row, 3, QString::number(record.pts));
    SetTableItemText(frame_table_, row, 4, QString::number(record.gop_index));
    SetTableItemText(frame_table_, row, 5, QString::number(record.gop_position));
}

void VideoFrameTableWidget::UpdateGopRowInTable(int row, const GopSummary& summary) {
    SetTableItemText(gop_table_, row, 0, QString::number(summary.gop_index));
    SetTableItemText(gop_table_, row, 1, QString::number(summary.start_frame));
    SetTableItemText(gop_table_, row, 2, QString::number(summary.end_frame));
    SetTableItemText(gop_table_, row, 3, QString::number(summary.start_ts, 'f', 3));
    SetTableItemText(gop_table_, row, 4, QString::number(summary.end_ts, 'f', 3));
    SetTableItemText(gop_table_, row, 5, QString::number(summary.total_frames));
    SetTableItemText(gop_table_, row, 6, QString::number(summary.i_count));
    SetTableItemText(gop_table_, row, 7, QString::number(summary.p_count));
    SetTableItemText(gop_table_, row, 8, QString::number(summary.b_count));
}

void VideoFrameTableWidget::OnExportFrameCsv() {
    if (frame_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的帧分析数据。"));
        return;
    }

    ExportCsvStream(this,
        tr("导出视频帧 CSV"),
        QString("videoeye_frames_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,frame_type,is_key_frame,timestamp_seconds,pts,gop_index,gop_position\n";
            for (const auto& record : frame_records_) {
                out << record.index << ','
                    << FrameTypeToString(record.frame_type) << ','
                    << (record.is_key_frame ? 1 : 0) << ','
                    << QString::number(record.timestamp_seconds, 'f', 6) << ','
                    << record.pts << ','
                    << record.gop_index << ','
                    << record.gop_position << '\n';
            }
        },
        static_cast<int>(frame_records_.size()));
}

void VideoFrameTableWidget::OnExportGopCsv() {
    if (gop_summaries_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 GOP 摘要数据。"));
        return;
    }

    ExportCsvStream(this,
        tr("导出 GOP 摘要 CSV"),
        QString("videoeye_gop_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "gop_index,start_frame,end_frame,start_ts,end_ts,total_frames,i_count,p_count,b_count,key_count\n";
            for (const auto& s : gop_summaries_) {
                out << s.gop_index << ','
                    << s.start_frame << ','
                    << s.end_frame << ','
                    << QString::number(s.start_ts, 'f', 6) << ','
                    << QString::number(s.end_ts, 'f', 6) << ','
                    << s.total_frames << ','
                    << s.i_count << ','
                    << s.p_count << ','
                    << s.b_count << ','
                    << s.key_count << '\n';
            }
        },
        static_cast<int>(gop_summaries_.size()));
}

}  // namespace ui
}  // namespace videoeye
