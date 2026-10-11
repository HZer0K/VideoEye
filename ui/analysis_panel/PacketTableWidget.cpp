#include "ui/analysis_panel/PacketTableWidget.h"

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

// 包标志 / 媒体类型常量（AV_PKT_FLAG_* / AVMEDIA_TYPE_*）。原先由 core/analysis 头文件
// 间接带入；现在 UI 直接依赖 FFmpeg 公共常量，显式 include（与"静态库 PRIVATE 不传
// include 目录"的一致）。
#include <libavcodec/avcodec.h>

#include <climits>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

namespace {
// 记录上限统一取 10 万（见 RecordTableModel.h）。以前本页是 1 万 —— 一小时视频的包
// 数远不止一万（每帧至少一个视频包），1 万意味着只留得下开头几十秒的包。
constexpr std::size_t kMaxPacketRecords = kDefaultMaxRecordCount;
}  // namespace

PacketTableWidget::PacketTableWidget(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

PacketTableWidget::~PacketTableWidget() = default;

void PacketTableWidget::SetupUi() {
    // 本组件直接充当「包」子页（父页 addTab 本组件），布局语义与原 packet_sub_ 一致。
    QVBoxLayout* p_layout = new QVBoxLayout(this);
    p_layout->setContentsMargins(4, 4, 4, 4);
    p_layout->setSpacing(4);

    QHBoxLayout* p_toolbar = new QHBoxLayout();
    packet_summary_label_ = new QLabel(tr("总包数: 0 | 视频包: 0 | 音频包: 0 | 其他包: 0"), this);
    p_toolbar->addWidget(packet_summary_label_, 1);

    p_toolbar->addWidget(new QLabel(tr("筛选:"), this));
    packet_filter_combo_ = new QComboBox(this);
    packet_filter_combo_->addItems({tr("全部流"), tr("视频流"), tr("音频流"), tr("其他流")});
    packet_filter_combo_->setCurrentIndex(0);
    p_toolbar->addWidget(packet_filter_combo_);

    export_packet_csv_button_ = new QPushButton(tr("导出 CSV"), this);
    p_toolbar->addWidget(export_packet_csv_button_);

    packet_toggle_ = new QCheckBox(tr("启用分析"), this);
    packet_toggle_->setToolTip(tr("播放时实时采集，需要额外的数据留存，默认关闭，按需开启。"));
    p_toolbar->addWidget(packet_toggle_);
    p_layout->addLayout(p_toolbar);

    QGroupBox* p_table_group = new QGroupBox(tr("数据包信息"), this);
    QVBoxLayout* p_table_layout = new QVBoxLayout(p_table_group);

    // 模型由本组件持有并在析构里释放：QTableView::setModel 只是引用，不接管所有权。
    auto columns = std::vector<RecordTableModel<PacketRecord>::ColumnSpec>();
    using Spec = RecordTableModel<PacketRecord>::ColumnSpec;
    columns.push_back(Spec{tr("#"),
                           [](const PacketRecord& r) { return QString::number(r.index); }, {}});
    columns.push_back(Spec{tr("流"), [this](const PacketRecord& r) {
                               // 「流」列把流索引与类型合到一个单元格, 形如 "#0 视频"
                               return QString("#%1 %2")
                                   .arg(r.stream_index)
                                   .arg(PacketStreamTypeToName(r.stream_type));
                           }, {}});
    columns.push_back(Spec{tr("播放时间(s)"),
                           [](const PacketRecord& r) {
                               return QString::number(r.timestamp_seconds, 'f', 3);
                           }, {}});
    columns.push_back(Spec{tr("显示时间(PTS)"),
                           [](const PacketRecord& r) { return QString::number(r.pts); }, {}});
    columns.push_back(Spec{tr("解码时间(DTS)"),
                           [](const PacketRecord& r) { return QString::number(r.dts); }, {}});
    columns.push_back(Spec{tr("时长"),
                           [](const PacketRecord& r) { return QString::number(r.duration); }, {}});
    columns.push_back(Spec{tr("包大小"),
                           [](const PacketRecord& r) { return QString::number(r.size); }, {}});
    packet_model_ = new RecordTableModel<PacketRecord>(std::move(columns), this);

    packet_table_ = new QTableView(p_table_group);
    packet_table_->setModel(packet_model_);
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

    // 筛选与导出是本子页内部的事（只依赖自己的记录缓存），就地连回。
    connect(packet_filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        packet_filter_mode_ = idx - 1;  // 0->全部(-1), 1->视频(0), 2->音频(1), 3->其他(2)
        ApplyFilter();
    });
    connect(export_packet_csv_button_, &QPushButton::clicked, this, &PacketTableWidget::OnExportPacketCsv);
}

const std::vector<PacketRecord>& PacketTableWidget::PacketRecords() const {
    return packet_model_->records();
}

bool PacketTableWidget::HasPending() const {
    return packet_table_dirty_ || packet_summary_dirty_;
}

void PacketTableWidget::FlushPending() {
    if (packet_table_dirty_) {
        FlushPendingPacketTableUpdates();
        packet_table_dirty_ = false;
    }
    if (packet_summary_dirty_) {
        UpdatePacketSummary();
        packet_summary_dirty_ = false;
    }
}

void PacketTableWidget::ResetPackets() {
    pending_packets_.clear();
    packet_table_dirty_ = false;
    packet_summary_dirty_ = true;
    if (packet_model_) {
        packet_model_->Clear();
    }
    UpdatePacketSummary();
}

void PacketTableWidget::AppendPacket(const model::PacketInfo& packet_info) {
    if (!packet_model_) {
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

    // 只入队 + 置脏，FlushPending 时一次性交给模型（一次 rowsInserted 通知一批）
    pending_packets_.push_back(record);
    packet_table_dirty_ = true;
    packet_summary_dirty_ = true;
}

QString PacketTableWidget::PacketFlagsToString(int flags) const {
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

QString PacketTableWidget::PacketStreamTypeToName(int type) const {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return tr("视频");
        case AVMEDIA_TYPE_AUDIO: return tr("音频");
        case AVMEDIA_TYPE_DATA: return tr("数据");
        case AVMEDIA_TYPE_SUBTITLE: return tr("字幕");
        case AVMEDIA_TYPE_ATTACHMENT: return tr("附件");
        default: return tr("未知");
    }
}

bool PacketTableWidget::PacketMatchesFilter(const PacketRecord& record) const {
    switch (packet_filter_mode_) {
        case 0: return record.stream_type == AVMEDIA_TYPE_VIDEO;
        case 1: return record.stream_type == AVMEDIA_TYPE_AUDIO;
        case 2: return record.stream_type != AVMEDIA_TYPE_VIDEO &&
                       record.stream_type != AVMEDIA_TYPE_AUDIO;
        default: return true;  // -1 = 全部
    }
}

// 筛选条件变了：让模型按新条件重算可见行。
//
// 以前这里是 setRowCount(0) + 逐条重插（O(n) 次 insertRow）。现在模型内部
// 重排可见索引并 reset —— 记录本身不动，也不会因为"重建"丢掉刚进来的包。
void PacketTableWidget::ApplyFilter() {
    if (!packet_model_) return;
    if (packet_filter_mode_ < 0) {
        packet_model_->SetFilter(nullptr);
        return;
    }
    // 捕获 mode 的副本而不是 this 上的成员：筛选谓词会在模型里被长期持有，
    // 捕获 this 的话每次判定都要解引用一个可能已经析构的组件。
    const int mode = packet_filter_mode_;
    packet_model_->SetFilter([mode](const PacketRecord& r) {
        switch (mode) {
            case 0: return r.stream_type == AVMEDIA_TYPE_VIDEO;
            case 1: return r.stream_type == AVMEDIA_TYPE_AUDIO;
            case 2: return r.stream_type != AVMEDIA_TYPE_VIDEO &&
                           r.stream_type != AVMEDIA_TYPE_AUDIO;
            default: return true;
        }
    });
}

void PacketTableWidget::UpdatePacketSummary() {
    if (!packet_summary_label_ || !packet_model_) {
        return;
    }

    const auto& records = packet_model_->records();
    int video_packets = 0;
    int audio_packets = 0;
    int other_packets = 0;
    long long total_bytes = 0;
    long long sum_size = 0;
    int max_size = 0;
    int min_size = INT32_MAX;
    for (const auto& record : records) {
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

    const int avg_size = records.empty() ? 0 : static_cast<int>(sum_size / records.size());

    packet_summary_label_->setText(
        tr("总包数: %1 | 总字节数: %2 | 视频包: %3 | 音频包: %4 | 其他包: %5 | 平均包大小: %6 B | 最大包大小: %7 B | 最小包大小: %8 B")
            .arg(records.size())
            .arg(format_bytes(total_bytes))
            .arg(video_packets)
            .arg(audio_packets)
            .arg(other_packets)
            .arg(avg_size)
            .arg(max_size)
            .arg(records.empty() ? 0 : min_size));
}

void PacketTableWidget::OnExportPacketCsv() {
    if (!packet_model_ || packet_model_->record_count() == 0) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的包分析数据。"));
        return;
    }

    const int rows = static_cast<int>(packet_model_->record_count());
    ExportCsvStream(this,
        tr("导出包分析 CSV"),
        QString("videoeye_packets_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,stream_index,timestamp_seconds,pts,dts,duration,size,flags,file_pos\n";
            for (const auto& record : packet_model_->records()) {
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
        },
        rows);
}

void PacketTableWidget::FlushPendingPacketTableUpdates() {
    if (!packet_model_ || !packet_table_) {
        return;
    }

    if (!pending_packets_.empty()) {
        packet_model_->AppendRecords(std::move(pending_packets_));
        pending_packets_.clear();
    }

    const std::size_t count = packet_model_->record_count();
    if (count > kMaxPacketRecords) {
        packet_model_->TrimFront(count - kMaxPacketRecords);
    }

    if (packet_model_->rowCount() > 0) {
        packet_table_->scrollToBottom();
    }
}

}  // namespace ui
}  // namespace videoeye
