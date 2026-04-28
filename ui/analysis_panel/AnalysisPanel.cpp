#include "AnalysisPanel.h"
#include "utils/Logger.h"
#include <QGroupBox>
#include <QHeaderView>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include <QTextStream>
#include <QtCharts>
#include <algorithm>

namespace videoeye {
namespace ui {

namespace {
constexpr int kUiFlushIntervalMs = 120;
constexpr int kMaxChartSamples = 300;
}

AnalysisPanel::AnalysisPanel(QWidget* parent)
    : QWidget(parent)
    , bitrate_chart_object_(nullptr)
    , fps_chart_object_(nullptr)
    , bitrate_series_(nullptr)
    , fps_series_(nullptr)
    , bitrate_axis_x_(nullptr)
    , bitrate_axis_y_(nullptr)
    , fps_axis_x_(nullptr)
    , fps_axis_y_(nullptr) {
    
    SetupUI();
    
    update_timer_ = new QTimer(this);
    connect(update_timer_, &QTimer::timeout, this, &AnalysisPanel::FlushPendingUiUpdates);
    update_timer_->start(kUiFlushIntervalMs);
    
    LOG_INFO("分析面板已初始化");
}

AnalysisPanel::~AnalysisPanel() {
    LOG_INFO("分析面板已销毁");
}

void AnalysisPanel::SetupUI() {
    QVBoxLayout* main_layout = new QVBoxLayout(this);
    
    // 创建标签页
    tab_widget_ = new QTabWidget(this);
    
    SetupStreamTab();
    SetupFrameTab();
    SetupAudioFrameTab();
    SetupHistogramTab();
    SetupFaceTab();
    
    main_layout->addWidget(tab_widget_);
    
    // 底部按钮
    QHBoxLayout* button_layout = new QHBoxLayout();
    export_button_ = new QPushButton(tr("导出分析报告"), this);
    button_layout->addStretch();
    button_layout->addWidget(export_button_);
    main_layout->addLayout(button_layout);
    
    // 连接信号
    connect(export_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportReport);
}

void AnalysisPanel::SetupStreamTab() {
    stream_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(stream_tab_);
    
    // 统计信息表格
    QGroupBox* stats_group = new QGroupBox(tr("流统计信息"), stream_tab_);
    QVBoxLayout* stats_layout = new QVBoxLayout(stats_group);
    
    stats_table_ = new QTableWidget(14, 2, stats_group);
    stats_table_->setHorizontalHeaderLabels({"参数", "值"});
    stats_table_->setColumnWidth(0, 150);
    stats_table_->verticalHeader()->setVisible(false);
    
    // 填充初始数据
    QStringList labels = {
        "总数据包数", "总字节数", "视频帧数", "音频帧数",
        "当前帧率", "平均帧率", "当前码率", "平均码率",
        "峰值码率", "GOP长度", "I帧数量", "P帧数量",
        "B帧数量", "分析时长"
    };
    
    for (int i = 0; i < labels.size(); ++i) {
        stats_table_->setItem(i, 0, new QTableWidgetItem(labels[i]));
        stats_table_->setItem(i, 1, new QTableWidgetItem("0"));
    }
    
    stats_layout->addWidget(stats_table_);
    layout->addWidget(stats_group);
    
    // 图表区域
    QHBoxLayout* charts_layout = new QHBoxLayout();
    
    // 码率图表
    QGroupBox* bitrate_group = new QGroupBox(tr("码率变化"), stream_tab_);
    QVBoxLayout* bitrate_layout = new QVBoxLayout(bitrate_group);
    bitrate_chart_ = new QChartView();
    bitrate_chart_->setMinimumHeight(200);
    bitrate_layout->addWidget(bitrate_chart_);
    charts_layout->addWidget(bitrate_group);
    
    // 帧率图表
    QGroupBox* fps_group = new QGroupBox(tr("帧率变化"), stream_tab_);
    QVBoxLayout* fps_layout = new QVBoxLayout(fps_group);
    fps_chart_ = new QChartView();
    fps_chart_->setMinimumHeight(200);
    fps_layout->addWidget(fps_chart_);
    charts_layout->addWidget(fps_group);
    
    layout->addLayout(charts_layout);
    
    tab_widget_->addTab(stream_tab_, tr("流分析"));
    
    bitrate_series_ = new QLineSeries(this);
    bitrate_chart_object_ = new QChart();
    bitrate_chart_object_->setTitle(tr("码率变化 (Kbps)"));
    bitrate_chart_object_->legend()->hide();
    bitrate_chart_object_->addSeries(bitrate_series_);
    bitrate_axis_x_ = new QValueAxis(this);
    bitrate_axis_y_ = new QValueAxis(this);
    bitrate_axis_x_->setLabelFormat("%d");
    bitrate_axis_y_->setLabelFormat("%.0f");
    bitrate_axis_y_->setMin(0.0);
    bitrate_axis_y_->setMax(1.0);
    bitrate_chart_object_->addAxis(bitrate_axis_x_, Qt::AlignBottom);
    bitrate_chart_object_->addAxis(bitrate_axis_y_, Qt::AlignLeft);
    bitrate_series_->attachAxis(bitrate_axis_x_);
    bitrate_series_->attachAxis(bitrate_axis_y_);
    bitrate_chart_->setChart(bitrate_chart_object_);
    bitrate_chart_->setRenderHint(QPainter::Antialiasing);

    fps_series_ = new QLineSeries(this);
    fps_chart_object_ = new QChart();
    fps_chart_object_->setTitle(tr("帧率变化 (FPS)"));
    fps_chart_object_->legend()->hide();
    fps_chart_object_->addSeries(fps_series_);
    fps_axis_x_ = new QValueAxis(this);
    fps_axis_y_ = new QValueAxis(this);
    fps_axis_x_->setLabelFormat("%d");
    fps_axis_y_->setLabelFormat("%.1f");
    fps_axis_y_->setMin(0.0);
    fps_axis_y_->setMax(1.0);
    fps_chart_object_->addAxis(fps_axis_x_, Qt::AlignBottom);
    fps_chart_object_->addAxis(fps_axis_y_, Qt::AlignLeft);
    fps_series_->attachAxis(fps_axis_x_);
    fps_series_->attachAxis(fps_axis_y_);
    fps_chart_->setChart(fps_chart_object_);
    fps_chart_->setRenderHint(QPainter::Antialiasing);
}

void AnalysisPanel::SetupFrameTab() {
    frame_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(frame_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    toolbar_layout->addWidget(new QLabel(tr("筛选:"), frame_tab_));
    frame_filter_combo_ = new QComboBox(frame_tab_);
    frame_filter_combo_->addItems({tr("全部帧"), tr("仅 I 帧"), tr("仅关键帧")});
    toolbar_layout->addWidget(frame_filter_combo_);

    frame_summary_label_ = new QLabel(tr("总帧数: 0 | 显示: 0 | GOP: 0"), frame_tab_);
    toolbar_layout->addWidget(frame_summary_label_, 1);

    export_frame_csv_button_ = new QPushButton(tr("导出 CSV"), frame_tab_);
    toolbar_layout->addWidget(export_frame_csv_button_);
    layout->addLayout(toolbar_layout);
    
    QGroupBox* table_group = new QGroupBox(tr("视频帧信息"), frame_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);
    
    frame_table_ = new QTableWidget(0, 7, table_group);
    frame_table_->setHorizontalHeaderLabels({"序号", "帧类型", "关键帧", "时间戳(s)", "PTS", "GOP", "GOP内位置"});
    frame_table_->verticalHeader()->setVisible(false);
    frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    frame_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    frame_table_->setSortingEnabled(false);
    frame_table_->horizontalHeader()->setStretchLastSection(true);
    frame_table_->setColumnWidth(0, 80);
    frame_table_->setColumnWidth(1, 80);
    frame_table_->setColumnWidth(2, 80);
    frame_table_->setColumnWidth(3, 140);
    frame_table_->setColumnWidth(5, 80);
    
    table_layout->addWidget(frame_table_);
    layout->addWidget(table_group);

    QGroupBox* gop_group = new QGroupBox(tr("GOP 分段统计"), frame_tab_);
    QVBoxLayout* gop_layout = new QVBoxLayout(gop_group);
    gop_table_ = new QTableWidget(0, 9, gop_group);
    gop_table_->setHorizontalHeaderLabels({"GOP", "起始帧", "结束帧", "起始时间(s)", "结束时间(s)", "总帧数", "I", "P", "B"});
    gop_table_->verticalHeader()->setVisible(false);
    gop_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    gop_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gop_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    gop_table_->setSortingEnabled(false);
    gop_table_->horizontalHeader()->setStretchLastSection(true);
    gop_layout->addWidget(gop_table_);
    layout->addWidget(gop_group);
    
    tab_widget_->addTab(frame_tab_, tr("视频帧"));

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        OnFrameFilterChanged();
    });
    connect(export_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportFrameCsv);
}

void AnalysisPanel::SetupAudioFrameTab() {
    audio_frame_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(audio_frame_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    audio_frame_summary_label_ = new QLabel(tr("总音频帧数: 0 | 总样本数: 0 | 总字节数: 0"), audio_frame_tab_);
    toolbar_layout->addWidget(audio_frame_summary_label_, 1);

    export_audio_frame_csv_button_ = new QPushButton(tr("导出 CSV"), audio_frame_tab_);
    toolbar_layout->addWidget(export_audio_frame_csv_button_);
    layout->addLayout(toolbar_layout);

    QGroupBox* table_group = new QGroupBox(tr("音频帧信息"), audio_frame_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);

    audio_frame_table_ = new QTableWidget(0, 7, table_group);
    audio_frame_table_->setHorizontalHeaderLabels({"序号", "时间戳(s)", "PTS", "样本数", "采样率(Hz)", "声道数", "字节数"});
    audio_frame_table_->verticalHeader()->setVisible(false);
    audio_frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    audio_frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    audio_frame_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    audio_frame_table_->setSortingEnabled(false);
    audio_frame_table_->horizontalHeader()->setStretchLastSection(true);
    audio_frame_table_->setColumnWidth(0, 80);
    audio_frame_table_->setColumnWidth(1, 140);
    audio_frame_table_->setColumnWidth(2, 120);
    audio_frame_table_->setColumnWidth(3, 100);
    audio_frame_table_->setColumnWidth(4, 120);
    audio_frame_table_->setColumnWidth(5, 90);

    table_layout->addWidget(audio_frame_table_);
    layout->addWidget(table_group);

    tab_widget_->addTab(audio_frame_tab_, tr("音频帧"));

    connect(export_audio_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportAudioFrameCsv);
}

void AnalysisPanel::SetupHistogramTab() {
    histogram_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(histogram_tab_);
    
    QGroupBox* hist_group = new QGroupBox(tr("直方图分析"), histogram_tab_);
    QVBoxLayout* hist_layout = new QVBoxLayout(hist_group);
    
    histogram_chart_ = new QChartView();
    histogram_chart_->setMinimumHeight(400);
    hist_layout->addWidget(histogram_chart_);
    
    layout->addWidget(hist_group);
    
    tab_widget_->addTab(histogram_tab_, tr("直方图"));
}

void AnalysisPanel::SetupFaceTab() {
    face_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(face_tab_);
    
    // 人脸计数
    face_count_label_ = new QLabel(tr("检测到人脸数: 0"), face_tab_);
    face_count_label_->setStyleSheet("font-size: 16px; font-weight: bold;");
    layout->addWidget(face_count_label_);
    
    // 人脸详情表格
    QGroupBox* face_group = new QGroupBox(tr("人脸详细信息"), face_tab_);
    QVBoxLayout* face_layout = new QVBoxLayout(face_group);
    
    face_table_ = new QTableWidget(0, 4, face_group);
    face_table_->setHorizontalHeaderLabels({"序号", "置信度", "位置", "大小"});
    face_table_->horizontalHeader()->setStretchLastSection(true);
    face_layout->addWidget(face_table_);
    
    layout->addWidget(face_group);
    
    // 人脸图像显示
    QGroupBox* image_group = new QGroupBox(tr("人脸预览"), face_tab_);
    QVBoxLayout* image_layout = new QVBoxLayout(image_group);
    face_image_label_ = new QLabel(tr("暂无人脸图像"), image_group);
    face_image_label_->setAlignment(Qt::AlignCenter);
    face_image_label_->setMinimumHeight(300);
    face_image_label_->setStyleSheet("background-color: #f0f0f0;");
    image_layout->addWidget(face_image_label_);
    
    layout->addWidget(image_group);
    
    tab_widget_->addTab(face_tab_, tr("人脸检测"));
}

void AnalysisPanel::UpdateStreamStats(const analyzer::StreamStats& stats) {
    current_stats_ = stats;
    pending_stream_stats_ = stats;
    has_pending_stream_stats_ = true;
}

void AnalysisPanel::UpdateHistogram(const analyzer::HistogramData& hist) {
    current_hist_ = hist;
    UpdateHistogramChart(hist);
}

void AnalysisPanel::UpdateFaceDetection(const std::vector<analyzer::FaceInfo>& faces) {
    current_faces_ = faces;
    
    // 更新计数
    face_count_label_->setText(tr("检测到人脸数: %1").arg(faces.size()));
    
    // 更新表格
    face_table_->setRowCount(faces.size());
    
    for (size_t i = 0; i < faces.size(); ++i) {
        const auto& face = faces[i];
        
        face_table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        face_table_->setItem(i, 1, new QTableWidgetItem(
            QString::number(face.confidence, 'f', 2)));
        face_table_->setItem(i, 2, new QTableWidgetItem(
            QString("(%1, %2)").arg(face.bounding_box.x).arg(face.bounding_box.y)));
        face_table_->setItem(i, 3, new QTableWidgetItem(
            QString("%1x%2").arg(face.bounding_box.width).arg(face.bounding_box.height)));
    }
}

void AnalysisPanel::ResetVideoFrameList() {
    frame_records_.clear();
    gop_summaries_.clear();
    frame_table_synced_record_count_ = 0;
    gop_table_synced_count_ = 0;
    frame_table_dirty_ = false;
    gop_table_dirty_ = false;
    frame_summary_dirty_ = true;
    if (frame_table_) {
        frame_table_->setRowCount(0);
    }
    if (gop_table_) {
        gop_table_->setRowCount(0);
    }
    UpdateFrameSummary();
}

void AnalysisPanel::ResetAudioFrameList() {
    audio_frame_records_.clear();
    audio_frame_table_synced_record_count_ = 0;
    audio_frame_table_dirty_ = false;
    audio_frame_summary_dirty_ = true;
    if (audio_frame_table_) {
        audio_frame_table_->setRowCount(0);
    }
    UpdateAudioFrameSummary();
}

void AnalysisPanel::AppendVideoFrameInfo(int index, int frame_type, bool is_key_frame, qint64 pts, double timestamp_seconds) {
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
}

void AnalysisPanel::AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds,
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
}

QString AnalysisPanel::FrameTypeToString(int frame_type) const {
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

bool AnalysisPanel::MatchesFrameFilter(const VideoFrameRecord& record) const {
    if (!frame_filter_combo_) {
        return true;
    }

    switch (frame_filter_combo_->currentIndex()) {
    case 1:
        return record.frame_type == AV_PICTURE_TYPE_I;
    case 2:
        return record.is_key_frame;
    default:
        return true;
    }
}

void AnalysisPanel::RebuildFrameTable() {
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

void AnalysisPanel::RebuildGopTable() {
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

void AnalysisPanel::RebuildAudioFrameTable() {
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

void AnalysisPanel::UpdateFrameSummary() {
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

void AnalysisPanel::UpdateAudioFrameSummary() {
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

void AnalysisPanel::OnExportFrameCsv() {
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

void AnalysisPanel::OnExportAudioFrameCsv() {
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

void AnalysisPanel::OnFrameFilterChanged() {
    RebuildFrameTable();
    UpdateFrameSummary();
}

void AnalysisPanel::FlushPendingUiUpdates() {
    if (has_pending_stream_stats_) {
        RefreshStreamStatsUi(pending_stream_stats_);
        has_pending_stream_stats_ = false;
    }
    if (frame_table_dirty_) {
        FlushPendingFrameTableUpdates();
        frame_table_dirty_ = false;
    }
    if (gop_table_dirty_) {
        FlushPendingGopTableUpdates();
        gop_table_dirty_ = false;
    }
    if (audio_frame_table_dirty_) {
        FlushPendingAudioFrameTableUpdates();
        audio_frame_table_dirty_ = false;
    }
    if (frame_summary_dirty_) {
        UpdateFrameSummary();
        frame_summary_dirty_ = false;
    }
    if (audio_frame_summary_dirty_) {
        UpdateAudioFrameSummary();
        audio_frame_summary_dirty_ = false;
    }
}

void AnalysisPanel::FlushPendingFrameTableUpdates() {
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

void AnalysisPanel::FlushPendingGopTableUpdates() {
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

void AnalysisPanel::FlushPendingAudioFrameTableUpdates() {
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

void AnalysisPanel::RefreshStreamStatsUi(const analyzer::StreamStats& stats) {
    if (!stats_table_) {
        return;
    }

    SetTableItemText(stats_table_, 0, 1, QString::number(stats.total_packets));
    SetTableItemText(stats_table_, 1, 1, QString::number(stats.total_bytes));
    SetTableItemText(stats_table_, 2, 1, QString::number(stats.total_video_frames));
    SetTableItemText(stats_table_, 3, 1, QString::number(stats.total_audio_frames));
    SetTableItemText(stats_table_, 4, 1, QString::number(stats.current_fps, 'f', 2));
    SetTableItemText(stats_table_, 5, 1, QString::number(stats.avg_fps, 'f', 2));
    SetTableItemText(stats_table_, 6, 1, QString::number(stats.current_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 7, 1, QString::number(stats.avg_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 8, 1, QString::number(stats.peak_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 9, 1, QString::number(stats.gop_size));
    SetTableItemText(stats_table_, 10, 1, QString::number(stats.i_frame_count));
    SetTableItemText(stats_table_, 11, 1, QString::number(stats.p_frame_count));
    SetTableItemText(stats_table_, 12, 1, QString::number(stats.b_frame_count));

    const auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats.start_time);
    SetTableItemText(stats_table_, 13, 1, QString::number(duration.count()) + " s");

    const qreal bitrate_kbps = stats.current_bitrate_bps / 1000.0;
    const qreal fps = stats.current_fps;
    bitrate_chart_values_.push_back(bitrate_kbps);
    fps_chart_values_.push_back(fps);
    if (bitrate_chart_values_.size() > kMaxChartSamples) {
        bitrate_chart_values_.pop_front();
    }
    if (fps_chart_values_.size() > kMaxChartSamples) {
        fps_chart_values_.pop_front();
    }

    bitrate_series_->append(stream_chart_sample_index_, bitrate_kbps);
    fps_series_->append(stream_chart_sample_index_, fps);
    if (bitrate_series_->count() > kMaxChartSamples) {
        bitrate_series_->removePoints(0, bitrate_series_->count() - kMaxChartSamples);
    }
    if (fps_series_->count() > kMaxChartSamples) {
        fps_series_->removePoints(0, fps_series_->count() - kMaxChartSamples);
    }

    const int x_min = std::max(0, stream_chart_sample_index_ - kMaxChartSamples + 1);
    const int x_max = std::max(1, stream_chart_sample_index_);
    bitrate_axis_x_->setRange(x_min, x_max);
    fps_axis_x_->setRange(x_min, x_max);

    qreal bitrate_max = 1.0;
    for (qreal value : bitrate_chart_values_) {
        bitrate_max = std::max(bitrate_max, value);
    }
    qreal fps_max = 1.0;
    for (qreal value : fps_chart_values_) {
        fps_max = std::max(fps_max, value);
    }
    bitrate_axis_y_->setRange(0.0, bitrate_max * 1.1);
    fps_axis_y_->setRange(0.0, fps_max * 1.1);

    ++stream_chart_sample_index_;
}

void AnalysisPanel::SetTableItemText(QTableWidget* table, int row, int column, const QString& text) {
    if (!table) {
        return;
    }

    QTableWidgetItem* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setText(text);
}

void AnalysisPanel::AppendFrameRowToTable(const VideoFrameRecord& record) {
    const int row = frame_table_->rowCount();
    frame_table_->insertRow(row);
    SetTableItemText(frame_table_, row, 0, QString::number(record.index));
    SetTableItemText(frame_table_, row, 1, FrameTypeToString(record.frame_type));
    SetTableItemText(frame_table_, row, 2, record.is_key_frame ? tr("是") : tr("否"));
    SetTableItemText(frame_table_, row, 3, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(frame_table_, row, 4, QString::number(record.pts));
    SetTableItemText(frame_table_, row, 5, QString::number(record.gop_index));
    SetTableItemText(frame_table_, row, 6, QString::number(record.gop_position));
}

void AnalysisPanel::AppendAudioFrameRowToTable(const AudioFrameRecord& record) {
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

void AnalysisPanel::UpdateGopRowInTable(int row, const GopSummary& summary) {
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

void AnalysisPanel::UpdateBitrateChart(const analyzer::StreamStats& stats) {
    Q_UNUSED(stats);
}

void AnalysisPanel::UpdateFPSChart(const analyzer::StreamStats& stats) {
    Q_UNUSED(stats);
}

void AnalysisPanel::UpdateGOPChart(const analyzer::StreamStats& stats) {
    // GOP图表实现
}

void AnalysisPanel::UpdateHistogramChart(const analyzer::HistogramData& hist) {
    if (hist.gray_channel.empty()) {
        return;
    }
    
    QChart* chart = new QChart();
    chart->setTitle(tr("灰度直方图"));
    chart->legend()->hide();
    
    QLineSeries* series = new QLineSeries();
    for (size_t i = 0; i < hist.gray_channel.size(); ++i) {
        series->append(i, hist.gray_channel[i]);
    }
    
    chart->addSeries(series);
    chart->createDefaultAxes();
    
    histogram_chart_->setChart(chart);
    histogram_chart_->setRenderHint(QPainter::Antialiasing);
}

void AnalysisPanel::OnExportReport() {
    QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出分析报告"),
        QString("videoeye_report_%1.html").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("HTML文件 (*.html);;文本文件 (*.txt);;所有文件 (*)"));
    
    if (filename.isEmpty()) {
        return;
    }
    
    LOG_INFO("导出分析报告: " + filename.toStdString());
    
    QMessageBox::information(this, tr("成功"), tr("分析报告已导出到:\n%1").arg(filename));
}

} // namespace ui
} // namespace videoeye
