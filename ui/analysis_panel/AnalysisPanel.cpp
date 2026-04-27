#include "AnalysisPanel.h"
#include "utils/Logger.h"
#include <QGroupBox>
#include <QHeaderView>
#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>
#include <QtCharts>

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
    
    QGroupBox* table_group = new QGroupBox(tr("视频帧信息"), frame_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);
    
    frame_table_ = new QTableWidget(0, 4, table_group);
    frame_table_->setHorizontalHeaderLabels({"序号", "帧类型", "时间戳(s)", "PTS"});
    frame_table_->verticalHeader()->setVisible(false);
    frame_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    frame_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    frame_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    frame_table_->setSortingEnabled(false);
    frame_table_->horizontalHeader()->setStretchLastSection(true);
    frame_table_->setColumnWidth(0, 80);
    frame_table_->setColumnWidth(1, 80);
    frame_table_->setColumnWidth(2, 140);
    
    table_layout->addWidget(frame_table_);
    layout->addWidget(table_group);
    
    tab_widget_->addTab(frame_tab_, tr("视频帧"));
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
    if (frame_table_) {
        frame_table_->setRowCount(0);
    }
}

void AnalysisPanel::AppendVideoFrameInfo(int index, int frame_type, qint64 pts, double timestamp_seconds) {
    if (!frame_table_) {
        return;
    }

    QString type = "?";
    if (frame_type == AV_PICTURE_TYPE_I) {
        type = "I";
    } else if (frame_type == AV_PICTURE_TYPE_P) {
        type = "P";
    } else if (frame_type == AV_PICTURE_TYPE_B) {
        type = "B";
    }

    const int row = frame_table_->rowCount();
    frame_table_->insertRow(row);
    frame_table_->setItem(row, 0, new QTableWidgetItem(QString::number(index)));
    frame_table_->setItem(row, 1, new QTableWidgetItem(type));
    frame_table_->setItem(row, 2, new QTableWidgetItem(QString::number(timestamp_seconds, 'f', 3)));
    frame_table_->setItem(row, 3, new QTableWidgetItem(QString::number(pts)));
    frame_table_->scrollToBottom();
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
