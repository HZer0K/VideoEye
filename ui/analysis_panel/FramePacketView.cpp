#include "ui/analysis_panel/FramePacketView.h"

#include "ui/analysis_panel/AudioFrameTableWidget.h"
#include "ui/analysis_panel/PacketTableWidget.h"
#include "ui/analysis_panel/VideoFrameTableWidget.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

// 帧表 / 包表互跳要读 FFmpeg 媒体类型常量（AVMEDIA_TYPE_VIDEO）。原先由 core/analysis
// 头文件间接带入；现在 UI 直接依赖 FFmpeg 公共常量，显式 include（与"静态库 PRIVATE
// 不传 include 目录"的一致）。
#include <libavcodec/avcodec.h>

#include <cmath>
#include <limits>

namespace videoeye {
namespace ui {

FramePacketView::FramePacketView(QWidget* parent) : QWidget(parent) {
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
    connect(video_page_, &VideoFrameTableWidget::GopSummariesChanged, this, &FramePacketView::GopSummariesChanged);
    // GOP 行双击 → 切回「视频帧」子页并选中该段起始帧。切 tab 只有父页知道（子页拿不到
    // 自己的 tab 序号），选中则交给子页按帧号定位（筛选生效时行号 ≠ 记录索引）。
    connect(video_page_, &VideoFrameTableWidget::GopActivated, this, [this](int start_frame_index) {
        if (sub_tabs_) {
            sub_tabs_->setCurrentIndex(0);
        }
        video_page_->SelectFrameByIndex(start_frame_index);
    });

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
    // 两张表现在都是 QTableView（model/view），所以监听的是 selectionModel 的
    // currentRowChanged —— QTableView 没有 itemSelectionChanged（那是 QTableWidget 的
    // 便利信号，依赖"每行都有 item 对象"，而 model 视图恰恰没有）。
    if (auto* packet_selection = packet_page_->packetTable()->selectionModel()) {
        connect(packet_selection, &QItemSelectionModel::currentRowChanged, this,
                &FramePacketView::OnPacketTableSelectionChanged);
    }
    if (auto* frame_selection = video_page_->frameTable()->selectionModel()) {
        connect(frame_selection, &QItemSelectionModel::currentRowChanged, this,
                &FramePacketView::OnVideoFrameTableSelectionChanged);
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

void FramePacketView::ExportVideoFrameCsv() {
    video_page_->OnExportFrameCsv();
}

void FramePacketView::ExportGopCsv() {
    video_page_->OnExportGopCsv();
}

void FramePacketView::ExportAudioFrameCsv() {
    audio_page_->OnExportAudioFrameCsv();
}

void FramePacketView::ExportPacketCsv() {
    packet_page_->OnExportPacketCsv();
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

void FramePacketView::AppendVideoFrame(int index, int frame_type, bool is_key_frame, qint64 pts,
                                       double timestamp_seconds) {
    video_page_->AppendVideoFrame(index, frame_type, is_key_frame, pts, timestamp_seconds);
}

void FramePacketView::AppendAudioFrame(int index, qint64 pts, double timestamp_seconds, int sample_count,
                                       int sample_rate, int channels, int byte_count) {
    audio_page_->AppendAudioFrame(index, pts, timestamp_seconds, sample_count, sample_rate, channels, byte_count);
}

void FramePacketView::AppendPacket(const model::PacketInfo& packet_info) {
    packet_page_->AppendPacket(packet_info);
}

void FramePacketView::AppendPackets(const std::vector<model::PacketInfo>& packets) {
    packet_page_->AppendPackets(packets);
}

// 在记录序列里找时间戳最接近的那条，返回记录索引（找不到返回 records.size()）。
//
// 一律用归一化后的 timestamp_seconds 比较，不能比原始 PTS：包序列混着音视频流，
// 而 PTS 的单位是各流自己的 time_base（音频常见 1/48000、视频 1/90000），
// 跨流相减没有意义，只会跳到"数值上碰巧接近"的错误行。
// 缺失时间戳（NaN / AV_NOPTS_VALUE 派生的无效值）的记录不参与匹配。
//
// 过滤条件做成谓词参数：包序列里混着音频/字幕包，帧表→包表只需要视频包；
// 帧序列没有这个维度。
template <typename Record, typename Pred>
std::size_t NearestByTimestamp(const std::vector<Record>& records, double target_seconds, Pred pred) {
    std::size_t best = records.size();
    double best_diff = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (!pred(records[i]))
            continue;
        const double ts = records[i].timestamp_seconds;
        if (!std::isfinite(ts))
            continue;
        const double diff = std::fabs(ts - target_seconds);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

template <typename Record>
std::size_t NearestByTimestamp(const std::vector<Record>& records, double target_seconds) {
    return NearestByTimestamp(records, target_seconds, [](const Record&) { return true; });
}

// 把视图的当前行移到"记录索引 -> 行号"对应的那一行。
//
// 这一步为什么必须走模型：筛选生效时行号 ≠ 记录索引（比如只看 I 帧时，
// 第 3 条记录可能是第 0 行）。以前这里是逐行读"第 3 列文本"再 toLongLong
// 反查 —— 既慢（O(可见行数) 次字符串解析），又依赖"第 3 列恰好写着 PTS"
// 这个隐式契约；列顺序一改互跳就静默错位。现在映射是模型自己的职责，
// 一次二分搞定，也不再碰单元格文本。
bool SelectRecordRow(QTableView* view, int row) {
    if (view == nullptr || row < 0)
        return false;
    QAbstractItemModel* model = view->model();
    if (!model || row >= model->rowCount())
        return false;
    view->setCurrentIndex(model->index(row, 0));
    view->scrollTo(model->index(row, 0), QAbstractItemView::PositionAtCenter);
    return true;
}

// 包表选中 → 在视频帧记录中找 PTS 最接近的视频帧, 跳转并高亮
void FramePacketView::OnPacketTableSelectionChanged() {
    if (linking_)
        return;
    if (!video_page_ || !packet_page_)
        return;

    auto* packet_model = packet_page_->packetModel();
    QTableView* packet_table = packet_page_->packetTable();
    if (!packet_model || !packet_table)
        return;

    auto* frame_model = video_page_->frameModel();
    QTableView* frame_table = video_page_->frameTable();
    if (!frame_model || !frame_table)
        return;

    const int row = packet_table->currentIndex().row();
    if (row < 0)
        return;
    const PacketRecord* packet = packet_model->RecordAt(row);
    if (packet == nullptr)
        return;
    // 音频包的原始 PTS 与视频帧不同时基，统一按播放时间比较。
    const double target_seconds = packet->timestamp_seconds;
    if (!std::isfinite(target_seconds))
        return;

    const std::size_t best = NearestByTimestamp(frame_model->records(), target_seconds);
    if (best >= frame_model->record_count())
        return;

    // 帧表可能被筛选：记录索引 -> 行号要问模型
    const int target_row = frame_model->RowOfRecord(best);
    if (target_row < 0)
        return; // 该帧被筛掉了，不动

    linking_ = true;
    SelectRecordRow(frame_table, target_row);
    linking_ = false;

    // 自动切到「视频帧」子页
    if (sub_tabs_) {
        sub_tabs_->setCurrentIndex(0);
    }
}

// 帧表选中 → 在包记录中找 PTS 最接近的视频包, 跳转并高亮
void FramePacketView::OnVideoFrameTableSelectionChanged() {
    if (linking_)
        return;
    if (!video_page_ || !packet_page_)
        return;

    auto* frame_model = video_page_->frameModel();
    QTableView* frame_table = video_page_->frameTable();
    if (!frame_model || !frame_table)
        return;

    auto* packet_model = packet_page_->packetModel();
    QTableView* packet_table = packet_page_->packetTable();
    if (!packet_model || !packet_table)
        return;

    const int row = frame_table->currentIndex().row();
    if (row < 0)
        return;
    const VideoFrameRecord* frame = frame_model->RecordAt(row);
    if (frame == nullptr)
        return;
    const double target_seconds = frame->timestamp_seconds;
    if (!std::isfinite(target_seconds))
        return;

    // 包记录含视频/音频/其他包, 只匹配视频包（帧表是视频帧，其他流的包没有帧级对应物）
    const std::size_t best = NearestByTimestamp(packet_model->records(), target_seconds, [](const PacketRecord& p) {
        return p.stream_type == AVMEDIA_TYPE_VIDEO;
    });
    if (best >= packet_model->record_count())
        return;

    const int target_row = packet_model->RowOfRecord(best);
    if (target_row < 0)
        return; // 该包被筛掉了，不动

    linking_ = true;
    SelectRecordRow(packet_table, target_row);
    linking_ = false;

    // 自动切到「包」子页
    if (sub_tabs_) {
        sub_tabs_->setCurrentIndex(1);
    }
}

} // namespace ui
} // namespace videoeye