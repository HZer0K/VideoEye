#include "ui/analysis_panel/FramePacketView.h"

#include "ui/analysis_panel/AudioFrameTableWidget.h"
#include "ui/analysis_panel/PacketTableWidget.h"
#include "ui/analysis_panel/VideoFrameTableWidget.h"

#include <QCheckBox>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

// 帧表 / 包表互跳要读 FFmpeg 媒体类型常量（AVMEDIA_TYPE_VIDEO）。原先由 core/analysis
// 头文件间接带入；现在 UI 直接依赖 FFmpeg 公共常量，显式 include（与"静态库 PRIVATE
// 不传 include 目录"的一致）。
#include <libavcodec/avcodec.h>

#include <limits>

namespace videoeye {
namespace ui {

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
    // 已抽为独立组件 PacketTableWidget：记录缓存 / 筛选 / 脏标志 / 增量游标 / CSV 导出
    // 全在组件内，本页只负责建页 + 转发数据 + 转发 flush；跨表互跳要同时碰帧表与包表，
    // 连线留在本页（组件只外露 packetTable() / PacketRecords()）。
    packet_page_ = new PacketTableWidget(this);

    // ---- 子页 3: 音频帧 ----
    // 已抽为独立组件 AudioFrameTableWidget：记录缓存 / 脏标志 / 增量游标 / CSV 导出
    // 全在组件内，本页只负责建页 + 转发数据 + 转发 flush。
    audio_page_ = new AudioFrameTableWidget(this);

    // 按原有顺序 addTab：tab 序号是帧表↔包表互跳（setCurrentIndex(0/1)）的依据，
    // 不能因为把子页挪进组件就变。
    sub_tabs_->addTab(video_page_->videoFramePage(), tr("视频帧"));
    sub_tabs_->addTab(packet_page_, tr("包"));
    sub_tabs_->addTab(video_page_->gopPage(), tr("GOP 摘要"));
    sub_tabs_->addTab(audio_page_, tr("音频帧"));

    layout->addWidget(sub_tabs_);

    // 各子页内部的按钮与筛选框都连在各自组件内。
    // 帧表 ↔ 包表按 PTS 互跳要同时碰两张表，连线留在本页。
    connect(packet_page_->packetTable(), &QTableWidget::itemSelectionChanged,
            this, &FramePacketView::OnPacketTableSelectionChanged);
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
        {packet_page_->toggle(), 2},
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
        {packet_page_->toggle(), 2},
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
    return video_page_->HasPending() || audio_page_->HasPending() || packet_page_->HasPending();
}

void FramePacketView::FlushPending() {
    // 各子页的脏标志与增量游标都在组件内，交给它们自己 flush；
    // GopSummariesChanged 也由 VideoFrameTableWidget 发出，本页只是转发给面板。
    video_page_->FlushPending();
    packet_page_->FlushPending();
    audio_page_->FlushPending();
}

void FramePacketView::ResetVideoFrames() {
    video_page_->ResetVideoFrames();
}

void FramePacketView::ResetAudioFrames() {
    audio_page_->ResetAudioFrames();
}

void FramePacketView::ResetPackets() {
    packet_page_->ResetPackets();
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
    packet_page_->AppendPacket(packet_info);
}

// 包表选中 → 在视频帧记录中找 PTS 最接近的视频帧, 跳转并高亮
void FramePacketView::OnPacketTableSelectionChanged() {
    if (linking_) return;
    if (!video_page_ || !packet_page_) return;

    QTableWidget* packet_table = packet_page_->packetTable();
    if (!packet_table) return;

    // 帧记录与帧表都在 VideoFrameTableWidget 里（帧表 ↔ 包表互跳要同时碰两侧）
    const std::vector<VideoFrameRecord>& frame_records = video_page_->FrameRecords();
    QTableWidget* frame_table = video_page_->frameTable();
    if (!frame_table) return;

    const int row = packet_table->currentRow();
    if (row < 0 || row >= packet_table->rowCount()) return;

    // 从表中读 PTS (原始 timebase 单位) 用于匹配
    QTableWidgetItem* pts_item = packet_table->item(row, 3);  // 列 3 = 显示时间(PTS)
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

// 帧表选中 → 在包记录中找 PTS 最接近的视频包, 跳转并高亮
void FramePacketView::OnVideoFrameTableSelectionChanged() {
    if (linking_) return;
    if (!video_page_ || !packet_page_) return;

    QTableWidget* frame_table = video_page_->frameTable();
    QTableWidget* packet_table = packet_page_->packetTable();
    if (!frame_table || !packet_table) return;

    const int row = frame_table->currentRow();
    if (row < 0 || row >= frame_table->rowCount()) return;

    QTableWidgetItem* pts_item = frame_table->item(row, 3);  // 列 3 = 原始 PTS
    if (!pts_item) return;
    bool ok = false;
    const qint64 target_pts = pts_item->text().toLongLong(&ok);
    if (!ok) return;

    // 包记录含视频/音频包, 仅匹配视频包以保证 PTS 时基一致
    const std::vector<PacketRecord>& packet_records = packet_page_->PacketRecords();
    int best_index = -1;
    qint64 best_diff = std::numeric_limits<qint64>::max();
    for (size_t i = 0; i < packet_records.size(); ++i) {
        if (packet_records[i].stream_type != AVMEDIA_TYPE_VIDEO) continue;
        const qint64 diff = std::llabs(packet_records[i].pts - target_pts);
        if (diff < best_diff) {
            best_diff = diff;
            best_index = static_cast<int>(i);
        }
    }
    if (best_index < 0) return;

    // 包表行号需要按 PacketMatchesFilter 过滤后映射; 为避免过滤导致错位,
    // 通过遍历当前可见行做反向查找
    int target_visible_row = -1;
    for (int r = 0; r < packet_table->rowCount(); ++r) {
        QTableWidgetItem* pts_cell = packet_table->item(r, 3);  // 列 3 = 显示时间(PTS)
        if (!pts_cell) continue;
        bool ok2 = false;
        const qint64 cell_pts = pts_cell->text().toLongLong(&ok2);
        if (ok2 && cell_pts == packet_records[best_index].pts) {
            target_visible_row = r;
            break;
        }
    }
    if (target_visible_row < 0) return;

    linking_ = true;
    packet_table->setCurrentCell(target_visible_row, 0);
    packet_table->scrollToItem(packet_table->item(target_visible_row, 0),
                               QAbstractItemView::PositionAtCenter);
    linking_ = false;

    // 自动切到「包」子页
    if (sub_tabs_) {
        sub_tabs_->setCurrentIndex(1);
    }
}

}  // namespace ui
}  // namespace videoeye