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
    , sync_series_(nullptr)
    , timeline_video_series_(nullptr)
    , timeline_audio_series_(nullptr)
    , timeline_event_series_(nullptr)
    , waveform_series_(nullptr)
    , spectrum_series_(nullptr)
    , bitrate_axis_x_(nullptr)
    , bitrate_axis_y_(nullptr)
    , fps_axis_x_(nullptr)
    , fps_axis_y_(nullptr)
    , sync_axis_x_(nullptr)
    , sync_axis_y_(nullptr)
    , timeline_axis_x_(nullptr)
    , timeline_axis_y_(nullptr)
    , waveform_axis_x_(nullptr)
    , waveform_axis_y_(nullptr)
    , spectrum_axis_x_(nullptr)
    , spectrum_axis_y_(nullptr) {
    
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
    SetupPacketTab();
    SetupEventTab();
    SetupSyncTab();
    SetupTimelineTab();
    SetupAudioVisualizationTab();
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

void AnalysisPanel::SetupPacketTab() {
    packet_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(packet_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    packet_summary_label_ = new QLabel(tr("总包数: 0 | 视频包: 0 | 音频包: 0 | 其他包: 0"), packet_tab_);
    toolbar_layout->addWidget(packet_summary_label_, 1);

    export_packet_csv_button_ = new QPushButton(tr("导出 CSV"), packet_tab_);
    toolbar_layout->addWidget(export_packet_csv_button_);
    layout->addLayout(toolbar_layout);

    QGroupBox* table_group = new QGroupBox(tr("数据包信息"), packet_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);

    packet_table_ = new QTableWidget(0, 9, table_group);
    packet_table_->setHorizontalHeaderLabels({"序号", "流索引", "时间戳(s)", "PTS", "DTS", "时长", "大小", "标记", "文件偏移"});
    packet_table_->verticalHeader()->setVisible(false);
    packet_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    packet_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    packet_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    packet_table_->setSortingEnabled(false);
    packet_table_->horizontalHeader()->setStretchLastSection(true);
    packet_table_->setColumnWidth(0, 80);
    packet_table_->setColumnWidth(1, 80);
    packet_table_->setColumnWidth(2, 120);
    packet_table_->setColumnWidth(3, 120);
    packet_table_->setColumnWidth(4, 120);
    packet_table_->setColumnWidth(5, 100);
    packet_table_->setColumnWidth(6, 90);
    packet_table_->setColumnWidth(7, 120);

    table_layout->addWidget(packet_table_);
    layout->addWidget(table_group);

    tab_widget_->addTab(packet_tab_, tr("包分析"));

    connect(export_packet_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportPacketCsv);
}

void AnalysisPanel::SetupEventTab() {
    event_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(event_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    event_summary_label_ = new QLabel(tr("总事件数: 0 | 错误: 0 | 警告: 0 | 信息: 0"), event_tab_);
    toolbar_layout->addWidget(event_summary_label_, 1);

    export_event_csv_button_ = new QPushButton(tr("导出 CSV"), event_tab_);
    toolbar_layout->addWidget(export_event_csv_button_);
    layout->addLayout(toolbar_layout);

    QGroupBox* table_group = new QGroupBox(tr("异常事件"), event_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);

    event_table_ = new QTableWidget(0, 8, table_group);
    event_table_->setHorizontalHeaderLabels({"序号", "级别", "类型", "流索引", "时间戳(s)", "PTS", "摘要", "详情"});
    event_table_->verticalHeader()->setVisible(false);
    event_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    event_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    event_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    event_table_->setSortingEnabled(false);
    event_table_->horizontalHeader()->setStretchLastSection(true);
    event_table_->setColumnWidth(0, 80);
    event_table_->setColumnWidth(1, 90);
    event_table_->setColumnWidth(2, 120);
    event_table_->setColumnWidth(3, 80);
    event_table_->setColumnWidth(4, 120);
    event_table_->setColumnWidth(5, 120);
    event_table_->setColumnWidth(6, 240);

    table_layout->addWidget(event_table_);
    layout->addWidget(table_group);

    tab_widget_->addTab(event_tab_, tr("异常事件"));

    connect(export_event_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportEventCsv);
}

void AnalysisPanel::SetupSyncTab() {
    sync_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(sync_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    sync_summary_label_ = new QLabel(tr("样本数: 0 | 平均偏移: 0.00 ms | 最大偏移: 0.00 ms"), sync_tab_);
    toolbar_layout->addWidget(sync_summary_label_, 1);

    export_sync_csv_button_ = new QPushButton(tr("导出 CSV"), sync_tab_);
    toolbar_layout->addWidget(export_sync_csv_button_);
    layout->addLayout(toolbar_layout);

    QGroupBox* chart_group = new QGroupBox(tr("音视频时间差"), sync_tab_);
    QVBoxLayout* chart_layout = new QVBoxLayout(chart_group);
    sync_chart_ = new QChartView(sync_tab_);
    sync_chart_->setMinimumHeight(220);
    chart_layout->addWidget(sync_chart_);
    layout->addWidget(chart_group);

    QGroupBox* table_group = new QGroupBox(tr("同步样本"), sync_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);
    sync_table_ = new QTableWidget(0, 5, table_group);
    sync_table_->setHorizontalHeaderLabels({"序号", "音频时间(s)", "视频时间(s)", "差值(ms)", "锚点"});
    sync_table_->verticalHeader()->setVisible(false);
    sync_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sync_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    sync_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    sync_table_->setSortingEnabled(false);
    sync_table_->horizontalHeader()->setStretchLastSection(true);
    sync_table_->setColumnWidth(0, 80);
    sync_table_->setColumnWidth(1, 120);
    sync_table_->setColumnWidth(2, 120);
    sync_table_->setColumnWidth(3, 120);
    table_layout->addWidget(sync_table_);
    layout->addWidget(table_group);

    tab_widget_->addTab(sync_tab_, tr("同步分析"));

    sync_series_ = new QLineSeries(this);
    QChart* sync_chart_object = new QChart();
    sync_chart_object->setTitle(tr("A-V 差值 (ms)"));
    sync_chart_object->legend()->hide();
    sync_chart_object->addSeries(sync_series_);
    sync_axis_x_ = new QValueAxis(this);
    sync_axis_y_ = new QValueAxis(this);
    sync_axis_x_->setLabelFormat("%d");
    sync_axis_y_->setLabelFormat("%.0f");
    sync_axis_y_->setRange(-1.0, 1.0);
    sync_chart_object->addAxis(sync_axis_x_, Qt::AlignBottom);
    sync_chart_object->addAxis(sync_axis_y_, Qt::AlignLeft);
    sync_series_->attachAxis(sync_axis_x_);
    sync_series_->attachAxis(sync_axis_y_);
    sync_chart_->setChart(sync_chart_object);
    sync_chart_->setRenderHint(QPainter::Antialiasing);

    connect(export_sync_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportSyncCsv);
}

void AnalysisPanel::SetupTimelineTab() {
    timeline_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(timeline_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    timeline_summary_label_ = new QLabel(tr("事件数: 0 | 视频关键帧: 0 | 音频采样: 0 | 异常事件: 0"), timeline_tab_);
    toolbar_layout->addWidget(timeline_summary_label_, 1);

    export_timeline_csv_button_ = new QPushButton(tr("导出 CSV"), timeline_tab_);
    toolbar_layout->addWidget(export_timeline_csv_button_);
    layout->addLayout(toolbar_layout);

    QGroupBox* chart_group = new QGroupBox(tr("统一时间轴"), timeline_tab_);
    QVBoxLayout* chart_layout = new QVBoxLayout(chart_group);
    timeline_chart_ = new QChartView(timeline_tab_);
    timeline_chart_->setMinimumHeight(220);
    chart_layout->addWidget(timeline_chart_);
    layout->addWidget(chart_group);

    QGroupBox* table_group = new QGroupBox(tr("时间轴事件"), timeline_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);
    timeline_table_ = new QTableWidget(0, 5, table_group);
    timeline_table_->setHorizontalHeaderLabels({"序号", "类别", "时间戳(s)", "标签", "详情"});
    timeline_table_->verticalHeader()->setVisible(false);
    timeline_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    timeline_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    timeline_table_->setSortingEnabled(false);
    timeline_table_->horizontalHeader()->setStretchLastSection(true);
    timeline_table_->setColumnWidth(0, 80);
    timeline_table_->setColumnWidth(1, 100);
    timeline_table_->setColumnWidth(2, 120);
    timeline_table_->setColumnWidth(3, 200);
    table_layout->addWidget(timeline_table_);
    layout->addWidget(table_group);

    tab_widget_->addTab(timeline_tab_, tr("统一时间轴"));

    timeline_video_series_ = new QLineSeries(this);
    timeline_video_series_->setName(tr("视频关键帧"));
    timeline_video_series_->setPointsVisible(true);
    timeline_audio_series_ = new QLineSeries(this);
    timeline_audio_series_->setName(tr("音频采样"));
    timeline_audio_series_->setPointsVisible(true);
    timeline_event_series_ = new QLineSeries(this);
    timeline_event_series_->setName(tr("异常事件"));
    timeline_event_series_->setPointsVisible(true);

    QChart* timeline_chart_object = new QChart();
    timeline_chart_object->setTitle(tr("统一时间轴"));
    timeline_chart_object->addSeries(timeline_video_series_);
    timeline_chart_object->addSeries(timeline_audio_series_);
    timeline_chart_object->addSeries(timeline_event_series_);
    timeline_axis_x_ = new QValueAxis(this);
    timeline_axis_y_ = new QValueAxis(this);
    timeline_axis_x_->setLabelFormat("%.2f");
    timeline_axis_y_->setRange(0.5, 3.5);
    timeline_axis_y_->setTickCount(4);
    timeline_chart_object->addAxis(timeline_axis_x_, Qt::AlignBottom);
    timeline_chart_object->addAxis(timeline_axis_y_, Qt::AlignLeft);
    timeline_video_series_->attachAxis(timeline_axis_x_);
    timeline_video_series_->attachAxis(timeline_axis_y_);
    timeline_audio_series_->attachAxis(timeline_axis_x_);
    timeline_audio_series_->attachAxis(timeline_axis_y_);
    timeline_event_series_->attachAxis(timeline_axis_x_);
    timeline_event_series_->attachAxis(timeline_axis_y_);
    timeline_chart_->setChart(timeline_chart_object);
    timeline_chart_->setRenderHint(QPainter::Antialiasing);

    connect(export_timeline_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportTimelineCsv);
}

void AnalysisPanel::SetupAudioVisualizationTab() {
    audio_visualization_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(audio_visualization_tab_);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    audio_visualization_summary_label_ = new QLabel(
        tr("暂无音频可视化数据 | 电平: 0.000 | 采样率: 0 Hz | 声道: 0"),
        audio_visualization_tab_);
    toolbar_layout->addWidget(audio_visualization_summary_label_, 1);

    export_audio_visualization_csv_button_ = new QPushButton(tr("导出 CSV"), audio_visualization_tab_);
    toolbar_layout->addWidget(export_audio_visualization_csv_button_);
    layout->addLayout(toolbar_layout);

    QGroupBox* waveform_group = new QGroupBox(tr("音频波形"), audio_visualization_tab_);
    QVBoxLayout* waveform_layout = new QVBoxLayout(waveform_group);
    waveform_chart_ = new QChartView(audio_visualization_tab_);
    waveform_chart_->setMinimumHeight(220);
    waveform_layout->addWidget(waveform_chart_);
    layout->addWidget(waveform_group);

    QGroupBox* spectrum_group = new QGroupBox(tr("音频频谱"), audio_visualization_tab_);
    QVBoxLayout* spectrum_layout = new QVBoxLayout(spectrum_group);
    spectrum_chart_ = new QChartView(audio_visualization_tab_);
    spectrum_chart_->setMinimumHeight(220);
    spectrum_layout->addWidget(spectrum_chart_);
    layout->addWidget(spectrum_group);

    tab_widget_->addTab(audio_visualization_tab_, tr("音频可视化"));

    waveform_series_ = new QLineSeries(this);
    QChart* waveform_chart_object = new QChart();
    waveform_chart_object->setTitle(tr("波形快照"));
    waveform_chart_object->legend()->hide();
    waveform_chart_object->addSeries(waveform_series_);
    waveform_axis_x_ = new QValueAxis(this);
    waveform_axis_y_ = new QValueAxis(this);
    waveform_axis_x_->setLabelFormat("%d");
    waveform_axis_y_->setLabelFormat("%.2f");
    waveform_axis_y_->setRange(-1.0, 1.0);
    waveform_chart_object->addAxis(waveform_axis_x_, Qt::AlignBottom);
    waveform_chart_object->addAxis(waveform_axis_y_, Qt::AlignLeft);
    waveform_series_->attachAxis(waveform_axis_x_);
    waveform_series_->attachAxis(waveform_axis_y_);
    waveform_chart_->setChart(waveform_chart_object);
    waveform_chart_->setRenderHint(QPainter::Antialiasing);

    spectrum_series_ = new QLineSeries(this);
    QChart* spectrum_chart_object = new QChart();
    spectrum_chart_object->setTitle(tr("频谱快照"));
    spectrum_chart_object->legend()->hide();
    spectrum_chart_object->addSeries(spectrum_series_);
    spectrum_axis_x_ = new QValueAxis(this);
    spectrum_axis_y_ = new QValueAxis(this);
    spectrum_axis_x_->setLabelFormat("%d");
    spectrum_axis_y_->setLabelFormat("%.3f");
    spectrum_axis_y_->setRange(0.0, 1.0);
    spectrum_chart_object->addAxis(spectrum_axis_x_, Qt::AlignBottom);
    spectrum_chart_object->addAxis(spectrum_axis_y_, Qt::AlignLeft);
    spectrum_series_->attachAxis(spectrum_axis_x_);
    spectrum_series_->attachAxis(spectrum_axis_y_);
    spectrum_chart_->setChart(spectrum_chart_object);
    spectrum_chart_->setRenderHint(QPainter::Antialiasing);

    connect(export_audio_visualization_csv_button_, &QPushButton::clicked,
            this, &AnalysisPanel::OnExportAudioVisualizationCsv);
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

void AnalysisPanel::ResetPacketList() {
    packet_records_.clear();
    packet_table_synced_record_count_ = 0;
    packet_table_dirty_ = false;
    packet_summary_dirty_ = true;
    if (packet_table_) {
        packet_table_->setRowCount(0);
    }
    UpdatePacketSummary();
}

void AnalysisPanel::ResetAnalysisEventList() {
    analysis_event_records_.clear();
    event_table_synced_record_count_ = 0;
    event_table_dirty_ = false;
    event_summary_dirty_ = true;
    if (event_table_) {
        event_table_->setRowCount(0);
    }
    UpdateEventSummary();
}

void AnalysisPanel::ResetSyncSampleList() {
    sync_sample_records_.clear();
    sync_table_synced_record_count_ = 0;
    sync_table_dirty_ = false;
    sync_summary_dirty_ = true;
    sync_chart_values_.clear();
    if (sync_table_) {
        sync_table_->setRowCount(0);
    }
    if (sync_series_) {
        sync_series_->clear();
    }
    if (sync_axis_x_) {
        sync_axis_x_->setRange(0, 1);
    }
    if (sync_axis_y_) {
        sync_axis_y_->setRange(-1.0, 1.0);
    }
    UpdateSyncSummary();
}

void AnalysisPanel::ResetTimelineEventList() {
    timeline_event_records_.clear();
    timeline_table_synced_record_count_ = 0;
    timeline_table_dirty_ = false;
    timeline_summary_dirty_ = true;
    if (timeline_table_) {
        timeline_table_->setRowCount(0);
    }
    if (timeline_video_series_) {
        timeline_video_series_->clear();
    }
    if (timeline_audio_series_) {
        timeline_audio_series_->clear();
    }
    if (timeline_event_series_) {
        timeline_event_series_->clear();
    }
    if (timeline_axis_x_) {
        timeline_axis_x_->setRange(0.0, 1.0);
    }
    if (timeline_axis_y_) {
        timeline_axis_y_->setRange(0.5, 3.5);
    }
    UpdateTimelineSummary();
}

void AnalysisPanel::ResetAudioVisualization() {
    has_audio_visualization_record_ = false;
    audio_visualization_dirty_ = false;
    audio_visualization_record_ = AudioVisualizationRecord();
    if (waveform_series_) {
        waveform_series_->clear();
    }
    if (spectrum_series_) {
        spectrum_series_->clear();
    }
    if (waveform_axis_x_) {
        waveform_axis_x_->setRange(0, 1);
    }
    if (waveform_axis_y_) {
        waveform_axis_y_->setRange(-1.0, 1.0);
    }
    if (spectrum_axis_x_) {
        spectrum_axis_x_->setRange(0, 1);
    }
    if (spectrum_axis_y_) {
        spectrum_axis_y_->setRange(0.0, 1.0);
    }
    UpdateAudioVisualizationSummary();
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

void AnalysisPanel::AppendPacketInfo(const model::PacketInfo& packet_info) {
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
}

void AnalysisPanel::AppendAnalysisEvent(const model::AnalysisEvent& event_info) {
    if (!event_table_) {
        return;
    }

    AnalysisEventRecord record;
    record.index = event_info.index;
    record.severity = event_info.severity;
    record.type = event_info.type;
    record.stream_index = event_info.stream_index;
    record.pts = event_info.pts;
    record.timestamp_seconds = event_info.timestamp_seconds;
    record.summary = event_info.summary;
    record.detail = event_info.detail;

    analysis_event_records_.push_back(record);
    event_table_dirty_ = true;
    event_summary_dirty_ = true;
}

void AnalysisPanel::AppendSyncSample(const model::SyncSample& sample) {
    if (!sync_table_) {
        return;
    }

    SyncSampleRecord record;
    record.index = sample.index;
    record.audio_timestamp_seconds = sample.audio_timestamp_seconds;
    record.video_timestamp_seconds = sample.video_timestamp_seconds;
    record.diff_ms = sample.diff_ms;
    record.audio_anchor = sample.audio_anchor;

    sync_sample_records_.push_back(record);
    sync_table_dirty_ = true;
    sync_summary_dirty_ = true;
}

void AnalysisPanel::AppendTimelineEvent(const model::TimelineEvent& event) {
    if (!timeline_table_) {
        return;
    }

    TimelineEventRecord record;
    record.index = event.index;
    record.category = event.category;
    record.timestamp_seconds = event.timestamp_seconds;
    record.label = event.label;
    record.detail = event.detail;

    timeline_event_records_.push_back(record);
    timeline_table_dirty_ = true;
    timeline_summary_dirty_ = true;
}

void AnalysisPanel::AppendAudioVisualization(const model::AudioVisualizationFrame& frame) {
    if (!waveform_chart_ || !spectrum_chart_) {
        return;
    }

    audio_visualization_record_.index = frame.index;
    audio_visualization_record_.timestamp_seconds = frame.timestamp_seconds;
    audio_visualization_record_.level = frame.level;
    audio_visualization_record_.sample_rate = frame.sample_rate;
    audio_visualization_record_.channels = frame.channels;
    audio_visualization_record_.waveform_points = frame.waveform_points;
    audio_visualization_record_.spectrum_bins = frame.spectrum_bins;
    has_audio_visualization_record_ = true;
    audio_visualization_dirty_ = true;
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

QString AnalysisPanel::PacketFlagsToString(int flags) const {
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

void AnalysisPanel::RebuildPacketTable() {
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

void AnalysisPanel::RebuildEventTable() {
    if (!event_table_) {
        return;
    }

    event_table_->setUpdatesEnabled(false);
    event_table_->setRowCount(0);
    for (const auto& record : analysis_event_records_) {
        AppendEventRowToTable(record);
    }
    event_table_->setUpdatesEnabled(true);
    event_table_synced_record_count_ = analysis_event_records_.size();
}

void AnalysisPanel::RebuildSyncTable() {
    if (!sync_table_) {
        return;
    }

    sync_table_->setUpdatesEnabled(false);
    sync_table_->setRowCount(0);
    for (const auto& record : sync_sample_records_) {
        AppendSyncRowToTable(record);
    }
    sync_table_->setUpdatesEnabled(true);
    sync_table_synced_record_count_ = sync_sample_records_.size();
    UpdateSyncChart();
}

void AnalysisPanel::RebuildTimelineTable() {
    if (!timeline_table_) {
        return;
    }

    timeline_table_->setUpdatesEnabled(false);
    timeline_table_->setRowCount(0);
    for (const auto& record : timeline_event_records_) {
        AppendTimelineRowToTable(record);
    }
    timeline_table_->setUpdatesEnabled(true);
    timeline_table_synced_record_count_ = timeline_event_records_.size();
    UpdateTimelineChart();
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

void AnalysisPanel::UpdatePacketSummary() {
    if (!packet_summary_label_) {
        return;
    }

    int video_packets = 0;
    int audio_packets = 0;
    int other_packets = 0;
    for (const auto& record : packet_records_) {
        if (record.stream_type == AVMEDIA_TYPE_VIDEO) {
            video_packets++;
        } else if (record.stream_type == AVMEDIA_TYPE_AUDIO) {
            audio_packets++;
        } else {
            other_packets++;
        }
    }

    packet_summary_label_->setText(
        tr("总包数: %1 | 视频包: %2 | 音频包: %3 | 其他包: %4")
            .arg(packet_records_.size())
            .arg(video_packets)
            .arg(audio_packets)
            .arg(other_packets));
}

void AnalysisPanel::UpdateEventSummary() {
    if (!event_summary_label_) {
        return;
    }

    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    for (const auto& record : analysis_event_records_) {
        if (record.severity == tr("错误")) {
            error_count++;
        } else if (record.severity == tr("警告")) {
            warning_count++;
        } else {
            info_count++;
        }
    }

    event_summary_label_->setText(
        tr("总事件数: %1 | 错误: %2 | 警告: %3 | 信息: %4")
            .arg(analysis_event_records_.size())
            .arg(error_count)
            .arg(warning_count)
            .arg(info_count));
}

void AnalysisPanel::UpdateSyncSummary() {
    if (!sync_summary_label_) {
        return;
    }

    if (sync_sample_records_.empty()) {
        sync_summary_label_->setText(tr("样本数: 0 | 平均偏移: 0.00 ms | 最大偏移: 0.00 ms"));
        return;
    }

    double abs_sum = 0.0;
    double max_abs = 0.0;
    for (const auto& record : sync_sample_records_) {
        const double abs_diff = std::abs(record.diff_ms);
        abs_sum += abs_diff;
        max_abs = std::max(max_abs, abs_diff);
    }

    sync_summary_label_->setText(
        tr("样本数: %1 | 平均偏移: %2 ms | 最大偏移: %3 ms")
            .arg(sync_sample_records_.size())
            .arg(abs_sum / static_cast<double>(sync_sample_records_.size()), 0, 'f', 2)
            .arg(max_abs, 0, 'f', 2));
}

void AnalysisPanel::UpdateTimelineSummary() {
    if (!timeline_summary_label_) {
        return;
    }

    int video_count = 0;
    int audio_count = 0;
    int event_count = 0;
    for (const auto& record : timeline_event_records_) {
        if (record.category == tr("视频关键帧")) {
            video_count++;
        } else if (record.category == tr("音频采样")) {
            audio_count++;
        } else if (record.category == tr("事件")) {
            event_count++;
        }
    }

    timeline_summary_label_->setText(
        tr("事件数: %1 | 视频关键帧: %2 | 音频采样: %3 | 异常事件: %4")
            .arg(timeline_event_records_.size())
            .arg(video_count)
            .arg(audio_count)
            .arg(event_count));
}

void AnalysisPanel::UpdateAudioVisualizationSummary() {
    if (!audio_visualization_summary_label_) {
        return;
    }

    if (!has_audio_visualization_record_) {
        audio_visualization_summary_label_->setText(
            tr("暂无音频可视化数据 | 电平: 0.000 | 采样率: 0 Hz | 声道: 0"));
        return;
    }

    audio_visualization_summary_label_->setText(
        tr("快照 #%1 | 时间戳: %2 s | 电平: %3 | 采样率: %4 Hz | 声道: %5 | 波形点: %6 | 频谱桶: %7")
            .arg(audio_visualization_record_.index)
            .arg(audio_visualization_record_.timestamp_seconds, 0, 'f', 3)
            .arg(audio_visualization_record_.level, 0, 'f', 3)
            .arg(audio_visualization_record_.sample_rate)
            .arg(audio_visualization_record_.channels)
            .arg(audio_visualization_record_.waveform_points.size())
            .arg(audio_visualization_record_.spectrum_bins.size()));
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

void AnalysisPanel::OnExportPacketCsv() {
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

void AnalysisPanel::OnExportEventCsv() {
    if (analysis_event_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的异常事件数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出异常事件 CSV"),
        QString("videoeye_events_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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
    out << "index,severity,type,stream_index,timestamp_seconds,pts,summary,detail\n";
    for (const auto& record : analysis_event_records_) {
        out << record.index << ','
            << '"' << record.severity << '"' << ','
            << '"' << record.type << '"' << ','
            << record.stream_index << ','
            << QString::number(record.timestamp_seconds, 'f', 6) << ','
            << record.pts << ','
            << '"' << record.summary << '"' << ','
            << '"' << record.detail << '"' << '\n';
    }

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
}

void AnalysisPanel::OnExportSyncCsv() {
    if (sync_sample_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的同步分析数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出同步分析 CSV"),
        QString("videoeye_sync_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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
    out << "index,audio_timestamp_seconds,video_timestamp_seconds,diff_ms,anchor\n";
    for (const auto& record : sync_sample_records_) {
        out << record.index << ','
            << QString::number(record.audio_timestamp_seconds, 'f', 6) << ','
            << QString::number(record.video_timestamp_seconds, 'f', 6) << ','
            << QString::number(record.diff_ms, 'f', 3) << ','
            << '"' << (record.audio_anchor ? tr("音频") : tr("视频")) << '"' << '\n';
    }

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
}

void AnalysisPanel::OnExportTimelineCsv() {
    if (timeline_event_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的时间轴数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出统一时间轴 CSV"),
        QString("videoeye_timeline_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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
    out << "index,category,timestamp_seconds,label,detail\n";
    for (const auto& record : timeline_event_records_) {
        out << record.index << ','
            << '"' << record.category << '"' << ','
            << QString::number(record.timestamp_seconds, 'f', 6) << ','
            << '"' << record.label << '"' << ','
            << '"' << record.detail << '"' << '\n';
    }

    QMessageBox::information(this, tr("成功"), tr("CSV 已导出到:\n%1").arg(filename));
}

void AnalysisPanel::OnExportAudioVisualizationCsv() {
    if (!has_audio_visualization_record_) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的音频可视化数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出音频可视化 CSV"),
        QString("videoeye_audio_visual_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
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
    out << "section,index,timestamp_seconds,level,sample_rate,channels,point_index,value\n";
    for (int i = 0; i < audio_visualization_record_.waveform_points.size(); ++i) {
        out << "waveform,"
            << audio_visualization_record_.index << ','
            << QString::number(audio_visualization_record_.timestamp_seconds, 'f', 6) << ','
            << QString::number(audio_visualization_record_.level, 'f', 6) << ','
            << audio_visualization_record_.sample_rate << ','
            << audio_visualization_record_.channels << ','
            << i << ','
            << QString::number(audio_visualization_record_.waveform_points[i], 'f', 6) << '\n';
    }
    for (int i = 0; i < audio_visualization_record_.spectrum_bins.size(); ++i) {
        out << "spectrum,"
            << audio_visualization_record_.index << ','
            << QString::number(audio_visualization_record_.timestamp_seconds, 'f', 6) << ','
            << QString::number(audio_visualization_record_.level, 'f', 6) << ','
            << audio_visualization_record_.sample_rate << ','
            << audio_visualization_record_.channels << ','
            << i << ','
            << QString::number(audio_visualization_record_.spectrum_bins[i], 'f', 6) << '\n';
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
    if (packet_table_dirty_) {
        FlushPendingPacketTableUpdates();
        packet_table_dirty_ = false;
    }
    if (event_table_dirty_) {
        FlushPendingEventTableUpdates();
        event_table_dirty_ = false;
    }
    if (sync_table_dirty_) {
        FlushPendingSyncTableUpdates();
        sync_table_dirty_ = false;
    }
    if (timeline_table_dirty_) {
        FlushPendingTimelineTableUpdates();
        timeline_table_dirty_ = false;
    }
    if (audio_visualization_dirty_) {
        FlushPendingAudioVisualization();
        audio_visualization_dirty_ = false;
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
    if (event_summary_dirty_) {
        UpdateEventSummary();
        event_summary_dirty_ = false;
    }
    if (sync_summary_dirty_) {
        UpdateSyncSummary();
        sync_summary_dirty_ = false;
    }
    if (timeline_summary_dirty_) {
        UpdateTimelineSummary();
        timeline_summary_dirty_ = false;
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

void AnalysisPanel::FlushPendingPacketTableUpdates() {
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

void AnalysisPanel::FlushPendingEventTableUpdates() {
    if (!event_table_) {
        return;
    }

    event_table_->setUpdatesEnabled(false);
    for (size_t i = event_table_synced_record_count_; i < analysis_event_records_.size(); ++i) {
        AppendEventRowToTable(analysis_event_records_[i]);
    }
    event_table_->setUpdatesEnabled(true);
    event_table_synced_record_count_ = analysis_event_records_.size();

    if (event_table_->rowCount() > 0) {
        event_table_->scrollToBottom();
    }
}

void AnalysisPanel::FlushPendingSyncTableUpdates() {
    if (!sync_table_) {
        return;
    }

    sync_table_->setUpdatesEnabled(false);
    for (size_t i = sync_table_synced_record_count_; i < sync_sample_records_.size(); ++i) {
        AppendSyncRowToTable(sync_sample_records_[i]);
    }
    sync_table_->setUpdatesEnabled(true);
    sync_table_synced_record_count_ = sync_sample_records_.size();
    UpdateSyncChart();

    if (sync_table_->rowCount() > 0) {
        sync_table_->scrollToBottom();
    }
}

void AnalysisPanel::FlushPendingTimelineTableUpdates() {
    if (!timeline_table_) {
        return;
    }

    timeline_table_->setUpdatesEnabled(false);
    for (size_t i = timeline_table_synced_record_count_; i < timeline_event_records_.size(); ++i) {
        AppendTimelineRowToTable(timeline_event_records_[i]);
    }
    timeline_table_->setUpdatesEnabled(true);
    timeline_table_synced_record_count_ = timeline_event_records_.size();
    UpdateTimelineChart();

    if (timeline_table_->rowCount() > 0) {
        timeline_table_->scrollToBottom();
    }
}

void AnalysisPanel::FlushPendingAudioVisualization() {
    UpdateAudioVisualizationCharts();
    UpdateAudioVisualizationSummary();
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

void AnalysisPanel::AppendPacketRowToTable(const PacketRecord& record) {
    const int row = packet_table_->rowCount();
    packet_table_->insertRow(row);
    SetTableItemText(packet_table_, row, 0, QString::number(record.index));
    SetTableItemText(packet_table_, row, 1, QString::number(record.stream_index));
    SetTableItemText(packet_table_, row, 2, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(packet_table_, row, 3, QString::number(record.pts));
    SetTableItemText(packet_table_, row, 4, QString::number(record.dts));
    SetTableItemText(packet_table_, row, 5, QString::number(record.duration));
    SetTableItemText(packet_table_, row, 6, QString::number(record.size));
    SetTableItemText(packet_table_, row, 7, PacketFlagsToString(record.flags));
    SetTableItemText(packet_table_, row, 8, QString::number(record.pos));
}

void AnalysisPanel::AppendEventRowToTable(const AnalysisEventRecord& record) {
    const int row = event_table_->rowCount();
    event_table_->insertRow(row);
    SetTableItemText(event_table_, row, 0, QString::number(record.index));
    SetTableItemText(event_table_, row, 1, record.severity);
    SetTableItemText(event_table_, row, 2, record.type);
    SetTableItemText(event_table_, row, 3, QString::number(record.stream_index));
    SetTableItemText(event_table_, row, 4, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(event_table_, row, 5, QString::number(record.pts));
    SetTableItemText(event_table_, row, 6, record.summary);
    SetTableItemText(event_table_, row, 7, record.detail);
}

void AnalysisPanel::AppendSyncRowToTable(const SyncSampleRecord& record) {
    const int row = sync_table_->rowCount();
    sync_table_->insertRow(row);
    SetTableItemText(sync_table_, row, 0, QString::number(record.index));
    SetTableItemText(sync_table_, row, 1, QString::number(record.audio_timestamp_seconds, 'f', 3));
    SetTableItemText(sync_table_, row, 2, QString::number(record.video_timestamp_seconds, 'f', 3));
    SetTableItemText(sync_table_, row, 3, QString::number(record.diff_ms, 'f', 3));
    SetTableItemText(sync_table_, row, 4, record.audio_anchor ? tr("音频") : tr("视频"));
}

void AnalysisPanel::AppendTimelineRowToTable(const TimelineEventRecord& record) {
    const int row = timeline_table_->rowCount();
    timeline_table_->insertRow(row);
    SetTableItemText(timeline_table_, row, 0, QString::number(record.index));
    SetTableItemText(timeline_table_, row, 1, record.category);
    SetTableItemText(timeline_table_, row, 2, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(timeline_table_, row, 3, record.label);
    SetTableItemText(timeline_table_, row, 4, record.detail);
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

void AnalysisPanel::UpdateSyncChart() {
    if (!sync_series_ || !sync_axis_x_ || !sync_axis_y_) {
        return;
    }

    sync_series_->clear();
    sync_chart_values_.clear();
    const int start = std::max(0, static_cast<int>(sync_sample_records_.size()) - kMaxChartSamples);
    for (int i = start; i < static_cast<int>(sync_sample_records_.size()); ++i) {
        const qreal diff = static_cast<qreal>(sync_sample_records_[i].diff_ms);
        sync_series_->append(sync_sample_records_[i].index, diff);
        sync_chart_values_.push_back(diff);
    }

    const int x_min = sync_sample_records_.empty() ? 0 : sync_sample_records_[start].index;
    const int x_max = sync_sample_records_.empty() ? 1 : sync_sample_records_.back().index;
    sync_axis_x_->setRange(x_min, std::max(x_min + 1, x_max));

    qreal max_abs = 1.0;
    for (qreal value : sync_chart_values_) {
        max_abs = std::max(max_abs, std::abs(value));
    }
    sync_axis_y_->setRange(-max_abs * 1.1, max_abs * 1.1);
}

void AnalysisPanel::UpdateTimelineChart() {
    if (!timeline_video_series_ || !timeline_audio_series_ || !timeline_event_series_ ||
        !timeline_axis_x_ || !timeline_axis_y_) {
        return;
    }

    timeline_video_series_->clear();
    timeline_audio_series_->clear();
    timeline_event_series_->clear();

    if (timeline_event_records_.empty()) {
        timeline_axis_x_->setRange(0.0, 1.0);
        timeline_axis_y_->setRange(0.5, 3.5);
        return;
    }

    const int start = std::max(0, static_cast<int>(timeline_event_records_.size()) - kMaxChartSamples);
    double min_ts = timeline_event_records_[start].timestamp_seconds;
    double max_ts = timeline_event_records_[start].timestamp_seconds;
    for (int i = start; i < static_cast<int>(timeline_event_records_.size()); ++i) {
        const auto& record = timeline_event_records_[i];
        if (record.category == tr("视频关键帧")) {
            timeline_video_series_->append(record.timestamp_seconds, 3.0);
        } else if (record.category == tr("音频采样")) {
            timeline_audio_series_->append(record.timestamp_seconds, 2.0);
        } else {
            timeline_event_series_->append(record.timestamp_seconds, 1.0);
        }
        min_ts = std::min(min_ts, record.timestamp_seconds);
        max_ts = std::max(max_ts, record.timestamp_seconds);
    }

    if (min_ts == max_ts) {
        max_ts += 0.001;
    }
    timeline_axis_x_->setRange(min_ts, max_ts);
    timeline_axis_y_->setRange(0.5, 3.5);
}

void AnalysisPanel::UpdateAudioVisualizationCharts() {
    if (!waveform_series_ || !spectrum_series_ ||
        !waveform_axis_x_ || !waveform_axis_y_ ||
        !spectrum_axis_x_ || !spectrum_axis_y_) {
        return;
    }

    waveform_series_->clear();
    spectrum_series_->clear();

    if (!has_audio_visualization_record_) {
        waveform_axis_x_->setRange(0, 1);
        waveform_axis_y_->setRange(-1.0, 1.0);
        spectrum_axis_x_->setRange(0, 1);
        spectrum_axis_y_->setRange(0.0, 1.0);
        return;
    }

    for (int i = 0; i < audio_visualization_record_.waveform_points.size(); ++i) {
        waveform_series_->append(i, audio_visualization_record_.waveform_points[i]);
    }
    for (int i = 0; i < audio_visualization_record_.spectrum_bins.size(); ++i) {
        spectrum_series_->append(i, audio_visualization_record_.spectrum_bins[i]);
    }

    const int waveform_max_x = std::max(1, static_cast<int>(audio_visualization_record_.waveform_points.size()) - 1);
    const int spectrum_max_x = std::max(1, static_cast<int>(audio_visualization_record_.spectrum_bins.size()) - 1);
    waveform_axis_x_->setRange(0, waveform_max_x);
    waveform_axis_y_->setRange(-1.0, 1.0);
    spectrum_axis_x_->setRange(0, spectrum_max_x);

    qreal spectrum_max = 1.0;
    for (double value : audio_visualization_record_.spectrum_bins) {
        spectrum_max = std::max(spectrum_max, static_cast<qreal>(value));
    }
    spectrum_axis_y_->setRange(0.0, spectrum_max * 1.1);
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
