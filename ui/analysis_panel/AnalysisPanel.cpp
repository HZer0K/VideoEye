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

AnalysisPanel::AnalysisPanel(QWidget* parent)
    : QWidget(parent)
    , bitrate_series_(nullptr)
    , fps_series_(nullptr) {
    
    SetupUI();
    
    // 设置定时器用于定时更新
    update_timer_ = new QTimer(this);
    connect(update_timer_, &QTimer::timeout, this, [this]() {
        // 定时刷新显示
        update();
    });
    update_timer_->start(1000); // 每秒更新
    
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
    
    // 初始化图表数据系列
    bitrate_series_ = new QLineSeries();
    fps_series_ = new QLineSeries();
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
    
    // 更新表格
    stats_table_->setItem(0, 1, new QTableWidgetItem(QString::number(stats.total_packets)));
    stats_table_->setItem(1, 1, new QTableWidgetItem(QString::number(stats.total_bytes)));
    stats_table_->setItem(2, 1, new QTableWidgetItem(QString::number(stats.video_packets)));
    stats_table_->setItem(3, 1, new QTableWidgetItem(QString::number(stats.audio_packets)));
    stats_table_->setItem(4, 1, new QTableWidgetItem(QString::number(stats.current_fps, 'f', 2)));
    stats_table_->setItem(5, 1, new QTableWidgetItem(QString::number(stats.avg_fps, 'f', 2)));
    stats_table_->setItem(6, 1, new QTableWidgetItem(QString::number(stats.current_bitrate_bps / 1000) + " Kbps"));
    stats_table_->setItem(7, 1, new QTableWidgetItem(QString::number(stats.avg_bitrate_bps / 1000) + " Kbps"));
    stats_table_->setItem(8, 1, new QTableWidgetItem(QString::number(stats.peak_bitrate_bps / 1000) + " Kbps"));
    stats_table_->setItem(9, 1, new QTableWidgetItem(QString::number(stats.gop_size)));
    stats_table_->setItem(10, 1, new QTableWidgetItem(QString::number(stats.i_frame_count)));
    stats_table_->setItem(11, 1, new QTableWidgetItem(QString::number(stats.p_frame_count)));
    stats_table_->setItem(12, 1, new QTableWidgetItem(QString::number(stats.b_frame_count)));
    
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats.start_time);
    stats_table_->setItem(13, 1, new QTableWidgetItem(
        QString::number(duration.count()) + " s"));
    
    // 更新图表
    UpdateBitrateChart(stats);
    UpdateFPSChart(stats);
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
    RebuildFrameTable();
    RebuildGopTable();
    UpdateFrameSummary();
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

    if (MatchesFrameFilter(record)) {
        const int row = frame_table_->rowCount();
        frame_table_->insertRow(row);
        frame_table_->setItem(row, 0, new QTableWidgetItem(QString::number(record.index)));
        frame_table_->setItem(row, 1, new QTableWidgetItem(FrameTypeToString(record.frame_type)));
        frame_table_->setItem(row, 2, new QTableWidgetItem(record.is_key_frame ? tr("是") : tr("否")));
        frame_table_->setItem(row, 3, new QTableWidgetItem(QString::number(record.timestamp_seconds, 'f', 3)));
        frame_table_->setItem(row, 4, new QTableWidgetItem(QString::number(record.pts)));
        frame_table_->setItem(row, 5, new QTableWidgetItem(QString::number(record.gop_index)));
        frame_table_->setItem(row, 6, new QTableWidgetItem(QString::number(record.gop_position)));
        frame_table_->scrollToBottom();
    }

    const GopSummary& latest_gop = gop_summaries_.back();
    const int gop_row = latest_gop.gop_index - 1;
    if (gop_table_->rowCount() <= gop_row) {
        gop_table_->insertRow(gop_row);
    }
    gop_table_->setItem(gop_row, 0, new QTableWidgetItem(QString::number(latest_gop.gop_index)));
    gop_table_->setItem(gop_row, 1, new QTableWidgetItem(QString::number(latest_gop.start_frame)));
    gop_table_->setItem(gop_row, 2, new QTableWidgetItem(QString::number(latest_gop.end_frame)));
    gop_table_->setItem(gop_row, 3, new QTableWidgetItem(QString::number(latest_gop.start_ts, 'f', 3)));
    gop_table_->setItem(gop_row, 4, new QTableWidgetItem(QString::number(latest_gop.end_ts, 'f', 3)));
    gop_table_->setItem(gop_row, 5, new QTableWidgetItem(QString::number(latest_gop.total_frames)));
    gop_table_->setItem(gop_row, 6, new QTableWidgetItem(QString::number(latest_gop.i_count)));
    gop_table_->setItem(gop_row, 7, new QTableWidgetItem(QString::number(latest_gop.p_count)));
    gop_table_->setItem(gop_row, 8, new QTableWidgetItem(QString::number(latest_gop.b_count)));

    UpdateFrameSummary();
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

    frame_table_->setRowCount(0);
    for (const auto& record : frame_records_) {
        if (!MatchesFrameFilter(record)) {
            continue;
        }
        const int row = frame_table_->rowCount();
        frame_table_->insertRow(row);
        frame_table_->setItem(row, 0, new QTableWidgetItem(QString::number(record.index)));
        frame_table_->setItem(row, 1, new QTableWidgetItem(FrameTypeToString(record.frame_type)));
        frame_table_->setItem(row, 2, new QTableWidgetItem(record.is_key_frame ? tr("是") : tr("否")));
        frame_table_->setItem(row, 3, new QTableWidgetItem(QString::number(record.timestamp_seconds, 'f', 3)));
        frame_table_->setItem(row, 4, new QTableWidgetItem(QString::number(record.pts)));
        frame_table_->setItem(row, 5, new QTableWidgetItem(QString::number(record.gop_index)));
        frame_table_->setItem(row, 6, new QTableWidgetItem(QString::number(record.gop_position)));
    }
}

void AnalysisPanel::RebuildGopTable() {
    if (!gop_table_) {
        return;
    }

    gop_table_->setRowCount(0);
    for (const auto& summary : gop_summaries_) {
        const int row = gop_table_->rowCount();
        gop_table_->insertRow(row);
        gop_table_->setItem(row, 0, new QTableWidgetItem(QString::number(summary.gop_index)));
        gop_table_->setItem(row, 1, new QTableWidgetItem(QString::number(summary.start_frame)));
        gop_table_->setItem(row, 2, new QTableWidgetItem(QString::number(summary.end_frame)));
        gop_table_->setItem(row, 3, new QTableWidgetItem(QString::number(summary.start_ts, 'f', 3)));
        gop_table_->setItem(row, 4, new QTableWidgetItem(QString::number(summary.end_ts, 'f', 3)));
        gop_table_->setItem(row, 5, new QTableWidgetItem(QString::number(summary.total_frames)));
        gop_table_->setItem(row, 6, new QTableWidgetItem(QString::number(summary.i_count)));
        gop_table_->setItem(row, 7, new QTableWidgetItem(QString::number(summary.p_count)));
        gop_table_->setItem(row, 8, new QTableWidgetItem(QString::number(summary.b_count)));
    }
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

void AnalysisPanel::OnFrameFilterChanged() {
    RebuildFrameTable();
    UpdateFrameSummary();
}

void AnalysisPanel::UpdateBitrateChart(const analyzer::StreamStats& stats) {
    // 创建或更新码率图表
    QChart* chart = new QChart();
    chart->setTitle(tr("码率变化 (Kbps)"));
    chart->legend()->hide();
    
    QLineSeries* series = new QLineSeries();
    // 这里应该保存历史数据，现在简单示例
    series->append(0, stats.current_bitrate_bps / 1000.0);
    
    chart->addSeries(series);
    chart->createDefaultAxes();
    
    bitrate_chart_->setChart(chart);
    bitrate_chart_->setRenderHint(QPainter::Antialiasing);
}

void AnalysisPanel::UpdateFPSChart(const analyzer::StreamStats& stats) {
    QChart* chart = new QChart();
    chart->setTitle(tr("帧率变化 (FPS)"));
    chart->legend()->hide();
    
    QLineSeries* series = new QLineSeries();
    series->append(0, stats.current_fps);
    
    chart->addSeries(series);
    chart->createDefaultAxes();
    
    fps_chart_->setChart(chart);
    fps_chart_->setRenderHint(QPainter::Antialiasing);
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
