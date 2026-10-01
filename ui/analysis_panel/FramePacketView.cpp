#include "ui/analysis_panel/FramePacketView.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"

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
constexpr size_t kMaxFrameRecords = 50000;
constexpr size_t kMaxAudioFrameRecords = 30000;
constexpr size_t kMaxPacketRecords = 10000;

// 裁剪记录向量到指定上限，移除最早的多余记录并重置表格
template<typename T>
void TrimRecords(std::vector<T>& records, size_t& synced_count,
                 QTableWidget* table, bool& table_dirty, size_t max_count) {
    if (records.size() <= max_count) return;
    const size_t remove_count = records.size() - max_count;
    records.erase(records.begin(), records.begin() + remove_count);
    if (table) {
        table->setRowCount(0);
    }
    synced_count = 0;
    table_dirty = true;
}
}  // namespace

FramePacketView::FramePacketView(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

void FramePacketView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setDocumentMode(true);

    // ---- 子页 0: 视频帧 ----
    video_sub_ = new QWidget(sub_tabs_);
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

    sub_tabs_->addTab(video_sub_, tr("视频帧"));

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

    sub_tabs_->addTab(packet_sub_, tr("包"));

    // ---- 子页 2: GOP 摘要 ----
    gop_sub_ = new QWidget(sub_tabs_);
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

    sub_tabs_->addTab(gop_sub_, tr("GOP 摘要"));

    // ---- 子页 3: 音频帧 ----
    audio_sub_ = new QWidget(sub_tabs_);
    QVBoxLayout* a_layout = new QVBoxLayout(audio_sub_);
    a_layout->setContentsMargins(4, 4, 4, 4);
    a_layout->setSpacing(4);

    QHBoxLayout* a_toolbar = new QHBoxLayout();
    audio_frame_summary_label_ = new QLabel(tr("总音频帧数: 0 | 总样本数: 0 | 总字节数: 0"), audio_sub_);
    a_toolbar->addWidget(audio_frame_summary_label_, 1);

    export_audio_frame_csv_button_ = new QPushButton(tr("导出 CSV"), audio_sub_);
    a_toolbar->addWidget(export_audio_frame_csv_button_);

    audio_toggle_ = new QCheckBox(tr("启用分析"), audio_sub_);
    a_toolbar->addWidget(audio_toggle_);
    a_layout->addLayout(a_toolbar);

    QGroupBox* a_table_group = new QGroupBox(tr("音频帧信息"), audio_sub_);
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

    sub_tabs_->addTab(audio_sub_, tr("音频帧"));

    layout->addWidget(sub_tabs_);

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        OnFrameFilterChanged();
    });
    connect(export_frame_csv_button_, &QPushButton::clicked, this, &FramePacketView::OnExportFrameCsv);
    connect(export_audio_frame_csv_button_, &QPushButton::clicked, this, &FramePacketView::OnExportAudioFrameCsv);
    connect(g_export_btn, &QPushButton::clicked, this, &FramePacketView::OnExportGopCsv);

    connect(packet_filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        packet_filter_mode_ = idx - 1;  // 0->全部(-1), 1->视频(0), 2->音频(1), 3->其他(2)
        RebuildPacketTable();
    });
    connect(export_packet_csv_button_, &QPushButton::clicked, this, &FramePacketView::OnExportPacketCsv);
    connect(packet_table_, &QTableWidget::itemSelectionChanged, this, &FramePacketView::OnPacketTableSelectionChanged);

    connect(frame_table_, &QTableWidget::itemSelectionChanged, this, &FramePacketView::OnVideoFrameTableSelectionChanged);
}

void FramePacketView::SetFeatureHooks(std::function<bool(int)> is_enabled,
                                      std::function<void(int, bool)> set_enabled) {
    is_enabled_ = std::move(is_enabled);
    set_enabled_ = std::move(set_enabled);

    SyncTogglesFromHooks();

    const struct { QCheckBox* box; int id; } toggles[] = {
        {video_toggle_, 0},
        {audio_toggle_, 1},
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
        {video_toggle_, 0},
        {audio_toggle_, 1},
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
    return frame_table_dirty_ || gop_table_dirty_ ||
           audio_frame_table_dirty_ || packet_table_dirty_ ||
           frame_summary_dirty_ || audio_frame_summary_dirty_ || packet_summary_dirty_;
}

void FramePacketView::FlushPending() {
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
    if (audio_frame_table_dirty_) {
        FlushPendingAudioFrameTableUpdates();
        audio_frame_table_dirty_ = false;
    }
    if (packet_table_dirty_) {
        FlushPendingPacketTableUpdates();
        packet_table_dirty_ = false;
    }
    if (frame_summary_dirty_) {
        UpdateFrameSummary();
        frame_summary_dirty_ = false;
    }
    if (audio_frame_summary_dirty_) {
        UpdateAudioFrameSummary();
        audio_frame_summary_dirty_ = false;
    }
    if (packet_summary_dirty_) {
        UpdatePacketSummary();
        packet_summary_dirty_ = false;
    }
}

void FramePacketView::ResetVideoFrames() {
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

void FramePacketView::ResetAudioFrames() {
    audio_frame_records_.clear();
    audio_frame_table_synced_record_count_ = 0;
    audio_frame_table_dirty_ = false;
    audio_frame_summary_dirty_ = true;
    if (audio_frame_table_) {
        audio_frame_table_->setRowCount(0);
    }
    UpdateAudioFrameSummary();
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
        const size_t old_size = frame_records_.size();
        TrimRecords(frame_records_, frame_table_synced_record_count_, frame_table_, frame_table_dirty_, kMaxFrameRecords);
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
    TrimRecords(gop_summaries_, gop_table_synced_count_, gop_table_, gop_table_dirty_, kMaxFrameRecords / 10);
}

void FramePacketView::AppendAudioFrame(int index, qint64 pts, double timestamp_seconds,
                                       int sample_count, int sample_rate, int channels, int byte_count) {
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
    TrimRecords(audio_frame_records_, audio_frame_table_synced_record_count_, audio_frame_table_, audio_frame_table_dirty_, kMaxAudioFrameRecords);
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

QString FramePacketView::FrameTypeToString(int frame_type) const {
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

bool FramePacketView::MatchesFrameFilter(const VideoFrameRecord& record) const {
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

void FramePacketView::RebuildFrameTable() {
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

void FramePacketView::RebuildGopTable() {
    if (!gop_table_) {
        return;
    }

    gop_table_->setUpdatesEnabled(false);
    gop_table_->setRowCount(0);
    for (const auto& summary : gop_summaries_) {
        const int row = gop_table_->rowCount();
        gop_table_->insertRow(row);
        UpdateGopRowInTable(row, summary);
    }
    gop_table_->setUpdatesEnabled(true);
    gop_table_synced_count_ = gop_summaries_.size();
}

void FramePacketView::RebuildAudioFrameTable() {
    if (!audio_frame_table_) {
        return;
    }

    audio_frame_table_->setUpdatesEnabled(false);
    audio_frame_table_->setRowCount(0);
    for (const auto& record : audio_frame_records_) {
        AppendAudioFrameRowToTable(record);
    }
    audio_frame_table_->setUpdatesEnabled(true);
    audio_frame_table_synced_record_count_ = audio_frame_records_.size();
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

void FramePacketView::UpdateFrameSummary() {
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

void FramePacketView::UpdateAudioFrameSummary() {
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

void FramePacketView::OnExportFrameCsv() {
    if (frame_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的帧分析数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出视频帧 CSV"),
        QString("videoeye_frames_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
}

void FramePacketView::OnExportAudioFrameCsv() {
    if (audio_frame_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的音频帧数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出音频帧 CSV"),
        QString("videoeye_audio_frames_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
}

void FramePacketView::OnExportGopCsv() {
    if (gop_summaries_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 GOP 摘要数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出 GOP 摘要 CSV"),
        QString("videoeye_gop_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
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

void FramePacketView::OnFrameFilterChanged() {
    RebuildFrameTable();
    UpdateFrameSummary();
}

void FramePacketView::FlushPendingFrameTableUpdates() {
    if (!frame_table_) {
        return;
    }

    frame_table_->setUpdatesEnabled(false);
    for (size_t i = frame_table_synced_record_count_; i < frame_records_.size(); ++i) {
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

void FramePacketView::FlushPendingGopTableUpdates() {
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

void FramePacketView::FlushPendingAudioFrameTableUpdates() {
    if (!audio_frame_table_) {
        return;
    }

    audio_frame_table_->setUpdatesEnabled(false);
    for (size_t i = audio_frame_table_synced_record_count_; i < audio_frame_records_.size(); ++i) {
        AppendAudioFrameRowToTable(audio_frame_records_[i]);
    }
    audio_frame_table_->setUpdatesEnabled(true);
    audio_frame_table_synced_record_count_ = audio_frame_records_.size();

    if (audio_frame_table_->rowCount() > 0) {
        audio_frame_table_->scrollToBottom();
    }
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
void FramePacketView::AppendFrameRowToTable(const VideoFrameRecord& record) {
    const int row = frame_table_->rowCount();
    frame_table_->insertRow(row);
    SetTableItemText(frame_table_, row, 0, QString::number(record.index));
    SetTableItemText(frame_table_, row, 1, FrameTypeToString(record.frame_type));
    SetTableItemText(frame_table_, row, 2, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(frame_table_, row, 3, QString::number(record.pts));
    SetTableItemText(frame_table_, row, 4, QString::number(record.gop_index));
    SetTableItemText(frame_table_, row, 5, QString::number(record.gop_position));
}

void FramePacketView::AppendAudioFrameRowToTable(const AudioFrameRecord& record) {
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

void FramePacketView::UpdateGopRowInTable(int row, const GopSummary& summary) {
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

// 包表选中 → 在 frame_records_ 中找 PTS 最接近的视频帧, 跳转并高亮
void FramePacketView::OnPacketTableSelectionChanged() {
    if (linking_) return;
    if (!packet_table_ || !frame_table_) return;

    const int row = packet_table_->currentRow();
    if (row < 0 || row >= packet_table_->rowCount()) return;

    // 从表中读 PTS (原始 timebase 单位) 用于匹配
    QTableWidgetItem* pts_item = packet_table_->item(row, 3);  // 列 3 = 显示时间(PTS)
    if (!pts_item) return;
    bool ok = false;
    const qint64 target_pts = pts_item->text().toLongLong(&ok);
    if (!ok) return;

    // 在 frame_records_ 中找 PTS 最接近的记录
    int best_index = -1;
    qint64 best_diff = std::numeric_limits<qint64>::max();
    for (size_t i = 0; i < frame_records_.size(); ++i) {
        const qint64 diff = std::llabs(frame_records_[i].pts - target_pts);
        if (diff < best_diff) {
            best_diff = diff;
            best_index = static_cast<int>(i);
        }
    }
    if (best_index < 0) return;

    // 帧表可能被 RebuildFrameTable 过滤, 行号 != 下标. 反向查可见行.
    int target_visible_row = -1;
    for (int r = 0; r < frame_table_->rowCount(); ++r) {
        QTableWidgetItem* pts_cell = frame_table_->item(r, 3);  // 列 3 = 原始 PTS
        if (!pts_cell) continue;
        bool ok2 = false;
        const qint64 cell_pts = pts_cell->text().toLongLong(&ok2);
        if (ok2 && cell_pts == frame_records_[best_index].pts) {
            target_visible_row = r;
            break;
        }
    }
    if (target_visible_row < 0) return;

    linking_ = true;
    frame_table_->setCurrentCell(target_visible_row, 0);
    frame_table_->scrollToItem(frame_table_->item(target_visible_row, 0),
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
    if (!frame_table_ || !packet_table_) return;

    const int row = frame_table_->currentRow();
    if (row < 0 || row >= frame_table_->rowCount()) return;

    QTableWidgetItem* pts_item = frame_table_->item(row, 3);  // 列 3 = 原始 PTS
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
