#include "AnalysisPanel.h"
#include "utils/Logger.h"
#include "utils/ReportExporter.h"
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QMessageBox>
#include <QTextStream>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QtCharts>
#include <algorithm>
#include <functional>

namespace videoeye {
namespace ui {

namespace {
constexpr int kUiFlushIntervalMs = 120;
constexpr int kMaxChartSamples = 300;
constexpr size_t kMaxFrameRecords = 50000;
constexpr size_t kMaxAudioFrameRecords = 30000;
constexpr size_t kMaxPacketRecords = 10000;
constexpr size_t kMaxEventRecords = 5000;
constexpr size_t kMaxSyncRecords = 5000;
constexpr size_t kMaxTimelineRecords = 5000;

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
} // namespace

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
    
    // 默认启用: 基础功能, 关闭: 高性能分析
    feature_enabled_[AnalysisFeature::Master] = true;
    feature_enabled_[AnalysisFeature::StreamStats] = true;
    feature_enabled_[AnalysisFeature::VideoFrame] = true;
    feature_enabled_[AnalysisFeature::AudioFrame] = false;
    feature_enabled_[AnalysisFeature::Packet] = false;
    feature_enabled_[AnalysisFeature::Event] = false;
    feature_enabled_[AnalysisFeature::SyncSample] = false;
    feature_enabled_[AnalysisFeature::Timeline] = false;
    feature_enabled_[AnalysisFeature::AudioVis] = false;
    feature_enabled_[AnalysisFeature::Histogram] = false;
    
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
    main_layout->setContentsMargins(4, 4, 4, 4);
    main_layout->setSpacing(4);
    
    setMinimumSize(500, 300);
    
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
    SetupMp4BoxTab();
    SetupEbmlTab();
    
    main_layout->addWidget(tab_widget_);
    
    // 连接信号 - 移除了底部全局导出按钮，导出功能分散到各 Tab
}

bool AnalysisPanel::IsFeatureEnabled(AnalysisFeature feature) const {
    return feature_enabled_.value(feature, true);
}

QWidget* AnalysisPanel::CreateToggleHeader(AnalysisFeature feature, const QString& title, QWidget* parent) {
    QWidget* header = new QWidget(parent);
    QHBoxLayout* hlayout = new QHBoxLayout(header);
    hlayout->setContentsMargins(0, 0, 0, 4);
    
    QLabel* title_label = new QLabel(title, header);
    QFont title_font = title_label->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 1);
    title_label->setFont(title_font);
    hlayout->addWidget(title_label);
    
    hlayout->addStretch();
    
    QCheckBox* toggle = new QCheckBox(tr("启用分析"), header);
    toggle->setChecked(feature_enabled_.value(feature, true));
    toggle->setToolTip(tr("启用或禁用该分析功能，关闭可降低 CPU 占用"));
    hlayout->addWidget(toggle);
    
    // 直方图开启时需要确保 StreamAnalyzer 已启动
    connect(toggle, &QCheckBox::toggled, this, [this, feature](bool checked) {
        feature_enabled_[feature] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(feature), checked);
    });
    
    return header;
}

void AnalysisPanel::AddTabWithScroll(QWidget* tab_widget, const QString& title) {
    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(tab_widget);
    tab_widget_->addTab(scroll, title);
}

void AnalysisPanel::SetupStreamTab() {
    stream_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(stream_tab_);
    layout->addWidget(CreateToggleHeader(AnalysisFeature::StreamStats, tr("流统计"), stream_tab_));
    
    // 统计信息表格
    QGroupBox* stats_group = new QGroupBox(tr("流统计信息"), stream_tab_);
    QVBoxLayout* stats_layout = new QVBoxLayout(stats_group);
    
    stats_table_ = new QTableWidget(14, 2, stats_group);
    stats_table_->setHorizontalHeaderLabels({"参数", "值"});
    stats_table_->setColumnWidth(0, 150);
    stats_table_->setMinimumWidth(300);
    stats_table_->setMinimumHeight(200);
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
    bitrate_chart_->setMinimumWidth(280);
    bitrate_layout->addWidget(bitrate_chart_);
    charts_layout->addWidget(bitrate_group);
    
    // 帧率图表
    QGroupBox* fps_group = new QGroupBox(tr("帧率变化"), stream_tab_);
    QVBoxLayout* fps_layout = new QVBoxLayout(fps_group);
    fps_chart_ = new QChartView();
    fps_chart_->setMinimumHeight(200);
    fps_chart_->setMinimumWidth(280);
    fps_layout->addWidget(fps_chart_);
    charts_layout->addWidget(fps_group);
    
    layout->addLayout(charts_layout);
    
    AddTabWithScroll(stream_tab_, tr("流分析"));
    
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
    
    // 导出报告按钮
    export_button_ = new QPushButton(tr("导出分析报告"), stream_tab_);
    export_button_->setToolTip(tr("将当前流统计信息导出为 HTML/JSON/TXT 报告"));
    layout->addWidget(export_button_, 0, Qt::AlignRight);
    
    connect(export_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportReport);
}

void AnalysisPanel::SetupFrameTab() {
    frame_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(frame_tab_);
    layout->addWidget(CreateToggleHeader(AnalysisFeature::VideoFrame, tr("视频帧"), frame_tab_));

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    toolbar_layout->addWidget(new QLabel(tr("筛选:"), frame_tab_));
    frame_filter_combo_ = new QComboBox(frame_tab_);
    frame_filter_combo_->addItems({tr("全部帧"), tr("仅 I 帧")});
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
    frame_table_->horizontalHeader()->setMinimumSectionSize(40);
    frame_table_->setColumnWidth(0, 60);
    frame_table_->setColumnWidth(1, 70);
    frame_table_->setColumnWidth(2, 70);
    frame_table_->setColumnWidth(3, 100);
    frame_table_->setColumnWidth(5, 60);
    frame_table_->setMinimumWidth(400);
    frame_table_->setMinimumHeight(120);
    
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
    gop_table_->setMinimumWidth(500);
    gop_table_->setMinimumHeight(120);
    gop_layout->addWidget(gop_table_);
    layout->addWidget(gop_group);
    
    AddTabWithScroll(frame_tab_, tr("视频帧"));

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        OnFrameFilterChanged();
    });
    connect(export_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportFrameCsv);
}

void AnalysisPanel::SetupAudioFrameTab() {
    audio_frame_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(audio_frame_tab_);
    layout->addWidget(CreateToggleHeader(AnalysisFeature::AudioFrame, tr("音频帧"), audio_frame_tab_));

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
    audio_frame_table_->horizontalHeader()->setMinimumSectionSize(50);
    audio_frame_table_->setColumnWidth(0, 60);
    audio_frame_table_->setColumnWidth(1, 100);
    audio_frame_table_->setColumnWidth(2, 100);
    audio_frame_table_->setColumnWidth(3, 100);
    audio_frame_table_->setColumnWidth(4, 120);
    audio_frame_table_->setColumnWidth(5, 90);
    audio_frame_table_->setMinimumWidth(550);
    audio_frame_table_->setMinimumHeight(120);

    table_layout->addWidget(audio_frame_table_);
    layout->addWidget(table_group);

    AddTabWithScroll(audio_frame_tab_, tr("音频帧"));

    connect(export_audio_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportAudioFrameCsv);
}

void AnalysisPanel::SetupPacketTab() {
    packet_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(packet_tab_);
    layout->addWidget(CreateToggleHeader(AnalysisFeature::Packet, tr("数据包"), packet_tab_));

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
    packet_table_->horizontalHeader()->setMinimumSectionSize(50);
    packet_table_->setColumnWidth(0, 60);
    packet_table_->setColumnWidth(1, 70);
    packet_table_->setColumnWidth(2, 100);
    packet_table_->setColumnWidth(3, 100);
    packet_table_->setColumnWidth(4, 100);
    packet_table_->setColumnWidth(5, 100);
    packet_table_->setColumnWidth(6, 90);
    packet_table_->setColumnWidth(7, 120);
    packet_table_->setMinimumWidth(600);
    packet_table_->setMinimumHeight(120);

    table_layout->addWidget(packet_table_);
    layout->addWidget(table_group);

    AddTabWithScroll(packet_tab_, tr("包分析"));

    connect(export_packet_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportPacketCsv);
}

void AnalysisPanel::SetupEventTab() {
    event_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(event_tab_);
    layout->addWidget(CreateToggleHeader(AnalysisFeature::Event, tr("事件"), event_tab_));

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
    event_table_->horizontalHeader()->setMinimumSectionSize(50);
    event_table_->setColumnWidth(0, 60);
    event_table_->setColumnWidth(1, 70);
    event_table_->setColumnWidth(2, 90);
    event_table_->setColumnWidth(3, 70);
    event_table_->setColumnWidth(4, 100);
    event_table_->setColumnWidth(5, 100);
    event_table_->setColumnWidth(6, 200);
    event_table_->setMinimumWidth(550);
    event_table_->setMinimumHeight(120);

    table_layout->addWidget(event_table_);
    layout->addWidget(table_group);

    AddTabWithScroll(event_tab_, tr("异常事件"));

    connect(export_event_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportEventCsv);
}

void AnalysisPanel::SetupSyncTab() {
    sync_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(sync_tab_);
    layout->addWidget(CreateToggleHeader(AnalysisFeature::SyncSample, tr("同步分析"), sync_tab_));

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
    sync_chart_->setMinimumWidth(280);
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
    sync_table_->setMinimumWidth(360);
    sync_table_->setMinimumHeight(120);
    table_layout->addWidget(sync_table_);
    layout->addWidget(table_group);

    AddTabWithScroll(sync_tab_, tr("同步分析"));

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
    layout->addWidget(CreateToggleHeader(AnalysisFeature::Timeline, tr("时间线"), timeline_tab_));

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
    timeline_chart_->setMinimumWidth(280);
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
    timeline_table_->setMinimumWidth(400);
    timeline_table_->setMinimumHeight(120);
    table_layout->addWidget(timeline_table_);
    layout->addWidget(table_group);

    AddTabWithScroll(timeline_tab_, tr("统一时间轴"));

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
    layout->addWidget(CreateToggleHeader(AnalysisFeature::AudioVis, tr("音频可视化"), audio_visualization_tab_));

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
    waveform_chart_->setMinimumWidth(280);
    waveform_layout->addWidget(waveform_chart_);
    layout->addWidget(waveform_group);

    QGroupBox* spectrum_group = new QGroupBox(tr("音频频谱"), audio_visualization_tab_);
    QVBoxLayout* spectrum_layout = new QVBoxLayout(spectrum_group);
    spectrum_chart_ = new QChartView(audio_visualization_tab_);
    spectrum_chart_->setMinimumHeight(220);
    spectrum_chart_->setMinimumWidth(280);
    spectrum_layout->addWidget(spectrum_chart_);
    layout->addWidget(spectrum_group);

    AddTabWithScroll(audio_visualization_tab_, tr("音频可视化"));

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
    layout->addWidget(CreateToggleHeader(AnalysisFeature::Histogram, tr("直方图"), histogram_tab_));
    
    QGroupBox* hist_group = new QGroupBox(tr("直方图分析"), histogram_tab_);
    QVBoxLayout* hist_layout = new QVBoxLayout(hist_group);
    
    histogram_chart_ = new QChartView();
    histogram_chart_->setMinimumHeight(200);
    histogram_chart_->setMinimumWidth(400);
    hist_layout->addWidget(histogram_chart_);
    
    layout->addWidget(hist_group);
    
    // 导出按钮
    export_histogram_button_ = new QPushButton(tr("导出直方图数据"), histogram_tab_);
    export_histogram_button_->setToolTip(tr("将直方图通道数据导出为 CSV"));
    layout->addWidget(export_histogram_button_, 0, Qt::AlignRight);
    
    AddTabWithScroll(histogram_tab_, tr("直方图"));
    
    connect(export_histogram_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportHistogramCsv);
}

void AnalysisPanel::SetupMp4BoxTab() {
    mp4_box_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(mp4_box_tab_);
    layout->setContentsMargins(4, 0, 4, 4);
    layout->setSpacing(2);
    
    // 概要标签 (兼作标题，节省空间)
    mp4_box_summary_label_ = new QLabel(tr("未加载 MP4 文件"), mp4_box_tab_);
    mp4_box_summary_label_->setStyleSheet("font-size: 12px; color: #666; padding: 2px 4px;");
    mp4_box_summary_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(mp4_box_summary_label_);
    
    // 分割布局: 左侧 Box 树 + 右侧详情
    QSplitter* splitter = new QSplitter(Qt::Horizontal, mp4_box_tab_);
    
    // 左侧: Box 树
    QGroupBox* tree_group = new QGroupBox(tr("Box 树结构"), splitter);
    QVBoxLayout* tree_layout = new QVBoxLayout(tree_group);
    box_tree_widget_ = new QTreeWidget(tree_group);
    box_tree_widget_->setHeaderLabels({tr("Box 类型"), tr("大小 (字节)"), tr("偏移"), tr("关键属性")});
    box_tree_widget_->setColumnWidth(0, 130);
    box_tree_widget_->setColumnWidth(1, 100);
    box_tree_widget_->setColumnWidth(2, 80);
    box_tree_widget_->columnAt(3);
    box_tree_widget_->header()->setStretchLastSection(true);
    box_tree_widget_->header()->setMinimumSectionSize(60);
    box_tree_widget_->setAlternatingRowColors(true);
    box_tree_widget_->setAnimated(true);
    box_tree_widget_->setIndentation(20);
    box_tree_widget_->setStyleSheet(
        "QTreeWidget {"
        "  show-decoration-selected: 1;"
        "}"
        "QTreeWidget::item {"
        "  padding: 2px 4px;"
        "  border-bottom: 1px solid #e8e8e8;"
        "}"
        "QTreeWidget::branch:has-children:!has-siblings:closed,"
        "QTreeWidget::branch:closed:has-children:has-siblings {"
        "  border-image: none;"
        "  image: none;"
        "}"
        "QTreeWidget::branch:open:has-children:!has-siblings,"
        "QTreeWidget::branch:open:has-children:has-siblings {"
        "  border-image: none;"
        "  image: none;"
        "}"
    );
    box_tree_widget_->setMinimumWidth(360);
    tree_layout->addWidget(box_tree_widget_);
    splitter->addWidget(tree_group);
    
    // 右侧: 详情选项卡
    box_detail_tabs_ = new QTabWidget(splitter);
    
    // stts 表
    QWidget* stts_widget = new QWidget();
    QVBoxLayout* stts_layout = new QVBoxLayout(stts_widget);
    stts_table_ = new QTableWidget(0, 3, stts_widget);
    stts_table_->setHorizontalHeaderLabels({tr("索引"), tr("样本计数"), tr("样本增量 (timescale)")});
    stts_table_->horizontalHeader()->setStretchLastSection(true);
    stts_table_->horizontalHeader()->setMinimumSectionSize(60);
    stts_table_->setAlternatingRowColors(true);
    stts_table_->setMinimumWidth(250);
    stts_table_->setMinimumHeight(100);
    stts_layout->addWidget(stts_table_);
    box_detail_tabs_->addTab(stts_widget, tr("stts (Time-to-Sample)"));
    
    // stco 表
    QWidget* stco_widget = new QWidget();
    QVBoxLayout* stco_layout = new QVBoxLayout(stco_widget);
    stco_table_ = new QTableWidget(0, 2, stco_widget);
    stco_table_->setHorizontalHeaderLabels({tr("索引"), tr("Chunk 偏移")});
    stco_table_->horizontalHeader()->setStretchLastSection(true);
    stco_table_->horizontalHeader()->setMinimumSectionSize(60);
    stco_table_->setAlternatingRowColors(true);
    stco_table_->setMinimumWidth(200);
    stco_table_->setMinimumHeight(100);
    stco_layout->addWidget(stco_table_);
    box_detail_tabs_->addTab(stco_widget, tr("stco (Chunk Offset)"));
    
    // stsc 表
    QWidget* stsc_widget = new QWidget();
    QVBoxLayout* stsc_layout = new QVBoxLayout(stsc_widget);
    stsc_table_ = new QTableWidget(0, 4, stsc_widget);
    stsc_table_->setHorizontalHeaderLabels({tr("索引"), tr("首个 Chunk"), tr("每 Chunk 样本数"), tr("样本描述索引")});
    stsc_table_->horizontalHeader()->setStretchLastSection(true);
    stsc_table_->horizontalHeader()->setMinimumSectionSize(60);
    stsc_table_->setAlternatingRowColors(true);
    stsc_table_->setMinimumWidth(300);
    stsc_table_->setMinimumHeight(100);
    stsc_layout->addWidget(stsc_table_);
    box_detail_tabs_->addTab(stsc_widget, tr("stsc (Sample-to-Chunk)"));
    
    // stsz 表
    QWidget* stsz_widget = new QWidget();
    QVBoxLayout* stsz_layout = new QVBoxLayout(stsz_widget);
    stsz_table_ = new QTableWidget(0, 3, stsz_widget);
    stsz_table_->setHorizontalHeaderLabels({tr("索引"), tr("样本大小 (字节)"), tr("备注")});
    stsz_table_->horizontalHeader()->setStretchLastSection(true);
    stsz_table_->horizontalHeader()->setMinimumSectionSize(60);
    stsz_table_->setAlternatingRowColors(true);
    stsz_table_->setMinimumWidth(250);
    stsz_table_->setMinimumHeight(100);
    stsz_layout->addWidget(stsz_table_);
    box_detail_tabs_->addTab(stsz_widget, tr("stsz (Sample Size)"));
    
    // stss 表
    QWidget* stss_widget = new QWidget();
    QVBoxLayout* stss_layout = new QVBoxLayout(stss_widget);
    stss_table_ = new QTableWidget(0, 2, stss_widget);
    stss_table_->setHorizontalHeaderLabels({tr("索引"), tr("关键帧样本号")});
    stss_table_->horizontalHeader()->setStretchLastSection(true);
    stss_table_->horizontalHeader()->setMinimumSectionSize(60);
    stss_table_->setAlternatingRowColors(true);
    stss_table_->setMinimumWidth(200);
    stss_table_->setMinimumHeight(100);
    stss_layout->addWidget(stss_table_);
    box_detail_tabs_->addTab(stss_widget, tr("stss (Sync Sample)"));
    
    // co64 表
    QWidget* co64_widget = new QWidget();
    QVBoxLayout* co64_layout = new QVBoxLayout(co64_widget);
    co64_table_ = new QTableWidget(0, 2, co64_widget);
    co64_table_->setHorizontalHeaderLabels({tr("索引"), tr("Chunk 偏移 (64位)")});
    co64_table_->horizontalHeader()->setStretchLastSection(true);
    co64_table_->horizontalHeader()->setMinimumSectionSize(60);
    co64_table_->setAlternatingRowColors(true);
    co64_table_->setMinimumWidth(200);
    co64_table_->setMinimumHeight(100);
    co64_layout->addWidget(co64_table_);
    box_detail_tabs_->addTab(co64_widget, tr("co64 (64-bit Chunk)"));
    
    splitter->addWidget(box_detail_tabs_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setMinimumWidth(540);
    splitter->setChildrenCollapsible(false);
    
    layout->addWidget(splitter);
    
    // 导出按钮
    export_mp4_box_button_ = new QPushButton(tr("导出 Box 数据"), mp4_box_tab_);
    export_mp4_box_button_->setToolTip(tr("将 Box 树结构和表格数据导出为文本文件"));
    layout->addWidget(export_mp4_box_button_, 0, Qt::AlignRight);
    
    AddTabWithScroll(mp4_box_tab_, tr("MP4 Box"));
    
    connect(export_mp4_box_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportMp4Box);
}

void AnalysisPanel::SetupEbmlTab() {
    ebml_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(ebml_tab_);
    layout->setContentsMargins(4, 0, 4, 4);
    layout->setSpacing(2);

    ebml_summary_label_ = new QLabel(tr("未加载 MKV/WebM 文件"), ebml_tab_);
    ebml_summary_label_->setStyleSheet("font-size: 12px; color: #666; padding: 2px 4px;");
    ebml_summary_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(ebml_summary_label_);

    // 左右分割: 左侧元素树, 右侧详情表
    QSplitter* splitter = new QSplitter(Qt::Horizontal, ebml_tab_);

    // --- 左侧: 元素树 ---
    QWidget* leftPanel = new QWidget(splitter);
    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    QLabel* treeLabel = new QLabel(tr("EBML 元素树:"), leftPanel);
    treeLabel->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    leftLayout->addWidget(treeLabel);
    ebml_tree_ = new QTreeWidget(leftPanel);
    ebml_tree_->setHeaderLabels({tr("元素名称"), tr("大小"), tr("偏移"), tr("值")});
    ebml_tree_->setColumnWidth(0, 160);
    ebml_tree_->setColumnWidth(1, 70);
    ebml_tree_->setColumnWidth(2, 70);
    ebml_tree_->header()->setStretchLastSection(true);
    ebml_tree_->setAlternatingRowColors(true);
    ebml_tree_->setAnimated(true);
    ebml_tree_->setIndentation(16);
    ebml_tree_->setStyleSheet(
        "QTreeWidget::item { padding: 1px 3px; font-size: 11px; }"
    );
    leftLayout->addWidget(ebml_tree_);
    splitter->addWidget(leftPanel);

    // --- 右侧: 详情表页 (轨道/Cues/Block) ---
    QWidget* rightPanel = new QWidget(splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    ebml_detail_tabs_ = new QTabWidget(rightPanel);
    ebml_detail_tabs_->setStyleSheet("QTabWidget::pane { border: 1px solid #ccc; }");

    // 轨道表
    ebml_track_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_track_table_->setColumnCount(10);
    ebml_track_table_->setHorizontalHeaderLabels({
        tr("#"), tr("类型"), tr("编码"), tr("CodecID"),
        tr("分辨率/采样率"), tr("声道"), tr("语言"),
        tr("帧率"), tr("默认"), tr("强制")
    });
    ebml_track_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_track_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_track_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_track_table_, tr("轨道 (%1)").arg(0));

    // Cues 索引表
    ebml_cue_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_cue_table_->setColumnCount(4);
    ebml_cue_table_->setHorizontalHeaderLabels({
        tr("#"), tr("时间"), tr("轨道"), tr("Cluster 偏移")
    });
    ebml_cue_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_cue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_cue_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_cue_table_, tr("Cues (%1)").arg(0));

    // Block 摘要表
    ebml_block_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_block_table_->setColumnCount(5);
    ebml_block_table_->setHorizontalHeaderLabels({
        tr("#"), tr("轨道"), tr("Timecode"), tr("关键帧"), tr("大小")
    });
    ebml_block_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_block_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_block_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_block_table_, tr("Block (%1)").arg(0));

    rightLayout->addWidget(ebml_detail_tabs_);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    // 导出
    export_ebml_button_ = new QPushButton(tr("导出 EBML 结构"), ebml_tab_);
    export_ebml_button_->setToolTip(tr("将 EBML 元素树结构导出为文本文件"));
    layout->addWidget(export_ebml_button_, 0, Qt::AlignRight);

    AddTabWithScroll(ebml_tab_, tr("MKV/WebM"));

    connect(export_ebml_button_, &QPushButton::clicked, this, [this]() {
        if (!current_ebml_result_.valid) {
            QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 EBML 数据。"));
            return;
        }
        const QString filename = QFileDialog::getSaveFileName(
            this, tr("导出 EBML 结构"),
            QString("videoeye_ebml_%1.txt")
                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
            tr("文本文件 (*.txt);;所有文件 (*)"));
        if (filename.isEmpty()) return;
        QFile file(filename);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);
        std::function<void(const QVector<model::EbmlElementNode>&, int)> printTree;
        printTree = [&](const QVector<model::EbmlElementNode>& nodes, int d) {
            for (const auto& n : nodes) {
                QString indent(d * 2, ' ');
                out << indent << n.name << " [" << n.id_hex
                    << "]  size=" << n.size
                    << "  offset=0x" << Qt::hex << n.startOffset() << Qt::dec;
                if (!n.value.isEmpty())
                    out << "  value=" << n.value;
                out << "\n";
                printTree(n.children, d + 1);
            }
        };
        out << "DocType: " << current_ebml_result_.doc_type
            << " v" << current_ebml_result_.doc_type_version << "\n\n";
        printTree(current_ebml_result_.element_tree, 0);
        QMessageBox::information(this, tr("导出成功"),
                                 tr("已导出到:\n%1").arg(filename));
    });
}

void AnalysisPanel::OnEbmlAnalysisReady(const model::EbmlAnalysisResult& result) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    current_ebml_result_ = result;

    if (result.valid) {
        int total = 0;
        std::function<int(const QVector<model::EbmlElementNode>&)> count;
        count = [&](const QVector<model::EbmlElementNode>& nodes) -> int {
            int c = nodes.size();
            for (const auto& n : nodes) c += count(n.children);
            return c;
        };
        total = count(result.element_tree);
        ebml_summary_label_->setText(
            tr("%1 | %2 个元素").arg(result.doc_type).arg(total));
        ebml_summary_label_->setStyleSheet(
            "font-size: 12px; color: #333; font-weight: bold; padding: 2px 4px;");
    } else {
        ebml_summary_label_->setText(tr("未检测到 EBML 格式"));
        ebml_summary_label_->setStyleSheet(
            "font-size: 12px; color: #666; padding: 2px 4px;");
    }

    ebml_tree_->clear();

    if (!result.valid) return;

    static const QColor kDepthColors[] = {
        QColor("#f0f4ff"), QColor("#f5f5f5"), QColor("#fffff0"),
        QColor("#f0fff0"), QColor("#fff0f5"), QColor("#f0ffff"),
        QColor("#faf5f0"),
    };

    std::function<void(QTreeWidgetItem*, const QVector<model::EbmlElementNode>&)> addNodes;
    addNodes = [&](QTreeWidgetItem* parent, const QVector<model::EbmlElementNode>& nodes) {
        for (const auto& n : nodes) {
            auto* item = new QTreeWidgetItem();
            item->setText(0, n.name);
            item->setText(1, QString::number(n.size));
            item->setText(2, QString("0x%1").arg(n.startOffset(), 0, 16));
            item->setText(3, n.value);

            int d = n.depth;
            QColor bg = kDepthColors[d % 7];
            for (int c = 0; c < 4; ++c) item->setBackground(c, bg);

            if (parent) parent->addChild(item);
            else ebml_tree_->addTopLevelItem(item);
            addNodes(item, n.children);
        }
    };
    for (const auto& n : result.element_tree) {
        auto* top = new QTreeWidgetItem();
        top->setText(0, n.name);
        top->setText(1, QString::number(n.size));
        top->setText(2, QString("0x%1").arg(n.startOffset(), 0, 16));
        top->setText(3, n.value);
        ebml_tree_->addTopLevelItem(top);
        addNodes(top, n.children);
    }
    ebml_tree_->expandAll();

    // --- 填充右侧详情表 ---
    // 轨道表
    {
        ebml_track_table_->setRowCount(result.tracks.size());
        for (int i = 0; i < result.tracks.size(); ++i) {
            const auto& t = result.tracks[i];
            auto set = [&](int col, const QString& v) {
                ebml_track_table_->setItem(i, col, new QTableWidgetItem(v));
            };
            set(0, QString::number(t.track_number));
            set(1, t.track_type_name);
            set(2, t.codec_name);
            set(3, t.codec_id);
            if (t.track_type == 1) {
                set(4, QString("%1x%2").arg(t.pixel_width).arg(t.pixel_height));
                set(5, "-");
                set(7, t.frame_rate > 0 ? QString("%1 fps").arg(t.frame_rate, 0, 'f', 2) : "-");
            } else if (t.track_type == 2) {
                set(4, t.sampling_frequency > 0 ? QString("%1 Hz").arg(t.sampling_frequency, 0, 'f', 0) : "-");
                set(5, t.channels > 0 ? QString::number(t.channels) : "-");
                set(7, "-");
            } else {
                set(4, "-");
                set(5, "-");
                set(7, "-");
            }
            set(6, t.language);
            set(8, t.default_track ? "✓" : "");
            set(9, t.forced ? "✓" : "");
        }
        ebml_detail_tabs_->setTabText(0, tr("轨道 (%1)").arg(result.tracks.size()));
    }

    // Cues 表
    {
        ebml_cue_table_->setRowCount(result.cues.size());
        for (int i = 0; i < result.cues.size(); ++i) {
            const auto& c = result.cues[i];
            auto set = [&](int col, const QString& v) {
                ebml_cue_table_->setItem(i, col, new QTableWidgetItem(v));
            };
            set(0, QString::number(i + 1));
            set(1, QString::number(c.time));
            set(2, QString::number(c.track_number));
            set(3, QString("0x%1").arg(c.cluster_position, 0, 16));
        }
        ebml_detail_tabs_->setTabText(1, tr("Cues (%1)").arg(result.cues.size()));
    }

    // Block 摘要表 (最多 500 行)
    {
        int n = qMin(result.blocks.size(), 500);
        ebml_block_table_->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            const auto& b = result.blocks[i];
            auto set = [&](int col, const QString& v) {
                ebml_block_table_->setItem(i, col, new QTableWidgetItem(v));
            };
            set(0, QString::number(i + 1));
            set(1, QString::number(b.track_number));
            set(2, QString::number(b.timecode));
            set(3, b.keyframe ? tr("KEY") : "");
            set(4, QString::number(b.data_size));
        }
        ebml_detail_tabs_->setTabText(2, tr("Block (%1/%2)")
            .arg(n).arg(result.blocks.size()));
    }
}

// 前向声明辅助函数
static void PopulateMp4BoxTablesImpl(const model::Mp4BoxAnalysisResult& result,
                                       QTableWidget* stts_table, QTableWidget* stco_table,
                                       QTableWidget* stsc_table, QTableWidget* stsz_table,
                                       QTableWidget* co64_table, QTableWidget* stss_table);

void AnalysisPanel::OnMp4BoxAnalysisReady(const model::Mp4BoxAnalysisResult& result) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Mp4Box, true)) return;
    
    current_box_result_ = result;
    
    if (!result.valid) {
        mp4_box_summary_label_->setText(tr("MP4 Box 分析失败: %1").arg(result.error_message));
        return;
    }
    
    // 更新概要
    int box_count = 0;
    auto countBoxes = [&](const auto& nodes, auto&& self_ref) -> void {
        for (const auto& node : nodes) {
            box_count++;
            self_ref(node.children, self_ref);
        }
    };
    countBoxes(result.box_tree, countBoxes);
    
    mp4_box_summary_label_->setText(
        tr("MP4 Box 分析完成 | 顶级 Box: %1 个 | 总 Box: %2 个 | Track 表: %3 个")
        .arg(result.box_tree.size())
        .arg(box_count)
        .arg(result.track_tables.size()));
    
    // 填充 Box 树
    box_tree_widget_->clear();
    
    // 深度对应的背景色 (浅色梯度，层次清晰)
    static const QColor kDepthColors[] = {
        QColor("#f0f4ff"),  // depth 0 - 淡蓝
        QColor("#f5f5f5"),  // depth 1 - 浅灰
        QColor("#fffff0"),  // depth 2 - 淡黄
        QColor("#f0fff0"),  // depth 3 - 淡绿
        QColor("#fff0f5"),  // depth 4 - 淡粉
        QColor("#f0ffff"),  // depth 5 - 淡青
        QColor("#faf5f0"),  // depth 6 - 淡橙
    };
    constexpr int kColorCount = sizeof(kDepthColors) / sizeof(kDepthColors[0]);
    
    auto makeItem = [&](const model::Mp4BoxNode& node) -> QTreeWidgetItem* {
        auto item = new QTreeWidgetItem();
        item->setText(0, node.type);
        item->setText(1, QString::number(node.size));
        item->setData(1, Qt::UserRole, static_cast<qulonglong>(node.size));
        
        // 偏移列 (十六进制)
        item->setText(2, QString("0x%1").arg(node.offset, 0, 16));
        
        // 深度层次背景色
        int depth = static_cast<int>(node.depth);
        QColor bg = kDepthColors[depth % kColorCount];
        for (int col = 0; col < 4; ++col) {
            item->setBackground(col, bg);
        }
        
        // 字段信息: tooltip + 关键属性摘要
        QString tooltip;
        QString key_props;
        for (const auto& f : node.fields) {
            if (!tooltip.isEmpty()) tooltip += "\n";
            tooltip += f.name + ": " + f.value;
            if (f.name == "handler_type" || f.name == "id" ||
                f.name == "timescale" || f.name == "duration" ||
                f.name == "width" || f.name == "height" ||
                f.name == "sample_rate" || f.name == "channel_count" ||
                f.name == "entry_count" || f.name == "sample_size" ||
                f.name == "sample_count" || f.name == "major_brand" ||
                f.name == "minor_version" ||
                f.name == "language" || f.name == "handler_name" ||
                f.name == "balance" || f.name == "graphics_mode" ||
                f.name.startsWith("compatible_brand") ||
                f.name.startsWith("entry_count")) {
                if (!key_props.isEmpty()) key_props += " | ";
                key_props += f.name + "=" + f.value;
            }
        }
        if (!tooltip.isEmpty()) {
            item->setToolTip(0, tooltip);
            item->setToolTip(1, tooltip);
            item->setToolTip(2, tooltip);
            item->setToolTip(3, tooltip);
        }
        if (!key_props.isEmpty()) {
            item->setText(3, key_props);
        }
        return item;
    };
    
    std::function<void(QTreeWidgetItem*, const QVector<model::Mp4BoxNode>&)> addNodes;
    addNodes = [&](QTreeWidgetItem* parent, const QVector<model::Mp4BoxNode>& nodes) {
        for (const auto& node : nodes) {
            auto item = makeItem(node);
            if (parent) {
                parent->addChild(item);
            } else {
                box_tree_widget_->addTopLevelItem(item);
            }
            addNodes(item, node.children);
        }
    };
    
    for (const auto& node : result.box_tree) {
        auto root = makeItem(node);
        box_tree_widget_->addTopLevelItem(root);
        addNodes(root, node.children);
    }
    box_tree_widget_->expandAll();
    
    // 填充 Track 表数据
    PopulateMp4BoxTablesImpl(result, stts_table_, stco_table_, stsc_table_,
                             stsz_table_, co64_table_, stss_table_);
}

// 辅助方法: 填充 MP4 Box 详情表
static void PopulateMp4BoxTablesImpl(const model::Mp4BoxAnalysisResult& result,
                                       QTableWidget* stts_table, QTableWidget* stco_table,
                                       QTableWidget* stsc_table, QTableWidget* stsz_table,
                                       QTableWidget* co64_table, QTableWidget* stss_table) {
    stts_table->setRowCount(0);
    stco_table->setRowCount(0);
    stsc_table->setRowCount(0);
    stsz_table->setRowCount(0);
    stss_table->setRowCount(0);
    co64_table->setRowCount(0);
    
    for (const auto& track : result.track_tables) {
        // 添加 Track 分隔行
        auto addTrackHeader = [](QTableWidget* table, const QString& text) {
            const int row = table->rowCount();
            table->insertRow(row);
            auto* item = new QTableWidgetItem(text);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            table->setItem(row, 0, item);
        };

        // stts
        if (!track.stts_entries.isEmpty()) {
            addTrackHeader(stts_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.stts_entries.size(); ++i) {
                const int row = stts_table->rowCount();
                stts_table->insertRow(row);
                stts_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                stts_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stts_entries[i].sample_count)));
                stts_table->setItem(row, 2, new QTableWidgetItem(QString::number(track.stts_entries[i].sample_delta)));
            }
        }
        
        // stco
        if (!track.stco_entries.isEmpty()) {
            addTrackHeader(stco_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.stco_entries.size(); ++i) {
                const int row = stco_table->rowCount();
                stco_table->insertRow(row);
                stco_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                stco_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stco_entries[i].chunk_offset)));
            }
        }
        
        // co64 (单独表格显示 64-bit chunk offset)
        if (!track.co64_entries.isEmpty()) {
            addTrackHeader(co64_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.co64_entries.size(); ++i) {
                const int row = co64_table->rowCount();
                co64_table->insertRow(row);
                co64_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                co64_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.co64_entries[i].chunk_offset)));
            }
        }
        
        // stsc
        if (!track.stsc_entries.isEmpty()) {
            addTrackHeader(stsc_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.stsc_entries.size(); ++i) {
                const int row = stsc_table->rowCount();
                stsc_table->insertRow(row);
                stsc_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                stsc_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stsc_entries[i].first_chunk)));
                stsc_table->setItem(row, 2, new QTableWidgetItem(QString::number(track.stsc_entries[i].samples_per_chunk)));
                stsc_table->setItem(row, 3, new QTableWidgetItem(QString::number(track.stsc_entries[i].sample_description_index)));
            }
        }
        
        // stsz
        if (!track.stsz_entries.isEmpty() || track.stsz_default_size > 0) {
            addTrackHeader(stsz_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            if (track.stsz_default_size > 0 && track.stsz_entries.isEmpty()) {
                // 固定大小模式
                const int row = stsz_table->rowCount();
                stsz_table->insertRow(row);
                stsz_table->setItem(row, 0, new QTableWidgetItem("0"));
                stsz_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stsz_default_size)));
                stsz_table->setItem(row, 2, new QTableWidgetItem(QString("固定大小, %1 个样本").arg(track.stsz_sample_count)));
            } else {
                for (int i = 0; i < track.stsz_entries.size(); ++i) {
                    const int row = stsz_table->rowCount();
                    stsz_table->insertRow(row);
                    stsz_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                    stsz_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stsz_entries[i].sample_size)));
                }
            }
        }

        // stss (关键帧列表)
        if (!track.stss_entries.isEmpty()) {
            addTrackHeader(stss_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.stss_entries.size(); ++i) {
                const int row = stss_table->rowCount();
                stss_table->insertRow(row);
                stss_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                stss_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stss_entries[i].sample_number)));
            }
        }
    }
    
    // 更新列宽自适应
    for (auto* table : {stts_table, stco_table, stsc_table, stsz_table,
                        co64_table, stss_table}) {
        table->resizeColumnsToContents();
    }
}

void AnalysisPanel::UpdateStreamStats(const analyzer::StreamStats& stats) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::StreamStats, true)) return;
    current_stats_ = stats;
    pending_stream_stats_ = stats;
    has_pending_stream_stats_ = true;
}

void AnalysisPanel::UpdateHistogram(const analyzer::HistogramData& hist) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Histogram, true)) return;
    current_hist_ = hist;
    UpdateHistogramChart(hist);
}

void AnalysisPanel::ResetVideoFrameList() {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::VideoFrame, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioFrame, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Packet, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Event, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::SyncSample, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Timeline, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioVis, true)) return;
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
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::VideoFrame, true)) return;
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

void AnalysisPanel::AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds,
                                         int sample_count, int sample_rate, int channels, int byte_count) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioFrame, true)) return;
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

void AnalysisPanel::AppendPacketInfo(const model::PacketInfo& packet_info) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Packet, true)) return;
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

void AnalysisPanel::AppendAnalysisEvent(const model::AnalysisEvent& event_info) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Event, true)) return;
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
    TrimRecords(analysis_event_records_, event_table_synced_record_count_, event_table_, event_table_dirty_, kMaxEventRecords);
}

void AnalysisPanel::AppendSyncSample(const model::SyncSample& sample) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::SyncSample, true)) return;
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
    TrimRecords(sync_sample_records_, sync_table_synced_record_count_, sync_table_, sync_table_dirty_, kMaxSyncRecords);
}

void AnalysisPanel::AppendTimelineEvent(const model::TimelineEvent& event) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Timeline, true)) return;
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
    TrimRecords(timeline_event_records_, timeline_table_synced_record_count_, timeline_table_, timeline_table_dirty_, kMaxTimelineRecords);
}

void AnalysisPanel::AppendAudioVisualization(const model::AudioVisualizationFrame& frame) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioVis, true)) return;
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

void AnalysisPanel::OnExportHistogramCsv() {
    if (current_hist_.bins == 0 || current_hist_.red_channel.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的直方图数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出直方图数据 CSV"),
        QString("videoeye_histogram_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("CSV 文件 (*.csv);;所有文件 (*)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "bin,red,green,blue,gray\n";
    for (int i = 0; i < current_hist_.bins && i < static_cast<int>(current_hist_.red_channel.size()); ++i) {
        out << i << ','
            << QString::number(current_hist_.red_channel[i], 'f', 6) << ','
            << QString::number(current_hist_.green_channel[i], 'f', 6) << ','
            << QString::number(current_hist_.blue_channel[i], 'f', 6) << ','
            << QString::number(current_hist_.gray_channel[i], 'f', 6) << '\n';
    }

    QMessageBox::information(this, tr("成功"), tr("直方图数据已导出到:\n%1").arg(filename));
}

void AnalysisPanel::OnExportMp4Box() {
    if (!current_box_result_.valid || current_box_result_.box_tree.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 MP4 Box 数据。"));
        return;
    }

    const QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出 MP4 Box 数据"),
        QString("videoeye_boxes_%1.txt").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("文本文件 (*.txt);;所有文件 (*)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // 导出 Box 树结构
    out << "========================================\n";
    out << "  MP4 Box 树结构\n";
    out << "========================================\n\n";

    std::function<void(const QVector<model::Mp4BoxNode>&, int)> printTree;
    printTree = [&](const QVector<model::Mp4BoxNode>& nodes, int depth) {
        for (const auto& node : nodes) {
            QString indent(depth * 2, ' ');
            out << indent << node.type
                << " | size=" << node.size
                << " | offset=" << node.offset;
            // 输出字段信息（handler type、version、flags 等）
            for (const auto& f : node.fields) {
                out << " | " << f.name << "=" << f.value;
            }
            out << '\n';
            printTree(node.children, depth + 1);
        }
    };
    printTree(current_box_result_.box_tree, 0);

    // 导出各 Track 的表格数据
    for (const auto& track : current_box_result_.track_tables) {
        out << "\n--- Track: " << track.track_type
            << " (ID=" << track.track_id << ") ---\n\n";

        // stts
        if (!track.stts_entries.isEmpty()) {
            out << "stts (Time-to-Sample):\n";
            out << "  Index\tSampleCount\tSampleDelta\n";
            for (int i = 0; i < track.stts_entries.size(); ++i) {
                out << "  " << i << "\t"
                    << track.stts_entries[i].sample_count << "\t"
                    << track.stts_entries[i].sample_delta << '\n';
            }
            out << '\n';
        }

        // stco
        if (!track.stco_entries.isEmpty()) {
            out << "stco (Chunk Offset):\n";
            out << "  Index\tChunkOffset\n";
            for (int i = 0; i < track.stco_entries.size(); ++i) {
                out << "  " << i << "\t"
                    << track.stco_entries[i].chunk_offset << '\n';
            }
            out << '\n';
        }

        // stsc
        if (!track.stsc_entries.isEmpty()) {
            out << "stsc (Sample-to-Chunk):\n";
            out << "  Index\tFirstChunk\tSamplesPerChunk\tSampleDescIndex\n";
            for (int i = 0; i < track.stsc_entries.size(); ++i) {
                out << "  " << i << "\t"
                    << track.stsc_entries[i].first_chunk << "\t"
                    << track.stsc_entries[i].samples_per_chunk << "\t"
                    << track.stsc_entries[i].sample_description_index << '\n';
            }
            out << '\n';
        }

        // stsz
        if (!track.stsz_entries.isEmpty() || track.stsz_default_size > 0) {
            out << "stsz (Sample Size):\n";
            if (track.stsz_default_size > 0 && track.stsz_entries.isEmpty()) {
                out << "  ConstantSize=" << track.stsz_default_size
                    << " (all " << track.stsz_sample_count << " samples)\n";
            } else {
                out << "  Index\tSize";
                if (track.stsz_default_size > 0) {
                    out << "\t[default=" << track.stsz_default_size << "]";
                }
                out << '\n';
                for (int i = 0; i < track.stsz_entries.size(); ++i) {
                    out << "  " << i << "\t"
                        << track.stsz_entries[i].sample_size << '\n';
                }
            }
            out << '\n';
        }

        // stss (关键帧列表)
        if (!track.stss_entries.isEmpty()) {
            out << "stss (Sync Sample / Key Frames):\n";
            out << "  Index\tSampleNumber\n";
            for (int i = 0; i < track.stss_entries.size(); ++i) {
                out << "  " << i << "\t"
                    << track.stss_entries[i].sample_number << '\n';
            }
            out << '\n';
        }

        // co64 (64-bit Chunk Offset)
        if (!track.co64_entries.isEmpty()) {
            out << "co64 (64-bit Chunk Offset):\n";
            out << "  Index\tChunkOffset\n";
            for (int i = 0; i < track.co64_entries.size(); ++i) {
                out << "  " << i << "\t"
                    << track.co64_entries[i].chunk_offset << '\n';
            }
            out << '\n';
        }
    }

    out << "========================================\n";
    out << "  报告结束\n";
    out << "========================================\n";

    QMessageBox::information(this, tr("成功"), tr("MP4 Box 数据已导出到:\n%1").arg(filename));
}

void AnalysisPanel::OnFrameFilterChanged() {
    RebuildFrameTable();
    UpdateFrameSummary();
}

void AnalysisPanel::FlushPendingUiUpdates() {
    // 面板不可见时跳过UI刷新以节省CPU
    if (!isVisible()) return;

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
        tr("HTML文件 (*.html);;JSON文件 (*.json);;文本文件 (*.txt);;所有文件 (*)"));
    
    if (filename.isEmpty()) {
        return;
    }
    
    LOG_INFO("导出分析报告: " + filename.toStdString());
    
    bool success = false;
    const QString ext = QFileInfo(filename).suffix().toLower();
    const std::string fname = filename.toStdString();
    
    // 将 deque 数据转换为 vector 供导出 API 使用
    std::vector<double> fps_history(fps_chart_values_.begin(), fps_chart_values_.end());
    std::vector<int> bitrate_history;
    bitrate_history.reserve(bitrate_chart_values_.size());
    for (qreal v : bitrate_chart_values_) {
        bitrate_history.push_back(static_cast<int>(v * 1000)); // Kbps → bps
    }
    
    if (ext == "html") {
        success = utils::ReportExporter::ExportHTMLReport(
            fname, current_stats_, fps_history, bitrate_history, current_video_path_);
    } else if (ext == "json") {
        success = utils::ReportExporter::ExportJSON(
            fname, current_stats_, current_video_path_);
    } else if (ext == "txt") {
        success = utils::ReportExporter::ExportTextReport(
            fname, current_stats_, current_video_path_);
    } else {
        // 未知扩展名，默认生成 HTML
        filename += ".html";
        success = utils::ReportExporter::ExportHTMLReport(
            filename.toStdString(), current_stats_, fps_history, bitrate_history, current_video_path_);
    }
    
    if (success) {
        QMessageBox::information(this, tr("成功"), tr("分析报告已导出到:\n%1").arg(filename));
    } else {
        QMessageBox::warning(this, tr("错误"), tr("导出分析报告失败:\n%1").arg(filename));
    }
}

} // namespace ui
} // namespace videoeye
