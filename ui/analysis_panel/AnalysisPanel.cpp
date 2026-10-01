#include "ui/analysis_panel/AnalysisPanel.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"
#include "core/reporting/QcReportExporter.h"
#include "core/reporting/StreamStatsExporter.h"
// facade 的公开头只暴露 AnalysisOptions / AnalysisResult / domain model，
// 下面两个分析器头是本 cpp 自己要用的（异常类型枚举与色彩/HDR 行构造器），
// 不再经由 facade 间接带入。
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/analysis/quality/ColorHdrAnalyzer.h"
// 帧类型 / 包标志 / 媒体类型常量（AV_PICTURE_TYPE_* / AV_PKT_FLAG_* / AVMEDIA_TYPE_*）。
// 原先由被移除的 core/analysis 头文件间接带入；现在 UI 直接依赖 FFmpeg 公共常量，
// 显式 include（与"静态库 PRIVATE 不传 include 目录"的一致）。
#include <libavcodec/avcodec.h>
#include "ui/theme/AppTheme.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QColor>
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
#include <QToolTip>
#include <QCursor>
#include <algorithm>
#include <functional>
#include <cmath>
#include <climits>
#include <cstdio>
#include <limits>

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

// SeriesBatch / TableBatch 统一用 AnalysisPageSupport.h 里那份：
// 抽出去的页面组件和面板共用，各存一份会在同一 TU 里变成歧义符号。
} // namespace

AnalysisPanel::AnalysisPanel(QWidget* parent)
    : QWidget(parent) {
    
    // 默认启用: 基础功能, 关闭: 高性能分析
    feature_enabled_[AnalysisFeature::Master] = true;
    feature_enabled_[AnalysisFeature::StreamStats] = true;
    feature_enabled_[AnalysisFeature::VideoFrame] = true;
    feature_enabled_[AnalysisFeature::AudioFrame] = false;
    feature_enabled_[AnalysisFeature::Packet] = false;
    feature_enabled_[AnalysisFeature::Event] = false;
    feature_enabled_[AnalysisFeature::SyncSample] = false;
    feature_enabled_[AnalysisFeature::Timeline] = false;
    feature_enabled_[AnalysisFeature::ContainerStructure] = true;
    feature_enabled_[AnalysisFeature::Macroblock] = false;
    feature_enabled_[AnalysisFeature::SceneChange] = false;
    feature_enabled_[AnalysisFeature::Diagnostics] = true;
    // 画面质量默认关: 播放时要额外做降采样与边缘统计，属于按需开启的重活
    feature_enabled_[AnalysisFeature::VisualDefect] = false;

    SetupUI();
    
    update_timer_ = new QTimer(this);
    connect(update_timer_, &QTimer::timeout, this, &AnalysisPanel::FlushPendingUiUpdates);
    update_timer_->start(kUiFlushIntervalMs);
    
    LOG_INFO("分析面板已初始化");
}

AnalysisPanel::~AnalysisPanel() {
    LOG_INFO("分析面板已销毁");
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
        AnalysisFeature::Macroblock,
        AnalysisFeature::VisualDefect,
    };
    for (auto feat : kFeatures) {
        bool enabled = feature_enabled_.value(feat, true);
        emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
    }
}


void AnalysisPanel::AddPageWithScroll(QWidget* tab_widget, const QString& title) {
    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(tab_widget);
    scroll->setProperty("pageTitle", title);  // MainWindow 用它生成侧边栏条目
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
    // stream_section_ 是 bitstream_tab_ 顶部的「流概览」区, 不再作为独立页
    stream_section_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(stream_section_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 第一行: 标题 + 流概览启用 toggle + 导出按钮
    {
        QWidget* row = new QWidget(stream_section_);
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

        export_button_ = new QPushButton(tr("导出分析报告"), row);
        export_button_->setToolTip(tr("将当前流统计信息导出为 HTML/JSON/TXT 报告"));
        rl->addWidget(export_button_);

        layout->addWidget(row);
    }

    // 第二行: 流概览 5 指标横排
    QStringList stream_labels = {
        tr("当前码率"), tr("平均码率"), tr("峰值码率"), tr("分析时长"), tr("最大GOP大小")
    };
    stats_table_ = new QTableWidget(1, stream_labels.size(), stream_section_);
    stats_table_->setHorizontalHeaderLabels(stream_labels);
    stats_table_->verticalHeader()->setVisible(false);
    for (int c = 0; c < stream_labels.size(); ++c) {
        stats_table_->setColumnWidth(c, 130);
        stats_table_->setItem(0, c, new QTableWidgetItem("0"));
    }
    layout->addWidget(stats_table_);

    // 第三行: 码率 / FPS / GOP 三条趋势横向并排 (而不是纵向堆叠)
    QWidget* chart_row = new QWidget(stream_section_);
    QHBoxLayout* chart_layout = new QHBoxLayout(chart_row);
    chart_layout->setContentsMargins(0, 0, 0, 0);
    chart_layout->setSpacing(8);

    auto make_chart_box = [&](const QString& title) -> QPair<MetricChartWidget*, QGroupBox*> {
        MetricChartWidget* view = new MetricChartWidget(chart_row);
        view->setMinimumHeight(180);
        view->SetLegendVisible(false);   // 标题已在 GroupBox 上, 图例无信息量
        QGroupBox* box = new QGroupBox(title, chart_row);
        QVBoxLayout* bl = new QVBoxLayout(box);
        bl->setContentsMargins(4, 12, 4, 4);
        bl->addWidget(view);
        chart_layout->addWidget(box, 1);
        return {view, box};
    };

    auto b = make_chart_box(tr("码率趋势 (Kbps)"));
    bitrate_chart_ = b.first;
    auto f = make_chart_box(tr("帧率趋势 (fps)"));
    fps_chart_ = f.first;
    auto g = make_chart_box(tr("GOP 帧数分布"));
    gop_chart_ = g.first;

    layout->addWidget(chart_row);

    // 码率图
    bitrate_series_ = bitrate_chart_->AddLineSeries(QString(), QColor("#42a5f5"));
    bitrate_axis_x_ = bitrate_chart_->AxisX();
    bitrate_axis_y_ = bitrate_chart_->AxisY();
    bitrate_axis_x_->SetLabelFormat("%d");
    bitrate_axis_y_->SetLabelFormat("%.0f");
    bitrate_axis_x_->SetTitleText(tr("采样"));
    bitrate_axis_y_->SetTitleText(tr("Kbps"));

    // 帧率图
    fps_series_ = fps_chart_->AddLineSeries(QString(), QColor("#66bb6a"));
    fps_axis_x_ = fps_chart_->AxisX();
    fps_axis_y_ = fps_chart_->AxisY();
    fps_axis_x_->SetLabelFormat("%d");
    fps_axis_y_->SetLabelFormat("%.1f");
    fps_axis_x_->SetTitleText(tr("采样"));
    fps_axis_y_->SetTitleText(tr("fps"));

    // GOP 图
    gop_series_ = gop_chart_->AddLineSeries(QString(), QColor("#ffa726"));
    gop_axis_x_ = gop_chart_->AxisX();
    gop_axis_y_ = gop_chart_->AxisY();
    gop_axis_x_->SetLabelFormat("%d");
    gop_axis_y_->SetLabelFormat("%d");
    gop_axis_x_->SetTitleText(tr("GOP 序号"));
    gop_axis_y_->SetTitleText(tr("帧数"));

    connect(export_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportReport);
}

void AnalysisPanel::SetupFrameTab() {
    // detail_sub_tabs_container_ 是 bitstream_tab_ 底部区, 内部装 detail_sub_tabs_ (QTabWidget)
    // 4 个子页: 视频帧 / 包 / GOP 摘要 / 音频帧
    detail_sub_tabs_container_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(detail_sub_tabs_container_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    detail_sub_tabs_ = new QTabWidget(detail_sub_tabs_container_);
    detail_sub_tabs_->setDocumentMode(true);

    // ---- 子页 0: 视频帧 ----
    video_sub_ = new QWidget(detail_sub_tabs_);
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

    QCheckBox* v_toggle = new QCheckBox(tr("启用分析"), video_sub_);
    v_toggle->setChecked(feature_enabled_.value(AnalysisFeature::VideoFrame, true));
    connect(v_toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::VideoFrame] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::VideoFrame), checked);
    });
    v_toolbar->addWidget(v_toggle);
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

    detail_sub_tabs_->addTab(video_sub_, tr("视频帧"));

    // ---- 子页 1: 包 ----
    packet_sub_ = new QWidget(detail_sub_tabs_);
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

    QCheckBox* p_toggle = new QCheckBox(tr("启用分析"), packet_sub_);
    p_toggle->setChecked(feature_enabled_.value(AnalysisFeature::Packet, true));
    connect(p_toggle, &QCheckBox::toggled, this, [this](bool checked) {
        feature_enabled_[AnalysisFeature::Packet] = checked;
        emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Packet), checked);
    });
    p_toolbar->addWidget(p_toggle);
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

    detail_sub_tabs_->addTab(packet_sub_, tr("包"));

    // ---- 子页 2: GOP 摘要 ----
    gop_summary_sub_ = new QWidget(detail_sub_tabs_);
    QVBoxLayout* g_layout = new QVBoxLayout(gop_summary_sub_);
    g_layout->setContentsMargins(4, 4, 4, 4);
    g_layout->setSpacing(4);

    QHBoxLayout* g_toolbar = new QHBoxLayout();
    g_toolbar->addWidget(new QLabel(tr("按解码帧 pict_type 统计的 GOP 摘要"), gop_summary_sub_), 1);
    QPushButton* g_export_btn = new QPushButton(tr("导出 CSV"), gop_summary_sub_);
    g_toolbar->addWidget(g_export_btn);
    g_layout->addLayout(g_toolbar);

    QGroupBox* g_table_group = new QGroupBox(tr("GOP 分段统计"), gop_summary_sub_);
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

    detail_sub_tabs_->addTab(gop_summary_sub_, tr("GOP 摘要"));

    // ---- 子页 3: 音频帧 ----
    audio_frame_sub_ = new QWidget(detail_sub_tabs_);
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

    detail_sub_tabs_->addTab(audio_frame_sub_, tr("音频帧"));

    layout->addWidget(detail_sub_tabs_);

    connect(frame_filter_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        OnFrameFilterChanged();
    });
    connect(export_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportFrameCsv);
    connect(export_audio_frame_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportAudioFrameCsv);
    connect(g_export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportGopCsv);

    connect(packet_filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        packet_filter_mode_ = idx - 1;  // 0->全部(-1), 1->视频(0), 2->音频(1), 3->其他(2)
        RebuildPacketTable();
    });
    connect(export_packet_csv_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportPacketCsv);
    connect(packet_table_, &QTableWidget::itemSelectionChanged, this, &AnalysisPanel::OnPacketTableSelectionChanged);

    connect(frame_table_, &QTableWidget::itemSelectionChanged, this, &AnalysisPanel::OnVideoFrameTableSelectionChanged);
}

void AnalysisPanel::SetupPacketTab() {
    // 已合并到 SetupFrameTab 的 detail_sub_tabs_ 第 1 子页 (packet_sub_)
    // 保留函数占位以兼容历史调用方
}

void AnalysisPanel::SetupBitstreamTab() {
    // 合并流/帧/包三页为单一「码流分析」页.
    // 顶部: 流概览 5 指标 + 三条趋势曲线 (来自 stream_section_)
    // 底部: QTabWidget 4 子页 - 视频帧/包/GOP 摘要/音频帧 (来自 detail_sub_tabs_container_)
    bitstream_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(bitstream_tab_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 顶部流概览区 (固定高度不可滚动, 让曲线一直可见)
    layout->addWidget(stream_section_);

    // 分隔条
    QFrame* sep = new QFrame(bitstream_tab_);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    // 底部 4 子 Tab 区 (可伸展, 占剩余高度)
    layout->addWidget(detail_sub_tabs_container_, 1);

    AddPageWithScroll(bitstream_tab_, tr("码流分析"));
    bitstream_page_index_ = page_widgets_.size() - 1;
}




// stsz / stco / stsc / stss 的条目数与样本数量级，2 小时视频可达数十万条。
// 这里统一预分配行数 + 关闭刷新 + 截断显示，避免主线程被 O(n^2) 的 insertRow 拖死。
static void PopulateMp4BoxTablesInContainer(const model::Mp4BoxAnalysisResult& result,
                                       QTableWidget* stts_table, QTableWidget* stco_table,
                                       QTableWidget* stsc_table, QTableWidget* stsz_table,
                                       QTableWidget* co64_table, QTableWidget* stss_table) {
    constexpr int kMaxBoxTableRows = 2000;   // 每张表每个 track 最多显示的行数

    for (auto* table : {stts_table, stco_table, stsc_table, stsz_table, co64_table, stss_table}) {
        table->setUpdatesEnabled(false);
        table->setRowCount(0);
    }

    // 追加一个 track 段：标题行 + min(total, cap) 条 + 可选"已截断"提示行
    // kMaxBoxTableRows 必须显式捕获：std::min 走 const& 形参，属于 odr-use，
    // C++17 的"constexpr 变量免捕获"豁免不适用（MSVC 不检查，GCC/Clang 会报错）。
    auto append_section = [kMaxBoxTableRows](QTableWidget* table, const QString& header, int total,
                                             const std::function<void(int row, int index)>& fill_row) {
        if (total <= 0) return;
        const int shown = std::min(total, kMaxBoxTableRows);
        const bool truncated = total > shown;
        const int first = table->rowCount();
        table->setRowCount(first + 1 + shown + (truncated ? 1 : 0));

        auto* head = new QTableWidgetItem(header);
        QFont hf = head->font();
        hf.setBold(true);
        head->setFont(hf);
        table->setItem(first, 0, head);

        for (int i = 0; i < shown; ++i) {
            fill_row(first + 1 + i, i);
        }

        if (truncated) {
            auto* tail = new QTableWidgetItem(
                QObject::tr("… 仅显示前 %1 条（共 %2 条，完整数据请导出查看）").arg(shown).arg(total));
            QFont tf = tail->font();
            tf.setItalic(true);
            tail->setFont(tf);
            table->setItem(first + 1 + shown, 0, tail);
        }
    };
    auto cell = [](QTableWidget* table, int row, int col, const QString& text) {
        table->setItem(row, col, new QTableWidgetItem(text));
    };

    for (const auto& track : result.track_tables) {
        const QString header = QString("Track %1 (%2)").arg(track.track_id).arg(track.track_type);

        append_section(stts_table, header, track.stts_entries.size(), [&](int row, int i) {
            cell(stts_table, row, 0, QString::number(i));
            cell(stts_table, row, 1, QString::number(track.stts_entries[i].sample_count));
            cell(stts_table, row, 2, QString::number(track.stts_entries[i].sample_delta));
        });
        append_section(stco_table, header, track.stco_entries.size(), [&](int row, int i) {
            cell(stco_table, row, 0, QString::number(i));
            cell(stco_table, row, 1, QString::number(track.stco_entries[i].chunk_offset));
        });
        append_section(co64_table, header, track.co64_entries.size(), [&](int row, int i) {
            cell(co64_table, row, 0, QString::number(i));
            cell(co64_table, row, 1, QString::number(track.co64_entries[i].chunk_offset));
        });
        append_section(stsc_table, header, track.stsc_entries.size(), [&](int row, int i) {
            cell(stsc_table, row, 0, QString::number(i));
            cell(stsc_table, row, 1, QString::number(track.stsc_entries[i].first_chunk));
            cell(stsc_table, row, 2, QString::number(track.stsc_entries[i].samples_per_chunk));
            cell(stsc_table, row, 3, QString::number(track.stsc_entries[i].sample_description_index));
        });
        if (!track.stsz_entries.isEmpty()) {
            append_section(stsz_table, header, track.stsz_entries.size(), [&](int row, int i) {
                cell(stsz_table, row, 0, QString::number(i));
                cell(stsz_table, row, 1, QString::number(track.stsz_entries[i].sample_size));
            });
        } else if (track.stsz_default_size > 0) {
            append_section(stsz_table, header, 1, [&](int row, int) {
                cell(stsz_table, row, 0, "0");
                cell(stsz_table, row, 1, QString::number(track.stsz_default_size));
                cell(stsz_table, row, 2, QString("default x%1").arg(track.stsz_sample_count));
            });
        }
        append_section(stss_table, header, track.stss_entries.size(), [&](int row, int i) {
            cell(stss_table, row, 0, QString::number(i));
            cell(stss_table, row, 1, QString::number(track.stss_entries[i].sample_number));
        });
    }

    for (auto* table : {stts_table, stco_table, stsc_table, stsz_table, co64_table, stss_table}) {
        table->setUpdatesEnabled(true);
        table->resizeColumnsToContents();
    }
}











void AnalysisPanel::UpdateStreamStats(const model::StreamStats& stats) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::StreamStats, true)) return;
    current_stats_ = stats;
    pending_stream_stats_ = stats;
    has_pending_stream_stats_ = true;
}

void AnalysisPanel::ResetVideoFrameList() {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::VideoFrame, true)) return;
    frame_records_.clear();
    gop_summaries_.clear();
    ResetStreamCharts();
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
    if (event_timeline_view_) event_timeline_view_->ResetAnalysisEventList();
}


void AnalysisPanel::ResetSyncSampleList() {
    if (event_timeline_view_) event_timeline_view_->ResetSyncSampleList();
}


void AnalysisPanel::ResetTimelineEventList() {
    if (event_timeline_view_) event_timeline_view_->ResetTimelineEventList();
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
    if (event_timeline_view_) event_timeline_view_->AppendAnalysisEvent(event_info);
}


void AnalysisPanel::AppendSyncSample(const model::SyncSample& sample) {
    if (event_timeline_view_) event_timeline_view_->AppendSyncSample(sample);
}


void AnalysisPanel::AppendTimelineEvent(const model::TimelineEvent& event) {
    if (event_timeline_view_) event_timeline_view_->AppendTimelineEvent(event);
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

void AnalysisPanel::OnExportGopCsv() {
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

void AnalysisPanel::OnExportMp4Box() {
    // MP4 box 详情跟着文件结构页走（结构解析结果归 ContainerStructurePage）
    model::ContainerStructureResult container;
    if (container_page_) container = container_page_->result();
    if (!container.valid || !container.mp4_detail.valid ||
        container.mp4_detail.box_tree.isEmpty()) {
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
    printTree(container.mp4_detail.box_tree, 0);

    // 导出各 Track 的表格数据
    for (const auto& track : container.mp4_detail.track_tables) {
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

    // GOP 表更新后同步流分析页的 GOP 曲线
    UpdateGOPChart();
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

void AnalysisPanel::RefreshStreamStatsUi(const model::StreamStats& stats) {
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
    // 最大 GOP 统一取 UI 侧 gop_summaries_ 的口径 (解码帧 pict_type),
    // 与「帧分析」页的 GOP 表保持一致。
    int ui_max_gop = 0;
    for (const auto& g : gop_summaries_) {
        if (g.total_frames > ui_max_gop) ui_max_gop = g.total_frames;
    }
    SetTableItemText(stats_table_, 0, 4, QString::number(ui_max_gop));

    // 收集历史数据供图表与导出报告共用
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

    UpdateBitrateChart(stats);
    UpdateFPSChart(stats);
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

// 与 SetupFrameTab 表头一一对应 (6 列): #/帧类型/播放时间(s)/原始 PTS/GOP #/GOP 内
void AnalysisPanel::AppendFrameRowToTable(const VideoFrameRecord& record) {
    const int row = frame_table_->rowCount();
    frame_table_->insertRow(row);
    SetTableItemText(frame_table_, row, 0, QString::number(record.index));
    SetTableItemText(frame_table_, row, 1, FrameTypeToString(record.frame_type));
    SetTableItemText(frame_table_, row, 2, QString::number(record.timestamp_seconds, 'f', 3));
    SetTableItemText(frame_table_, row, 3, QString::number(record.pts));
    SetTableItemText(frame_table_, row, 4, QString::number(record.gop_index));
    SetTableItemText(frame_table_, row, 5, QString::number(record.gop_position));
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

// 与 SetupFrameTab 包表头一一对应 (7 列): #/流/播放时间(s)/显示时间(PTS)/解码时间(DTS)/时长/包大小
void AnalysisPanel::AppendPacketRowToTable(const PacketRecord& record) {
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

void AnalysisPanel::UpdateBitrateChart(const model::StreamStats& stats) {
    Q_UNUSED(stats);
    if (!bitrate_series_ || !bitrate_axis_x_ || !bitrate_axis_y_) {
        return;
    }

    bitrate_series_->Clear();
    qreal max_value = 0.0;
    {
        SeriesBatch batch(bitrate_series_);
        batch.Reserve(static_cast<int>(bitrate_chart_values_.size()));
        for (size_t i = 0; i < bitrate_chart_values_.size(); ++i) {
            const qreal v = bitrate_chart_values_[i];
            batch.Add(static_cast<qreal>(i), v);
            if (v > max_value) max_value = v;
        }
    }
    bitrate_axis_x_->SetRange(0, std::max<qreal>(1.0, bitrate_chart_values_.size()));
    // 上限留 10% 余量, 避免曲线贴顶; 全零时给一个最小量程防止坐标轴退化
    bitrate_axis_y_->SetRange(0, std::max<qreal>(100.0, max_value * 1.1));
}

void AnalysisPanel::UpdateFPSChart(const model::StreamStats& stats) {
    Q_UNUSED(stats);
    if (!fps_series_ || !fps_axis_x_ || !fps_axis_y_) {
        return;
    }

    fps_series_->Clear();
    qreal max_value = 0.0;
    {
        SeriesBatch batch(fps_series_);
        batch.Reserve(static_cast<int>(fps_chart_values_.size()));
        for (size_t i = 0; i < fps_chart_values_.size(); ++i) {
            const qreal v = fps_chart_values_[i];
            batch.Add(static_cast<qreal>(i), v);
            if (v > max_value) max_value = v;
        }
    }
    fps_axis_x_->SetRange(0, std::max<qreal>(1.0, fps_chart_values_.size()));
    fps_axis_y_->SetRange(0, std::max<qreal>(30.0, max_value * 1.1));
}

void AnalysisPanel::UpdateGOPChart() {
    if (!gop_series_ || !gop_axis_x_ || !gop_axis_y_) {
        return;
    }

    // 数据源统一为 UI 侧的 gop_summaries_ (由解码帧 pict_type 推导),
    // 不再使用 StreamAnalyzer 基于 packet flags 的独立 GOP 统计。
    gop_series_->Clear();
    if (gop_summaries_.empty()) {
        gop_axis_x_->SetRange(0, 1);
        gop_axis_y_->SetRange(0, 1);
        return;
    }

    int max_frames = 0;
    {
        SeriesBatch batch(gop_series_);
        batch.Reserve(static_cast<int>(gop_summaries_.size()));
        for (const auto& g : gop_summaries_) {
            batch.Add(static_cast<qreal>(g.gop_index), static_cast<qreal>(g.total_frames));
            if (g.total_frames > max_frames) max_frames = g.total_frames;
        }
    }
    gop_axis_x_->SetRange(0, std::max(1, static_cast<int>(gop_summaries_.size())));
    gop_axis_y_->SetRange(0, std::max(1, max_frames));
}

void AnalysisPanel::ResetStreamCharts() {
    bitrate_chart_values_.clear();
    fps_chart_values_.clear();
    if (bitrate_series_) bitrate_series_->Clear();
    if (fps_series_) fps_series_->Clear();
    if (gop_series_) gop_series_->Clear();
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
        success = reporting::StreamStatsExporter::ExportHTMLReport(
            fname, current_stats_, fps_history, bitrate_history, current_video_path_);
    } else if (ext == "json") {
        success = reporting::StreamStatsExporter::ExportJSON(
            fname, current_stats_, current_video_path_);
    } else if (ext == "txt") {
        success = reporting::StreamStatsExporter::ExportTextReport(
            fname, current_stats_, current_video_path_);
    } else {
        // 未知扩展名，默认生成 HTML
        filename += ".html";
        success = reporting::StreamStatsExporter::ExportHTMLReport(
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
// 画面质量 / 视觉缺陷标签页
// ===========================================================================

















// ===========================================================================
// 场景切换检测标签页
// ===========================================================================








// 包表选中 → 在 frame_records_ 中找 PTS 最接近的视频帧, 跳转并高亮
void AnalysisPanel::OnPacketTableSelectionChanged() {
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

    // 自动切到 detail_sub_tabs_ 的「视频帧」子页 (合并后只切内部 Tab 不切外部页)
    if (detail_sub_tabs_) {
        detail_sub_tabs_->setCurrentIndex(0);
    }
}

// 帧表选中 → 在 packet_records_ 中找 PTS 最接近的视频包, 跳转并高亮
void AnalysisPanel::OnVideoFrameTableSelectionChanged() {
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

    // 包表行号需要按 PacketMatchesFilter 过滤后映射; 若无过滤则是直接下标
    // 简化处理: 调用 RebuildPacketTable 后行号未必一一对应, 这里走 setCurrentCell 失败则返回
    // 为避免过滤导致错位, 通过遍历当前可见行做反向查找
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

    // 自动切到 detail_sub_tabs_ 的「包」子页 (合并后只切内部 Tab 不切外部页)
    if (detail_sub_tabs_) {
        detail_sub_tabs_->setCurrentIndex(1);
    }
}

// ===========================================================================
// 诊断与报告标签页（全文件扫描 + QC 规则引擎）
// ===========================================================================

// ============================================================
// 码率与 GOP 深度分析
// ============================================================
namespace {
constexpr int kMaxBitrateChartPoints = 4000;   // 折线图最多绘制的点数
constexpr int kMaxBitrateChartMarkers = 1500;  // 每类标记最多绘制的点数
constexpr int kMaxGopTableRows = 5000;
constexpr int kMaxAnomalyTableRows = 2000;

QString FormatMetricValue(double value, const QString& unit) {
    if (unit == QStringLiteral("B")) {
        if (value >= 1024.0 * 1024.0) return QString::number(value / 1048576.0, 'f', 2) + " MB";
        if (value >= 1024.0) return QString::number(value / 1024.0, 'f', 1) + " KB";
        return QString::number(value, 'f', 0) + " B";
    }
    if (unit == QStringLiteral("kbps")) return QString::number(value, 'f', 0) + " kbps";
    if (unit == QStringLiteral("帧") || unit == QStringLiteral("个")) {
        return QString::number(value, 'f', 0) + " " + unit;
    }
    if (unit == QStringLiteral("s")) return QString::number(value, 'f', 2) + " s";
    return QString::number(value, 'f', 3);
}

QString FormatKb(double bytes) { return QString::number(bytes / 1024.0, 'f', 1); }

// 抽稀（保留每组最大值，避免丢掉峰值）；批量提交，避免逐点刷新图表
void AppendDecimated(ChartSeries* series, const model::MetricSeries& curve, int limit) {
    const size_t n = curve.Size();
    if (n == 0) return;
    SeriesBatch batch(series);
    if (n <= static_cast<size_t>(limit)) {
        batch.Reserve(static_cast<int>(n));
        for (const auto& s : curve.samples) batch.Add(s.timestamp_seconds, s.value);
        return;
    }
    const size_t group = (n + limit - 1) / limit;
    batch.Reserve(limit);
    for (size_t i = 0; i < n; i += group) {
        const size_t end = std::min(i + group, n);
        double v = curve.samples[i].value;
        for (size_t j = i + 1; j < end; ++j) v = std::max(v, curve.samples[j].value);
        batch.Add(curve.samples[i].timestamp_seconds, v);
    }
}
}  // namespace



















// ===========================================================================
// 音频 QC 标签页（响度 / 真峰值 / 削波 / 静音 / 声道相位 / metadata 一致性）
// ===========================================================================
namespace {

// 参与「音频 QC → 规则结果」展示的规则 id（与 QcModels.cpp 的 audio.* 保持一致）
const char* const kAudioQcRuleIds[] = {
    "audio.loudness.target_high", "audio.loudness.target_low", "audio.loudness.range",
    "audio.true_peak",            "audio.clipping",            "audio.silence.longest",
    "audio.silence.ratio",        "audio.dc_offset",           "audio.phase_correlation",
    "audio.metadata.layout",      "audio.metadata.duration_mismatch",
};

QString AudioVerdictText(const model::QcReport& report, const QString& rule_id,
                         QString* detail_out = nullptr) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id != rule_id.toStdString()) continue;
        if (detail_out) *detail_out = QString::fromStdString(issue.detail);
        switch (issue.severity) {
            case model::IssueSeverity::Critical:
            case model::IssueSeverity::Error:    return QStringLiteral("失败");
            case model::IssueSeverity::Warning:  return QStringLiteral("警告");
            default:                             return QStringLiteral("提示");
        }
    }
    return QStringLiteral("通过");
}

QString AudioFormatDb(double value, double silence_floor) {
    if (value <= silence_floor + 1.0) return QStringLiteral("-∞");
    return QString::number(value, 'f', 2);
}

// 抽稀（保留每组极值，避免丢掉峰值）
void AppendAudioPoints(ChartSeries* series, const std::vector<model::LoudnessPoint>& points,
                       int limit, double (model::LoudnessPoint::*member), double floor_value) {
    if (points.empty()) return;
    const double fallback = floor_value;
    auto get = [member, fallback](const model::LoudnessPoint& p) {
        const double v = p.*member;
        return (v <= fallback + 1.0) ? fallback : v;
    };
    const size_t n = points.size();
    const size_t step = (n <= static_cast<size_t>(limit)) ? 1 : (n + limit - 1) / limit;
    const size_t out_n = (n + step - 1) / step;
    SeriesBatch batch(series);
    batch.Reserve(static_cast<int>(out_n));
    for (size_t i = 0; i < n; i += step) {
        batch.Add(points[i].timestamp_seconds, get(points[i]));
    }
}

}  // namespace










// ==========================================================================
// 色彩与 HDR 分析页
// 与「码率与 GOP」「音频 QC」「诊断与报告」共用同一次全文件扫描结果
// ==========================================================================

void AnalysisPanel::SetupStreamingPackageTab() {
    // 页面本体就是 StreamingPanel：manifest 结构树 + 码率阶梯表 +
    // 分片时间轴对齐表 + 问题表都封装在里面。
    streaming_panel_ = new StreamingPanel(this);
    streaming_panel_->setMinimumHeight(560);

    connect(streaming_panel_, &StreamingPanel::RefreshRequested,
            this, &AnalysisPanel::OnStreamingRefreshRequested);

    AddPageWithScroll(streaming_panel_, tr("流媒体包"));
}

void AnalysisPanel::OnStreamingRefreshRequested() {
    // 清单解析是纯本地文件读取（第一阶段不联网），跟着全文件扫描一起跑。
    // 扫描总控在诊断页，这里只改一个开关再交给它跑
    diagnostics_page_->options().analyze_streaming_package = true;
    diagnostics_page_->StartScan(diagnostics_page_->options());
}

void AnalysisPanel::UpdateStreamingUi() {
    if (!streaming_panel_) return;
    // 优先用容器结构分析的结果（打开清单文件时由 ContainerStructureAnalyzer 直接产出）；
    // 没有的话再退到全文件扫描的 streaming_package。
    if (container_page_ && container_page_->result().streaming_package.valid) {
        streaming_panel_->SetResult(container_page_->result().streaming_package);
        return;
    }
    if (diagnostics_page_ && diagnostics_page_->hasResult() &&
        diagnostics_page_->result().streaming_analyzed) {
        streaming_panel_->SetResult(diagnostics_page_->result().streaming_package);
        return;
    }
    streaming_panel_->Clear();
}

void AnalysisPanel::SetupParameterSetTab() {
    // 页面本体就是 BitstreamPanel：结构树 + 容器/码流对比 + 不一致表都封装在里面。
    // 外面套的是 AddPageWithScroll 的 QScrollArea，给个最小高度免得被压扁。
    bitstream_params_panel_ = new BitstreamPanel(this);
    bitstream_params_panel_->setMinimumHeight(560);

    connect(bitstream_params_panel_, &BitstreamPanel::RefreshRequested,
            this, &AnalysisPanel::OnBitstreamRefreshRequested);

    AddPageWithScroll(bitstream_params_panel_, tr("参数集解析"));
}

void AnalysisPanel::OnBitstreamRefreshRequested() {
    // 码流解析只读 extradata（KB 级），成本可忽略，跟着全文件扫描一起跑。
    diagnostics_page_->options().analyze_bitstream = true;
    diagnostics_page_->StartScan(diagnostics_page_->options());
}

void AnalysisPanel::UpdateBitstreamUi() {
    if (!bitstream_params_panel_) return;
    if (!diagnostics_page_ || !diagnostics_page_->hasResult() ||
        !diagnostics_page_->result().bitstream_analyzed) {
        bitstream_params_panel_->Clear();
        return;
    }
    bitstream_params_panel_->SetResult(diagnostics_page_->result().bitstream_analysis);
}





// ============================================================
// 字幕 / 时码 / 辅助数据页（功能 9）
//
// 数据全部来自「诊断与报告」那一次全文件扫描（AnalysisResult 的 subtitle /
// timecode / aux_data），这一页不再单独发起 demux —— 字幕包本来就在同一次
// demux 里过了一遍，重复扫一遍纯属浪费 I/O。
// ============================================================

namespace {

// 统一建表: 列宽策略 / 选择行为 / 表头字号在本面板各页保持一致
QTableWidget* MakeAuxTable(const QStringList& headers, QWidget* parent) {
    QTableWidget* table = new QTableWidget(parent);
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

// CSV 字段转义（与导出报告其它页保持一致：含分隔符/引号/换行才加引号）
QString AuxCsvField(const QString& text) {
    if (text.contains(',') || text.contains('"') || text.contains('\n')) {
        QString out = text;
        out.replace('"', QStringLiteral("\"\""));
        return QLatin1Char('"') + out + QLatin1Char('"');
    }
    return text;
}

QString AuxSecondsText(double seconds) {
    if (seconds < 0.0) return QStringLiteral("-");
    return QString::number(seconds, 'f', 3);
}

}  // namespace

// 播放器打开失败进入「分析模式」时的自动扫描入口：用诊断页的默认选项跑一次。
// 静默失败路径 —— 不弹"请先打开文件"提示框，原因写进诊断页自己的汇总标签。
void AnalysisPanel::StartDiagnosticsScanForCurrentFile() {
    if (!diagnostics_page_ || current_video_path_.empty()) return;
    diagnostics_page_->StartScan(diagnostics_page_->options());
}

// ---------------------------------------------------------------------------
// 播放期时间轴统计：数据归诊断页（TimelineAnalyzer 也在那里），面板只转发
// ---------------------------------------------------------------------------

void AnalysisPanel::OnTimelinePacket(const model::PacketTiming& timing) {
    if (diagnostics_page_) diagnostics_page_->OnPacketTiming(timing);
}

void AnalysisPanel::OnFrameTiming(const model::FrameTimingInfo& timing) {
    if (diagnostics_page_) diagnostics_page_->OnFrameTiming(timing);
}

// ===========================================================================
// 已拆成独立页面组件的部分
//
// 这一节只做三件事：建页 + 把页面发出的意图接到面板（面板才知道当前文件路径 /
// 扫描编排 / 全局 feature 表），把播放器回来的数据喂给页面，以及全文件扫描结束
// 时把结果分发给共用同一次扫描的几页。页面本身不持有 facade、不认识面板成员。
// ===========================================================================

void AnalysisPanel::SetupUI() {
    // 不再创建内部 QTabWidget，页面由 AddPageWithScroll 收集
    // PopulateStackedWidget 时添加到外部 QStackedWidget

    SetupStreamTab();
    SetupFrameTab();
    SetupPacketTab();
    SetupBitstreamTab();
    SetupEventTimelineView();
    SetupContainerStructurePage();
    SetupMacroblockTab();
    SetupSceneChangePage();
    SetupVisualDefectPage();
    SetupBitrateGopPage();
    SetupAudioQcPage();
    SetupColorHdrPage();
    SetupParameterSetTab();
    SetupStreamingPackageTab();
    SetupSubtitleAuxPage();
    SetupDiagnosticsPage();
    SetupReportingPanelTab();

    qRegisterMetaType<model::SceneChangeResult>();
    qRegisterMetaType<analyzer::AnalysisResult>();
    qRegisterMetaType<model::FrameQualityMetric>();
    qRegisterMetaType<model::VisualDefect>();
    qRegisterMetaType<model::VisualDefectOptions>();

    // 事件与时间轴页要把同步样本喂给诊断页构建音视频偏移曲线，诊断页在后面才建好，
    // 所以等所有页都建完再注入，避免建页顺序耦合。
    if (event_timeline_view_ && diagnostics_page_) {
        event_timeline_view_->SetDiagnosticsPage(diagnostics_page_);
    }
}

void AnalysisPanel::SetupEventTimelineView() {
    event_timeline_view_ = new EventTimelineView(this);

    // 视图内部用 0/1/2 表示 Event/Sync/Timeline，这里映射到面板的 AnalysisFeature 并回写
    // feature_enabled_，同时转发 AnalysisFeatureToggled，保证 MainWindow 的连接行为不变。
    event_timeline_view_->SetFeatureHooks(
        [this](int view_feature) {
            switch (view_feature) {
                case 0: return feature_enabled_.value(AnalysisFeature::Event, true);
                case 1: return feature_enabled_.value(AnalysisFeature::SyncSample, true);
                case 2: return feature_enabled_.value(AnalysisFeature::Timeline, true);
                default: return true;
            }
        },
        [this](int view_feature, bool enabled) {
            AnalysisFeature feat = AnalysisFeature::Event;
            switch (view_feature) {
                case 0: feat = AnalysisFeature::Event; break;
                case 1: feat = AnalysisFeature::SyncSample; break;
                case 2: feat = AnalysisFeature::Timeline; break;
                default: return;
            }
            feature_enabled_[feat] = enabled;
            emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
        });

    AddPageWithScroll(event_timeline_view_, tr("事件与时间轴"));
}

void AnalysisPanel::SetupContainerStructurePage() {
    container_page_ = new ContainerStructurePage(
        feature_enabled_.value(AnalysisFeature::ContainerStructure, true), this);
    connect(container_page_, &ContainerStructurePage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::ContainerStructure] = enabled;
                emit AnalysisFeatureToggled(
                    static_cast<int>(AnalysisFeature::ContainerStructure), enabled);
            });
    AddPageWithScroll(container_page_, tr("文件结构"));
}

void AnalysisPanel::SetupSceneChangePage() {
    scene_change_page_ = new SceneChangePage(
        feature_enabled_.value(AnalysisFeature::SceneChange, false), this);
    connect(scene_change_page_, &SceneChangePage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::SceneChange] = enabled;
                emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::SceneChange), enabled);
            });

    AddPageWithScroll(scene_change_page_, tr("场景切换"));
}

void AnalysisPanel::SetupVisualDefectPage() {
    visual_defect_page_ = new VisualDefectPage(
        feature_enabled_.value(AnalysisFeature::VisualDefect, false), visual_defect_options_, this);
    connect(visual_defect_page_, &VisualDefectPage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::VisualDefect] = enabled;
                emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::VisualDefect), enabled);
            });
    connect(visual_defect_page_, &VisualDefectPage::OptionsChanged,
            this, &AnalysisPanel::VisualDefectOptionsChanged);
    connect(visual_defect_page_, &VisualDefectPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    AddPageWithScroll(visual_defect_page_, tr("画面质量"));
}

void AnalysisPanel::SetupBitrateGopPage() {
    bitrate_gop_page_ = new BitrateGopPage(this);
    bitrate_gop_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    // 扫描请求统一由诊断页编排（页面才持有 facade 与进度条总控）
    connect(bitrate_gop_page_, &BitrateGopPage::ScanRequested, this, [this]() {
        analyzer::AnalysisOptions options = diagnostics_page_->options();
        bitrate_gop_page_->FillScanOptions(options);
        diagnostics_page_->StartScan(options);
    });
    connect(bitrate_gop_page_, &BitrateGopPage::CancelRequested,
            this, [this]() { if (diagnostics_page_) diagnostics_page_->CancelScan(); });
    connect(bitrate_gop_page_, &BitrateGopPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    connect(bitrate_gop_page_, &BitrateGopPage::SceneLinkRequested,
            this, &AnalysisPanel::OnSceneLinkRequested);
    AddPageWithScroll(bitrate_gop_page_, tr("码率与 GOP"));
}

void AnalysisPanel::SetupAudioQcPage() {
    audio_qc_page_ = new AudioQcPage(this);
    audio_qc_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    connect(audio_qc_page_, &AudioQcPage::ScanRequested, this, [this]() {
        analyzer::AnalysisOptions options = diagnostics_page_->options();
        audio_qc_page_->FillScanOptions(options);
        diagnostics_page_->StartScan(options);
    });
    connect(audio_qc_page_, &AudioQcPage::CancelRequested, this, [this]() { if (diagnostics_page_) diagnostics_page_->CancelScan(); });
    connect(audio_qc_page_, &AudioQcPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    AddPageWithScroll(audio_qc_page_, tr("音频 QC"));
}

void AnalysisPanel::SetupColorHdrPage() {
    color_hdr_page_ = new ColorHdrPage(this);
    color_hdr_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    connect(color_hdr_page_, &ColorHdrPage::ScanRequested, this, [this]() {
        analyzer::AnalysisOptions options = diagnostics_page_->options();
        color_hdr_page_->FillScanOptions(options);
        diagnostics_page_->StartScan(options);
    });
    connect(color_hdr_page_, &ColorHdrPage::CancelRequested, this, [this]() { if (diagnostics_page_) diagnostics_page_->CancelScan(); });
    AddPageWithScroll(color_hdr_page_, tr("色彩与 HDR"));
}

void AnalysisPanel::SetupSubtitleAuxPage() {
    subtitle_aux_page_ = new SubtitleAuxPage(this);
    connect(subtitle_aux_page_, &SubtitleAuxPage::ScanRequested, this, [this]() {
        // 字幕页自己不发 demux：字幕包本来就在同一次扫描里过了一遍
        analyzer::AnalysisOptions options = diagnostics_page_->options();
        diagnostics_page_->StartScan(options);
    });
    connect(subtitle_aux_page_, &SubtitleAuxPage::StartTimecodeReady,
            this, &AnalysisPanel::StartTimecodeReady);
    connect(subtitle_aux_page_, &SubtitleAuxPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    AddPageWithScroll(subtitle_aux_page_, tr("字幕 / 辅助数据"));
}

// ---------------------------------------------------------------------------
// 播放器回调：只转发给页面，面板不再自己攒这些状态
// ---------------------------------------------------------------------------

void AnalysisPanel::OnContainerStructureReady(const model::ContainerStructureResult& result) {
    VE_PERF("OnContainerStructureReady 总计");
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    if (container_page_) container_page_->SetResult(result);
    // 流媒体清单：顺手把「流媒体包」页也喂上（同一份结果，只是换了个视图）
    if (streaming_panel_) UpdateStreamingUi();
}

void AnalysisPanel::OnSceneChangeDetected(const model::SceneChangeResult& result) {
    if (scene_change_page_) scene_change_page_->AppendResult(result);
}

void AnalysisPanel::OnVisualDefectFrame(const model::FrameQualityMetric& metric) {
    if (visual_defect_page_) visual_defect_page_->AppendFrame(metric);
}

void AnalysisPanel::OnVisualDefectDetected(const model::VisualDefect& defect) {
    if (visual_defect_page_) visual_defect_page_->AppendDefect(defect);
}

void AnalysisPanel::OnVisualDefectReset() {
    if (visual_defect_page_) visual_defect_page_->ResetAll();
}

void AnalysisPanel::OnVisualDefectStats(int analyzed_frames, int dropped_frames,
                                        const model::ActivePictureArea& effective_area) {
    if (visual_defect_page_) {
        visual_defect_page_->ApplyStats(analyzed_frames, dropped_frames, effective_area);
    }
}

void AnalysisPanel::OnVisualDefectOptionChanged() {
    if (visual_defect_page_) emit VisualDefectOptionsChanged(visual_defect_page_->options());
}

// 「关联场景切换」：判定表归码率页，报告重算留在诊断页（那里才有 facade 与问题表），
// 面板这一层只做转发 + 让码率页弹出自己的汇总。
void AnalysisPanel::OnSceneLinkRequested(const std::vector<model::SceneChangeResult>& records,
                                         const analyzer::BitrateGopOptions& options) {
    if (diagnostics_page_) {
        diagnostics_page_->ApplySceneLink(records, options);
    }
    if (bitrate_gop_page_) bitrate_gop_page_->ShowSceneLinkSummary();
}

// ---------------------------------------------------------------------------
// 批量刷新定时器：拆出去的页面同样只置脏，重绘等这一拍
// ---------------------------------------------------------------------------

void AnalysisPanel::FlushPendingUiUpdates() {
    // 面板不可见时跳过UI刷新以节省CPU
    if (!isVisible()) return;

    // 批量刷新跑在主线程: 单次超过 50ms 就会被用户感知为卡顿, 打点记录便于定位
    const auto flush_begin = std::chrono::steady_clock::now();

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
    if (macroblock_dirty_) {
        RefreshMacroblockUi();
        macroblock_dirty_ = false;
    }
    if (scene_change_page_ && scene_change_page_->HasPending()) {
        scene_change_page_->FlushPending();
    }
    if (visual_defect_page_ && visual_defect_page_->HasPending()) {
        visual_defect_page_->FlushPending();
    }
    if (event_timeline_view_) event_timeline_view_->FlushPendingUiUpdates();

    const double flush_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - flush_begin).count();
    if (flush_ms >= 50.0) {
        LOG_WARN("[perf] 分析面板批量 UI 刷新耗时 " +
                 std::to_string(static_cast<long long>(flush_ms)) + " ms");
    }
}

// ---------------------------------------------------------------------------
// 诊断与报告：一次 demux 供诊断页 / 码率 GOP / 音频 QC / 色彩 HDR / 字幕辅助
// 五页共用，所以扫描生命周期由诊断页自己编排，面板只做两件跨页的事 ——
// 把进度同步给共用扫描的其它页、把扫描结果分发给它们。
// ---------------------------------------------------------------------------

void AnalysisPanel::SetupDiagnosticsPage() {
    diagnostics_page_ = new DiagnosticsPage(this);
    diagnostics_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    // 字幕的「过短 / 过长 / 阅读速度」阈值以「规则与阈值」那张可编辑的表为准。
    // 装成扫描前的钩子，任何入口发起的扫描（本页按钮 / 其它几页 / 流媒体包 /
    // 参数集）都会先同步一次，免得选项与规则两处阈值各说各话。
    diagnostics_page_->SetBeforeScanHook(
        [this](analyzer::AnalysisOptions& options) { SyncSubtitleThresholds(options); });

    // 扫描生命周期 -> 面板同步其它几页的按钮 / 进度条
    connect(diagnostics_page_, &DiagnosticsPage::ScanStarted,
            this, &AnalysisPanel::OnScanStarted);
    connect(diagnostics_page_, &DiagnosticsPage::ProgressChanged,
            this, &AnalysisPanel::OnDiagnosticsProgress);
    connect(diagnostics_page_, &DiagnosticsPage::ScanFinished,
            this, &AnalysisPanel::OnDiagnosticsFinished);
    connect(diagnostics_page_, &DiagnosticsPage::ScanCancelled,
            this, &AnalysisPanel::OnScanCancelled);
    // 报告重算（改规则 / 关联场景切换）后，音频 QC 与色彩 HDR 的判定表要跟着换
    connect(diagnostics_page_, &DiagnosticsPage::QcReportChanged,
            this, [this](const model::QcReport& report) {
                if (audio_qc_page_) audio_qc_page_->SetQcReport(report);
                if (color_hdr_page_) color_hdr_page_->SetQcReport(report);
            });
    connect(diagnostics_page_, &DiagnosticsPage::SeekRequested, this, &AnalysisPanel::SeekRequested);

    AddPageWithScroll(diagnostics_page_, tr("诊断与报告"));
}

void AnalysisPanel::SyncSubtitleThresholds(analyzer::AnalysisOptions& options) {
    if (!subtitle_aux_page_ || !diagnostics_page_) return;
    subtitle_aux_page_->ApplyRuleThresholds(diagnostics_page_->rules(), options.subtitle_options);
}

void AnalysisPanel::SetupReportingPanelTab() {
    // 报告与批量 QC（功能 12）：模板选择 + 单文件报告 + 目录批量扫描 + 导出。
    // 该页自带后台线程，状态冒泡给主窗口状态栏。
    reporting_panel_ = new ui::ReportingPanel(this);
    connect(reporting_panel_, &ui::ReportingPanel::StatusMessage,
            this, &AnalysisPanel::StatusMessage);
    AddPageWithScroll(reporting_panel_, tr("报告与批量 QC"));
}

void AnalysisPanel::OnScanStarted() {
    // 与诊断页共用同一次扫描的三页：先把它们的开始/取消状态对齐
    if (bitrate_gop_page_) bitrate_gop_page_->SetScanActive(true);
    if (audio_qc_page_) audio_qc_page_->SetScanActive(true);
    if (color_hdr_page_) color_hdr_page_->SetScanActive(true);
}

void AnalysisPanel::OnScanCancelled() {
    if (bitrate_gop_page_) bitrate_gop_page_->SetScanActive(false);
    if (audio_qc_page_) audio_qc_page_->SetScanActive(false);
    if (color_hdr_page_) color_hdr_page_->SetScanActive(false);
}

void AnalysisPanel::OnDiagnosticsProgress(double percent, const QString& stage) {
    if (bitrate_gop_page_) {
        bitrate_gop_page_->SetProgress(static_cast<int>(percent));
        bitrate_gop_page_->SetProgressFormat(stage + " %p%");
    }
    if (audio_qc_page_) {
        audio_qc_page_->SetProgress(static_cast<int>(percent));
        audio_qc_page_->SetProgressFormat(stage + " %p%");
    }
    if (color_hdr_page_) {
        color_hdr_page_->SetProgress(static_cast<int>(percent));
        color_hdr_page_->SetProgressFormat(stage + " %p%");
    }
}

void AnalysisPanel::OnDiagnosticsFinished(bool completed) {
    if (!diagnostics_page_) return;
    VE_PERF("AnalysisPanel 分发扫描结果");

    // 「关联场景切换」要用场景切换页的记录，先喂给码率页
    if (bitrate_gop_page_) {
        bitrate_gop_page_->SetScanActive(false);
        bitrate_gop_page_->SetProgress(100);
        bitrate_gop_page_->SetProgressFormat(completed ? tr("分析完成") : tr("已取消（结果不完整）"));
        bitrate_gop_page_->SetSceneChanges(scene_change_page_ ? scene_change_page_->records()
                                                              : std::vector<model::SceneChangeResult>());
        bitrate_gop_page_->SetResult(diagnostics_page_->result());
    }
    // 音频 QC / 色彩 HDR 的异常表来自 QC 规则引擎，所以连报告一起给
    if (audio_qc_page_) {
        audio_qc_page_->SetScanActive(false);
        audio_qc_page_->SetResult(diagnostics_page_->result(), diagnostics_page_->qcReport());
    }
    if (color_hdr_page_) {
        color_hdr_page_->SetScanActive(false);
        color_hdr_page_->SetResult(diagnostics_page_->result(), diagnostics_page_->qcReport());
    }
    {
        VE_PERF("UpdateBitstreamUi");
        UpdateBitstreamUi();  // 参数集页（SPS/PPS/Sequence Header）同样共用同一次扫描结果
    }
    {
        VE_PERF("UpdateStreamingUi");
        UpdateStreamingUi();  // 流媒体包页：清单文件的 QC 结果来自同一次扫描
    }
    if (subtitle_aux_page_) {
        VE_PERF("SubtitleAuxPage::SetResult");
        subtitle_aux_page_->SetResult(diagnostics_page_->result());
    }

    // MP4 样本表：扫描跑过就顺带刷新容器页（与打开文件时那次解析结果一致）
    const analyzer::AnalysisResult& result = diagnostics_page_->result();
    if (result.mp4_samples_analyzed && result.mp4_samples.valid) {
        VE_PERF("诊断后刷新 MP4 样本表");
        if (container_page_) container_page_->ApplySampleTable(result.mp4_samples);
    }
}

} // namespace ui
} // namespace videoeye