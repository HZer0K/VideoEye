#include "ui/analysis_panel/FramePacketView.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/analysis_panel/AudioFrameTableWidget.h"
#include "ui/analysis_panel/VideoFrameTableWidget.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

// 帧类型 / 包标志 / 媒体类型常量（AV_PICTURE_TYPE_* / AV_PKT_FLAG_* / AVMEDIA_TYPE_*）。
// 原先由被移除的 core/analysis 头文件间接带入；现在 UI 直接依赖 FFmpeg 公共常量，
// 显式 include（与"静态库 PRIVATE 不传 include 目录"的一致）。
#include <libavcodec/avcodec.h>

#include <climits>
#include <cstdint>
#include <limits>

namespace videoeye {
namespace ui {

namespace {
constexpr std::size_t kMaxPacketRecords = 10000;
}  // namespace
// 注：帧记录上限与 TrimRecords 已随视频帧/音频帧子页抽出 —— TrimRecords 提到
// AnalysisPageSupport.h（视频帧/GOP/包/音频帧四张表共用同一套裁剪语义）。

FramePacketView::FramePacketView(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

const std::vector<GopSummary>& FramePacketView::GopSummaries() const {
    return video_page_->GopSummaries();
}

void FramePacketView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setDocumentMode(true);

    // ---- 子页 0: 视频帧 / 子页 2: GOP 摘要 ----
    // 已抽为独立组件 VideoFrameTableWidget：帧记录缓存、GOP 推导、脏标志、增量游标、
    // CSV 导出全在组件内，本页只负责建页 + 转发数据 + 转发 flush + 转发 GOP 变化信号。
    // GOP 必须和视频帧住在一起：它是从帧的 pict_type/is_key_frame 推导出来的派生数据，
    // 帧记录被裁剪时 GOP 要一并清空重建。
    video_page_ = new VideoFrameTableWidget(this);
    // GOP 摘要是组件产出的派生数据，原样转发给面板（面板再转「流概览」区）。
    connect(video_page_, &VideoFrameTableWidget::GopSummariesChanged,
            this, &FramePacketView::GopSummariesChanged);

    // ---- 子页 1: 包 ----
    packet_sub_ = new QWidget(sub_tabs_);
    QVBoxLayout* p_layout = new QVBoxLayout(packet_sub_);
    p_layout->setContentsMargins(4, 4, 4, 4);
    p_layout->setSpacing(4);

    QHBoxLayout* p_toolbar = new QHBoxLayout();
    packet_summary_label_ = new QLabel(tr("总包数: 0 | 视频包: 0 | 音频包: 0 | 其他包: 0"), packet_sub_);
    p_toolbar->addWidget(packet_summary_label_, 1);

    p_toolbar->addWidget(new QLabel(tr("筛选:"), packet_sub_));
    packet_filter_combo_ = new QComboBox(packet_sub_);
    packet_filter_combo_->addItems({tr("全部流"), tr("视频流"), tr("音频流"), tr("其他流")});
    packet_filter_combo_->setCurrentIndex(0);
    p_toolbar->addWidget(packet_filter_combo_);

    export_packet_csv_button_ = new QPushButton(tr("导出 CSV"), packet_sub_);
    p_toolbar->addWidget(export_packet_csv_button_);

    packet_toggle_ = new QCheckBox(tr("启用分析"), packet_sub_);
    p_toolbar->addWidget(packet_toggle_);
    p_layout->addLayout(p_toolbar);

    QGroupBox* p_table_group = new QGroupBox(tr("数据包信息"), packet_sub_);
    QVBoxLayout* p_table_layout = new QVBoxLayout(p_table_group);
    // 包表列名升级 + 删除面向开发者的列 (原 10 列 → 7 列, 去掉了「标记」「文件偏移」
    // 等非用户语义字段, 合并「流索引」+「流类型」为单列, 协议术语 PTS/DTS 括注用途)
    packet_table_ = new QTableWidget(0, 7, p_table_group);
    packet_table_->setHorizontalHeaderLabels({
        "#", "流", "播放时间(s)", "显示时间(PTS)", "解码时间(DTS)", "时长", "包大小"
    });
    packet_table_->verticalHeader()->setVisible(false);
    packet_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    packet_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    packet_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    packet_table_->setSortingEnabled(false);
    packet_table_->horizontalHeader()->setStretchLastSection(true);
    packet_table_->horizontalHeader()->setMinimumSectionSize(50);
    packet_table_->setColumnWidth(0, 60);   // #
    packet_table_->setColumnWidth(1, 90);   // 流
    packet_table_->setColumnWidth(2, 110);  // 播放时间(s)
    packet_table_->setColumnWidth(3, 110);  // 显示时间(PTS)
    packet_table_->setColumnWidth(4, 110);  // 解码时间(DTS)
    packet_table_->setColumnWidth(5, 80);   // 时长
    packet_table_->setColumnWidth(6, 90);   // 包大小
    packet_table_->setMinimumWidth(650);
    packet_table_->setMinimumHeight(120);
    p_table_layout->addWidget(packet_table_);
    p_layout->addWidget(p_table_group);

    // ---- 子页 3: 音频帧 ----
    // 已抽为独立组件 AudioFrameTableWidget：记录缓存 / 脏标志 / 增量游标 / CSV 导出
    // 全在组件内，本页只负责建页 + 转发数据 + 转发 flush。
    audio_page_ = new AudioFrameTableWidget(this);

    // 按原有顺序 addTab：tab 序号是帧表↔包表互跳（setCurrentIndex(0/1)）的依据，
    // 不能因为把子页挪进组件就变。
    sub_tabs_->addTab(video_page_->videoFramePage(), tr("视频帧"));
    sub_tabs_->addTab(packet_sub_, tr("包"));
    sub_tabs_->addTab(video_page_->gopPage(), tr("GOP 摘要"));
    sub_tabs_->addTab(audio_page_, tr("音频帧"));

    layout->addWidget(sub_tabs_);

    // 视频帧 / GOP / 音频帧三个子页内部的按钮与筛选框都连在各自组件内。
    connect(packet_filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        packet_filter_mode_ = idx - 1;  // 0->全部(-1), 1->视频(0), 2->音频(1), 3->其他(2)
        RebuildPacketTable();
    });
    connect(export_packet_csv_button_, &QPushButton::clicked, this, &FramePacketView::OnExportPacketCsv);
    connect(packet_table_, &QTableWidget::itemSelectionChanged, this, &FramePacketView::OnPacketTableSelectionChanged);

    // 帧表 ↔ 包表按 PTS 互跳要同时碰两张表，连线留在本页（组件只外露 frameTable()）。
    connect(video_page_->frameTable(), &QTableWidget::itemSelectionChanged,
            this, &FramePacketView::OnVideoFrameTableSelectionChanged);
}

void FramePacketView::SetFeatureHooks(std::function<bool(int)> is_enabled,
                                      std::function<void(int, bool)> set_enabled) {
    is_enabled_ = std::move(is_enabled);
    set_enabled_ = std::move(set_enabled);

    SyncTogglesFromHooks();

    const struct { QCheckBox* box; int id; } toggles[] = {
        {video_page_->toggle(), 0},
        {audio_page_->toggle(), 1},
        {packet_toggle_, 2},
    };
    for (const auto& t : toggles) {
        if (!t.box) continue;
        const int id = t.id;
        connect(t.box, &QCheckBox::toggled, this, [this, id](bool checked) {
            if (set_enabled_) set_enabled_(id, checked);
        });
    }
}

void FramePacketView::SyncTogglesFromHooks() {
    const struct { QCheckBox* box; int id; } toggles[] = {
        {video_page_->toggle(), 0},
        {audio_page_->toggle(), 1},
        {packet_toggle_, 2},
    };
    for (const auto& t : toggles) {
        if (!t.box) continue;
        // 没有钩子时按控件当前值保留（组件可脱离面板单独构造）
        const bool enabled = is_enabled_ ? is_enabled_(t.id) : t.box->isChecked();
        if (enabled == t.box->isChecked()) continue;
        const QSignalBlocker blocker(t.box);
        t.box->setChecked(enabled);
    }
}

bool FramePacketView::HasPending() const {
    return packet_table_dirty_ || packet_summary_dirty_ ||
           video_page_->HasPending() || audio_page_->HasPending();
}

void FramePacketView::FlushPending() {
    // 视频帧 / GOP 的脏标志与增量游标都在组件内，交给它自己 flush；
    // GopSummariesChanged 也由组件发出，本页只是转发给面板。
    video_page_->FlushPending();
    if (packet_table_dirty_) {
        FlushPendingPacketTableUpdates();
        packet_table_dirty_ = false;
    }
    // 音频帧的表/摘要脏标志与增量游标都在组件内，交给它自己 flush。
    audio_page_->FlushPending();
    if (packet_summary_dirty_) {
        UpdatePacketSummary();
        packet_summary_dirty_ = false;
    }
}

void FramePacketView::ResetVideoFrames() {
    video_page_->ResetVideoFrames();
}

void FramePacketView::ResetAudioFrames() {
    audio_page_->ResetAudioFrames();
}

void FramePacketView::ResetPackets() {
    packet_records_.clear();
    packet_table_synced_record_count_ = 0;
    packet_table_dirty_ = false;
    packet_summary_dirty_ = true;
    if (packet_table_) {
        packet_table_->setRowCount(0);
    }
    UpdatePacketSummary();
}

void FramePacketView::AppendVideoFrame(int index, int frame_type, bool is_key_frame,
                                       qint64 pts, double timestamp_seconds) {
    video_page_->AppendVideoFrame(index, frame_type, is_key_frame, pts, timestamp_seconds);
}

void FramePacketView::AppendAudioFrame(int index, qint64 pts, double timestamp_seconds,
                                       int sample_count, int sample_rate, int channels, int byte_count) {
    audio_page_->AppendAudioFrame(index, pts, timestamp_seconds,
                                  sample_count, sample_rate, channels, byte_count);
}

void FramePacketView::AppendPacket(const model::PacketInfo& packet_info) {
    if (!packet_table_) {
        return;
    }

    PacketRecord record;
    record.index = packet_info.index;
    record.stream_index = packet_info.stream_index;
    record.stream_type = packet_info.stream_type;
    record.pts = static_cast<qint64>(packet_info.pts);
    record.dts = static_cast<qint64>(packet_info.dts);
    record.duration = static_cast<qint64>(packet_info.duration);
    record.size = packet_info.size;
    record.flags = packet_info.flags;
    record.pos = static_cast<qint64>(packet_info.pos);
    record.timestamp_seconds = packet_info.timestamp_seconds;

    packet_records_.push_back(record);
    packet_table_dirty_ = true;
    packet_summary_dirty_ = true;
    TrimRecords(packet_records_, packet_table_synced_record_count_, packet_table_, packet_table_dirty_, kMaxPacketRecords);
}

QString FramePacketView::PacketFlagsToString(int flags) const {
    QStringList values;
    if ((flags & AV_PKT_FLAG_KEY) != 0) {
        values << tr("KEY");
    }
    if ((flags & AV_PKT_FLAG_CORRUPT) != 0) {
        values << tr("CORRUPT");
    }
    if ((flags & AV_PKT_FLAG_DISCARD) != 0) {
        values << tr("DISCARD");
    }
    if ((flags & AV_PKT_FLAG_TRUSTED) != 0) {
        values << tr("TRUSTED");
    }
    if ((flags & AV_PKT_FLAG_DISPOSABLE) != 0) {
        values << tr("DISPOSABLE");
    }
    return values.isEmpty() ? tr("-") : values.join('|');
}

QString FramePacketView::PacketStreamTypeToName(int type) const {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return tr("视频");
        case AVMEDIA_TYPE_AUDIO: return tr("音频");
        case AVMEDIA_TYPE_DATA: return tr("数据");
        case AVMEDIA_TYPE_SUBTITLE: return tr("字幕");
        case AVMEDIA_TYPE_ATTACHMENT: return tr("附件");
        default: return tr("未知");
    }
}

bool FramePacketView::PacketMatchesFilter(const PacketRecord& record) const {
    switch (packet_filter_mode_) {
        case 0: return record.stream_type == AVMEDIA_TYPE_VIDEO;
        case 1: return record.stream_type == AVMEDIA_TYPE_AUDIO;
        case 2: return record.stream_type != AVMEDIA_TYPE_VIDEO &&
                       record.stream_type != AVMEDIA_TYPE_AUDIO;
        default: return true;  // -1 = 全部
    }
}

void FramePacketView::RebuildPacketTable() {
    if (!packet_table_) {
        return;
    }

    packet_table_->setUpdatesEnabled(false);
    packet_table_->setRowCount(0);
    for (const auto& record : packet_records_) {
        AppendPacketRowToTable(record);
    }
    packet_table_->setUpdatesEnabled(true);
    packet_table_synced_record_count_ = packet_records_.size();
}

void FramePacketView::UpdatePacketSummary() {
    if (!packet_summary_label_) {
        return;
    }

    int video_packets = 0;
    int audio_packets = 0;
    int other_packets = 0;
    long long total_bytes = 0;
    long long sum_size = 0;
    int max_size = 0;
    int min_size = INT32_MAX;
    for (const auto& record : packet_records_) {
        if (record.stream_type == AVMEDIA_TYPE_VIDEO) {
            video_packets++;
        } else if (record.stream_type == AVMEDIA_TYPE_AUDIO) {
            audio_packets++;
        } else {
            other_packets++;
        }
        total_bytes += record.size;
        sum_size += record.size;
        if (record.size > max_size) {
            max_size = record.size;
        }
        if (record.size < min_size) {
            min_size = record.size;
        }
    }

    auto format_bytes = [](long long bytes) -> QString {
        if (bytes >= 1024LL * 1024 * 1024) {
            return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
        } else if (bytes >= 1024 * 1024) {
            return QString::number(bytes / (1024.0 * 1024), 'f', 2) + " MB";
        } else if (bytes >= 1024) {
            return QString::number(bytes / 1024.0, 'f', 1) + " KB";
        }
        return QString::number(bytes) + " B";
    };

    const int avg_size = packet_records_.empty() ? 0 : static_cast<int>(sum_size / packet_records_.size());

    packet_summary_label_->setText(
        tr("总包数: %1 | 总字节数: %2 | 视频包: %3 | 音频包: %4 | 其他包: %5 | 平均包大小: %6 B | 最大包大小: %7 B | 最小包大小: %8 B")
            .arg(packet_records_.size())
            .arg(format_bytes(total_bytes))
            .arg(video_packets)
            .arg(audio_packets)
            .arg(other_packets)
            .arg(avg_size)
            .arg(max_size)
            .arg(packet_records_.empty() ? 0 : min_size));
}

void FramePacketView::OnExportPacketCsv() {
    if (packet_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的包分析数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出包分析 CSV"),
        QString("videoeye_packets_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("CSV 文件 (*.csv);;所有文件 (*)"));
    if (filename.isEmpty()) {
        return;
    }

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "index,stream_index,timestamp_seconds,pts,dts,duration,size,flags,file_pos\n";
    for (const auto& record : packet_records_) {
        out << record.index << ','
            << record.stream_index << ','
            << QString::number(record.timestamp_seconds, 'f', 6) << ','
            << record.pts << ','
            << record.dts << ','
            << record.duration << ','
            << record.size << ','
            << '"' << PacketFlagsToString(record.flags) << '"' << ','
            << record.pos << '\n';
    }

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
}

void FramePacketView::FlushPendingPacketTableUpdates() {
    if (!packet_table_) {
        return;
    }

    packet_table_->setUpdatesEnabled(false);
    for (size_t i = packet_table_synced_record_count_; i < packet_records_.size(); ++i) {
        AppendPacketRowToTable(packet_records_[i]);
    }
    packet_table_->setUpdatesEnabled(true);
    packet_table_synced_record_count_ = packet_records_.size();

    if (packet_table_->rowCount() > 0) {
        packet_table_->scrollToBottom();
    }
}

// 与 SetupUi 帧表表头一一对应 (6 列): #/帧类型/播放时间(s)/原始 PTS/GOP #/GOP 内
// 与 SetupUi 包表表头一一对应 (7 列): #/流/播放时间(s)/显示时间(PTS)/解码时间(DTS)/时长/包大小
void FramePacketView::AppendPacketRowToTable(const PacketRecord& record) {
    if (!PacketMatchesFilter(record)) {
        return;
    }
    const int row = packet_table_->rowCount();
    packet_table_->insertRow(row);
    SetTableItemText(packet_table_, row, 0, QString::number(record.index));
    // 「流」列把流索引与类型合到一个单元格, 形如 "#0 视频"
    SetTableItemText(packet_table_, row, 1,
        QString("#%1 %2").arg(record.stream_index).arg(PacketStreamTypeToName(record.stream_type)));
    SetTableItemText(packet_table_, row, 2, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(packet_table_, row, 3, QString::number(record.pts));
    SetTableItemText(packet_table_, row, 4, QString::number(record.dts));
    SetTableItemText(packet_table_, row, 5, QString::number(record.duration));
    SetTableItemText(packet_table_, row, 6, QString::number(record.size));
}

// 包表选中 → 在视频帧记录中找 PTS 最接近的视频帧, 跳转并高亮
void FramePacketView::OnPacketTableSelectionChanged() {
    if (linking_) return;
    if (!packet_table_ || !video_page_) return;

    // 帧记录与帧表都在 VideoFrameTableWidget 里（帧表 ↔ 包表互跳要同时碰两侧）
    const std::vector<VideoFrameRecord>& frame_records = video_page_->FrameRecords();
    QTableWidget* frame_table = video_page_->frameTable();
    if (!frame_table) return;

    const int row = packet_table_->currentRow();
    if (row < 0 || row >= packet_table_->rowCount()) return;

    // 从表中读 PTS (原始 timebase 单位) 用于匹配
    QTableWidgetItem* pts_item = packet_table_->item(row, 3);  // 列 3 = 显示时间(PTS)
    if (!pts_item) return;
    bool ok = false;
    const qint64 target_pts = pts_item->text().toLongLong(&ok);
    if (!ok) return;

    // 在帧记录中找 PTS 最接近的记录
    int best_index = -1;
    qint64 best_diff = std::numeric_limits<qint64>::max();
    for (std::size_t i = 0; i < frame_records.size(); ++i) {
        const qint64 diff = std::llabs(frame_records[i].pts - target_pts);
        if (diff < best_diff) {
            best_diff = diff;
            best_index = static_cast<int>(i);
        }
    }
    if (best_index < 0) return;

    // 帧表可能被筛选过滤, 行号 != 下标. 反向查可见行.
    int target_visible_row = -1;
    for (int r = 0; r < frame_table->rowCount(); ++r) {
        QTableWidgetItem* pts_cell = frame_table->item(r, 3);  // 列 3 = 原始 PTS
        if (!pts_cell) continue;
        bool ok2 = false;
        const qint64 cell_pts = pts_cell->text().toLongLong(&ok2);
        if (ok2 && cell_pts == frame_records[best_index].pts) {
            target_visible_row = r;
            break;
        }
    }
    if (target_visible_row < 0) return;

    linking_ = true;
    frame_table->setCurrentCell(target_visible_row, 0);
    frame_table->scrollToItem(frame_table->item(target_visible_row, 0),
                              QAbstractItemView::PositionAtCenter);
    linking_ = false;

    // 自动切到「视频帧」子页
    if (sub_tabs_) {
        sub_tabs_->setCurrentIndex(0);
    }
}

// 帧表选中 → 在 packet_records_ 中找 PTS 最接近的视频包, 跳转并高亮
void FramePacketView::OnVideoFrameTableSelectionChanged() {
    if (linking_) return;
    if (!video_page_ || !packet_table_) return;

    QTableWidget* frame_table = video_page_->frameTable();
    if (!frame_table) return;

    const int row = frame_table->currentRow();
    if (row < 0 || row >= frame_table->rowCount()) return;

    QTableWidgetItem* pts_item = frame_table->item(row, 3);  // 列 3 = 原始 PTS
    if (!pts_item) return;
    bool ok = false;
    const qint64 target_pts = pts_item->text().toLongLong(&ok);
    if (!ok) return;

    // packet_records_ 含视频/音频包, 仅匹配视频包以保证 PTS 时基一致
    int best_index = -1;
    qint64 best_diff = std::numeric_limits<qint64>::max();
    for (size_t i = 0; i < packet_records_.size(); ++i) {
        if (packet_records_[i].stream_type != AVMEDIA_TYPE_VIDEO) continue;
        const qint64 diff = std::llabs(packet_records_[i].pts - target_pts);
        if (diff < best_diff) {
            best_diff = diff;
            best_index = static_cast<int>(i);
        }
    }
    if (best_index < 0) return;

    // 包表行号需要按 PacketMatchesFilter 过滤后映射; 为避免过滤导致错位,
    // 通过遍历当前可见行做反向查找
    int target_visible_row = -1;
    for (int r = 0; r < packet_table_->rowCount(); ++r) {
        QTableWidgetItem* pts_cell = packet_table_->item(r, 3);  // 列 3 = 显示时间(PTS)
        if (!pts_cell) continue;
        bool ok2 = false;
        const qint64 cell_pts = pts_cell->text().toLongLong(&ok2);
        if (ok2 && cell_pts == packet_records_[best_index].pts) {
            target_visible_row = r;
            break;
        }
    }
    if (target_visible_row < 0) return;

    linking_ = true;
    packet_table_->setCurrentCell(target_visible_row, 0);
    packet_table_->scrollToItem(packet_table_->item(target_visible_row, 0),
                                QAbstractItemView::PositionAtCenter);
    linking_ = false;

    // 自动切到「包」子页
    if (sub_tabs_) {
        sub_tabs_->setCurrentIndex(1);
    }
}

}  // namespace ui
}  // namespace videoeye
