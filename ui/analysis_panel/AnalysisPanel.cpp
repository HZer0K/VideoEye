#include "AnalysisPanel.h"
#include "utils/Logger.h"
#include "utils/ReportExporter.h"
#include "ui/theme/AppTheme.h"
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
#include <QPainter>
#include <QPen>
#include <QtCharts>
#include <algorithm>
#include <functional>
#include <cmath>
#include <climits>

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
    , sync_series_(nullptr)
    , timeline_video_series_(nullptr)
    , timeline_audio_series_(nullptr)
    , timeline_event_series_(nullptr)
    , sync_axis_x_(nullptr)
    , sync_axis_y_(nullptr)
    , timeline_axis_x_(nullptr)
    , timeline_axis_y_(nullptr) {
    
    // 默认启用: 基础功能, 关闭: 高性能分析
    feature_enabled_[AnalysisFeature::Master] = true;
    feature_enabled_[AnalysisFeature::StreamStats] = true;
    feature_enabled_[AnalysisFeature::VideoFrame] = true;
    feature_enabled_[AnalysisFeature::AudioFrame] = false;
    feature_enabled_[AnalysisFeature::Packet] = false;
    feature_enabled_[AnalysisFeature::Event] = false;
    feature_enabled_[AnalysisFeature::SyncSample] = false;
    feature_enabled_[AnalysisFeature::Timeline] = false;
    feature_enabled_[AnalysisFeature::AudioLoudness] = true;
    feature_enabled_[AnalysisFeature::Histogram] = false;
    feature_enabled_[AnalysisFeature::ContainerStructure] = true;
    feature_enabled_[AnalysisFeature::Macroblock] = false;
    feature_enabled_[AnalysisFeature::SceneChange] = false;
    feature_enabled_[AnalysisFeature::Quality] = false;
    
    SetupUI();
    
    update_timer_ = new QTimer(this);
    connect(update_timer_, &QTimer::timeout, this, &AnalysisPanel::FlushPendingUiUpdates);
    update_timer_->start(kUiFlushIntervalMs);
    
    LOG_INFO("分析面板已初始化");
}

AnalysisPanel::~AnalysisPanel() {
    // 若质量评估仍在后台线程运行, 置取消标志并等待其结束, 避免 std::thread
    // 析构时调用 std::terminate。已排队的 QMetaCallEvent 会由 ~QObject 自动移除。
    quality_analyzer_.cancel_ = true;
    if (quality_thread_.joinable()) {
        quality_thread_.join();
    }
    LOG_INFO("分析面板已销毁");
}

void AnalysisPanel::SetupUI() {
    // 不再创建内部 QTabWidget，页面由 AddPageWithScroll 收集
    // PopulateStackedWidget 时添加到外部 QStackedWidget
    
    SetupStreamTab();
    SetupFrameTab();
    SetupPacketTab();
    SetupEventAnalysisTab();
    SetupAudioLoudnessTab();
    SetupHistogramTab();
    SetupContainerStructureTab();
    SetupMacroblockTab();
    SetupSceneChangeTab();
    SetupQualityTab();

    qRegisterMetaType<analyzer::SceneChangeResult>();
    qRegisterMetaType<analyzer::QualityFrameResult>();
    qRegisterMetaType<analyzer::QualitySummary>();
}

bool AnalysisPanel::IsFeatureEnabled(AnalysisFeature feature) const {
    return feature_enabled_.value(feature, true);
}

void AnalysisPanel::EmitInitialFeatureStates() {
    // 对每个启用状态的 feature 重新发射信号 (除了 Master 和 Mp4Box)
    static const AnalysisFeature kFeatures[] = {
        AnalysisFeature::StreamStats,
        AnalysisFeature::VideoFrame,
        AnalysisFeature::AudioFrame,
        AnalysisFeature::Packet,
        AnalysisFeature::Event,
        AnalysisFeature::SyncSample,
        AnalysisFeature::Timeline,
        AnalysisFeature::AudioLoudness,
        AnalysisFeature::Histogram,
        AnalysisFeature::Macroblock,
    };
    for (auto feat : kFeatures) {
        bool enabled = feature_enabled_.value(feat, true);
        emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
    }
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

void AnalysisPanel::AddPageWithScroll(QWidget* tab_widget, const QString& title) {
    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(tab_widget);
    page_widgets_.append(scroll);
    page_titles_.append(title);
}

int AnalysisPanel::PopulateStackedWidget(QStackedWidget* stack) {
    external_stack_ = stack;
    for (int i = 0; i < page_widgets_.size(); ++i) {
        stack->addWidget(page_widgets_[i]);
    }
    return page_widgets_.size();
}

void AnalysisPanel::SetCurrentPageIndex(int index) {
    if (external_stack_ && index >= 0 && index < page_widgets_.size()) {
        // 外部 stack 的 page 0 是媒体信息，分析页从 page 1 开始
        external_stack_->setCurrentIndex(index + 1);
    }
}

void AnalysisPanel::SetupStreamTab() {
    stream_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(stream_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 流统计信息标题 + 启用分析复选框 同一行
    {
        QWidget* row = new QWidget(stream_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("流概览"), row);
        rl->addWidget(title);
        rl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用分析"), row);
        toggle->setChecked(feature_enabled_.value(AnalysisFeature::StreamStats, true));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_enabled_[AnalysisFeature::StreamStats] = checked;
            emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::StreamStats), checked);
        });
        rl->addWidget(toggle);
        layout->addWidget(row);
    }

    // 流概览表格 (仅流级指标：码率/时长/GOP；包级统计见「数据包」tab)
    // 参数名一行 (表头) + 值一行，无多余空白
    QStringList stream_labels = {
        tr("当前码率"), tr("平均码率"), tr("峰值码率"), tr("分析时长"), tr("最大GOP大小")
    };
    stats_table_ = new QTableWidget(1, stream_labels.size(), stream_tab_);
    stats_table_->setHorizontalHeaderLabels(stream_labels);
    stats_table_->verticalHeader()->setVisible(false);
    for (int c = 0; c < stream_labels.size(); ++c) {
        stats_table_->setColumnWidth(c, 130);
        stats_table_->setItem(0, c, new QTableWidgetItem("0"));
    }

    layout->addWidget(stats_table_);

    AddPageWithScroll(stream_tab_, tr("流分析"));
    
    // 导出报告按钮
    export_button_ = new QPushButton(tr("导出分析报告"), stream_tab_);
    export_button_->setToolTip(tr("将当前流统计信息导出为 HTML/JSON/TXT 报告"));
    layout->addWidget(export_button_, 0, Qt::AlignRight);
    
    connect(export_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportReport);
}

void AnalysisPanel::SetupFrameTab() {
    frame_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(frame_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 子切换：视频帧 / 音频帧
    frame_sub_tabs_ = new QTabWidget(frame_tab_);

    // ---- 视频帧子页 ----
    QWidget* video_sub = new QWidget(frame_sub_tabs_);
    QVBoxLayout* v_layout = new QVBoxLayout(video_sub);
    v_layout->setContentsMargins(4, 4, 4, 4);
    v_layout->setSpacing(4);

    QHBoxLayout* v_toolbar = new QHBoxLayout();
    v_toolbar->addWidget(new QLabel(tr("筛选:"), video_sub));
    frame_filter_combo_ = new QComboBox(video_sub);
    frame_filter_combo_->addItems({tr("全部帧"), tr("仅 I 帧")});
    v_toolbar->addWidget(frame_filter_combo_);

    frame_summary_label_ = new QLabel(tr("总帧数: 0 | 显示: 0 | GOP: 0"), video_sub);
    v_toolbar->addWidget(frame_summary_label_, 1);

    export_frame_csv_button_ = new QPushButton(tr("导出 CSV"), video_sub);
    v_toolbar->addWidget(export_frame_csv_button_);

    QCheckBox* v_toggle = new QCheckBox(tr("启用分析"), video_sub);
    v_toggle->setChecked(feature_enabled_.value(AnalysisFeature::VideoFrame, true));
    connect(v_toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::VideoFrame] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::VideoFrame), checked);
    });
    v_toolbar->addWidget(v_toggle);
    v_layout->addLayout(v_toolbar);

    QGroupBox* table_group = new QGroupBox(tr("视频帧信息"), video_sub);
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
    v_layout->addWidget(table_group);

    QGroupBox* gop_group = new QGroupBox(tr("GOP 分段统计"), video_sub);
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
    v_layout->addWidget(gop_group);

    frame_sub_tabs_->addTab(video_sub, tr("视频帧"));

    // ---- 音频帧子页 ----
    audio_frame_sub_ = new QWidget(frame_sub_tabs_);
    QVBoxLayout* a_layout = new QVBoxLayout(audio_frame_sub_);
    a_layout->setContentsMargins(4, 4, 4, 4);
    a_layout->setSpacing(4);

    QHBoxLayout* a_toolbar = new QHBoxLayout();
    audio_frame_summary_label_ = new QLabel(tr("总音频帧数: 0 | 总样本数: 0 | 总字节数: 0"), audio_frame_sub_);
    a_toolbar->addWidget(audio_frame_summary_label_, 1);

    export_audio_frame_csv_button_ = new QPushButton(tr("导出 CSV"), audio_frame_sub_);
    a_toolbar->addWidget(export_audio_frame_csv_button_);

    QCheckBox* a_toggle = new QCheckBox(tr("启用分析"), audio_frame_sub_);
    a_toggle->setChecked(feature_enabled_.value(AnalysisFeature::AudioFrame, true));
    connect(a_toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::AudioFrame] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::AudioFrame), checked);
    });
    a_toolbar->addWidget(a_toggle);
    a_layout->addLayout(a_toolbar);

    QGroupBox* a_table_group = new QGroupBox(tr("音频帧信息"), audio_frame_sub_);
    QVBoxLayout* a_table_layout = new QVBoxLayout(a_table_group);
    audio_frame_table_ = new QTableWidget(0, 7, a_table_group);
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
    a_table_layout->addWidget(audio_frame_table_);
    a_layout->addWidget(a_table_group);

    frame_sub_tabs_->addTab(audio_frame_sub_, tr("音频帧"));

    layout->addWidget(frame_sub_tabs_);

    AddPageWithScroll(frame_tab_, tr("帧分析"));

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        OnFrameFilterChanged();
    });
    connect(export_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportFrameCsv);
    connect(export_audio_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportAudioFrameCsv);
}

void AnalysisPanel::SetupPacketTab() {
    packet_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(packet_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    packet_summary_label_ = new QLabel(tr("总包数: 0 | 视频包: 0 | 音频包: 0 | 其他包: 0"), packet_tab_);
    toolbar_layout->addWidget(packet_summary_label_, 1);

    toolbar_layout->addWidget(new QLabel(tr("筛选:"), packet_tab_));
    packet_filter_combo_ = new QComboBox(packet_tab_);
    packet_filter_combo_->addItems({tr("全部流"), tr("视频流"), tr("音频流"), tr("其他流")});
    packet_filter_combo_->setCurrentIndex(0);
    toolbar_layout->addWidget(packet_filter_combo_);

    export_packet_csv_button_ = new QPushButton(tr("导出 CSV"), packet_tab_);
    toolbar_layout->addWidget(export_packet_csv_button_);

    QCheckBox* toggle = new QCheckBox(tr("启用分析"), packet_tab_);
    toggle->setChecked(feature_enabled_.value(AnalysisFeature::Packet, true));
    connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::Packet] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Packet), checked);
    });
    toolbar_layout->addWidget(toggle);
    layout->addLayout(toolbar_layout);

    QGroupBox* table_group = new QGroupBox(tr("数据包信息"), packet_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);

    packet_table_ = new QTableWidget(0, 10, table_group);
    packet_table_->setHorizontalHeaderLabels({"序号", "流索引", "流类型", "时间戳(s)", "PTS", "DTS", "时长", "大小", "标记", "文件偏移"});
    packet_table_->verticalHeader()->setVisible(false);
    packet_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    packet_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    packet_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    packet_table_->setSortingEnabled(false);
    packet_table_->horizontalHeader()->setStretchLastSection(true);
    packet_table_->horizontalHeader()->setMinimumSectionSize(50);
    packet_table_->setColumnWidth(0, 60);
    packet_table_->setColumnWidth(1, 70);
    packet_table_->setColumnWidth(2, 80);
    packet_table_->setColumnWidth(3, 100);
    packet_table_->setColumnWidth(4, 100);
    packet_table_->setColumnWidth(5, 100);
    packet_table_->setColumnWidth(6, 100);
    packet_table_->setColumnWidth(7, 90);
    packet_table_->setColumnWidth(8, 110);
    packet_table_->setColumnWidth(9, 100);
    packet_table_->setMinimumWidth(650);
    packet_table_->setMinimumHeight(120);

    table_layout->addWidget(packet_table_);
    layout->addWidget(table_group);

    AddPageWithScroll(packet_tab_, tr("包分析"));

    connect(packet_filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        packet_filter_mode_ = idx - 1;  // 0->全部(-1), 1->视频(0), 2->音频(1), 3->其他(2)
        RebuildPacketTable();
    });
    connect(export_packet_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportPacketCsv);
}

void AnalysisPanel::SetupEventTab() {
    event_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(event_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    event_summary_label_ = new QLabel(tr("总事件数: 0 | 错误: 0 | 警告: 0 | 信息: 0"), event_tab_);
    toolbar_layout->addWidget(event_summary_label_, 1);

    export_event_csv_button_ = new QPushButton(tr("导出 CSV"), event_tab_);
    toolbar_layout->addWidget(export_event_csv_button_);

    QCheckBox* toggle = new QCheckBox(tr("启用分析"), event_tab_);
    toggle->setChecked(feature_enabled_.value(AnalysisFeature::Event, true));
    connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::Event] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Event), checked);
    });
    toolbar_layout->addWidget(toggle);
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

    connect(export_event_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportEventCsv);
}

void AnalysisPanel::SetupSyncTab() {
    sync_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(sync_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    sync_summary_label_ = new QLabel(tr("样本数: 0 | 平均偏移: 0.00 ms | 最大偏移: 0.00 ms"), sync_tab_);
    toolbar_layout->addWidget(sync_summary_label_, 1);

    export_sync_csv_button_ = new QPushButton(tr("导出 CSV"), sync_tab_);
    toolbar_layout->addWidget(export_sync_csv_button_);

    QCheckBox* toggle = new QCheckBox(tr("启用分析"), sync_tab_);
    toggle->setChecked(feature_enabled_.value(AnalysisFeature::SyncSample, true));
    connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::SyncSample] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::SyncSample), checked);
    });
    toolbar_layout->addWidget(toggle);
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
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    timeline_summary_label_ = new QLabel(tr("事件数: 0 | 视频关键帧: 0 | 音频采样: 0 | 异常事件: 0"), timeline_tab_);
    toolbar_layout->addWidget(timeline_summary_label_, 1);

    export_timeline_csv_button_ = new QPushButton(tr("导出 CSV"), timeline_tab_);
    toolbar_layout->addWidget(export_timeline_csv_button_);

    QCheckBox* toggle = new QCheckBox(tr("启用分析"), timeline_tab_);
    toggle->setChecked(feature_enabled_.value(AnalysisFeature::Timeline, true));
    connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::Timeline] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Timeline), checked);
    });
    toolbar_layout->addWidget(toggle);
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

void AnalysisPanel::SetupEventAnalysisTab() {
    event_analysis_tab_ = new QWidget();
    QVBoxLayout* root_layout = new QVBoxLayout(event_analysis_tab_);
    root_layout->setContentsMargins(0, 0, 0, 0);

    event_analysis_sub_tabs_ = new QTabWidget(event_analysis_tab_);
    root_layout->addWidget(event_analysis_sub_tabs_);

    SetupEventTab();
    SetupSyncTab();
    SetupTimelineTab();

    event_analysis_sub_tabs_->addTab(event_tab_, tr("异常事件"));
    event_analysis_sub_tabs_->addTab(timeline_tab_, tr("时间轴"));
    event_analysis_sub_tabs_->addTab(sync_tab_, tr("同步分析"));

    AddPageWithScroll(event_analysis_tab_, tr("事件与时间轴"));
}

void AnalysisPanel::SetupAudioLoudnessTab() {
    audio_loudness_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(audio_loudness_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    loudness_summary_label_ = new QLabel(
        tr("音频响度 | LUFS: -70.0 | Peak dBFS: -70.0 | True Peak: -70.0 dBTP"),
        audio_loudness_tab_);
    toolbar_layout->addWidget(loudness_summary_label_, 1);

    QCheckBox* toggle = new QCheckBox(tr("启用监测"), audio_loudness_tab_);
    toggle->setChecked(feature_enabled_.value(AnalysisFeature::AudioLoudness, true));
    connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::AudioLoudness] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::AudioLoudness), checked);
    });
    toolbar_layout->addWidget(toggle);
    layout->addLayout(toolbar_layout);

    // 响度历史图
    loudness_series_ = new QLineSeries(this);
    loudness_series_->setName(tr("Momentary LUFS"));
    peak_series_ = new QLineSeries(this);
    peak_series_->setName(tr("Peak dBFS"));

    loudness_chart_object_ = new QChart();
    loudness_chart_object_->setTitle(tr("响度历史"));
    loudness_chart_object_->addSeries(loudness_series_);
    loudness_chart_object_->addSeries(peak_series_);
    loudness_chart_object_->legend()->setVisible(true);
    loudness_chart_object_->legend()->setAlignment(Qt::AlignBottom);

    loudness_axis_x_ = new QValueAxis(this);
    loudness_axis_x_->setLabelFormat("%d");
    loudness_axis_x_->setTitleText(tr("帧"));
    loudness_axis_y_ = new QValueAxis(this);
    loudness_axis_y_->setLabelFormat("%.0f");
    loudness_axis_y_->setTitleText(tr("dB"));
    loudness_axis_y_->setRange(-70.0, 6.0);

    loudness_chart_object_->addAxis(loudness_axis_x_, Qt::AlignBottom);
    loudness_chart_object_->addAxis(loudness_axis_y_, Qt::AlignLeft);
    loudness_series_->attachAxis(loudness_axis_x_);
    loudness_series_->attachAxis(loudness_axis_y_);
    peak_series_->attachAxis(loudness_axis_x_);
    peak_series_->attachAxis(loudness_axis_y_);

    loudness_chart_ = new QChartView(audio_loudness_tab_);
    loudness_chart_->setChart(loudness_chart_object_);
    loudness_chart_->setRenderHint(QPainter::Antialiasing);
    loudness_chart_->setMinimumHeight(220);
    loudness_chart_->setMinimumWidth(280);
    layout->addWidget(loudness_chart_);

    AddPageWithScroll(audio_loudness_tab_, tr("音频响度"));
}

void AnalysisPanel::UpdateAudioLoudness(const model::AudioVisualizationFrame& frame) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioLoudness, true)) return;
    if (!loudness_summary_label_ || !loudness_series_ || !peak_series_) return;

    // 新文件时重置累计状态
    if (frame.index == 0) {
        loudness_history_.clear();
        peak_history_.clear();
        integrated_lufs_ = -70.0;
        loudness_range_lu_ = 0.0;
        max_true_peak_dbtp_ = -70.0;
        max_peak_dbfs_ = -70.0;
        loudness_sample_count_ = 0;
        loudness_sum_ = 0.0;
    }

    const double lufs = frame.loudness_momentary_lufs;
    const double peak = frame.peak_dbfs;
    const double tp = frame.true_peak_dbtp;

    // 累计统计
    ++loudness_sample_count_;
    if (lufs > -70.0) {
        // 线性域累加用于 integrated LUFS
        loudness_sum_ += std::pow(10.0, (lufs + 0.691) / 10.0);
    }
    if (peak > max_peak_dbfs_) max_peak_dbfs_ = peak;
    if (tp > max_true_peak_dbtp_) max_true_peak_dbtp_ = tp;

    // Integrated LUFS
    if (loudness_sample_count_ > 0 && loudness_sum_ > 1e-10) {
        double mean_sq = loudness_sum_ / static_cast<double>(loudness_sample_count_);
        integrated_lufs_ = -0.691 + 10.0 * std::log10(mean_sq);
    }

    // LRA (简化：最高和最低响度之差)
    if (!loudness_history_.empty()) {
        double max_lufs = *std::max_element(loudness_history_.begin(), loudness_history_.end());
        double min_lufs = *std::min_element(loudness_history_.begin(), loudness_history_.end());
        loudness_range_lu_ = max_lufs - min_lufs;
    }

    // 历史数据 (保留最近 200 个样本)
    constexpr size_t kMaxHistory = 200;
    loudness_history_.push_back(lufs);
    peak_history_.push_back(peak);
    while (loudness_history_.size() > kMaxHistory) loudness_history_.pop_front();
    while (peak_history_.size() > kMaxHistory) peak_history_.pop_front();

    // 更新图表
    loudness_series_->clear();
    peak_series_->clear();
    for (size_t i = 0; i < loudness_history_.size(); ++i) {
        loudness_series_->append(static_cast<qreal>(i), loudness_history_[i]);
        peak_series_->append(static_cast<qreal>(i), peak_history_[i]);
    }
    loudness_axis_x_->setRange(0, static_cast<qreal>(loudness_history_.size()));

    // 摘要文本
    loudness_summary_label_->setText(
        tr("Momentary: %1 LUFS | Peak: %2 dBFS | True Peak: %3 dBTP | "
           "Integrated: %4 LUFS | LRA: %5 LU")
            .arg(lufs, 0, 'f', 1)
            .arg(peak, 0, 'f', 1)
            .arg(tp, 0, 'f', 1)
            .arg(integrated_lufs_, 0, 'f', 1)
            .arg(loudness_range_lu_, 0, 'f', 1));
}

void AnalysisPanel::SetupHistogramTab() {
    histogram_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(histogram_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);
    
    QGroupBox* hist_group = new QGroupBox(tr("直方图分析"), histogram_tab_);
    QVBoxLayout* hist_layout = new QVBoxLayout(hist_group);
    
    histogram_chart_ = new QChartView();
    histogram_chart_->setMinimumHeight(200);
    histogram_chart_->setMinimumWidth(400);
    hist_layout->addWidget(histogram_chart_);
    
    // 导出按钮 + 启用分析 同一行 (放在图表上方，确保始终可见)
    {
        QHBoxLayout* toolbar_layout = new QHBoxLayout();
        toolbar_layout->addStretch();
        export_histogram_button_ = new QPushButton(tr("导出直方图数据"), histogram_tab_);
        toolbar_layout->addWidget(export_histogram_button_);

        QCheckBox* toggle = new QCheckBox(tr("启用分析"), histogram_tab_);
        toggle->setChecked(feature_enabled_.value(AnalysisFeature::Histogram, true));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_enabled_[AnalysisFeature::Histogram] = checked;
            emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Histogram), checked);
        });
        toolbar_layout->addWidget(toggle);
        layout->addLayout(toolbar_layout);
    }
    
    layout->addWidget(hist_group);
    
    AddPageWithScroll(histogram_tab_, tr("直方图"));
    
    connect(export_histogram_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportHistogramCsv);
}

void AnalysisPanel::SetupContainerStructureTab() {
    container_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(container_tab_);
    layout->setContentsMargins(4, 0, 4, 4);
    layout->setSpacing(2);

    // 动态标题 + 启用分析开关 (同一行)
    {
        QWidget* titleRow = new QWidget(container_tab_);
        QHBoxLayout* trl = new QHBoxLayout(titleRow);
        trl->setContentsMargins(0, 0, 0, 0);
        container_title_label_ = new QLabel(tr("未加载文件"), titleRow);
        container_title_label_->setStyleSheet("font-size: 13px; font-weight: bold; color: #F0F6FC; padding: 2px 4px;");
        container_title_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        trl->addWidget(container_title_label_);
        trl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用分析"), titleRow);
        toggle->setChecked(feature_enabled_.value(AnalysisFeature::ContainerStructure, true));
        toggle->setToolTip(tr("启用或禁用容器结构分析，关闭可跳过打开文件时的结构解析"));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_enabled_[AnalysisFeature::ContainerStructure] = checked;
            emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::ContainerStructure), checked);
        });
        trl->addWidget(toggle);
        layout->addWidget(titleRow);
    }

    // 概要标签
    container_summary_label_ = new QLabel(tr("打开媒体文件后将自动分析容器结构"), container_tab_);
    container_summary_label_->setStyleSheet("font-size: 12px; color: #8B949E; padding: 2px 4px;");
    container_summary_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(container_summary_label_);

    // 分割布局: 左侧结构树 + 右侧详情
    QSplitter* splitter = new QSplitter(Qt::Horizontal, container_tab_);

    // --- 左侧: 通用结构树 ---
    QWidget* leftPanel = new QWidget(splitter);
    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    QLabel* treeLabel = new QLabel(tr("容器结构树:"), leftPanel);
    treeLabel->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    leftLayout->addWidget(treeLabel);
    container_tree_ = new QTreeWidget(leftPanel);
    container_tree_->setHeaderLabels({tr("名称"), tr("类型"), tr("大小"), tr("偏移"), tr("值/属性")});
    container_tree_->setColumnWidth(0, 160);
    container_tree_->setColumnWidth(1, 70);
    container_tree_->setColumnWidth(2, 80);
    container_tree_->setColumnWidth(3, 70);
    container_tree_->header()->setStretchLastSection(true);
    container_tree_->setAlternatingRowColors(true);
    container_tree_->setAnimated(true);
    container_tree_->setIndentation(16);
    container_tree_->setStyleSheet(
        "QTreeWidget { background-color: #0D1117; border: 1px solid #30363D; border-radius: 4px; color: #F0F6FC; font-size: 11px; }"
        "QTreeWidget::item { padding: 2px 4px; color: #F0F6FC; }"
        "QTreeWidget::item:hover { background-color: #161B22; }"
        "QTreeWidget::item:selected { background-color: rgba(88, 166, 255, 0.15); color: #F0F6FC; }"
        "QHeaderView::section { background-color: #161B22; color: #8B949E; padding: 3px; border: none; border-bottom: 1px solid #30363D; font-weight: bold; }"
    );
    leftLayout->addWidget(container_tree_);
    splitter->addWidget(leftPanel);

    // --- 右侧: 详情区 (QStackedWidget) ---
    QWidget* rightPanel = new QWidget(splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    container_detail_stack_ = new QStackedWidget(rightPanel);

    // ===== Page 0: 通用信息 (流信息 + 元数据) =====
    QWidget* generic_page = new QWidget();
    QVBoxLayout* generic_layout = new QVBoxLayout(generic_page);
    generic_layout->setContentsMargins(0, 0, 0, 0);

    QLabel* stream_label = new QLabel(tr("流信息:"), generic_page);
    stream_label->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    generic_layout->addWidget(stream_label);
    container_stream_table_ = new QTableWidget(0, 4, generic_page);
    container_stream_table_->setHorizontalHeaderLabels({tr("#"), tr("类型"), tr("编码"), tr("详情")});
    container_stream_table_->horizontalHeader()->setStretchLastSection(true);
    container_stream_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    container_stream_table_->setAlternatingRowColors(true);
    generic_layout->addWidget(container_stream_table_);

    QLabel* meta_label = new QLabel(tr("元数据:"), generic_page);
    meta_label->setStyleSheet("font-weight: bold; font-size: 11px; padding: 2px;");
    generic_layout->addWidget(meta_label);
    container_metadata_table_ = new QTableWidget(0, 2, generic_page);
    container_metadata_table_->setHorizontalHeaderLabels({tr("键"), tr("值")});
    container_metadata_table_->horizontalHeader()->setStretchLastSection(true);
    container_metadata_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    container_metadata_table_->setAlternatingRowColors(true);
    generic_layout->addWidget(container_metadata_table_);

    container_detail_stack_->addWidget(generic_page);

    // ===== Page 1: MP4 专用详情表 =====
    mp4_detail_tabs_ = new QTabWidget();

    QWidget* stts_w = new QWidget();
    QVBoxLayout* stts_l = new QVBoxLayout(stts_w);
    stts_table_ = new QTableWidget(0, 3, stts_w);
    stts_table_->setHorizontalHeaderLabels({tr("索引"), tr("样本计数"), tr("样本增量")});
    stts_table_->horizontalHeader()->setStretchLastSection(true);
    stts_table_->setAlternatingRowColors(true);
    stts_l->addWidget(stts_table_);
    mp4_detail_tabs_->addTab(stts_w, "stts");

    QWidget* stco_w = new QWidget();
    QVBoxLayout* stco_l = new QVBoxLayout(stco_w);
    stco_table_ = new QTableWidget(0, 2, stco_w);
    stco_table_->setHorizontalHeaderLabels({tr("索引"), tr("Chunk 偏移")});
    stco_table_->horizontalHeader()->setStretchLastSection(true);
    stco_table_->setAlternatingRowColors(true);
    stco_l->addWidget(stco_table_);
    mp4_detail_tabs_->addTab(stco_w, "stco");

    QWidget* stsc_w = new QWidget();
    QVBoxLayout* stsc_l = new QVBoxLayout(stsc_w);
    stsc_table_ = new QTableWidget(0, 4, stsc_w);
    stsc_table_->setHorizontalHeaderLabels({tr("索引"), tr("首个Chunk"), tr("样本/Chunk"), tr("描述索引")});
    stsc_table_->horizontalHeader()->setStretchLastSection(true);
    stsc_table_->setAlternatingRowColors(true);
    stsc_l->addWidget(stsc_table_);
    mp4_detail_tabs_->addTab(stsc_w, "stsc");

    QWidget* stsz_w = new QWidget();
    QVBoxLayout* stsz_l = new QVBoxLayout(stsz_w);
    stsz_table_ = new QTableWidget(0, 3, stsz_w);
    stsz_table_->setHorizontalHeaderLabels({tr("索引"), tr("样本大小"), tr("备注")});
    stsz_table_->horizontalHeader()->setStretchLastSection(true);
    stsz_table_->setAlternatingRowColors(true);
    stsz_l->addWidget(stsz_table_);
    mp4_detail_tabs_->addTab(stsz_w, "stsz");

    QWidget* stss_w = new QWidget();
    QVBoxLayout* stss_l = new QVBoxLayout(stss_w);
    stss_table_ = new QTableWidget(0, 2, stss_w);
    stss_table_->setHorizontalHeaderLabels({tr("索引"), tr("关键帧样本号")});
    stss_table_->horizontalHeader()->setStretchLastSection(true);
    stss_table_->setAlternatingRowColors(true);
    stss_l->addWidget(stss_table_);
    mp4_detail_tabs_->addTab(stss_w, "stss");

    QWidget* co64_w = new QWidget();
    QVBoxLayout* co64_l = new QVBoxLayout(co64_w);
    co64_table_ = new QTableWidget(0, 2, co64_w);
    co64_table_->setHorizontalHeaderLabels({tr("索引"), tr("Chunk偏移(64位)")});
    co64_table_->horizontalHeader()->setStretchLastSection(true);
    co64_table_->setAlternatingRowColors(true);
    co64_l->addWidget(co64_table_);
    mp4_detail_tabs_->addTab(co64_w, "co64");

    container_detail_stack_->addWidget(mp4_detail_tabs_);

    // ===== Page 2: EBML 专用详情表 =====
    ebml_detail_tabs_ = new QTabWidget();

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
    ebml_detail_tabs_->addTab(ebml_track_table_, tr("轨道"));

    ebml_cue_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_cue_table_->setColumnCount(4);
    ebml_cue_table_->setHorizontalHeaderLabels({tr("#"), tr("时间"), tr("轨道"), tr("Cluster偏移")});
    ebml_cue_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_cue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_cue_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_cue_table_, tr("Cues"));

    ebml_block_table_ = new QTableWidget(ebml_detail_tabs_);
    ebml_block_table_->setColumnCount(5);
    ebml_block_table_->setHorizontalHeaderLabels({tr("#"), tr("轨道"), tr("Timecode"), tr("关键帧"), tr("大小")});
    ebml_block_table_->horizontalHeader()->setStretchLastSection(true);
    ebml_block_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ebml_block_table_->setAlternatingRowColors(true);
    ebml_detail_tabs_->addTab(ebml_block_table_, tr("Block"));

    container_detail_stack_->addWidget(ebml_detail_tabs_);

    rightLayout->addWidget(container_detail_stack_);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setMinimumWidth(540);
    splitter->setChildrenCollapsible(false);

    layout->addWidget(splitter, 1);

    // 导出按钮
    export_container_button_ = new QPushButton(tr("导出结构数据"), container_tab_);
    export_container_button_->setToolTip(tr("将容器结构树和表格数据导出为文本文件"));
    layout->addWidget(export_container_button_, 0, Qt::AlignRight);

    AddPageWithScroll(container_tab_, tr("文件结构"));

    connect(export_container_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportContainerStructure);
}

static void PopulateMp4BoxTablesInContainer(const model::Mp4BoxAnalysisResult& result,
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
        auto addTrackHeader = [](QTableWidget* table, const QString& text) {
            const int row = table->rowCount();
            table->insertRow(row);
            auto* item = new QTableWidgetItem(text);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            table->setItem(row, 0, item);
        };

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
        if (!track.stco_entries.isEmpty()) {
            addTrackHeader(stco_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.stco_entries.size(); ++i) {
                const int row = stco_table->rowCount();
                stco_table->insertRow(row);
                stco_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                stco_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stco_entries[i].chunk_offset)));
            }
        }
        if (!track.co64_entries.isEmpty()) {
            addTrackHeader(co64_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            for (int i = 0; i < track.co64_entries.size(); ++i) {
                const int row = co64_table->rowCount();
                co64_table->insertRow(row);
                co64_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                co64_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.co64_entries[i].chunk_offset)));
            }
        }
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
        if (!track.stsz_entries.isEmpty() || track.stsz_default_size > 0) {
            addTrackHeader(stsz_table, QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type));
            if (track.stsz_default_size > 0 && track.stsz_entries.isEmpty()) {
                const int row = stsz_table->rowCount();
                stsz_table->insertRow(row);
                stsz_table->setItem(row, 0, new QTableWidgetItem("0"));
                stsz_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stsz_default_size)));
                stsz_table->setItem(row, 2, new QTableWidgetItem(QString("default x%1").arg(track.stsz_sample_count)));
            } else {
                for (int i = 0; i < track.stsz_entries.size(); ++i) {
                    const int row = stsz_table->rowCount();
                    stsz_table->insertRow(row);
                    stsz_table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                    stsz_table->setItem(row, 1, new QTableWidgetItem(QString::number(track.stsz_entries[i].sample_size)));
                }
            }
        }
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
    for (auto* table : {stts_table, stco_table, stsc_table, stsz_table, co64_table, stss_table}) {
        table->resizeColumnsToContents();
    }
}

void AnalysisPanel::OnContainerStructureReady(const model::ContainerStructureResult& result) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    current_container_result_ = result;

    // 更新动态标题
    container_title_label_->setText(result.valid ? result.format_name + " " + tr("结构分析") : tr("文件结构"));
    container_title_label_->setStyleSheet(
        result.valid ? "font-size: 13px; font-weight: bold; color: #1a73e8; padding: 2px 4px;"
                     : "font-size: 13px; font-weight: bold; color: #8B949E; padding: 2px 4px;");

    if (!result.valid) {
        container_summary_label_->setText(result.error_message.isEmpty() ? tr("无法分析该格式") : result.error_message);
        container_summary_label_->setStyleSheet("font-size: 12px; color: #8B949E; padding: 2px 4px;");
        container_tree_->clear();
        container_detail_stack_->setCurrentIndex(0);
        return;
    }

    container_summary_label_->setText(result.summary);
    container_summary_label_->setStyleSheet("font-size: 12px; color: #F0F6FC; font-weight: bold; padding: 2px 4px;");

    // 填充通用结构树
    container_tree_->clear();
    static const QColor kDepthColors[] = {
        QColor("#161B22"),  // level 0 — card background
        QColor("#0D1117"),  // level 1 — base background
        QColor("#131A21"),  // level 2
        QColor("#111920"),  // level 3
        QColor("#141C24"),  // level 4
        QColor("#10171D"),  // level 5
        QColor("#151D25"),  // level 6
    };

    std::function<void(QTreeWidgetItem*, const QVector<model::ContainerElement>&)> addNodes;
    addNodes = [&](QTreeWidgetItem* parent, const QVector<model::ContainerElement>& nodes) {
        for (const auto& n : nodes) {
            auto* item = new QTreeWidgetItem();
            item->setText(0, n.name);
            item->setText(1, n.type);
            item->setText(2, QString::number(n.size));
            item->setText(3, QString("0x%1").arg(n.offset, 0, 16));
            item->setText(4, n.value);
            if (!n.extra.isEmpty()) {
                item->setToolTip(0, n.extra);
                item->setToolTip(4, n.extra);
            }
            int d = n.depth;
            QColor bg = kDepthColors[d % 7];
            for (int c = 0; c < 5; ++c) item->setBackground(c, bg);
            if (parent) parent->addChild(item);
            else container_tree_->addTopLevelItem(item);
            addNodes(item, n.children);
        }
    };
    for (const auto& n : result.element_tree) {
        auto* top = new QTreeWidgetItem();
        top->setText(0, n.name);
        top->setText(1, n.type);
        top->setText(2, QString::number(n.size));
        top->setText(3, QString("0x%1").arg(n.offset, 0, 16));
        top->setText(4, n.value);
        if (!n.extra.isEmpty()) {
            top->setToolTip(0, n.extra);
            top->setToolTip(4, n.extra);
        }
        container_tree_->addTopLevelItem(top);
        addNodes(top, n.children);
    }
    container_tree_->expandAll();

    // 填充通用信息表 (Page 0)
    // 流信息
    container_stream_table_->setRowCount(result.streams.size());
    for (int i = 0; i < result.streams.size(); ++i) {
        const auto& s = result.streams[i];
        container_stream_table_->setItem(i, 0, new QTableWidgetItem(QString::number(s.index)));
        container_stream_table_->setItem(i, 1, new QTableWidgetItem(s.type));
        container_stream_table_->setItem(i, 2, new QTableWidgetItem(s.codec));
        container_stream_table_->setItem(i, 3, new QTableWidgetItem(s.details));
    }
    // 元数据
    container_metadata_table_->setRowCount(result.metadata.size());
    int row = 0;
    for (auto it = result.metadata.begin(); it != result.metadata.end(); ++it, ++row) {
        container_metadata_table_->setItem(row, 0, new QTableWidgetItem(it.key()));
        container_metadata_table_->setItem(row, 1, new QTableWidgetItem(it.value()));
    }

    // 根据格式切换详情页面
    using CF = model::ContainerFormat;
    switch (result.format) {
    case CF::MP4:
    case CF::MOV:
        container_detail_stack_->setCurrentIndex(1);  // MP4 详情页
        // 填充 MP4 详细表
        if (result.mp4_detail.valid) {
            PopulateMp4BoxTablesInContainer(result.mp4_detail, stts_table_, stco_table_,
                                            stsc_table_, stsz_table_, co64_table_, stss_table_);
        }
        break;
    case CF::MKV:
    case CF::WebM:
        container_detail_stack_->setCurrentIndex(2);  // EBML 详情页
        // 填充 EBML 详细表
        if (result.ebml_detail.valid) {
            const auto& er = result.ebml_detail;
            // 轨道表
            ebml_track_table_->setRowCount(er.tracks.size());
            for (int i = 0; i < er.tracks.size(); ++i) {
                const auto& t = er.tracks[i];
                auto set = [&](int col, const QString& v) {
                    ebml_track_table_->setItem(i, col, new QTableWidgetItem(v));
                };
                set(0, QString::number(t.track_number));
                set(1, t.track_type_name);
                set(2, t.codec_name);
                set(3, t.codec_id);
                if (t.track_type == 1) {
                    set(4, QString("%1x%2").arg(t.pixel_width).arg(t.pixel_height));
                    set(5, "-"); set(7, t.frame_rate > 0 ? QString("%1 fps").arg(t.frame_rate, 0, 'f', 2) : "-");
                } else if (t.track_type == 2) {
                    set(4, t.sampling_frequency > 0 ? QString("%1 Hz").arg(t.sampling_frequency, 0, 'f', 0) : "-");
                    set(5, t.channels > 0 ? QString::number(t.channels) : "-"); set(7, "-");
                } else { set(4, "-"); set(5, "-"); set(7, "-"); }
                set(6, t.language);
                set(8, t.default_track ? QString::fromUtf8("\u2713") : "");
                set(9, t.forced ? QString::fromUtf8("\u2713") : "");
            }
            ebml_detail_tabs_->setTabText(0, tr("轨道 (%1)").arg(er.tracks.size()));
            // Cues 表
            ebml_cue_table_->setRowCount(er.cues.size());
            for (int i = 0; i < er.cues.size(); ++i) {
                const auto& c = er.cues[i];
                ebml_cue_table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
                ebml_cue_table_->setItem(i, 1, new QTableWidgetItem(QString::number(c.time)));
                ebml_cue_table_->setItem(i, 2, new QTableWidgetItem(QString::number(c.track_number)));
                ebml_cue_table_->setItem(i, 3, new QTableWidgetItem(QString("0x%1").arg(c.cluster_position, 0, 16)));
            }
            ebml_detail_tabs_->setTabText(1, tr("Cues (%1)").arg(er.cues.size()));
            // Block 表
            int bn = qMin(er.blocks.size(), 500);
            ebml_block_table_->setRowCount(bn);
            for (int i = 0; i < bn; ++i) {
                const auto& b = er.blocks[i];
                ebml_block_table_->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
                ebml_block_table_->setItem(i, 1, new QTableWidgetItem(QString::number(b.track_number)));
                ebml_block_table_->setItem(i, 2, new QTableWidgetItem(QString::number(b.timecode)));
                ebml_block_table_->setItem(i, 3, new QTableWidgetItem(b.keyframe ? "KEY" : ""));
                ebml_block_table_->setItem(i, 4, new QTableWidgetItem(QString::number(b.data_size)));
            }
            ebml_detail_tabs_->setTabText(2, tr("Block (%1/%2)").arg(bn).arg(er.blocks.size()));
        }
        break;
    default:
        container_detail_stack_->setCurrentIndex(0);  // 通用信息页
        break;
    }
}

void AnalysisPanel::OnExportContainerStructure() {
    if (!current_container_result_.valid) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的结构数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出文件结构"),
        QString("videoeye_structure_%1.txt")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("文本文件 (*.txt);;所有文件 (*)"));
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "Format: " << current_container_result_.format_name << "\n";
    out << "File: " << current_container_result_.file_path << "\n";
    out << "Summary: " << current_container_result_.summary << "\n\n";

    std::function<void(const QVector<model::ContainerElement>&, int)> printTree;
    printTree = [&](const QVector<model::ContainerElement>& nodes, int d) {
        for (const auto& n : nodes) {
            QString indent(d * 2, ' ');
            out << indent << n.name << " [" << n.type << "]  size=" << n.size
                << "  offset=0x" << Qt::hex << n.offset << Qt::dec;
            if (!n.value.isEmpty()) out << "  value=" << n.value;
            out << "\n";
            printTree(n.children, d + 1);
        }
    };
    printTree(current_container_result_.element_tree, 0);

    if (!current_container_result_.metadata.isEmpty()) {
        out << "\n--- Metadata ---\n";
        for (auto it = current_container_result_.metadata.begin();
             it != current_container_result_.metadata.end(); ++it) {
            out << it.key() << " = " << it.value() << "\n";
        }
    }

    // 流信息表
    if (!current_container_result_.streams.isEmpty()) {
        out << "\n--- Streams ---\n";
        for (const auto& s : current_container_result_.streams) {
            out << "  #" << s.index << "  " << s.type << "  " << s.codec;
            if (!s.details.isEmpty()) out << "  (" << s.details << ")";
            out << "\n";
        }
    }

    // MP4/MOV 详细 Box 表
    const auto fmt = current_container_result_.format;
    if (fmt == model::ContainerFormat::MP4 || fmt == model::ContainerFormat::MOV) {
        const auto& mp4 = current_container_result_.mp4_detail;
        if (mp4.valid && !mp4.track_tables.isEmpty()) {
            out << "\n========== MP4 Sample Tables ==========\n";
            for (const auto& t : mp4.track_tables) {
                out << "\n--- Track " << t.track_id << " (" << t.track_type << ") ---\n";

                if (!t.stts_entries.isEmpty()) {
                    out << "  [stts] Time-to-Sample (" << t.stts_entries.size() << " entries)\n";
                    out << "    idx\tsample_count\tsample_delta\n";
                    int i = 0;
                    for (const auto& e : t.stts_entries)
                        out << "    " << i++ << "\t" << e.sample_count << "\t\t" << e.sample_delta << "\n";
                }
                if (!t.stsc_entries.isEmpty()) {
                    out << "  [stsc] Sample-to-Chunk (" << t.stsc_entries.size() << " entries)\n";
                    out << "    idx\tfirst_chunk\tsamples_per_chunk\tsample_desc_idx\n";
                    int i = 0;
                    for (const auto& e : t.stsc_entries)
                        out << "    " << i++ << "\t" << e.first_chunk << "\t\t" << e.samples_per_chunk
                            << "\t\t\t" << e.sample_description_index << "\n";
                }
                if (!t.stco_entries.isEmpty()) {
                    out << "  [stco] Chunk Offset (" << t.stco_entries.size() << " entries)\n";
                    out << "    idx\tchunk_offset\n";
                    int i = 0;
                    for (const auto& e : t.stco_entries)
                        out << "    " << i++ << "\t0x" << Qt::hex << e.chunk_offset << Qt::dec << "\n";
                }
                if (!t.co64_entries.isEmpty()) {
                    out << "  [co64] 64-bit Chunk Offset (" << t.co64_entries.size() << " entries)\n";
                    out << "    idx\tchunk_offset\n";
                    int i = 0;
                    for (const auto& e : t.co64_entries)
                        out << "    " << i++ << "\t0x" << Qt::hex << e.chunk_offset << Qt::dec << "\n";
                }
                if (!t.stsz_entries.isEmpty() || t.stsz_default_size > 0) {
                    out << "  [stsz] Sample Size (count=" << t.stsz_sample_count
                        << ", default=" << t.stsz_default_size << ")\n";
                    if (!t.stsz_entries.isEmpty()) {
                        out << "    idx\tsample_size\n";
                        int i = 0;
                        for (const auto& e : t.stsz_entries)
                            out << "    " << i++ << "\t" << e.sample_size << "\n";
                    }
                }
                if (!t.stss_entries.isEmpty()) {
                    out << "  [stss] Sync Sample / Keyframes (" << t.stss_entries.size() << " entries)\n";
                    out << "    idx\tsample_number\n";
                    int i = 0;
                    for (const auto& e : t.stss_entries)
                        out << "    " << i++ << "\t" << e.sample_number << "\n";
                }
            }
        }
    }

    // MKV/WebM 详细表 (轨道 / Cues / Blocks)
    if (fmt == model::ContainerFormat::MKV || fmt == model::ContainerFormat::WebM) {
        const auto& ebml = current_container_result_.ebml_detail;
        if (ebml.valid) {
            out << "\n========== EBML/Matroska Detail ==========\n";
            out << "DocType: " << ebml.doc_type << " v" << ebml.doc_type_version << "\n";
            out << "TimestampScale: " << ebml.timestamp_scale << " ns\n";
            out << "Duration: " << ebml.duration_seconds << " s\n";
            out << "Clusters: " << ebml.total_clusters
                << "  BlockGroups: " << ebml.total_blockgroups
                << "  SimpleBlocks: " << ebml.total_simpleblocks << "\n";

            if (!ebml.tracks.isEmpty()) {
                out << "\n--- Tracks (" << ebml.tracks.size() << ") ---\n";
                for (const auto& tr : ebml.tracks) {
                    out << "  Track #" << tr.track_number << "  " << tr.track_type_name
                        << "  codec=" << tr.codec_id;
                    if (!tr.codec_name.isEmpty()) out << " (" << tr.codec_name << ")";
                    if (!tr.language.isEmpty()) out << "  lang=" << tr.language;
                    if (tr.pixel_width > 0)
                        out << "  " << tr.pixel_width << "x" << tr.pixel_height;
                    if (tr.sampling_frequency > 0)
                        out << "  " << tr.sampling_frequency << "Hz " << tr.channels << "ch";
                    out << "\n";
                }
            }
            if (!ebml.cues.isEmpty()) {
                out << "\n--- Cues (" << ebml.cues.size() << " entries) ---\n";
                out << "    time\ttrack\tcluster_pos\tblock_no\n";
                for (const auto& c : ebml.cues)
                    out << "    " << c.time << "\t" << c.track_number
                        << "\t0x" << Qt::hex << c.cluster_position << Qt::dec
                        << "\t" << c.block_number << "\n";
            }
            if (!ebml.blocks.isEmpty()) {
                out << "\n--- Blocks (" << ebml.blocks.size() << " entries, may be truncated) ---\n";
                out << "    track\ttimecode\tkeyframe\tsize\tcluster_off\tblock_off\n";
                for (const auto& b : ebml.blocks)
                    out << "    " << b.track_number << "\t" << b.timecode
                        << "\t\t" << (b.keyframe ? "K" : "-")
                        << "\t" << b.data_size
                        << "\t0x" << Qt::hex << b.cluster_offset
                        << "\t0x" << b.block_offset << Qt::dec << "\n";
            }
        }
    }

    QMessageBox::information(this, tr("导出成功"), tr("已导出到:\n%1").arg(filename));
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
    // 延迟到主线程 FlushPendingUiUpdates 中更新图表,避免在解码线程操作 GUI
    pending_histogram_ = hist;
    has_pending_histogram_ = true;
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

QString AnalysisPanel::PacketStreamTypeToName(int type) const {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return tr("视频");
        case AVMEDIA_TYPE_AUDIO: return tr("音频");
        case AVMEDIA_TYPE_DATA: return tr("数据");
        case AVMEDIA_TYPE_SUBTITLE: return tr("字幕");
        case AVMEDIA_TYPE_ATTACHMENT: return tr("附件");
        default: return tr("未知");
    }
}

bool AnalysisPanel::PacketMatchesFilter(const PacketRecord& record) const {
    switch (packet_filter_mode_) {
        case 0: return record.stream_type == AVMEDIA_TYPE_VIDEO;
        case 1: return record.stream_type == AVMEDIA_TYPE_AUDIO;
        case 2: return record.stream_type != AVMEDIA_TYPE_VIDEO &&
                       record.stream_type != AVMEDIA_TYPE_AUDIO;
        default: return true;  // -1 = 全部
    }
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
    if (!current_container_result_.valid || !current_container_result_.mp4_detail.valid ||
        current_container_result_.mp4_detail.box_tree.isEmpty()) {
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
    printTree(current_container_result_.mp4_detail.box_tree, 0);

    // 导出各 Track 的表格数据
    for (const auto& track : current_container_result_.mp4_detail.track_tables) {
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
    if (has_pending_histogram_) {
        UpdateHistogramChart(pending_histogram_);
        has_pending_histogram_ = false;
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
    if (macroblock_dirty_) {
        RefreshMacroblockUi();
        macroblock_dirty_ = false;
    }
    if (scene_change_dirty_) {
        FlushPendingSceneChangeTable();
        scene_change_dirty_ = false;
    }
    if (quality_dirty_) {
        FlushPendingQualityTable();
        quality_dirty_ = false;
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

void AnalysisPanel::RefreshStreamStatsUi(const analyzer::StreamStats& stats) {
    if (!stats_table_) {
        return;
    }

    // 流级指标 (包级统计已移至「数据包」tab，避免重复维护)
    SetTableItemText(stats_table_, 0, 0, QString::number(stats.current_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 0, 1, QString::number(stats.avg_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 0, 2, QString::number(stats.peak_bitrate_bps / 1000) + " Kbps");

    const auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats.start_time);
    SetTableItemText(stats_table_, 0, 3, QString::number(duration.count()) + " s");
    SetTableItemText(stats_table_, 0, 4, QString::number(stats.max_gop_size));

    // 收集历史数据供导出报告使用（不再渲染图表）
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
    if (!PacketMatchesFilter(record)) {
        return;
    }
    const int row = packet_table_->rowCount();
    packet_table_->insertRow(row);
    SetTableItemText(packet_table_, row, 0, QString::number(record.index));
    SetTableItemText(packet_table_, row, 1, QString::number(record.stream_index));
    SetTableItemText(packet_table_, row, 2, PacketStreamTypeToName(record.stream_type));
    SetTableItemText(packet_table_, row, 3, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(packet_table_, row, 4, QString::number(record.pts));
    SetTableItemText(packet_table_, row, 5, QString::number(record.dts));
    SetTableItemText(packet_table_, row, 6, QString::number(record.duration));
    SetTableItemText(packet_table_, row, 7, QString::number(record.size));
    SetTableItemText(packet_table_, row, 8, PacketFlagsToString(record.flags));
    SetTableItemText(packet_table_, row, 9, QString::number(record.pos));
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

// ==================== 宏块分析 ====================

void AnalysisPanel::SetupMacroblockTab() {
    macroblock_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(macroblock_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 工具栏
    QHBoxLayout* toolbar = new QHBoxLayout();
    macroblock_summary_label_ = new QLabel(
        tr("帧: -- | 块总数: 0 | 前向: 0 | 后向: 0 | 帧内: 0 | 平均MV: 0.0 | 最大MV: 0.0"),
        macroblock_tab_);
    toolbar->addWidget(macroblock_summary_label_, 1);

    export_macroblock_csv_button_ = new QPushButton(tr("导出 CSV"), macroblock_tab_);
    toolbar->addWidget(export_macroblock_csv_button_);

    QCheckBox* toggle = new QCheckBox(tr("启用分析"), macroblock_tab_);
    toggle->setChecked(feature_enabled_.value(AnalysisFeature::Macroblock, false));
    connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::Macroblock] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Macroblock), checked);
    });
    toolbar->addWidget(toggle);
    layout->addLayout(toolbar);

    // 运动矢量表格 + 可视化预览 (水平分割)
    QSplitter* h_split = new QSplitter(Qt::Horizontal, macroblock_tab_);

    // 左: 运动矢量表格
    QGroupBox* mv_group = new QGroupBox(tr("运动矢量列表"), macroblock_tab_);
    QVBoxLayout* mv_layout = new QVBoxLayout(mv_group);
    macroblock_table_ = new QTableWidget(0, 9, mv_group);
    macroblock_table_->setHorizontalHeaderLabels(
        {"序号", "块X", "块Y", "块大小", "MVx(px)", "MVy(px)", "幅度(px)", "角度(°)", "参考"});
    macroblock_table_->verticalHeader()->setVisible(false);
    macroblock_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    macroblock_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    macroblock_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    macroblock_table_->horizontalHeader()->setStretchLastSection(true);
    macroblock_table_->setColumnWidth(0, 50);
    macroblock_table_->setColumnWidth(1, 60);
    macroblock_table_->setColumnWidth(2, 60);
    macroblock_table_->setColumnWidth(3, 70);
    macroblock_table_->setColumnWidth(4, 80);
    macroblock_table_->setColumnWidth(5, 80);
    macroblock_table_->setColumnWidth(6, 80);
    macroblock_table_->setColumnWidth(7, 70);
    macroblock_table_->setMinimumWidth(450);
    macroblock_table_->setMinimumHeight(200);
    mv_layout->addWidget(macroblock_table_);
    h_split->addWidget(mv_group);

    // 右: 运动矢量可视化预览
    QGroupBox* viz_group = new QGroupBox(tr("运动矢量可视化"), macroblock_tab_);
    QVBoxLayout* viz_layout = new QVBoxLayout(viz_group);
    macroblock_viz_label_ = new QLabel(viz_group);
    macroblock_viz_label_->setMinimumSize(320, 240);
    macroblock_viz_label_->setAlignment(Qt::AlignCenter);
    macroblock_viz_label_->setStyleSheet("background-color: #0D1117; border: 1px solid #30363D;");
    macroblock_viz_label_->setText(tr("等待视频播放...\n\n启用分析后将在播放时\n显示运动矢量可视化"));
    viz_layout->addWidget(macroblock_viz_label_);
    h_split->addWidget(viz_group);

    h_split->setStretchFactor(0, 3);
    h_split->setStretchFactor(1, 2);
    layout->addWidget(h_split, 1);

    // 块大小分布 + 幅度分布 (水平排列)
    QHBoxLayout* dist_layout = new QHBoxLayout();

    QGroupBox* blocksize_group = new QGroupBox(tr("块大小分布"), macroblock_tab_);
    QVBoxLayout* bs_layout = new QVBoxLayout(blocksize_group);
    macroblock_blocksize_table_ = new QTableWidget(0, 3, blocksize_group);
    macroblock_blocksize_table_->setHorizontalHeaderLabels({"块大小", "数量", "占比"});
    macroblock_blocksize_table_->verticalHeader()->setVisible(false);
    macroblock_blocksize_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    macroblock_blocksize_table_->horizontalHeader()->setStretchLastSection(true);
    macroblock_blocksize_table_->setMinimumHeight(160);
    bs_layout->addWidget(macroblock_blocksize_table_);
    dist_layout->addWidget(blocksize_group);

    QGroupBox* mag_group = new QGroupBox(tr("运动幅度分布"), macroblock_tab_);
    QVBoxLayout* mg_layout = new QVBoxLayout(mag_group);
    macroblock_mag_table_ = new QTableWidget(0, 3, mag_group);
    macroblock_mag_table_->setHorizontalHeaderLabels({"幅度范围(px)", "数量", "占比"});
    macroblock_mag_table_->verticalHeader()->setVisible(false);
    macroblock_mag_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    macroblock_mag_table_->horizontalHeader()->setStretchLastSection(true);
    macroblock_mag_table_->setMinimumHeight(160);
    mg_layout->addWidget(macroblock_mag_table_);
    dist_layout->addWidget(mag_group);

    layout->addLayout(dist_layout);

    AddPageWithScroll(macroblock_tab_, tr("宏块分析"));

    connect(export_macroblock_csv_button_, &QPushButton::clicked,
            this, &AnalysisPanel::OnExportMacroblockCsv);
}

void AnalysisPanel::UpdateMacroblockInfo(const model::MacroblockFrameAnalysis& analysis) {
    current_macroblock_analysis_ = analysis;
    macroblock_dirty_ = true;
}

void AnalysisPanel::RefreshMacroblockUi() {
    const auto& ma = current_macroblock_analysis_;
    const auto& s = ma.stats;

    // 术语自适应: HEVC→CTU, 其余→宏块
    const bool is_hevc = CodecType::IsHevc(ma.codec_id);
    const QString block_term = is_hevc ? tr("CTU") : tr("宏块");

    // 更新统计标签
    QString frame_type_str;
    switch (ma.frame_type) {
        case 1: frame_type_str = "I"; break;
        case 2: frame_type_str = "P"; break;
        case 3: frame_type_str = "B"; break;
        default: frame_type_str = "?"; break;
    }
    macroblock_summary_label_->setText(
        tr("帧 #%1 [%2] | 时间: %3s | %4总数: %5 | 前向: %6 | 后向: %7 | 帧内: %8 | 平均MV: %9 | 最大MV: %10")
            .arg(ma.frame_index)
            .arg(frame_type_str)
            .arg(ma.timestamp, 0, 'f', 3)
            .arg(block_term)
            .arg(s.total_blocks)
            .arg(s.forward_count)
            .arg(s.backward_count)
            .arg(s.intra_count)
            .arg(s.avg_motion_magnitude, 0, 'f', 2)
            .arg(s.max_motion_magnitude, 0, 'f', 2));

    // 填充运动矢量表格 (限制最多 500 行以保证流畅)
    macroblock_table_->setUpdatesEnabled(false);
    macroblock_table_->setRowCount(0);
    const int max_rows = 500;
    const int mv_count = static_cast<int>(ma.motion_vectors.size());
    const int display_count = qMin(mv_count, max_rows);
    macroblock_table_->setRowCount(display_count);

    for (int i = 0; i < display_count; ++i) {
        const auto& mv = ma.motion_vectors[i];
        double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
        double px_mx = mv.motion_x / scale;
        double px_my = mv.motion_y / scale;
        QString ref_str = (mv.source < 0) ? tr("前向") : (mv.source > 0) ? tr("后向") : tr("—");
        QString size_str = QString("%1x%2").arg(mv.block_w).arg(mv.block_h);

        SetTableItemText(macroblock_table_, i, 0, QString::number(i));
        SetTableItemText(macroblock_table_, i, 1, QString::number(mv.block_x));
        SetTableItemText(macroblock_table_, i, 2, QString::number(mv.block_y));
        SetTableItemText(macroblock_table_, i, 3, size_str);
        SetTableItemText(macroblock_table_, i, 4, QString::number(px_mx, 'f', 2));
        SetTableItemText(macroblock_table_, i, 5, QString::number(px_my, 'f', 2));
        SetTableItemText(macroblock_table_, i, 6, QString::number(mv.motion_magnitude, 'f', 2));
        SetTableItemText(macroblock_table_, i, 7, QString::number(mv.motion_angle, 'f', 1));
        SetTableItemText(macroblock_table_, i, 8, ref_str);
    }
    macroblock_table_->setUpdatesEnabled(true);

    // 填充块大小分布表
    auto fill_dist_table = [this](QTableWidget* table, const QStringList& labels, const QList<int>& counts, int total) {
        table->setUpdatesEnabled(false);
        table->setRowCount(labels.size());
        for (int i = 0; i < labels.size(); ++i) {
            SetTableItemText(table, i, 0, labels[i]);
            SetTableItemText(table, i, 1, QString::number(counts[i]));
            double ratio = (total > 0) ? (100.0 * counts[i] / total) : 0.0;
            SetTableItemText(table, i, 2, QString::number(ratio, 'f', 1) + "%");
        }
        table->setUpdatesEnabled(true);
    };

    // 块大小分布表 — 根据 codec 动态显示尺寸行
    QStringList bs_labels;
    QList<int> bs_counts;
    if (is_hevc) {
        // HEVC CTU/CU 大尺寸分区 + 共有小尺寸
        bs_labels = {"64x64", "32x32", "32x16", "16x32", "32x8", "8x32",
                     "16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4", tr("其他")};
        bs_counts = {s.count_64x64, s.count_32x32, s.count_32x16, s.count_16x32,
                     s.count_32x8, s.count_8x32, s.count_16x16, s.count_16x8,
                     s.count_8x16, s.count_8x8, s.count_8x4, s.count_4x8,
                     s.count_4x4, s.count_other};
    } else {
        // H.264 等传统编码: 宏块尺寸
        bs_labels = {"16x16", "16x8", "8x16", "8x8", "8x4", "4x8", "4x4", tr("其他")};
        bs_counts = {s.count_16x16, s.count_16x8, s.count_8x16, s.count_8x8,
                     s.count_8x4, s.count_4x8, s.count_4x4, s.count_other};
    }
    int bs_total = 0;
    for (int c : bs_counts) bs_total += c;
    fill_dist_table(macroblock_blocksize_table_, bs_labels, bs_counts, bs_total);

    int mg_total = s.mag_0_2 + s.mag_2_4 + s.mag_4_8 + s.mag_8_16 + s.mag_16_plus;
    fill_dist_table(macroblock_mag_table_,
                    {"0-2", "2-4", "4-8", "8-16", "16+"},
                    {s.mag_0_2, s.mag_2_4, s.mag_4_8, s.mag_8_16, s.mag_16_plus},
                    mg_total);

    // 绘制运动矢量可视化图
    if (ma.frame_width > 0 && ma.frame_height > 0 && !ma.motion_vectors.empty()) {
        const int kMaxVizW = 480;
        const int kMaxVizH = 320;
        double aspect = static_cast<double>(ma.frame_width) / ma.frame_height;
        int viz_w = kMaxVizW;
        int viz_h = static_cast<int>(viz_w / aspect);
        if (viz_h > kMaxVizH) {
            viz_h = kMaxVizH;
            viz_w = static_cast<int>(viz_h * aspect);
        }
        double scale_x = static_cast<double>(viz_w) / ma.frame_width;
        double scale_y = static_cast<double>(viz_h) / ma.frame_height;

        QImage viz_img(viz_w, viz_h, QImage::Format_RGB32);
        viz_img.fill(QColor(13, 17, 23));  // 深色背景

        QPainter painter(&viz_img);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // 绘制运动矢量箭头
        for (const auto& mv : ma.motion_vectors) {
            double mv_scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
            double px_mx = mv.motion_x / mv_scale;
            double px_my = mv.motion_y / mv_scale;

            double cx = (mv.block_x + mv.block_w / 2.0) * scale_x;
            double cy = (mv.block_y + mv.block_h / 2.0) * scale_y;
            double ex = cx + px_mx * scale_x;
            double ey = cy + px_my * scale_y;

            // 颜色: 前向=红, 后向=蓝
            QColor color = (mv.source < 0) ? QColor(255, 107, 107)
                                           : QColor(88, 166, 255);
            // 幅度越大越亮
            double brightness = qMin(1.0, mv.motion_magnitude / 16.0);
            color.setAlphaF(0.3 + 0.7 * brightness);

            QPen pen(color, 1.2);
            painter.setPen(pen);
            painter.drawLine(QPointF(cx, cy), QPointF(ex, ey));

            // 箭头头部
            if (mv.motion_magnitude > 0.5) {
                double angle = std::atan2(ey - cy, ex - cx);
                double arrow_len = 3.0;
                QPointF p1(ex - arrow_len * std::cos(angle - 0.5),
                           ey - arrow_len * std::sin(angle - 0.5));
                QPointF p2(ex - arrow_len * std::cos(angle + 0.5),
                           ey - arrow_len * std::sin(angle + 0.5));
                painter.drawLine(QPointF(ex, ey), p1);
                painter.drawLine(QPointF(ex, ey), p2);
            }
        }

        // 绘制图例
        painter.setPen(QPen(QColor(255, 107, 107), 2));
        painter.drawLine(10, viz_h - 20, 30, viz_h - 20);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(35, viz_h - 16, tr("前向"));
        painter.setPen(QPen(QColor(88, 166, 255), 2));
        painter.drawLine(80, viz_h - 20, 100, viz_h - 20);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(105, viz_h - 16, tr("后向"));

        painter.end();
        macroblock_viz_label_->setPixmap(QPixmap::fromImage(viz_img));
    } else if (ma.has_motion_vectors == false && ma.frame_width > 0) {
        // I 帧: 无运动矢量
        macroblock_viz_label_->setText(
            tr("I 帧 (无运动矢量)\n帧内块数: %1").arg(s.intra_count));
    }
}

void AnalysisPanel::OnExportMacroblockCsv() {
    const auto& ma = current_macroblock_analysis_;
    if (ma.motion_vectors.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有宏块分析数据可导出"));
        return;
    }

    QString filename = QFileDialog::getSaveFileName(
        this, tr("导出宏块分析 CSV"),
        QStringLiteral("macroblock_frame_%1.csv").arg(ma.frame_index),
        tr("CSV 文件 (*.csv)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("错误"), tr("无法打开文件写入"));
        return;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    // BOM for Excel
    stream << "\xEF\xBB\xBF";
    stream << "序号,块X,块Y,块宽,块高,MVx(raw),MVy(raw),精度分母,MVx(px),MVy(px),幅度(px),角度(度),参考方向\n";

    for (int i = 0; i < static_cast<int>(ma.motion_vectors.size()); ++i) {
        const auto& mv = ma.motion_vectors[i];
        double scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
        double px_mx = mv.motion_x / scale;
        double px_my = mv.motion_y / scale;
        QString ref = (mv.source < 0) ? "forward" : (mv.source > 0) ? "backward" : "none";

        stream << i << ","
               << mv.block_x << ","
               << mv.block_y << ","
               << static_cast<int>(mv.block_w) << ","
               << static_cast<int>(mv.block_h) << ","
               << mv.motion_x << ","
               << mv.motion_y << ","
               << mv.motion_scale << ","
               << QString::number(px_mx, 'f', 4) << ","
               << QString::number(px_my, 'f', 4) << ","
               << QString::number(mv.motion_magnitude, 'f', 4) << ","
               << QString::number(mv.motion_angle, 'f', 2) << ","
               << ref << "\n";
    }
    file.close();

    QMessageBox::information(this, tr("成功"),
        tr("已导出 %1 条运动矢量到:\n%2").arg(ma.motion_vectors.size()).arg(filename));
}

// ===========================================================================
// 场景切换检测标签页
// ===========================================================================

void AnalysisPanel::SetupSceneChangeTab() {
    scene_change_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(scene_change_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 启用开关
    {
        QWidget* row = new QWidget(scene_change_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("场景切换检测（镜头边界）"), row);
        rl->addWidget(title);
        rl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用检测"), row);
        toggle->setChecked(feature_enabled_.value(AnalysisFeature::SceneChange, false));
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_enabled_[AnalysisFeature::SceneChange] = checked;
            emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::SceneChange), checked);
        });
        rl->addWidget(toggle);
        layout->addWidget(row);
    }

    scene_change_summary_label_ = new QLabel(
        tr("启用检测并在播放中分析视频，将在此列出检测到的镜头切换点（时间戳 + 切换强度）。"));
    scene_change_summary_label_->setWordWrap(true);
    layout->addWidget(scene_change_summary_label_);

    // 切换强度柱状图
    scene_change_chart_object_ = new QChart();
    scene_change_chart_object_->setTitle(tr("切换强度（逐切换点）"));
    scene_change_bar_set_ = new QBarSet(tr("强度"));
    scene_change_series_ = new QBarSeries();
    scene_change_series_->append(scene_change_bar_set_);
    scene_change_chart_object_->addSeries(scene_change_series_);
    scene_change_axis_x_ = new QValueAxis();
    scene_change_axis_x_->setTitleText(tr("切换点序号"));
    scene_change_axis_y_ = new QValueAxis();
    scene_change_axis_y_->setTitleText(tr("强度"));
    scene_change_axis_y_->setRange(0, 1);
    scene_change_chart_object_->addAxis(scene_change_axis_x_, Qt::AlignBottom);
    scene_change_chart_object_->addAxis(scene_change_axis_y_, Qt::AlignLeft);
    scene_change_series_->attachAxis(scene_change_axis_x_);
    scene_change_series_->attachAxis(scene_change_axis_y_);
    scene_change_chart_ = new QChartView(scene_change_chart_object_);
    scene_change_chart_->setMinimumHeight(180);
    scene_change_chart_->setRenderHint(QPainter::Antialiasing);
    layout->addWidget(scene_change_chart_);

    // 切换点列表
    scene_change_table_ = new QTableWidget(0, 3, scene_change_tab_);
    scene_change_table_->setHorizontalHeaderLabels({tr("帧序号"), tr("时间戳"), tr("切换强度")});
    scene_change_table_->verticalHeader()->setVisible(false);
    scene_change_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scene_change_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    scene_change_table_->horizontalHeader()->setStretchLastSection(true);
    scene_change_table_->setMinimumHeight(200);
    layout->addWidget(scene_change_table_);

    // 导出按钮
    QPushButton* export_btn = new QPushButton(tr("导出切换点 CSV"), scene_change_tab_);
    connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportSceneChangeCsv);
    layout->addWidget(export_btn, 0, Qt::AlignRight);

    AddPageWithScroll(scene_change_tab_, tr("场景切换"));
}

void AnalysisPanel::OnSceneChangeDetected(const analyzer::SceneChangeResult& result) {
    scene_change_records_.push_back(result);
    scene_change_table_dirty_ = true;
    scene_change_dirty_ = true;
    if (update_timer_) update_timer_->start(kUiFlushIntervalMs);
}

void AnalysisPanel::FlushPendingSceneChangeTable() {
    if (!scene_change_table_dirty_) return;
    scene_change_table_dirty_ = false;
    const size_t total = scene_change_records_.size();
    for (size_t i = scene_change_table_synced_count_; i < total; ++i) {
        AppendSceneChangeRow(scene_change_records_[i]);
    }
    scene_change_table_synced_count_ = total;
    UpdateSceneChangeChart();
    UpdateSceneChangeSummary();
}

void AnalysisPanel::AppendSceneChangeRow(const analyzer::SceneChangeResult& result) {
    const int row = scene_change_table_->rowCount();
    scene_change_table_->insertRow(row);
    SetTableItemText(scene_change_table_, row, 0, QString::number(result.frame_index));
    SetTableItemText(scene_change_table_, row, 1, theme::font::formatTime(static_cast<int>(result.timestamp * 1000)));
    SetTableItemText(scene_change_table_, row, 2, QString::number(result.score, 'f', 3));
}

void AnalysisPanel::UpdateSceneChangeChart() {
    if (scene_change_bar_set_->count() > 0)
        scene_change_bar_set_->remove(0, scene_change_bar_set_->count());
    // 限制显示最近 200 个切换点, 避免柱状图过载
    const int max_bars = 200;
    const int start = scene_change_records_.size() > static_cast<size_t>(max_bars)
                          ? static_cast<int>(scene_change_records_.size() - max_bars) : 0;
    for (int i = start; i < static_cast<int>(scene_change_records_.size()); ++i) {
        scene_change_bar_set_->append(scene_change_records_[i].score);
    }
    scene_change_axis_x_->setRange(start, std::max(start + 1, static_cast<int>(scene_change_records_.size())));
}

void AnalysisPanel::UpdateSceneChangeSummary() {
    const int n = static_cast<int>(scene_change_records_.size());
    if (n == 0) {
        scene_change_summary_label_->setText(
            tr("启用检测并在播放中分析视频，将在此列出检测到的镜头切换点（时间戳 + 切换强度）。"));
        return;
    }
    double sum = 0.0;
    double max_s = 0.0;
    for (const auto& r : scene_change_records_) { sum += r.score; if (r.score > max_s) max_s = r.score; }
    scene_change_summary_label_->setText(
        tr("共检测到 %1 个切换点 | 平均强度 %2 | 最大强度 %3")
            .arg(n).arg(QString::number(sum / n, 'f', 3)).arg(QString::number(max_s, 'f', 3)));
}

void AnalysisPanel::OnExportSceneChangeCsv() {
    if (scene_change_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的切换点数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(this, tr("导出切换点 CSV"),
        QString::fromStdString(current_video_path_).section('/', -1) + "_scenecut.csv",
        tr("CSV 文件 (*.csv)"));
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("错误"), tr("无法打开文件: ") + filename);
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "\xEF\xBB\xBF";
    stream << "frame_index,timestamp_seconds,score\n";
    for (const auto& r : scene_change_records_) {
        stream << r.frame_index << "," << QString::number(r.timestamp, 'f', 3) << ","
               << QString::number(r.score, 'f', 4) << "\n";
    }
    file.close();
    QMessageBox::information(this, tr("成功"), tr("已导出 %1 个切换点到:\n%2")
        .arg(scene_change_records_.size()).arg(filename));
}

// ===========================================================================
// 质量评估（PSNR/SSIM）标签页
// ===========================================================================

void AnalysisPanel::SetupQualityTab() {
    quality_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(quality_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QLabel* title = new QLabel(tr("质量评估（PSNR / SSIM）"));
    layout->addWidget(title);

    QLabel* desc = new QLabel(
        tr("选择一段“参考视频”（与主视频内容相同、通常为原始/高质量版本），点击“开始评估”后，"
           "软件将逐帧对比计算 PSNR 与 SSIM。PSNR 越高、SSIM 越接近 1 表示质量越好。"));
    desc->setWordWrap(true);
    layout->addWidget(desc);

    // 控制行
    QWidget* ctrl = new QWidget(quality_tab_);
    QHBoxLayout* cl = new QHBoxLayout(ctrl);
    cl->setContentsMargins(0, 0, 0, 0);
    quality_select_ref_button_ = new QPushButton(tr("选择参考视频..."), ctrl);
    connect(quality_select_ref_button_, &QPushButton::clicked, this, &AnalysisPanel::OnSelectReferenceClicked);
    quality_start_button_ = new QPushButton(tr("开始评估"), ctrl);
    connect(quality_start_button_, &QPushButton::clicked, this, &AnalysisPanel::OnStartQualityClicked);
    quality_cancel_button_ = new QPushButton(tr("取消"), ctrl);
    quality_cancel_button_->setEnabled(false);
    connect(quality_cancel_button_, &QPushButton::clicked, this, &AnalysisPanel::OnCancelQualityClicked);
    cl->addWidget(quality_select_ref_button_);
    cl->addWidget(quality_start_button_);
    cl->addWidget(quality_cancel_button_);
    cl->addStretch();
    layout->addWidget(ctrl);

    quality_ref_label_ = new QLabel(tr("参考视频: 未选择"));
    layout->addWidget(quality_ref_label_);

    quality_progress_ = new QProgressBar(quality_tab_);
    quality_progress_->setRange(0, 0);  // 不确定模式
    quality_progress_->setTextVisible(false);
    quality_progress_->setVisible(false);
    layout->addWidget(quality_progress_);

    quality_summary_label_ = new QLabel(tr("就绪。请先选择参考视频。"));
    layout->addWidget(quality_summary_label_);

    // PSNR 曲线
    quality_psnr_chart_object_ = new QChart();
    quality_psnr_chart_object_->setTitle(tr("PSNR (dB)"));
    quality_psnr_series_ = new QLineSeries();
    quality_psnr_chart_object_->addSeries(quality_psnr_series_);
    quality_psnr_axis_x_ = new QValueAxis();
    quality_psnr_axis_x_->setTitleText(tr("帧序号"));
    quality_psnr_axis_y_ = new QValueAxis();
    quality_psnr_axis_y_->setTitleText(tr("PSNR (dB)"));
    quality_psnr_axis_y_->setRange(0, 60);
    quality_psnr_chart_object_->addAxis(quality_psnr_axis_x_, Qt::AlignBottom);
    quality_psnr_chart_object_->addAxis(quality_psnr_axis_y_, Qt::AlignLeft);
    quality_psnr_series_->attachAxis(quality_psnr_axis_x_);
    quality_psnr_series_->attachAxis(quality_psnr_axis_y_);
    quality_psnr_chart_ = new QChartView(quality_psnr_chart_object_);
    quality_psnr_chart_->setMinimumHeight(160);
    quality_psnr_chart_->setRenderHint(QPainter::Antialiasing);
    layout->addWidget(quality_psnr_chart_);

    // SSIM 曲线
    quality_ssim_chart_object_ = new QChart();
    quality_ssim_chart_object_->setTitle(tr("SSIM"));
    quality_ssim_series_ = new QLineSeries();
    quality_ssim_chart_object_->addSeries(quality_ssim_series_);
    quality_ssim_axis_x_ = new QValueAxis();
    quality_ssim_axis_x_->setTitleText(tr("帧序号"));
    quality_ssim_axis_y_ = new QValueAxis();
    quality_ssim_axis_y_->setTitleText(tr("SSIM"));
    quality_ssim_axis_y_->setRange(0, 1);
    quality_ssim_chart_object_->addAxis(quality_ssim_axis_x_, Qt::AlignBottom);
    quality_ssim_chart_object_->addAxis(quality_ssim_axis_y_, Qt::AlignLeft);
    quality_ssim_series_->attachAxis(quality_ssim_axis_x_);
    quality_ssim_series_->attachAxis(quality_ssim_axis_y_);
    quality_ssim_chart_ = new QChartView(quality_ssim_chart_object_);
    quality_ssim_chart_->setMinimumHeight(160);
    quality_ssim_chart_->setRenderHint(QPainter::Antialiasing);
    layout->addWidget(quality_ssim_chart_);

    // 明细表
    quality_table_ = new QTableWidget(0, 4, quality_tab_);
    quality_table_->setHorizontalHeaderLabels({tr("帧序号"), tr("时间戳"), tr("PSNR(dB)"), tr("SSIM")});
    quality_table_->verticalHeader()->setVisible(false);
    quality_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    quality_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    quality_table_->horizontalHeader()->setStretchLastSection(true);
    quality_table_->setMinimumHeight(160);
    layout->addWidget(quality_table_);

    QPushButton* export_btn = new QPushButton(tr("导出质量 CSV"), quality_tab_);
    connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportQualityCsv);
    layout->addWidget(export_btn, 0, Qt::AlignRight);

    AddPageWithScroll(quality_tab_, tr("质量评估"));
}

void AnalysisPanel::ResetQualityUi() {
    quality_records_.clear();
    quality_table_synced_count_ = 0;
    quality_table_->setRowCount(0);
    quality_psnr_series_->clear();
    quality_ssim_series_->clear();
    quality_psnr_axis_x_->setRange(0, 1);
    quality_ssim_axis_x_->setRange(0, 1);
}

void AnalysisPanel::OnSelectReferenceClicked() {
    const QString filename = QFileDialog::getOpenFileName(this, tr("选择参考视频"),
        QString(), tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.ts *.webm *.flv);;所有文件 (*.*)"));
    if (filename.isEmpty()) return;
    quality_ref_path_ = filename.toStdString();
    quality_ref_label_->setText(tr("参考视频: %1").arg(filename.section('/', -1)));
    quality_summary_label_->setText(tr("已选择参考视频，点击“开始评估”进行比较。"));
}

void AnalysisPanel::OnStartQualityClicked() {
    if (quality_running_) return;
    if (quality_main_path_.empty()) {
        QMessageBox::information(this, tr("提示"),
            tr("请先打开一个主视频（当前未加载任何文件）。"));
        return;
    }
    if (quality_ref_path_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("请先选择参考视频。"));
        return;
    }
    ResetQualityUi();
    quality_running_ = true;
    quality_cancel_button_->setEnabled(true);
    quality_start_button_->setEnabled(false);
    quality_select_ref_button_->setEnabled(false);
    quality_progress_->setVisible(true);
    quality_progress_->setTextVisible(true);
    quality_summary_label_->setText(tr("正在评估，请稍候..."));

    // 在工作线程中运行评估，避免阻塞 UI。
    analyzer::QualityAnalyzer* analyzer = &quality_analyzer_;
    const std::string main_path = quality_main_path_;
    const std::string ref_path = quality_ref_path_;

    if (quality_thread_.joinable()) quality_thread_.join();
    quality_thread_ = std::thread([this, analyzer, main_path, ref_path]() {
        analyzer->Run(
            main_path, ref_path,
            [this](int cur, int) {
                QMetaObject::invokeMethod(this, &AnalysisPanel::OnQualityProgress,
                                          Qt::QueuedConnection, cur);
            },
            [this](const analyzer::QualityFrameResult& fr) {
                QMetaObject::invokeMethod(this, &AnalysisPanel::OnQualityFrameResult,
                                          Qt::QueuedConnection, fr);
            },
            [this](const analyzer::QualitySummary& sum) {
                QMetaObject::invokeMethod(this, &AnalysisPanel::OnQualitySummary,
                                          Qt::QueuedConnection, sum);
            });
    });
}

void AnalysisPanel::OnCancelQualityClicked() {
    if (!quality_running_) return;
    quality_analyzer_.cancel_ = true;
    quality_summary_label_->setText(tr("正在取消..."));
}

void AnalysisPanel::OnQualityProgress(int current) {
    quality_progress_->setFormat(tr("已比较 %1 帧").arg(current));
}

void AnalysisPanel::OnQualityFrameResult(const analyzer::QualityFrameResult& result) {
    quality_records_.push_back(result);
    quality_table_dirty_ = true;
    quality_dirty_ = true;
    if (update_timer_) update_timer_->start(kUiFlushIntervalMs);
}

void AnalysisPanel::FlushPendingQualityTable() {
    if (!quality_table_dirty_) return;
    quality_table_dirty_ = false;
    const size_t total = quality_records_.size();
    for (size_t i = quality_table_synced_count_; i < total; ++i) {
        AppendQualityRow(quality_records_[i]);
    }
    quality_table_synced_count_ = total;
    UpdateQualityCharts();
}

void AnalysisPanel::AppendQualityRow(const analyzer::QualityFrameResult& result) {
    const int row = quality_table_->rowCount();
    quality_table_->insertRow(row);
    SetTableItemText(quality_table_, row, 0, QString::number(result.frame_index));
    SetTableItemText(quality_table_, row, 1, theme::font::formatTime(static_cast<int>(result.timestamp * 1000)));
    SetTableItemText(quality_table_, row, 2, QString::number(result.psnr, 'f', 2));
    SetTableItemText(quality_table_, row, 3, QString::number(result.ssim, 'f', 4));
}

void AnalysisPanel::UpdateQualityCharts() {
    quality_psnr_series_->clear();
    quality_ssim_series_->clear();
    const int n = static_cast<int>(quality_records_.size());
    for (int i = 0; i < n; ++i) {
        quality_psnr_series_->append(i, quality_records_[i].psnr);
        quality_ssim_series_->append(i, quality_records_[i].ssim);
    }
    if (n > 0) {
        quality_psnr_axis_x_->setRange(0, n - 1);
        quality_ssim_axis_x_->setRange(0, n - 1);
    }
}

void AnalysisPanel::OnQualitySummary(const analyzer::QualitySummary& summary) {
    quality_running_ = false;
    quality_progress_->setVisible(false);
    quality_cancel_button_->setEnabled(false);
    quality_start_button_->setEnabled(true);
    quality_select_ref_button_->setEnabled(true);
    if (quality_thread_.joinable()) quality_thread_.join();

    if (!summary.ok) {
        quality_summary_label_->setText(tr("评估失败: %1").arg(QString::fromStdString(summary.error)));
        return;
    }
    quality_summary_label_->setText(
        tr("比较 %1 帧 | 平均 PSNR %2 dB | 最低 PSNR %3 dB (帧 %4) | 平均 SSIM %5 | 最低 SSIM %6 (帧 %7)")
            .arg(summary.compared_frames)
            .arg(QString::number(summary.mean_psnr, 'f', 2))
            .arg(QString::number(summary.min_psnr, 'f', 2)).arg(summary.min_psnr_frame)
            .arg(QString::number(summary.mean_ssim, 'f', 4))
            .arg(QString::number(summary.min_ssim, 'f', 4)).arg(summary.min_ssim_frame));
}

void AnalysisPanel::OnExportQualityCsv() {
    if (quality_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的质量数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(this, tr("导出质量 CSV"),
        QString::fromStdString(current_video_path_).section('/', -1) + "_quality.csv",
        tr("CSV 文件 (*.csv)"));
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("错误"), tr("无法打开文件: ") + filename);
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "\xEF\xBB\xBF";
    stream << "frame_index,timestamp_seconds,psnr_db,ssim\n";
    for (const auto& r : quality_records_) {
        stream << r.frame_index << "," << QString::number(r.timestamp, 'f', 3) << ","
               << QString::number(r.psnr, 'f', 4) << "," << QString::number(r.ssim, 'f', 6) << "\n";
    }
    file.close();
    QMessageBox::information(this, tr("成功"), tr("已导出 %1 帧质量数据到:\n%2")
        .arg(quality_records_.size()).arg(filename));
}

} // namespace ui
} // namespace videoeye
