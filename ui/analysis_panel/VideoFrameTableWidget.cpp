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

#include <algorithm>

// 帧类型常量（AV_PICTURE_TYPE_*）。原先由被移除的 core/analysis 头文件间接带入；
// 现在 UI 直接依赖 FFmpeg 公共常量，显式 include（与"静态库 PRIVATE 不传 include
// 目录"的一致）。
#include <libavcodec/avcodec.h>

namespace videoeye {
namespace ui {

namespace {
// GOP 段数远少于帧数（典型几百~几千），上限取 5000。
constexpr std::size_t kMaxGopSummaries = 5000;
} // namespace

VideoFrameTableWidget::VideoFrameTableWidget(QWidget* parent, std::size_t max_records)
    : QWidget(parent), max_records_(max_records) {
    SetupUi();
}

VideoFrameTableWidget::~VideoFrameTableWidget() = default;

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
    frame_filter_combo_->addItems({tr("全部帧"), tr("仅 I 帧"), tr("仅非 I 帧")});
    v_toolbar->addWidget(frame_filter_combo_);

    frame_summary_label_ = new QLabel(tr("总帧数: 0 | 显示: 0 | GOP: 0"), video_sub_);
    // 采集开关在「播放统计与帧包」页头（整页一个聚合开关），子页只留筛选与摘要。
    v_toolbar->addWidget(frame_summary_label_, 1);
    v_layout->addLayout(v_toolbar);

    QGroupBox* table_group = new QGroupBox(tr("视频帧信息"), video_sub_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);
    // 视频帧列名升级 + 删除冗余列 (原 7 列 → 6 列, 去掉了「关键帧」列, 该信息
    // 已由「帧类型」(I) 覆盖; 同时将协议术语 PTS 改为更易懂的「原始 PTS」)
    using Spec = RecordTableModel<VideoFrameRecord>::ColumnSpec;
    std::vector<Spec> columns;
    columns.push_back(Spec{tr("#"), [](const VideoFrameRecord& r) { return QString::number(r.index); }, {}});
    columns.push_back(
        Spec{tr("帧类型"), [this](const VideoFrameRecord& r) { return FrameTypeToString(r.frame_type); }, {}});
    columns.push_back(Spec{
        tr("播放时间(s)"), [](const VideoFrameRecord& r) { return QString::number(r.timestamp_seconds, 'f', 3); }, {}});
    columns.push_back(Spec{tr("原始 PTS"), [](const VideoFrameRecord& r) { return QString::number(r.pts); }, {}});
    columns.push_back(Spec{tr("GOP #"), [](const VideoFrameRecord& r) { return QString::number(r.gop_index); }, {}});
    columns.push_back(
        Spec{tr("GOP 内"), [](const VideoFrameRecord& r) { return QString::number(r.gop_position); }, {}});
    frame_model_ = new RecordTableModel<VideoFrameRecord>(std::move(columns), this);

    frame_table_ = new QTableView(table_group);
    frame_table_->setModel(frame_model_);
    frame_table_->verticalHeader()->setVisible(false);
    frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    frame_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    // QSS 里已定义 alternate-background-color，但只有显式开启交替行色才会生效
    frame_table_->setAlternatingRowColors(true);
    frame_table_->setSortingEnabled(false);
    frame_table_->horizontalHeader()->setStretchLastSection(true);
    frame_table_->horizontalHeader()->setMinimumSectionSize(40);
    frame_table_->setColumnWidth(0, 60);  // #
    frame_table_->setColumnWidth(1, 70);  // 帧类型
    frame_table_->setColumnWidth(2, 110); // 播放时间(s)
    frame_table_->setColumnWidth(3, 110); // 原始 PTS
    frame_table_->setColumnWidth(4, 70);  // GOP #
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
    g_layout->addLayout(g_toolbar);

    QGroupBox* g_table_group = new QGroupBox(tr("GOP 分段统计"), gop_sub_);
    QVBoxLayout* g_table_layout = new QVBoxLayout(g_table_group);
    // GOP 列名升级 (列数保持 9 列, 起止类列改为「帧号」「时间(s)」表述, 与其它表统一)
    gop_table_ = new QTableWidget(0, 9, g_table_group);
    gop_table_->setHorizontalHeaderLabels(
        {"GOP #", "起始帧号", "结束帧号", "起始(s)", "结束(s)", "总帧数", "I", "P", "B"});
    gop_table_->verticalHeader()->setVisible(false);
    gop_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gop_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gop_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    gop_table_->setAlternatingRowColors(true);
    gop_table_->setSortingEnabled(false);
    gop_table_->horizontalHeader()->setStretchLastSection(true);
    gop_table_->setMinimumWidth(500);
    gop_table_->setMinimumHeight(120);
    g_table_layout->addWidget(gop_table_);
    g_layout->addWidget(g_table_group);

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this, [this](int) { OnFrameFilterChanged(); });
    // GOP 行双击 → 请求跳到该段起始帧；切 tab 与选中由 FramePacketView 完成（它持有两侧）。
    connect(gop_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || static_cast<std::size_t>(row) >= gop_summaries_.size()) {
            return;
        }
        emit GopActivated(gop_summaries_[static_cast<std::size_t>(row)].start_frame);
    });
    // 帧表的 itemSelectionChanged 不在这里连：帧表 ↔ 包表按 PTS 互跳要同时碰两张表，
    // 连线留在 FramePacketView（它持有两侧）。
}

const std::vector<VideoFrameRecord>& VideoFrameTableWidget::FrameRecords() const {
    return frame_model_->records();
}

bool VideoFrameTableWidget::SelectFrameByIndex(int frame_index) {
    if (!frame_model_ || !frame_table_) {
        return false;
    }
    const auto& records = frame_model_->records();
    // 记录按帧号递增入队，可以用二分定位；筛选生效时行号 ≠ 记录索引，
    // 所以先找回记录索引，再交给模型换成可见行号。
    const auto it = std::lower_bound(records.begin(), records.end(), frame_index,
                                     [](const VideoFrameRecord& record, int value) { return record.index < value; });
    if (it == records.end() || it->index != frame_index) {
        return false; // 该帧已被裁剪或本就没采到
    }
    const std::size_t record_index = static_cast<std::size_t>(std::distance(records.begin(), it));
    const int row = frame_model_->RowOfRecord(record_index);
    if (row < 0) {
        return false; // 当前筛选把它藏起来了
    }
    frame_table_->selectRow(row);
    frame_table_->scrollTo(frame_model_->index(row, 0));
    return true;
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
        // 统一在这里发一次，避免每个 GOP 边界都拷一次向量。
        emit GopSummariesChanged();
    }
    if (frame_summary_dirty_) {
        UpdateFrameSummary();
        frame_summary_dirty_ = false;
    }
}

void VideoFrameTableWidget::ResetVideoFrames() {
    pending_frames_.clear();
    last_gop_index_ = 0;
    last_gop_position_ = 0;
    gop_summaries_.clear();
    gop_table_synced_count_ = 0;
    key_count_ = 0;
    visible_count_ = 0;
    frame_table_dirty_ = false;
    // 清空 GOP 摘要后仍要走一次刷新：FlushPending 会据此发出 GopSummariesChanged()，
    // 让「流概览」区的最大 GOP 指标与分布曲线一并归零（两份缓存必须一起失效）。
    gop_table_dirty_ = true;
    frame_summary_dirty_ = true;
    if (frame_model_) {
        frame_model_->Clear();
    }
    if (gop_table_) {
        gop_table_->setRowCount(0);
    }
    UpdateFrameSummary();
}

void VideoFrameTableWidget::AppendVideoFrame(int index, int frame_type, bool is_key_frame, qint64 pts,
                                             double timestamp_seconds) {
    if (!frame_model_ || !gop_table_) {
        return;
    }

    VideoFrameRecord record;
    record.index = index;
    record.frame_type = frame_type;
    record.is_key_frame = is_key_frame;
    record.pts = pts;
    record.timestamp_seconds = timestamp_seconds;

    // 汇总计数在这里增量累加（记录最终都会进模型；Reset 会一并清零）。
    if (is_key_frame) {
        ++key_count_;
    }
    if (MatchesFrameFilter(record)) {
        ++visible_count_;
    }

    // 只入队 + 置脏：GOP 归属（gop_index / gop_position）与 GOP 摘要是**派生结果**，
    // 放到 FlushPending 里连同批量提交一起算 —— 每帧都回头翻一遍记录缓存没必要，
    // 而且 pending 还没进模型时"上一条记录"根本查不到。
    pending_frames_.push_back(record);
    frame_table_dirty_ = true;
    frame_summary_dirty_ = true;
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
    case 2:
        return record.frame_type != AV_PICTURE_TYPE_I;
    default:
        return true;
    }
}

bool VideoFrameTableWidget::MatchesFrameFilterIsActive() const {
    return frame_filter_combo_ != nullptr && frame_filter_combo_->currentIndex() != 0;
}

// 一条新帧落定后更新 GOP 摘要。
//
// 与帧记录同源同步：GOP 段是从 pict_type 推导出来的，所以只能在这里（帧真正被
// 接受并算出 GOP 归属之后）更新，不能等到刷 GOP 表时再算 —— 那样"段边界"和
// "帧的 gop_index"就是两个独立算出来的东西，迟早对不上。
void VideoFrameTableWidget::UpdateGopSummariesFor(const VideoFrameRecord& record) {
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
    TrimRecords(gop_summaries_, gop_table_synced_count_, gop_table_, gop_table_dirty_, kMaxGopSummaries);
}

// 筛选条件变了：让模型按新条件重算可见行（记录本身不动）。
void VideoFrameTableWidget::ApplyFrameFilter() {
    if (!frame_model_)
        return;
    if (!MatchesFrameFilterIsActive()) {
        frame_model_->SetFilter(nullptr);
        return;
    }
    // 捕获筛选模式的值而不是 this：谓词会被模型长期持有，捕获 this 的话
    // 每次判定都要解引用一个可能已经析构的组件。
    const int mode = frame_filter_combo_ ? frame_filter_combo_->currentIndex() : 0;
    frame_model_->SetFilter([mode](const VideoFrameRecord& r) {
        if (mode == 1) {
            return r.frame_type == AV_PICTURE_TYPE_I;
        }
        if (mode == 2) {
            return r.frame_type != AV_PICTURE_TYPE_I;
        }
        return true;
    });
}

void VideoFrameTableWidget::UpdateFrameSummary() {
    if (!frame_summary_label_ || !frame_model_) {
        return;
    }

    // 汇总只读增量计数器，不再每次刷新都扫一遍全部帧记录。
    frame_summary_label_->setText(tr("总帧数: %1 | 显示: %2 | 关键帧: %3 | GOP: %4")
                                      .arg(frame_model_->record_count())
                                      .arg(visible_count_)
                                      .arg(key_count_)
                                      .arg(gop_summaries_.size()));
}

void VideoFrameTableWidget::OnFrameFilterChanged() {
    ApplyFrameFilter();
    // 筛选条件变了：可见行数按新条件重算一次（仅在用户操作时发生，不必增量）。
    // pending 里的帧还没进模型，但同样要按新条件计入，否则下次 Flush 前显示的
    // 「显示」会少一截。
    visible_count_ = 0;
    if (frame_model_) {
        for (const auto& record : frame_model_->records()) {
            if (MatchesFrameFilter(record)) {
                ++visible_count_;
            }
        }
    }
    for (const auto& record : pending_frames_) {
        if (MatchesFrameFilter(record)) {
            ++visible_count_;
        }
    }
    UpdateFrameSummary();
}

void VideoFrameTableWidget::FlushPendingFrameTableUpdates() {
    if (!frame_model_ || !frame_table_) {
        return;
    }

    // 用户正在往回翻看时不要每 120ms 把他拽回底部；只有本来就跟随底部才继续跟随。
    const bool pinned_to_bottom = IsTablePinnedToBottom(frame_table_);

    if (!pending_frames_.empty()) {
        // 派生 GOP 归属：只依赖"上一条帧落在哪个 GOP 的第几位"，
        // 用成员游标而不是回查记录缓存 —— 记录被裁剪后索引会整体前移，
        // 回查会算错。
        for (auto& record : pending_frames_) {
            if (last_gop_index_ == 0) {
                record.gop_index = 1;
                record.gop_position = 1;
            } else if (record.is_key_frame) {
                record.gop_index = last_gop_index_ + 1;
                record.gop_position = 1;
            } else {
                record.gop_index = last_gop_index_;
                record.gop_position = last_gop_position_ + 1;
            }
            last_gop_index_ = record.gop_index;
            last_gop_position_ = record.gop_position;

            UpdateGopSummariesFor(record);
        }
        frame_model_->AppendRecords(std::move(pending_frames_));
        pending_frames_.clear();
    }

    const std::size_t count = frame_model_->record_count();
    if (count > max_records_) {
        const std::size_t remove = count - max_records_;
        // 先按将被删掉的记录扣减汇总计数，再真正裁剪。
        const auto& records = frame_model_->records();
        const std::size_t n = std::min(remove, records.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (records[i].is_key_frame && key_count_ > 0) {
                --key_count_;
            }
            if (MatchesFrameFilter(records[i]) && visible_count_ > 0) {
                --visible_count_;
            }
        }
        frame_model_->TrimFront(remove);
        // 帧记录被裁剪时同步清理 GOP 数据：GOP 是从帧记录推导的派生结果，
        // 帧都没了却留着 GOP 段，界面上就会出现"GOP 5 的帧一条都找不到"。
        gop_summaries_.clear();
        gop_table_synced_count_ = 0;
        if (gop_table_)
            gop_table_->setRowCount(0);
        gop_table_dirty_ = true;
        frame_summary_dirty_ = true;
    }

    FollowTableBottom(frame_table_, pinned_to_bottom);
}

void VideoFrameTableWidget::FlushPendingGopTableUpdates() {
    if (!gop_table_ || gop_summaries_.empty()) {
        return;
    }

    gop_table_->setUpdatesEnabled(false);
    while (gop_table_synced_count_ < gop_summaries_.size()) {
        gop_table_->insertRow(static_cast<int>(gop_table_synced_count_));
        UpdateGopRowInTable(static_cast<int>(gop_table_synced_count_), gop_summaries_[gop_table_synced_count_]);
        ++gop_table_synced_count_;
    }

    const int last_row = static_cast<int>(gop_summaries_.size()) - 1;
    UpdateGopRowInTable(last_row, gop_summaries_.back());
    gop_table_->setUpdatesEnabled(true);
}

void VideoFrameTableWidget::OnExportFrameCsv() {
    if (!frame_model_ || frame_model_->record_count() == 0) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的视频帧数据。"));
        return;
    }

    const int rows = static_cast<int>(frame_model_->record_count());
    ExportCsvStream(
        this, tr("导出视频帧 CSV"),
        QString("videoeye_frames_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,frame_type,timestamp_seconds,pts,gop_index,gop_position\n";
            for (const auto& record : frame_model_->records()) {
                out << record.index << ',' << FrameTypeToString(record.frame_type) << ','
                    << QString::number(record.timestamp_seconds, 'f', 6) << ',' << record.pts << ',' << record.gop_index
                    << ',' << record.gop_position << '\n';
            }
        },
        rows);
}

void VideoFrameTableWidget::OnExportGopCsv() {
    if (gop_summaries_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 GOP 数据。"));
        return;
    }

    ExportCsvStream(
        this, tr("导出 GOP CSV"),
        QString("videoeye_gop_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "gop_index,start_frame,end_frame,start_ts,end_ts,total_frames,key_count,"
                   "i_count,p_count,b_count\n";
            for (const auto& g : gop_summaries_) {
                out << g.gop_index << ',' << g.start_frame << ',' << g.end_frame << ','
                    << QString::number(g.start_ts, 'f', 6) << ',' << QString::number(g.end_ts, 'f', 6) << ','
                    << g.total_frames << ',' << g.key_count << ',' << g.i_count << ',' << g.p_count << ',' << g.b_count
                    << '\n';
            }
        },
        static_cast<int>(gop_summaries_.size()));
}

// 与 SetupUi GOP 表表头一一对应 (9 列):
// GOP #/起始帧号/结束帧号/起始(s)/结束(s)/总帧数/I/P/B
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

} // namespace ui
} // namespace videoeye
