#include "AnalysisPanel.h"
#include "utils/Logger.h"
#include "utils/ScopedTimer.h"
#include "utils/ReportExporter.h"
#include "ui/theme/AppTheme.h"
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
#include <QtCharts>
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

// 图表数据批量提交。
//
// QXYSeries::append() 每加一个点都会触发一次图表重算/重绘，几千点时主线程会被
// 拖到秒级卡顿（实测 7 条曲线 × 4000 点 ≈ 3.5 s）。先在内存里攒好，析构时
// 用 replace() 一次性提交（只触发一次刷新）。
class SeriesBatch {
public:
    explicit SeriesBatch(QXYSeries* series) : series_(series) {}
    ~SeriesBatch() {
        if (series_) series_->replace(points_);
    }

    void Reserve(int n) { points_.reserve(n); }
    void Add(double x, double y) { points_.append(QPointF(x, y)); }
    bool Empty() const { return points_.isEmpty(); }

private:
    QXYSeries* series_ = nullptr;
    QVector<QPointF> points_;
};

// 大表批量填充: 先关掉刷新与重绘, 一次性预分配行数, 比逐行 insertRow 快一个量级
// (insertRow 每次都要移动后续行, 1 万行时是 O(n^2))。
class TableBatch {
public:
    explicit TableBatch(QTableWidget* table) : table_(table) {
        if (!table_) return;
        was_updates_enabled_ = table_->updatesEnabled();
        table_->setUpdatesEnabled(false);
    }
    ~TableBatch() {
        if (!table_) return;
        table_->setUpdatesEnabled(was_updates_enabled_);
    }

    void SetRowCount(int rows) { if (table_) table_->setRowCount(rows); }
    int RowCount() const { return table_ ? table_->rowCount() : 0; }
    void SetText(int row, int column, const QString& text, bool bold = false) {
        if (!table_) return;
        auto* item = new QTableWidgetItem(text);
        if (bold) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        table_->setItem(row, column, item);
    }
    QTableWidgetItem* Item(int row, int column) const {
        return table_ ? table_->item(row, column) : nullptr;
    }

private:
    QTableWidget* table_ = nullptr;
    bool was_updates_enabled_ = true;
};
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
    feature_enabled_[AnalysisFeature::ContainerStructure] = true;
    feature_enabled_[AnalysisFeature::Macroblock] = false;
    feature_enabled_[AnalysisFeature::SceneChange] = false;
    feature_enabled_[AnalysisFeature::Diagnostics] = true;

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
    // 不再创建内部 QTabWidget，页面由 AddPageWithScroll 收集
    // PopulateStackedWidget 时添加到外部 QStackedWidget

    SetupStreamTab();
    SetupFrameTab();
    SetupPacketTab();
    SetupBitstreamTab();
    SetupEventAnalysisTab();
    SetupContainerStructureTab();
    SetupMacroblockTab();
    SetupSceneChangeTab();
    SetupBitrateGopTab();
    SetupAudioQcTab();
    SetupColorHdrTab();
    SetupDiagnosticsTab();

    qRegisterMetaType<analyzer::SceneChangeResult>();
    qRegisterMetaType<analyzer::AnalysisResult>();
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

    auto make_chart_box = [&](const QString& title) -> QPair<QChartView*, QGroupBox*> {
        QChartView* view = new QChartView(chart_row);
        view->setMinimumHeight(180);
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
    bitrate_series_ = new QLineSeries(this);
    bitrate_chart_object_ = new QChart();
    bitrate_chart_object_->setTitle(tr("码率 (Kbps)"));
    bitrate_chart_object_->legend()->hide();
    bitrate_chart_object_->addSeries(bitrate_series_);
    bitrate_axis_x_ = new QValueAxis(this);
    bitrate_axis_y_ = new QValueAxis(this);
    bitrate_axis_x_->setLabelFormat("%d");
    bitrate_axis_y_->setLabelFormat("%.0f");
    bitrate_axis_x_->setTitleText(tr("采样"));
    bitrate_chart_object_->addAxis(bitrate_axis_x_, Qt::AlignBottom);
    bitrate_chart_object_->addAxis(bitrate_axis_y_, Qt::AlignLeft);
    bitrate_series_->attachAxis(bitrate_axis_x_);
    bitrate_series_->attachAxis(bitrate_axis_y_);
    bitrate_chart_->setChart(bitrate_chart_object_);
    bitrate_chart_->setRenderHint(QPainter::Antialiasing);

    // 帧率图
    fps_series_ = new QLineSeries(this);
    fps_chart_object_ = new QChart();
    fps_chart_object_->setTitle(tr("帧率 (fps)"));
    fps_chart_object_->legend()->hide();
    fps_chart_object_->addSeries(fps_series_);
    fps_axis_x_ = new QValueAxis(this);
    fps_axis_y_ = new QValueAxis(this);
    fps_axis_x_->setLabelFormat("%d");
    fps_axis_y_->setLabelFormat("%.1f");
    fps_axis_x_->setTitleText(tr("采样"));
    fps_chart_object_->addAxis(fps_axis_x_, Qt::AlignBottom);
    fps_chart_object_->addAxis(fps_axis_y_, Qt::AlignLeft);
    fps_series_->attachAxis(fps_axis_x_);
    fps_series_->attachAxis(fps_axis_y_);
    fps_chart_->setChart(fps_chart_object_);
    fps_chart_->setRenderHint(QPainter::Antialiasing);

    // GOP 图
    gop_series_ = new QLineSeries(this);
    gop_chart_object_ = new QChart();
    gop_chart_object_->setTitle(tr("每个 GOP 的帧数"));
    gop_chart_object_->legend()->hide();
    gop_chart_object_->addSeries(gop_series_);
    gop_axis_x_ = new QValueAxis(this);
    gop_axis_y_ = new QValueAxis(this);
    gop_axis_x_->setLabelFormat("%d");
    gop_axis_y_->setLabelFormat("%d");
    gop_axis_x_->setTitleText(tr("GOP 序号"));
    gop_chart_object_->addAxis(gop_axis_x_, Qt::AlignBottom);
    gop_chart_object_->addAxis(gop_axis_y_, Qt::AlignLeft);
    gop_series_->attachAxis(gop_axis_x_);
    gop_series_->attachAxis(gop_axis_y_);
    gop_chart_->setChart(gop_chart_object_);
    gop_chart_->setRenderHint(QPainter::Antialiasing);

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

    // Sample Table 子页: 样本级展开 + 一致性问题 + 分片列表
    mp4_sample_sub_ = new QWidget();
    SetupMp4SampleTableSubPage(mp4_sample_sub_);
    mp4_detail_tabs_->addTab(mp4_sample_sub_, tr("Sample Table"));

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
    // 结构树点击 stts/ctts/stss/stco/stsz/elst/moof 等样本表相关 box 时联动到 Sample Table
    connect(container_tree_, &QTreeWidget::itemSelectionChanged,
            this, &AnalysisPanel::OnContainerTreeSelectionChanged);
}

void AnalysisPanel::SetupMp4SampleTableSubPage(QWidget* parent) {
    QVBoxLayout* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    // 顶部: 轨道选择 + 联动提示
    QHBoxLayout* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(new QLabel(tr("轨道:"), parent));
    mp4_sample_track_combo_ = new QComboBox(parent);
    mp4_sample_track_combo_->setMinimumWidth(240);
    mp4_sample_track_combo_->setToolTip(tr("选择要展开样本的轨道（视频/音频/字幕等）"));
    top->addWidget(mp4_sample_track_combo_);
    top->addStretch();
    mp4_sample_focus_label_ = new QLabel(tr("点击左侧结构树的 stts/ctts/stss/stco/stsz 可联动本表"), parent);
    mp4_sample_focus_label_->setStyleSheet("font-size: 11px; color: #8B949E;");
    top->addWidget(mp4_sample_focus_label_);
    export_mp4_sample_button_ = new QPushButton(tr("导出样本 CSV"), parent);
    export_mp4_sample_button_->setToolTip(tr("把当前轨道的样本表导出为 CSV（含偏移/DTS/PTS/大小/关键帧）"));
    top->addWidget(export_mp4_sample_button_);
    layout->addLayout(top);

    mp4_sample_summary_label_ = new QLabel(tr("样本表：未分析"), parent);
    mp4_sample_summary_label_->setStyleSheet("font-size: 11px; color: #8B949E; padding: 2px;");
    mp4_sample_summary_label_->setWordWrap(true);
    layout->addWidget(mp4_sample_summary_label_);

    mp4_sample_table_ = new QTableWidget(0, 9, parent);
    mp4_sample_table_->setHorizontalHeaderLabels({
        tr("#"), tr("偏移"), tr("DTS(s)"), tr("PTS(s)"), tr("ΔCTS(ms)"),
        tr("时长(ms)"), tr("大小(B)"), tr("Chunk"), tr("关键帧")});
    mp4_sample_table_->horizontalHeader()->setStretchLastSection(true);
    mp4_sample_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mp4_sample_table_->setAlternatingRowColors(true);
    mp4_sample_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mp4_sample_table_->setStyleSheet(
        "QTableWidget { background-color: #0D1117; alternate-background-color: #12181F; color: #F0F6FC; }"
        "QHeaderView::section { background-color: #161B22; color: #8B949E; padding: 3px; border: none; }");

    // 底部: 一致性问题 + 分片列表
    QTabWidget* bottom = new QTabWidget(parent);
    mp4_issue_table_ = new QTableWidget(0, 5, bottom);
    mp4_issue_table_->setHorizontalHeaderLabels({tr("级别"), tr("位置"), tr("问题"), tr("说明"), tr("建议")});
    mp4_issue_table_->horizontalHeader()->setStretchLastSection(true);
    mp4_issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mp4_issue_table_->setAlternatingRowColors(true);
    mp4_issue_table_->verticalHeader()->setVisible(false);
    bottom->addTab(mp4_issue_table_, tr("一致性问题"));

    mp4_fragment_table_ = new QTableWidget(0, 8, bottom);
    mp4_fragment_table_->setHorizontalHeaderLabels({
        tr("#"), tr("序号"), tr("Track"), tr("偏移"), tr("tfdt"),
        tr("时长"), tr("样本数"), tr("字节数")});
    mp4_fragment_table_->horizontalHeader()->setStretchLastSection(true);
    mp4_fragment_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mp4_fragment_table_->setAlternatingRowColors(true);
    mp4_fragment_table_->verticalHeader()->setVisible(false);
    bottom->addTab(mp4_fragment_table_, tr("分片 (moof)"));

    QSplitter* splitter = new QSplitter(Qt::Vertical, parent);
    splitter->addWidget(mp4_sample_table_);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    connect(mp4_sample_track_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AnalysisPanel::OnMp4SampleTrackChanged);
    connect(export_mp4_sample_button_, &QPushButton::clicked,
            this, &AnalysisPanel::OnExportMp4SampleCsv);
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
    auto append_section = [](QTableWidget* table, const QString& header, int total,
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

void AnalysisPanel::OnContainerStructureReady(const model::ContainerStructureResult& result) {
    VE_PERF("OnContainerStructureReady 总计");
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
    {
    VE_PERF("容器结构树填充");
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
    }
    {
        VE_PERF("容器结构树 expandAll");
        container_tree_->expandAll();
    }

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
        // 填充 Sample Table 子页（样本级展开 + 一致性问题 + 分片）
        mp4_samples_ = result.mp4_samples;
        mp4_sample_focus_box_.clear();
        {
            const QSignalBlocker blocker(mp4_sample_track_combo_);
            mp4_sample_track_combo_->clear();
            for (const auto& t : mp4_samples_.tracks) {
                QString label = QString("Track %1 (%2").arg(t.track_id)
                                    .arg(QString::fromStdString(t.type));
                if (!t.codec.empty()) {
                    label += " / " + QString::fromStdString(t.codec);
                }
                label += QString(") · %1 样本").arg(t.stsz_sample_count);
                mp4_sample_track_combo_->addItem(label, static_cast<quint32>(t.track_id));
            }
        }
        UpdateMp4SampleSummary();
        {
            VE_PERF("RebuildMp4SampleTable(主线程)");
            RebuildMp4SampleTable();
        }
        RebuildMp4IssueTable();
        RebuildMp4FragmentTable();
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

void AnalysisPanel::UpdateMp4SampleSummary() {
    const auto& r = mp4_samples_;
    if (!r.valid) {
        mp4_sample_summary_label_->setText(
            tr("样本表：未分析（文件不是 MP4/MOV，或 Bento4 不可用）"));
        mp4_sample_summary_label_->setStyleSheet("font-size: 11px; color: #8B949E; padding: 2px;");
        return;
    }

    QString order;
    for (const auto& box : r.top_level_order) {
        if (!order.isEmpty()) order += " → ";
        order += QString::fromStdString(box);
    }
    const int errors = r.CountIssues(model::IssueSeverity::Error) +
                       r.CountIssues(model::IssueSeverity::Critical);
    const int warnings = r.CountIssues(model::IssueSeverity::Warning);
    const int infos = r.CountIssues(model::IssueSeverity::Info);

    mp4_sample_summary_label_->setText(
        QString("文件 %1 字节 | 顶层: %2 | moov: %3 | faststart: %4 | 分片: %5 (sidx %6 / styp %7) | "
                "问题: 错误 %8 / 警告 %9 / 提示 %10")
            .arg(QString::number(static_cast<quint64>(r.file_size)))
            .arg(order.isEmpty() ? "-" : order)
            .arg(r.moov_size > 0 ? QString("0x%1 (%2 B)")
                                       .arg(static_cast<quint64>(r.moov_offset), 0, 16)
                                       .arg(QString::number(static_cast<quint64>(r.moov_size)))
                                 : tr("无"))
            .arg(r.IsFastStart() ? tr("是") : tr("否"))
            .arg(r.fragments.size())
            .arg(r.sidx_count)
            .arg(r.styp_count)
            .arg(errors)
            .arg(warnings)
            .arg(infos));
    mp4_sample_summary_label_->setStyleSheet(
        errors > 0 ? "font-size: 11px; color: #F85149; padding: 2px;"
                   : (warnings > 0 ? "font-size: 11px; color: #D29922; padding: 2px;"
                                   : "font-size: 11px; color: #3FB950; padding: 2px;"));
}

void AnalysisPanel::RebuildMp4SampleTable() {
    mp4_sample_table_->setRowCount(0);
    if (!mp4_samples_.valid) return;

    const int track_index = mp4_sample_track_combo_->currentIndex();
    if (track_index < 0 || track_index >= static_cast<int>(mp4_samples_.tracks.size())) return;
    const model::Mp4TrackSampleTable& track = mp4_samples_.tracks[track_index];
    if (track.samples.empty()) return;

    // 结构树点了 stss 时只看关键帧
    const bool keyframe_only = (mp4_sample_focus_box_ == "stss");
    // 结构树联动的列高亮
    QVector<int> focus_cols;
    if (mp4_sample_focus_box_ == "stts")       focus_cols = {2, 5};
    else if (mp4_sample_focus_box_ == "ctts")  focus_cols = {3, 4};
    else if (mp4_sample_focus_box_ == "stsz")  focus_cols = {6};
    else if (mp4_sample_focus_box_ == "stsc")  focus_cols = {7};
    else if (mp4_sample_focus_box_ == "stco" ||
             mp4_sample_focus_box_ == "co64")  focus_cols = {1, 7};
    else if (mp4_sample_focus_box_ == "stss")  focus_cols = {8};

    constexpr int kMaxSampleRows = 5000;  // UI 安全上限（样本本身可按 options 截断）

    // 先算出实际要显示多少行，一次性 setRowCount：
    // 逐行 insertRow 在几千行时是 O(n^2)，实测 5000 行要 200ms+。
    int visible = 0;
    for (const auto& s : track.samples) {
        if (keyframe_only && !s.keyframe) continue;
        if (visible >= kMaxSampleRows) break;
        ++visible;
    }
    TableBatch table(mp4_sample_table_);
    table.SetRowCount(visible);

    int row = 0;
    for (const auto& s : track.samples) {
        if (keyframe_only && !s.keyframe) continue;
        if (row >= kMaxSampleRows) break;

        auto set = [&](int col, const QString& text) {
            return table.SetText(row, col, text);
        };
        set(0, QString::number(s.index));
        set(1, QString("0x%1").arg(static_cast<quint64>(s.offset), 0, 16));
        set(2, QString::number(s.DtsSeconds(track.media_timescale), 'f', 4));
        set(3, QString::number(s.CtsSeconds(track.media_timescale), 'f', 4));
        set(4, track.media_timescale > 0
                   ? QString::number(static_cast<double>(s.cts_delta) * 1000.0 /
                                         track.media_timescale, 'f', 2)
                   : QString::number(s.cts_delta));
        set(5, track.media_timescale > 0
                   ? QString::number(static_cast<double>(s.duration) * 1000.0 /
                                         track.media_timescale, 'f', 2)
                   : QString::number(s.duration));
        set(6, QString::number(static_cast<quint64>(s.size)));
        set(7, QString("%1.%2").arg(s.chunk_index).arg(s.index_in_chunk));
        set(8, s.keyframe ? QString::fromUtf8("\u2713") : QString());

        // 异常行着色: 红=会导致读不到数据/解码错乱，黄=可能影响兼容与体验
        QStringList reasons;
        bool error_row = false, warn_row = false;
        auto note = [&reasons](const char* text) { reasons << QString::fromUtf8(text); };
        if (s.HasFlag(model::Mp4SampleFlags::kOffsetOutOfRange)) {
            error_row = true; note("偏移越过文件末尾");
        }
        if (s.HasFlag(model::Mp4SampleFlags::kDtsNotMonotonic)) {
            error_row = true; note("DTS 回退");
        }
        if (s.HasFlag(model::Mp4SampleFlags::kNegativeCts)) { warn_row = true; note("合成时间为负"); }
        if (s.HasFlag(model::Mp4SampleFlags::kZeroSize)) { warn_row = true; note("大小为 0"); }
        if (s.HasFlag(model::Mp4SampleFlags::kZeroDuration)) { warn_row = true; note("时长为 0"); }
        if (s.HasFlag(model::Mp4SampleFlags::kChunkDiscontinuity)) {
            warn_row = true; note("chunk 内偏移不连续");
        }

        if (error_row || warn_row) {
            const QColor bg = error_row ? QColor("#5C1F1F") : QColor("#5C4A1F");
            for (int c = 0; c < mp4_sample_table_->columnCount(); ++c) {
                auto* item = mp4_sample_table_->item(row, c);
                if (item) item->setBackground(bg);
            }
            for (int c = 0; c < mp4_sample_table_->columnCount(); ++c) {
                auto* item = mp4_sample_table_->item(row, c);
                if (item) item->setToolTip(reasons.join(" / "));
            }
        }
        // 结构树联动的高亮列
        for (int col : focus_cols) {
            auto* item = mp4_sample_table_->item(row, col);
            if (item) item->setForeground(QColor("#58A6FF"));
        }
        ++row;
    }

    // resizeColumnsToContents 要逐行算文本宽度 (5000 行 × 9 列 ≈ 300ms),
    // 行数多时改用固定列宽, 把主线程还给用户。
    if (mp4_sample_table_->rowCount() <= 1000) {
        mp4_sample_table_->resizeColumnsToContents();
    } else {
        static const int kColumnWidths[] = {70, 110, 90, 90, 80, 80, 80, 90, 60};
        for (int c = 0; c < mp4_sample_table_->columnCount() && c < 9; ++c) {
            mp4_sample_table_->setColumnWidth(c, kColumnWidths[c]);
        }
    }
}

void AnalysisPanel::RebuildMp4IssueTable() {
    mp4_issue_table_->setRowCount(0);
    if (!mp4_samples_.valid) return;

    int row = 0;
    for (const auto& issue : mp4_samples_.issues) {
        mp4_issue_table_->insertRow(row);
        auto set = [&](int col, const QString& text) {
            mp4_issue_table_->setItem(row, col, new QTableWidgetItem(text));
        };
        set(0, QString::fromUtf8(model::ToString(issue.severity)));

        QString where = tr("文件级");
        if (issue.track_id >= 0) {
            where = QString("Track %1").arg(issue.track_id);
        } else if (issue.fragment_index >= 0) {
            where = QString("分片 #%1").arg(issue.fragment_index);
        }
        if (issue.has_sample_index) where += QString(" @样本#%1").arg(issue.sample_index);
        if (issue.occurrence_count > 1) where += QString(" ×%1").arg(issue.occurrence_count);
        set(1, where);

        set(2, QString::fromStdString(issue.title));
        set(3, QString::fromStdString(issue.detail));
        set(4, QString::fromStdString(issue.suggestion));

        const QColor fg = (issue.severity == model::IssueSeverity::Error ||
                           issue.severity == model::IssueSeverity::Critical)
                              ? QColor("#F85149")
                              : (issue.severity == model::IssueSeverity::Warning
                                     ? QColor("#D29922")
                                     : QColor("#8B949E"));
        for (int c = 0; c < mp4_issue_table_->columnCount(); ++c) {
            if (auto* item = mp4_issue_table_->item(row, c)) item->setForeground(fg);
        }
        ++row;
    }
    mp4_issue_table_->resizeColumnsToContents();
}

void AnalysisPanel::RebuildMp4FragmentTable() {
    mp4_fragment_table_->setRowCount(0);
    if (!mp4_samples_.valid || mp4_samples_.fragments.empty()) return;

    int row = 0;
    for (const auto& f : mp4_samples_.fragments) {
        mp4_fragment_table_->insertRow(row);
        auto set = [&](int col, const QString& text) {
            mp4_fragment_table_->setItem(row, col, new QTableWidgetItem(text));
        };
        const model::Mp4TrackSampleTable* track = mp4_samples_.FindTrack(f.track_id);
        const uint32_t ts = track ? track->media_timescale : mp4_samples_.movie_timescale;
        set(0, QString::number(f.index));
        set(1, QString::number(f.sequence_number));
        set(2, QString::number(f.track_id));
        set(3, QString("0x%1").arg(static_cast<quint64>(f.offset), 0, 16));
        set(4, f.has_tfdt ? QString::number(static_cast<quint64>(f.base_media_decode_time))
                          : tr("缺失"));
        set(5, ts > 0 ? QString("%1 (%2 s)")
                            .arg(QString::number(static_cast<quint64>(f.duration)))
                            .arg(QString::number(static_cast<double>(f.duration) / ts, 'f', 3))
                      : QString::number(static_cast<quint64>(f.duration)));
        set(6, QString::number(f.sample_count));
        set(7, QString::number(static_cast<quint64>(f.total_size)));
        if (!f.has_tfdt || (!f.base_data_offset_present && !f.default_base_is_moof &&
                            !f.trun_data_offset_present)) {
            for (int c = 0; c < mp4_fragment_table_->columnCount(); ++c) {
                if (auto* item = mp4_fragment_table_->item(row, c)) {
                    item->setBackground(QColor("#5C4A1F"));
                }
            }
        }
        ++row;
    }
    mp4_fragment_table_->resizeColumnsToContents();
}

void AnalysisPanel::OnMp4SampleTrackChanged(int) {
    RebuildMp4SampleTable();
}

void AnalysisPanel::OnContainerTreeSelectionChanged() {
    QTreeWidgetItem* item = container_tree_->currentItem();
    if (!item) return;
    const QString box = item->text(0).trimmed();
    static const QStringList kSampleBoxes = {
        "stts", "ctts", "stss", "stsz", "stz2", "stsc", "stco", "co64",
        "elst", "moof", "traf", "tfhd", "tfdt", "trun", "mfhd"};
    if (!kSampleBoxes.contains(box)) return;

    mp4_sample_focus_box_ = box;

    // moof 系列: 直接跳到分片表
    const bool is_fragment_box = (box == "moof" || box == "traf" || box == "tfhd" ||
                                  box == "tfdt" || box == "trun" || box == "mfhd");

    // 顺着父链找 trak，用它在同级 trak 中的序号选对应轨道
    QTreeWidgetItem* node = item;
    while (node && node->text(0).trimmed() != "trak") node = node->parent();
    if (node && node->parent()) {
        int trak_index = -1, found = 0;
        QTreeWidgetItem* parent = node->parent();
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->child(i)->text(0).trimmed() != "trak") continue;
            if (parent->child(i) == node) { trak_index = found; break; }
            ++found;
        }
        if (trak_index >= 0 && trak_index < mp4_sample_track_combo_->count()) {
            mp4_sample_track_combo_->setCurrentIndex(trak_index);
        }
    }

    container_detail_stack_->setCurrentIndex(1);        // MP4 详情页
    mp4_detail_tabs_->setCurrentWidget(mp4_sample_sub_);  // Sample Table 子页
    if (is_fragment_box) {
        // 底部切到分片表
        if (auto* bottom = qobject_cast<QTabWidget*>(mp4_fragment_table_->parentWidget())) {
            bottom->setCurrentWidget(mp4_fragment_table_);
        }
        mp4_sample_focus_label_->setText(tr("已联动：%1（分片信息见下方「分片 (moof)」）").arg(box));
    } else {
        mp4_sample_focus_label_->setText(tr("已联动：%1（高亮关联列）").arg(box));
    }
    RebuildMp4SampleTable();
}

void AnalysisPanel::OnExportMp4SampleCsv() {
    if (!mp4_samples_.valid || mp4_samples_.tracks.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 MP4 样本数据。"));
        return;
    }
    const int track_index = mp4_sample_track_combo_->currentIndex();
    if (track_index < 0 || track_index >= static_cast<int>(mp4_samples_.tracks.size())) return;
    const model::Mp4TrackSampleTable& track = mp4_samples_.tracks[track_index];

    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出 MP4 样本表 CSV"),
        QString("videoeye_mp4_samples_track%1_%2.csv")
            .arg(track.track_id)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("CSV 文件 (*.csv);;所有文件 (*)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "index,offset,dts,pts,cts_delta,duration,size,chunk,index_in_chunk,keyframe,flags\n";
    const double ts = static_cast<double>(track.media_timescale);
    for (const auto& s : track.samples) {
        out << s.index << ','
            << s.offset << ','
            << QString::number(ts > 0 ? s.dts / ts : 0.0, 'f', 6) << ','
            << QString::number(ts > 0 ? s.cts / ts : 0.0, 'f', 6) << ','
            << s.cts_delta << ','
            << s.duration << ','
            << s.size << ','
            << s.chunk_index << ','
            << s.index_in_chunk << ','
            << (s.keyframe ? 1 : 0) << ','
            << s.flags << '\n';
    }
    file.close();
    QMessageBox::information(this, tr("导出完成"),
                             tr("已导出 %1 个样本到:\n%2").arg(track.samples.size()).arg(filename));
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

    // 同步采样同时喂给时间轴分析器（构建音视频偏移曲线）
    timeline_analyzer_.OnSyncSample(sample.audio_timestamp_seconds * 1000.0,
                                    sample.video_timestamp_seconds * 1000.0);
    timeline_dirty_ = true;
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
    if (timeline_dirty_) {
        // 实时解码路径：用当前累积状态做一份快照（Finish 在副本上执行，不破坏累积状态）
        timeline_result_ = timeline_analyzer_.Snapshot();
        timeline_offline_ = false;
        RefreshTimelineUi();
        timeline_dirty_ = false;
    }

    const double flush_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - flush_begin).count();
    if (flush_ms >= 50.0) {
        LOG_WARN("[perf] 分析面板批量 UI 刷新耗时 " +
                 std::to_string(static_cast<long long>(flush_ms)) + " ms");
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
    if (!bitrate_series_ || !bitrate_axis_x_ || !bitrate_axis_y_) {
        return;
    }

    bitrate_series_->clear();
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
    bitrate_axis_x_->setRange(0, std::max<qreal>(1.0, bitrate_chart_values_.size()));
    // 上限留 10% 余量, 避免曲线贴顶; 全零时给一个最小量程防止坐标轴退化
    bitrate_axis_y_->setRange(0, std::max<qreal>(100.0, max_value * 1.1));
}

void AnalysisPanel::UpdateFPSChart(const analyzer::StreamStats& stats) {
    Q_UNUSED(stats);
    if (!fps_series_ || !fps_axis_x_ || !fps_axis_y_) {
        return;
    }

    fps_series_->clear();
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
    fps_axis_x_->setRange(0, std::max<qreal>(1.0, fps_chart_values_.size()));
    fps_axis_y_->setRange(0, std::max<qreal>(30.0, max_value * 1.1));
}

void AnalysisPanel::UpdateGOPChart() {
    if (!gop_series_ || !gop_axis_x_ || !gop_axis_y_) {
        return;
    }

    // 数据源统一为 UI 侧的 gop_summaries_ (由解码帧 pict_type 推导),
    // 不再使用 StreamAnalyzer 基于 packet flags 的独立 GOP 统计。
    gop_series_->clear();
    if (gop_summaries_.empty()) {
        gop_axis_x_->setRange(0, 1);
        gop_axis_y_->setRange(0, 1);
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
    gop_axis_x_->setRange(0, std::max(1, static_cast<int>(gop_summaries_.size())));
    gop_axis_y_->setRange(0, std::max(1, max_frames));
}

void AnalysisPanel::ResetStreamCharts() {
    bitrate_chart_values_.clear();
    fps_chart_values_.clear();
    if (bitrate_series_) bitrate_series_->clear();
    if (fps_series_) fps_series_->clear();
    if (gop_series_) gop_series_->clear();
}

void AnalysisPanel::UpdateSyncChart() {
    if (!sync_series_ || !sync_axis_x_ || !sync_axis_y_) {
        return;
    }

    sync_series_->clear();
    sync_chart_values_.clear();
    const int start = std::max(0, static_cast<int>(sync_sample_records_.size()) - kMaxChartSamples);
    {
        SeriesBatch batch(sync_series_);
        for (int i = start; i < static_cast<int>(sync_sample_records_.size()); ++i) {
            const qreal diff = static_cast<qreal>(sync_sample_records_[i].diff_ms);
            batch.Add(sync_sample_records_[i].index, diff);
            sync_chart_values_.push_back(diff);
        }
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
    {
        SeriesBatch video_batch(timeline_video_series_);
        SeriesBatch audio_batch(timeline_audio_series_);
        SeriesBatch event_batch(timeline_event_series_);
        for (int i = start; i < static_cast<int>(timeline_event_records_.size()); ++i) {
            const auto& record = timeline_event_records_[i];
            if (record.category == tr("视频关键帧")) {
                video_batch.Add(record.timestamp_seconds, 3.0);
            } else if (record.category == tr("音频采样")) {
                audio_batch.Add(record.timestamp_seconds, 2.0);
            } else {
                event_batch.Add(record.timestamp_seconds, 1.0);
            }
            min_ts = std::min(min_ts, record.timestamp_seconds);
            max_ts = std::max(max_ts, record.timestamp_seconds);
        }
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
void AppendDecimated(QLineSeries* series, const model::MetricSeries& curve, int limit) {
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

void AnalysisPanel::SetupBitrateGopTab() {
    bitrate_gop_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(bitrate_gop_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 开始/取消
    {
        QWidget* row = new QWidget(bitrate_gop_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("码率与 GOP 深度分析"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        bitrate_gop_start_button_ = new QPushButton(tr("开始分析"), row);
        bitrate_gop_start_button_->setToolTip(
            tr("对当前文件做一次完整扫描：滑动窗口码率、I/P/B 分布、GOP 列表与异常识别。"
               "与「诊断与报告」共用同一次扫描结果，不会重复读文件。"));
        connect(bitrate_gop_start_button_, &QPushButton::clicked,
                this, &AnalysisPanel::OnStartBitrateGopAnalysis);
        rl->addWidget(bitrate_gop_start_button_);

        bitrate_gop_cancel_button_ = new QPushButton(tr("取消"), row);
        bitrate_gop_cancel_button_->setEnabled(false);
        connect(bitrate_gop_cancel_button_, &QPushButton::clicked,
                this, &AnalysisPanel::OnCancelBitrateGopAnalysis);
        rl->addWidget(bitrate_gop_cancel_button_);
        layout->addWidget(row);
    }

    bitrate_gop_progress_bar_ = new QProgressBar(bitrate_gop_tab_);
    bitrate_gop_progress_bar_->setRange(0, 100);
    bitrate_gop_progress_bar_->setValue(0);
    bitrate_gop_progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(bitrate_gop_progress_bar_);

    // 参数行
    {
        QWidget* row = new QWidget(bitrate_gop_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        rl->addWidget(new QLabel(tr("滑动窗口"), row));
        bitrate_window_combo_ = new QComboBox(row);
        for (double w : bitrate_gop_options_.windows_seconds) {
            bitrate_window_combo_->addItem(QString::number(w, 'f', 2) + tr(" 秒"), w);
        }
        bitrate_window_combo_->setCurrentIndex(0);
        connect(bitrate_window_combo_, &QComboBox::currentIndexChanged,
                this, &AnalysisPanel::OnBitrateGopWindowChanged);
        rl->addWidget(bitrate_window_combo_);

        rl->addWidget(new QLabel(tr("目标峰值 kbps（0=自动）"), row));
        bitrate_target_peak_spin_ = new QDoubleSpinBox(row);
        bitrate_target_peak_spin_->setRange(0.0, 1.0e7);
        bitrate_target_peak_spin_->setDecimals(0);
        bitrate_target_peak_spin_->setSingleStep(100.0);
        bitrate_target_peak_spin_->setValue(0.0);
        connect(bitrate_target_peak_spin_, &QDoubleSpinBox::valueChanged,
                this, &AnalysisPanel::OnBitrateGopOptionChanged);
        rl->addWidget(bitrate_target_peak_spin_);

        rl->addWidget(new QLabel(tr("GOP 上限 秒"), row));
        bitrate_max_gop_seconds_spin_ = new QDoubleSpinBox(row);
        bitrate_max_gop_seconds_spin_->setRange(0.5, 600.0);
        bitrate_max_gop_seconds_spin_->setDecimals(1);
        bitrate_max_gop_seconds_spin_->setSingleStep(1.0);
        bitrate_max_gop_seconds_spin_->setValue(bitrate_gop_options_.max_gop_seconds);
        connect(bitrate_max_gop_seconds_spin_, &QDoubleSpinBox::valueChanged,
                this, &AnalysisPanel::OnBitrateGopOptionChanged);
        rl->addWidget(bitrate_max_gop_seconds_spin_);

        rl->addWidget(new QLabel(tr("帧"), row));
        bitrate_max_gop_frames_spin_ = new QSpinBox(row);
        bitrate_max_gop_frames_spin_->setRange(1, 100000);
        bitrate_max_gop_frames_spin_->setSingleStep(10);
        bitrate_max_gop_frames_spin_->setValue(bitrate_gop_options_.max_gop_frames);
        connect(bitrate_max_gop_frames_spin_, &QSpinBox::valueChanged,
                this, &AnalysisPanel::OnBitrateGopOptionChanged);
        rl->addWidget(bitrate_max_gop_frames_spin_);

        bitrate_decode_types_check_ = new QCheckBox(tr("精确帧类型（解码，较慢）"), row);
        bitrate_decode_types_check_->setChecked(false);
        bitrate_decode_types_check_->setToolTip(
            tr("默认用 codec parser 判定帧类型（几乎零成本）；勾选后会完整解码一遍视频，"
               "I/P/B 比例最准确，但长文件明显变慢。"));
        connect(bitrate_decode_types_check_, &QCheckBox::toggled,
                this, &AnalysisPanel::OnBitrateGopOptionChanged);
        rl->addWidget(bitrate_decode_types_check_);

        QPushButton* link_btn = new QPushButton(tr("关联场景切换"), row);
        link_btn->setToolTip(tr("用「场景切换」页检测到的切换点，检查其附近是否有关键帧"));
        connect(link_btn, &QPushButton::clicked, this, &AnalysisPanel::OnLinkSceneChanges);
        rl->addWidget(link_btn);

        rl->addStretch();
        layout->addWidget(row);
    }

    bitrate_gop_summary_label_ = new QLabel(
        tr("点击「开始分析」扫描当前文件：输出滑动窗口码率曲线、I/P/B 比例、GOP 列表与异常峰值，"
           "并可与场景切换结果关联给出编码优化建议。"), bitrate_gop_tab_);
    bitrate_gop_summary_label_->setWordWrap(true);
    layout->addWidget(bitrate_gop_summary_label_);

    // 码率曲线（叠加 I 帧 / 场景切换 / 异常峰值标记）
    {
        bitrate_gop_chart_object_ = new QChart();
        bitrate_gop_chart_object_->setTitle(tr("滑动窗口码率（含 I 帧 / 场景切换 / 峰值标记）"));
        bitrate_gop_series_ = new QLineSeries();
        bitrate_gop_series_->setName(tr("码率"));
        bitrate_target_series_ = new QLineSeries();
        bitrate_target_series_->setName(tr("目标峰值"));
        bitrate_iframe_series_ = new QScatterSeries();
        bitrate_iframe_series_->setName(tr("I 帧"));
        bitrate_iframe_series_->setMarkerSize(6.0);
        bitrate_iframe_series_->setColor(QColor("#43a047"));
        bitrate_iframe_series_->setBorderColor(QColor("#43a047"));
        bitrate_scene_series_ = new QScatterSeries();
        bitrate_scene_series_->setName(tr("场景切换"));
        bitrate_scene_series_->setMarkerSize(9.0);
        bitrate_scene_series_->setMarkerShape(QScatterSeries::MarkerShapeRectangle);
        bitrate_scene_series_->setColor(QColor("#8e24aa"));
        bitrate_scene_series_->setBorderColor(QColor("#8e24aa"));
        bitrate_anomaly_series_ = new QScatterSeries();
        bitrate_anomaly_series_->setName(tr("异常峰值"));
        bitrate_anomaly_series_->setMarkerSize(11.0);
        bitrate_anomaly_series_->setMarkerShape(QScatterSeries::MarkerShapeTriangle);
        bitrate_anomaly_series_->setColor(QColor("#e53935"));
        bitrate_anomaly_series_->setBorderColor(QColor("#e53935"));

        bitrate_gop_chart_object_->addSeries(bitrate_gop_series_);
        bitrate_gop_chart_object_->addSeries(bitrate_target_series_);
        bitrate_gop_chart_object_->addSeries(bitrate_iframe_series_);
        bitrate_gop_chart_object_->addSeries(bitrate_scene_series_);
        bitrate_gop_chart_object_->addSeries(bitrate_anomaly_series_);

        bitrate_gop_axis_x_ = new QValueAxis();
        bitrate_gop_axis_x_->setTitleText(tr("时间 (s)"));
        bitrate_gop_axis_y_ = new QValueAxis();
        bitrate_gop_axis_y_->setTitleText(tr("kbps"));
        bitrate_gop_chart_object_->addAxis(bitrate_gop_axis_x_, Qt::AlignBottom);
        bitrate_gop_chart_object_->addAxis(bitrate_gop_axis_y_, Qt::AlignLeft);
        for (QAbstractSeries* s : {static_cast<QAbstractSeries*>(bitrate_gop_series_),
                                   static_cast<QAbstractSeries*>(bitrate_target_series_),
                                   static_cast<QAbstractSeries*>(bitrate_iframe_series_),
                                   static_cast<QAbstractSeries*>(bitrate_scene_series_),
                                   static_cast<QAbstractSeries*>(bitrate_anomaly_series_)}) {
            s->attachAxis(bitrate_gop_axis_x_);
            s->attachAxis(bitrate_gop_axis_y_);
        }
        bitrate_gop_chart_ = new QChartView(bitrate_gop_chart_object_, bitrate_gop_tab_);
        bitrate_gop_chart_->setMinimumHeight(240);
        bitrate_gop_chart_->setRenderHint(QPainter::Antialiasing);
        layout->addWidget(bitrate_gop_chart_);
    }

    // 子页: GOP 列表 / 异常 / 建议
    bitrate_gop_sub_tabs_ = new QTabWidget(bitrate_gop_tab_);
    bitrate_gop_sub_tabs_->setMinimumHeight(320);
    {
        QWidget* page = new QWidget(bitrate_gop_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        QLabel* hint = new QLabel(tr("点击任意一行即跳转到该 GOP 起始位置。"), page);
        pl->addWidget(hint);
        bitrate_gop_table_ = new QTableWidget(0, 11, page);
        bitrate_gop_table_->setHorizontalHeaderLabels(
            {tr("序号"), tr("起始"), tr("结束"), tr("时长(s)"), tr("帧数"), tr("大小(KB)"),
             tr("平均码率(kbps)"), tr("I / P / B"), tr("最大帧(KB)"), tr("类型"), tr("状态")});
        bitrate_gop_table_->verticalHeader()->setVisible(false);
        bitrate_gop_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        bitrate_gop_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        bitrate_gop_table_->horizontalHeader()->setStretchLastSection(true);
        bitrate_gop_table_->setMinimumHeight(220);
        connect(bitrate_gop_table_, &QTableWidget::cellClicked,
                this, &AnalysisPanel::OnBitrateGopCellClicked);
        pl->addWidget(bitrate_gop_table_);
        QPushButton* export_btn = new QPushButton(tr("导出 GOP CSV"), page);
        connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportBitrateGopCsv);
        pl->addWidget(export_btn, 0, Qt::AlignRight);
        bitrate_gop_sub_tabs_->addTab(page, tr("GOP 列表"));
    }
    {
        QWidget* page = new QWidget(bitrate_gop_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        bitrate_anomaly_table_ = new QTableWidget(0, 5, page);
        bitrate_anomaly_table_->setHorizontalHeaderLabels(
            {tr("类型"), tr("位置"), tr("实测值"), tr("阈值"), tr("说明")});
        bitrate_anomaly_table_->verticalHeader()->setVisible(false);
        bitrate_anomaly_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        bitrate_anomaly_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        bitrate_anomaly_table_->horizontalHeader()->setStretchLastSection(true);
        bitrate_anomaly_table_->setMinimumHeight(220);
        connect(bitrate_anomaly_table_, &QTableWidget::cellClicked,
                this, &AnalysisPanel::OnBitrateAnomalyCellClicked);
        pl->addWidget(bitrate_anomaly_table_);
        QPushButton* export_btn = new QPushButton(tr("导出异常 CSV"), page);
        connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportBitrateAnomalyCsv);
        pl->addWidget(export_btn, 0, Qt::AlignRight);
        bitrate_gop_sub_tabs_->addTab(page, tr("异常"));
    }
    {
        QWidget* page = new QWidget(bitrate_gop_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        bitrate_suggestion_list_ = new QListWidget(page);
        bitrate_suggestion_list_->setWordWrap(true);
        bitrate_suggestion_list_->setMinimumHeight(220);
        pl->addWidget(bitrate_suggestion_list_);
        QPushButton* export_btn = new QPushButton(tr("导出码率曲线 CSV"), page);
        connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportBitrateCurveCsv);
        pl->addWidget(export_btn, 0, Qt::AlignRight);
        bitrate_gop_sub_tabs_->addTab(page, tr("优化建议"));
    }
    layout->addWidget(bitrate_gop_sub_tabs_);

    AddPageWithScroll(bitrate_gop_tab_, tr("码率与 GOP"));
}

void AnalysisPanel::ApplyBitrateGopOptionsFromUi() {
    bitrate_gop_options_.target_peak_kbps = bitrate_target_peak_spin_->value();
    bitrate_gop_options_.max_gop_seconds = bitrate_max_gop_seconds_spin_->value();
    bitrate_gop_options_.max_gop_frames = bitrate_max_gop_frames_spin_->value();
    diagnostics_options_.bitrate_gop_options = bitrate_gop_options_;
    diagnostics_options_.analyze_bitrate_gop = true;
    diagnostics_options_.decode_frame_types = bitrate_decode_types_check_->isChecked();
}

void AnalysisPanel::OnStartBitrateGopAnalysis() {
    ApplyBitrateGopOptionsFromUi();
    StartDiagnosticsScan(diagnostics_options_);
}

void AnalysisPanel::OnCancelBitrateGopAnalysis() {
    diagnostics_coordinator_.Cancel();
    bitrate_gop_cancel_button_->setEnabled(false);
    bitrate_gop_progress_bar_->setFormat(tr("取消中..."));
}

void AnalysisPanel::OnBitrateGopWindowChanged() {
    const double w = bitrate_window_combo_->currentData().toDouble();
    if (w <= 0.0) return;
    bitrate_gop_display_window_ = w;
    UpdateBitrateGopChart();
    UpdateBitrateGopSummary();
}

void AnalysisPanel::OnBitrateGopOptionChanged() {
    // 阈值类参数在下一次「开始分析」时生效（重新扫描代价大，不自动触发）
    ApplyBitrateGopOptionsFromUi();
}

void AnalysisPanel::OnLinkSceneChanges() {
    if (!has_diagnostics_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先在本页点击「开始分析」完成一次扫描。"));
        return;
    }
    if (scene_change_records_.empty()) {
        QMessageBox::information(this, tr("提示"),
            tr("当前没有场景切换数据。请先在「场景切换」页启用检测并播放一段视频。"));
        return;
    }
    ApplyBitrateGopOptionsFromUi();
    analyzer::BitrateGopAnalyzer::ApplySceneChanges(diagnostics_result_.bitrate_gop,
                                                    scene_change_records_, bitrate_gop_options_);
    // 让新产生的"场景切换缺少关键帧"问题进入诊断报告
    current_qc_report_ = qc_rule_engine_.Evaluate(diagnostics_result_);
    RebuildIssueTable();
    UpdateQcSummary();
    UpdateBitrateGopUi();

    const size_t matched = diagnostics_result_.bitrate_gop.scene_matches.size();
    size_t missing = 0;
    for (const auto& m : diagnostics_result_.bitrate_gop.scene_matches) {
        if (!m.has_nearby_keyframe) ++missing;
    }
    QMessageBox::information(this, tr("已关联"),
        tr("共关联 %1 个场景切换点，其中 %2 处附近没有关键帧。")
            .arg(matched).arg(missing));
}

void AnalysisPanel::OnBitrateGopCellClicked(int row, int) {
    if (!bitrate_gop_table_ || row < 0) return;
    const auto& gops = diagnostics_result_.bitrate_gop.gops;
    if (row >= static_cast<int>(gops.size())) return;
    emit SeekRequested(gops[row].start_seconds);
}

void AnalysisPanel::OnBitrateAnomalyCellClicked(int row, int) {
    if (!bitrate_anomaly_table_ || row < 0) return;
    const auto& anomalies = diagnostics_result_.bitrate_gop.anomalies;
    if (row >= static_cast<int>(anomalies.size())) return;
    emit SeekRequested(anomalies[row].start_seconds);
}

void AnalysisPanel::UpdateBitrateGopUi() {
    UpdateBitrateGopSummary();
    UpdateBitrateGopChart();
    RebuildBitrateGopTable();
    RebuildBitrateAnomalyTable();
    UpdateBitrateGopSuggestions();
}

void AnalysisPanel::UpdateBitrateGopSummary() {
    if (!bitrate_gop_summary_label_) return;
    if (!has_diagnostics_result_) return;

    const auto& bg = diagnostics_result_.bitrate_gop;
    if (bg.total_frames == 0) {
        bitrate_gop_summary_label_->setText(tr("未检测到视频帧，无法进行码率与 GOP 分析。"));
        return;
    }

    const QString frame_types =
        bg.frame_types_known
            ? tr("I %1% / P %2% / B %3%")
                  .arg(QString::number(bg.IFrameRatio() * 100.0, 'f', 1))
                  .arg(QString::number(bg.PFrameRatio() * 100.0, 'f', 1))
                  .arg(QString::number(bg.BFrameRatio() * 100.0, 'f', 1))
            : tr("I %1 帧（其余未解析，勾选「精确帧类型」重新分析）")
                  .arg(static_cast<qlonglong>(bg.i_frame_count));

    bitrate_gop_summary_label_->setText(
        tr("窗口 <b>%1</b> 秒 ｜ 平均 <b>%2</b> kbps ｜ 峰值 <b>%3</b> kbps ｜ 最低 %4 kbps ｜ "
           "中位 %5 kbps ｜ P95 %6 kbps ｜ 峰均比 <b>%7</b><br>"
           "目标峰值 %8 kbps（%9） ｜ 帧类型 %10 ｜ 平均帧 %11 KB ｜ 平均 I 帧 %12 KB<br>"
           "GOP <b>%13</b> 个 ｜ 平均时长 %14 s（%15~%16 帧）｜ 最长 %17 s ｜ 超长 %18 个 ｜ "
           "closed %19 / open %20 ｜ 关键帧间隔 %21 ± %22 s<br>"
           "异常 %23 条")
            .arg(QString::number(bitrate_gop_display_window_, 'f', 2))
            .arg(QString::number(bg.avg_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.peak_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.min_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.median_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.p95_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.peak_to_mean_ratio, 'f', 2))
            .arg(QString::number(bg.target_peak_kbps, 'f', 0))
            .arg(bitrate_gop_options_.target_peak_kbps > 0.0 ? tr("手动") : tr("自动=均值×2"))
            .arg(frame_types)
            .arg(QString::number(bg.AverageFrameBytes() / 1024.0, 'f', 1))
            .arg(QString::number(bg.AverageIFrameBytes() / 1024.0, 'f', 1))
            .arg(bg.gops.size())
            .arg(QString::number(bg.gop_duration_mean, 'f', 2))
            .arg(bg.gop_frames_min)
            .arg(bg.gop_frames_max)
            .arg(QString::number(bg.gop_duration_max, 'f', 2))
            .arg(bg.long_gop_count)
            .arg(bg.closed_gop_count)
            .arg(bg.open_gop_count)
            .arg(QString::number(bg.key_interval_mean, 'f', 2))
            .arg(QString::number(bg.key_interval_stddev, 'f', 2))
            .arg(bg.anomalies.size()));
}

void AnalysisPanel::UpdateBitrateGopChart() {
    if (!bitrate_gop_series_) return;
    bitrate_gop_series_->clear();
    bitrate_target_series_->clear();
    bitrate_iframe_series_->clear();
    bitrate_scene_series_->clear();
    bitrate_anomaly_series_->clear();
    if (!has_diagnostics_result_) return;

    const auto& bg = diagnostics_result_.bitrate_gop;
    if (bg.bitrate_points.empty() && bg.window_curves.empty()) return;

    // 取当前窗口对应的曲线；找不到时回退到默认窗口的采样点
    const model::MetricSeries* curve = nullptr;
    for (const auto& c : bg.window_curves) {
        if (std::abs(c.name.find("bitrate_") == 0 ? 0.0 : 1.0) < 1e-9 &&
            c.name == std::string("bitrate_") +
                          [](double w) {
                              char buf[32];
                              std::snprintf(buf, sizeof(buf), "%.2f", w);
                              return std::string(buf);
                          }(bitrate_gop_display_window_) + "s") {
            curve = &c;
            break;
        }
    }
    if (curve != nullptr) {
        AppendDecimated(bitrate_gop_series_, *curve, kMaxBitrateChartPoints);
    } else {
        model::MetricSeries fallback;
        fallback.samples.reserve(bg.bitrate_points.size());
        for (const auto& p : bg.bitrate_points) {
            fallback.Add(p.timestamp_seconds, p.bitrate_kbps);
        }
        AppendDecimated(bitrate_gop_series_, fallback, kMaxBitrateChartPoints);
    }

    const double duration = std::max(bg.duration_seconds, 1.0);
    double y_max = std::max(bg.peak_bitrate_kbps, bg.target_peak_kbps) * 1.15;
    if (y_max <= 0.0) y_max = 1.0;

    if (bg.target_peak_kbps > 0.0) {
        bitrate_target_series_->append(0.0, bg.target_peak_kbps);
        bitrate_target_series_->append(duration, bg.target_peak_kbps);
    }

    // I 帧标记（画在基线）
    {
        const int iframe_step =
            std::max(1, static_cast<int>(bg.i_frame_seconds.size()) / kMaxBitrateChartMarkers);
        SeriesBatch batch(bitrate_iframe_series_);
        for (size_t i = 0; i < bg.i_frame_seconds.size(); i += iframe_step) {
            batch.Add(bg.i_frame_seconds[i], 0.0);
        }
    }

    // 场景切换点（基线，用关联结果；未关联时直接用检测记录）
    {
        SeriesBatch batch(bitrate_scene_series_);
        if (!bg.scene_matches.empty()) {
            const int scene_step =
                std::max(1, static_cast<int>(bg.scene_matches.size()) / kMaxBitrateChartMarkers);
            for (size_t i = 0; i < bg.scene_matches.size(); i += scene_step) {
                batch.Add(bg.scene_matches[i].timestamp_seconds, 0.0);
            }
        } else {
            const int scene_step =
                std::max(1, static_cast<int>(scene_change_records_.size()) / kMaxBitrateChartMarkers);
            for (size_t i = 0; i < scene_change_records_.size(); i += scene_step) {
                batch.Add(scene_change_records_[i].timestamp, 0.0);
            }
        }
    }

    // 异常峰值（标在实际码率高度）
    {
        SeriesBatch batch(bitrate_anomaly_series_);
        for (const auto& a : bg.anomalies) {
            if (a.type != analyzer::BitrateAnomalyType::PeakOvershoot) continue;
            batch.Add((a.start_seconds + a.end_seconds) * 0.5, a.value);
            y_max = std::max(y_max, a.value * 1.1);
        }
    }

    bitrate_gop_axis_x_->setRange(0.0, duration);
    bitrate_gop_axis_y_->setRange(0.0, y_max);
}

void AnalysisPanel::RebuildBitrateGopTable() {
    if (!bitrate_gop_table_) return;
    bitrate_gop_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;

    const auto& gops = diagnostics_result_.bitrate_gop.gops;
    const int rows = std::min(static_cast<int>(gops.size()), kMaxGopTableRows);
    bitrate_gop_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& g = gops[i];
        SetTableItemText(bitrate_gop_table_, i, 0, QString::number(g.index));
        SetTableItemText(bitrate_gop_table_, i, 1,
                         theme::font::formatTime(static_cast<int>(g.start_seconds * 1000)));
        SetTableItemText(bitrate_gop_table_, i, 2,
                         theme::font::formatTime(static_cast<int>(g.end_seconds * 1000)));
        SetTableItemText(bitrate_gop_table_, i, 3, QString::number(g.DurationSeconds(), 'f', 3));
        SetTableItemText(bitrate_gop_table_, i, 4, QString::number(g.frame_count));
        SetTableItemText(bitrate_gop_table_, i, 5, FormatKb(static_cast<double>(g.byte_count)));
        SetTableItemText(bitrate_gop_table_, i, 6,
                         QString::number(g.AverageBitrateKbps(), 'f', 0));
        SetTableItemText(bitrate_gop_table_, i, 7, QString::fromStdString(g.FrameTypeSummary()));
        SetTableItemText(bitrate_gop_table_, i, 8,
                         FormatKb(static_cast<double>(g.max_frame_bytes)));
        SetTableItemText(bitrate_gop_table_, i, 9, g.closed_gop ? tr("closed") : tr("open"));
        SetTableItemText(bitrate_gop_table_, i, 10, g.complete ? tr("已收尾") : tr("未收尾"));

        if (bitrate_gop_options_.max_gop_seconds > 0.0 &&
            g.DurationSeconds() > bitrate_gop_options_.max_gop_seconds) {
            if (QTableWidgetItem* cell = bitrate_gop_table_->item(i, 3)) {
                cell->setForeground(QColor("#ef6c00"));
            }
        }
    }
    bitrate_gop_table_->resizeColumnsToContents();
}

void AnalysisPanel::RebuildBitrateAnomalyTable() {
    if (!bitrate_anomaly_table_) return;
    bitrate_anomaly_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;

    const auto& anomalies = diagnostics_result_.bitrate_gop.anomalies;
    const int rows = std::min(static_cast<int>(anomalies.size()), kMaxAnomalyTableRows);
    bitrate_anomaly_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& a = anomalies[i];
        const QString unit = QString::fromStdString(a.unit);
        SetTableItemText(bitrate_anomaly_table_, i, 0,
                         QString::fromStdString(analyzer::ToString(a.type)));
        SetTableItemText(bitrate_anomaly_table_, i, 1,
                         QString::fromStdString(
                             model::TimeRange::Between(a.start_seconds, a.end_seconds).ToString()));
        SetTableItemText(bitrate_anomaly_table_, i, 2, FormatMetricValue(a.value, unit));
        SetTableItemText(bitrate_anomaly_table_, i, 3, FormatMetricValue(a.threshold, unit));
        SetTableItemText(bitrate_anomaly_table_, i, 4, QString::fromStdString(a.detail));
    }
    bitrate_anomaly_table_->resizeColumnsToContents();
}

void AnalysisPanel::UpdateBitrateGopSuggestions() {
    if (!bitrate_suggestion_list_) return;
    bitrate_suggestion_list_->clear();
    if (!has_diagnostics_result_) return;
    for (const auto& s : diagnostics_result_.bitrate_gop.suggestions) {
        bitrate_suggestion_list_->addItem(QString::fromStdString(s));
    }
}

void AnalysisPanel::OnExportBitrateGopCsv() {
    if (!has_diagnostics_result_ || diagnostics_result_.bitrate_gop.gops.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 GOP 数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出 GOP 列表 CSV"),
        QString::fromStdString(current_video_path_).section('/', -1) + "_gop.csv",
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
    stream << "index,start_seconds,end_seconds,duration_seconds,frame_count,byte_count,"
              "avg_bitrate_kbps,i_count,p_count,b_count,unknown_count,max_frame_bytes,"
              "closed_gop,complete\n";
    for (const auto& g : diagnostics_result_.bitrate_gop.gops) {
        stream << g.index << "," << QString::number(g.start_seconds, 'f', 3) << ","
               << QString::number(g.end_seconds, 'f', 3) << ","
               << QString::number(g.DurationSeconds(), 'f', 3) << "," << g.frame_count << ","
               << g.byte_count << "," << QString::number(g.AverageBitrateKbps(), 'f', 3) << ","
               << g.i_count << "," << g.p_count << "," << g.b_count << "," << g.unknown_count << ","
               << g.max_frame_bytes << "," << (g.closed_gop ? 1 : 0) << ","
               << (g.complete ? 1 : 0) << "\n";
    }
    file.close();
    QMessageBox::information(this, tr("成功"),
        tr("已导出 %1 个 GOP 到:\n%2").arg(diagnostics_result_.bitrate_gop.gops.size()).arg(filename));
}

void AnalysisPanel::OnExportBitrateCurveCsv() {
    if (!has_diagnostics_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次扫描。"));
        return;
    }
    const auto& bg = diagnostics_result_.bitrate_gop;
    if (bg.window_curves.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的码率曲线数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出码率曲线 CSV"),
        QString::fromStdString(current_video_path_).section('/', -1) + "_bitrate.csv",
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
    // 第一列时间，之后每个窗口一列
    stream << "timestamp_seconds";
    for (const auto& c : bg.window_curves) stream << "," << QString::fromStdString(c.name);
    stream << "\n";
    // 各窗口采样点数量不同，按默认窗口的时间轴输出，其余窗口按最近时刻取值
    const auto& base = bg.window_curves.front();
    for (const auto& s : base.samples) {
        stream << QString::number(s.timestamp_seconds, 'f', 3);
        for (const auto& c : bg.window_curves) {
            stream << "," << QString::number(c.ValueAt(s.timestamp_seconds), 'f', 3);
        }
        stream << "\n";
    }
    file.close();
    QMessageBox::information(this, tr("成功"), tr("已导出码率曲线到:\n%1").arg(filename));
}

void AnalysisPanel::OnExportBitrateAnomalyCsv() {
    if (!has_diagnostics_result_ || diagnostics_result_.bitrate_gop.anomalies.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的异常数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出异常 CSV"),
        QString::fromStdString(current_video_path_).section('/', -1) + "_bitrate_anomaly.csv",
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
    stream << "type,start_seconds,end_seconds,value,threshold,unit,detail,suggestion\n";
    for (const auto& a : diagnostics_result_.bitrate_gop.anomalies) {
        stream << QString::fromStdString(analyzer::ToString(a.type)) << ","
               << QString::number(a.start_seconds, 'f', 3) << ","
               << QString::number(a.end_seconds, 'f', 3) << ","
               << QString::number(a.value, 'f', 3) << ","
               << QString::number(a.threshold, 'f', 3) << ","
               << QString::fromStdString(a.unit) << ",\""
               << QString::fromStdString(a.detail).replace('"', "'") << "\",\""
               << QString::fromStdString(a.suggestion).replace('"', "'") << "\"\n";
    }
    file.close();
    QMessageBox::information(this, tr("成功"),
        tr("已导出 %1 条异常到:\n%2")
            .arg(diagnostics_result_.bitrate_gop.anomalies.size()).arg(filename));
}

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
void AppendAudioPoints(QLineSeries* series, const std::vector<model::LoudnessPoint>& points,
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

void AnalysisPanel::SetupAudioQcTab() {
    audio_qc_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(audio_qc_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 开始/取消/导出
    {
        QWidget* row = new QWidget(audio_qc_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("音频 QC（响度 / 真峰值 / 削波 / 静音 / 相位）"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        audio_qc_start_button_ = new QPushButton(tr("开始分析"), row);
        audio_qc_start_button_->setToolTip(
            tr("对当前文件解码音频流做一次完整体检：BS.1770 响度、4× 过采样真峰值、削波、"
               "静音段、声道相位与 metadata 一致性。与「码率与 GOP」「诊断与报告」共用同一次扫描。"));
        connect(audio_qc_start_button_, &QPushButton::clicked,
                this, &AnalysisPanel::OnStartAudioQcAnalysis);
        rl->addWidget(audio_qc_start_button_);

        audio_qc_cancel_button_ = new QPushButton(tr("取消"), row);
        audio_qc_cancel_button_->setEnabled(false);
        connect(audio_qc_cancel_button_, &QPushButton::clicked,
                this, &AnalysisPanel::OnCancelAudioQcAnalysis);
        rl->addWidget(audio_qc_cancel_button_);

        QPushButton* export_btn = new QPushButton(tr("导出响度 CSV"), row);
        connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportAudioQcCsv);
        rl->addWidget(export_btn);
        layout->addWidget(row);
    }

    audio_qc_progress_bar_ = new QProgressBar(audio_qc_tab_);
    audio_qc_progress_bar_->setRange(0, 100);
    audio_qc_progress_bar_->setValue(0);
    audio_qc_progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(audio_qc_progress_bar_);

    // 参数行
    {
        QWidget* row = new QWidget(audio_qc_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        rl->addWidget(new QLabel(tr("目标响度"), row));
        audio_qc_target_lufs_spin_ = new QDoubleSpinBox(row);
        audio_qc_target_lufs_spin_->setRange(-70.0, 0.0);
        audio_qc_target_lufs_spin_->setDecimals(1);
        audio_qc_target_lufs_spin_->setSingleStep(1.0);
        audio_qc_target_lufs_spin_->setValue(-23.0);
        audio_qc_target_lufs_spin_->setSuffix(tr(" LUFS"));
        audio_qc_target_lufs_spin_->setToolTip(tr("仅用于曲线上的参考线；合格判定阈值在「规则与阈值」页"));
        connect(audio_qc_target_lufs_spin_, &QDoubleSpinBox::valueChanged,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_target_lufs_spin_);

        rl->addWidget(new QLabel(tr("静音阈值"), row));
        audio_qc_silence_spin_ = new QDoubleSpinBox(row);
        audio_qc_silence_spin_->setRange(-120.0, 0.0);
        audio_qc_silence_spin_->setDecimals(0);
        audio_qc_silence_spin_->setSingleStep(5.0);
        audio_qc_silence_spin_->setValue(audio_qc_options_.silence_threshold_dbfs);
        audio_qc_silence_spin_->setSuffix(tr(" dBFS"));
        connect(audio_qc_silence_spin_, &QDoubleSpinBox::valueChanged,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_silence_spin_);

        rl->addWidget(new QLabel(tr("最短静音"), row));
        audio_qc_min_silence_spin_ = new QDoubleSpinBox(row);
        audio_qc_min_silence_spin_->setRange(0.0, 60.0);
        audio_qc_min_silence_spin_->setDecimals(2);
        audio_qc_min_silence_spin_->setSingleStep(0.1);
        audio_qc_min_silence_spin_->setValue(audio_qc_options_.min_silence_seconds);
        audio_qc_min_silence_spin_->setSuffix(tr(" s"));
        connect(audio_qc_min_silence_spin_, &QDoubleSpinBox::valueChanged,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_min_silence_spin_);

        rl->addWidget(new QLabel(tr("削波阈值"), row));
        audio_qc_clip_spin_ = new QDoubleSpinBox(row);
        audio_qc_clip_spin_->setRange(0.5, 1.0);
        audio_qc_clip_spin_->setDecimals(4);
        audio_qc_clip_spin_->setSingleStep(0.0005);
        audio_qc_clip_spin_->setValue(audio_qc_options_.clip_threshold);
        connect(audio_qc_clip_spin_, &QDoubleSpinBox::valueChanged,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_clip_spin_);

        audio_qc_loudness_check_ = new QCheckBox(tr("响度(BS.1770)"), row);
        audio_qc_loudness_check_->setChecked(audio_qc_options_.enable_loudness);
        connect(audio_qc_loudness_check_, &QCheckBox::toggled,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_loudness_check_);

        audio_qc_true_peak_check_ = new QCheckBox(tr("真峰值(4×)"), row);
        audio_qc_true_peak_check_->setChecked(audio_qc_options_.enable_true_peak);
        audio_qc_true_peak_check_->setToolTip(tr("4× 过采样检测，最耗时的一项；关闭后 dBTP 回落为采样峰值"));
        connect(audio_qc_true_peak_check_, &QCheckBox::toggled,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_true_peak_check_);

        audio_qc_correlation_check_ = new QCheckBox(tr("声道相关性"), row);
        audio_qc_correlation_check_->setChecked(audio_qc_options_.enable_correlation);
        connect(audio_qc_correlation_check_, &QCheckBox::toggled,
                this, &AnalysisPanel::OnAudioQcOptionChanged);
        rl->addWidget(audio_qc_correlation_check_);

        rl->addStretch();
        layout->addWidget(row);
    }

    audio_qc_summary_label_ = new QLabel(
        tr("点击「开始分析」对音频流做一次完整解码体检：Integrated / Short-term / Momentary LUFS、"
           "LRA、真峰值、削波、静音段、声道相位与 metadata 一致性。"), audio_qc_tab_);
    audio_qc_summary_label_->setWordWrap(true);
    layout->addWidget(audio_qc_summary_label_);

    audio_qc_sub_tabs_ = new QTabWidget(audio_qc_tab_);
    audio_qc_sub_tabs_->setMinimumHeight(340);

    // ---- 子页 0: 响度与电平 ----
    {
        QWidget* page = new QWidget(audio_qc_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        audio_lufs_chart_object_ = new QChart();
        audio_lufs_chart_object_->setTitle(tr("响度曲线（M 400ms / S 3s / I 累计）"));
        audio_momentary_series_ = new QLineSeries();
        audio_momentary_series_->setName(tr("瞬时 M"));
        audio_short_term_series_ = new QLineSeries();
        audio_short_term_series_->setName(tr("短期 S"));
        audio_integrated_series_ = new QLineSeries();
        audio_integrated_series_->setName(tr("累计 I"));
        audio_target_series_ = new QLineSeries();
        audio_target_series_->setName(tr("目标"));
        audio_target_series_->setColor(QColor("#e53935"));
        audio_lufs_chart_object_->addSeries(audio_momentary_series_);
        audio_lufs_chart_object_->addSeries(audio_short_term_series_);
        audio_lufs_chart_object_->addSeries(audio_integrated_series_);
        audio_lufs_chart_object_->addSeries(audio_target_series_);
        audio_lufs_axis_x_ = new QValueAxis();
        audio_lufs_axis_x_->setTitleText(tr("时间 (s)"));
        audio_lufs_axis_y_ = new QValueAxis();
        audio_lufs_axis_y_->setTitleText(tr("LUFS"));
        audio_lufs_chart_object_->addAxis(audio_lufs_axis_x_, Qt::AlignBottom);
        audio_lufs_chart_object_->addAxis(audio_lufs_axis_y_, Qt::AlignLeft);
        for (QAbstractSeries* s : {static_cast<QAbstractSeries*>(audio_momentary_series_),
                                   static_cast<QAbstractSeries*>(audio_short_term_series_),
                                   static_cast<QAbstractSeries*>(audio_integrated_series_),
                                   static_cast<QAbstractSeries*>(audio_target_series_)}) {
            s->attachAxis(audio_lufs_axis_x_);
            s->attachAxis(audio_lufs_axis_y_);
        }
        audio_lufs_chart_ = new QChartView(audio_lufs_chart_object_, page);
        audio_lufs_chart_->setMinimumHeight(210);
        audio_lufs_chart_->setRenderHint(QPainter::Antialiasing);
        pl->addWidget(audio_lufs_chart_);

        audio_level_chart_object_ = new QChart();
        audio_level_chart_object_->setTitle(tr("电平曲线（RMS / 采样峰值 / 真峰值）"));
        audio_rms_series_ = new QLineSeries();
        audio_rms_series_->setName(tr("RMS dBFS"));
        audio_peak_series_ = new QLineSeries();
        audio_peak_series_->setName(tr("峰值 dBFS"));
        audio_true_peak_series_ = new QLineSeries();
        audio_true_peak_series_->setName(tr("真峰值 dBTP"));
        audio_true_peak_series_->setColor(QColor("#fb8c00"));
        audio_level_chart_object_->addSeries(audio_rms_series_);
        audio_level_chart_object_->addSeries(audio_peak_series_);
        audio_level_chart_object_->addSeries(audio_true_peak_series_);
        audio_level_axis_x_ = new QValueAxis();
        audio_level_axis_x_->setTitleText(tr("时间 (s)"));
        audio_level_axis_y_ = new QValueAxis();
        audio_level_axis_y_->setTitleText(tr("dB"));
        audio_level_chart_object_->addAxis(audio_level_axis_x_, Qt::AlignBottom);
        audio_level_chart_object_->addAxis(audio_level_axis_y_, Qt::AlignLeft);
        for (QAbstractSeries* s : {static_cast<QAbstractSeries*>(audio_rms_series_),
                                   static_cast<QAbstractSeries*>(audio_peak_series_),
                                   static_cast<QAbstractSeries*>(audio_true_peak_series_)}) {
            s->attachAxis(audio_level_axis_x_);
            s->attachAxis(audio_level_axis_y_);
        }
        audio_level_chart_ = new QChartView(audio_level_chart_object_, page);
        audio_level_chart_->setMinimumHeight(210);
        audio_level_chart_->setRenderHint(QPainter::Antialiasing);
        pl->addWidget(audio_level_chart_);

        audio_qc_sub_tabs_->addTab(page, tr("响度与电平"));
    }

    // ---- 子页 1: 静音与削波 ----
    {
        QWidget* page = new QWidget(audio_qc_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        audio_event_chart_object_ = new QChart();
        audio_event_chart_object_->setTitle(tr("静音段（方波）与削波点（三角）时间轴"));
        audio_silence_series_ = new QLineSeries();
        audio_silence_series_->setName(tr("静音段"));
        audio_silence_series_->setColor(QColor("#1e88e5"));
        audio_clip_series_ = new QScatterSeries();
        audio_clip_series_->setName(tr("削波"));
        audio_clip_series_->setMarkerSize(9.0);
        audio_clip_series_->setMarkerShape(QScatterSeries::MarkerShapeTriangle);
        audio_clip_series_->setColor(QColor("#e53935"));
        audio_clip_series_->setBorderColor(QColor("#e53935"));
        audio_event_chart_object_->addSeries(audio_silence_series_);
        audio_event_chart_object_->addSeries(audio_clip_series_);
        audio_event_axis_x_ = new QValueAxis();
        audio_event_axis_x_->setTitleText(tr("时间 (s)"));
        audio_event_axis_y_ = new QValueAxis();
        audio_event_axis_y_->setTitleText(tr("静音 0/1 ｜ 削波 1.5"));
        audio_event_axis_y_->setRange(-0.2, 1.8);
        audio_event_chart_object_->addAxis(audio_event_axis_x_, Qt::AlignBottom);
        audio_event_chart_object_->addAxis(audio_event_axis_y_, Qt::AlignLeft);
        audio_silence_series_->attachAxis(audio_event_axis_x_);
        audio_silence_series_->attachAxis(audio_event_axis_y_);
        audio_clip_series_->attachAxis(audio_event_axis_x_);
        audio_clip_series_->attachAxis(audio_event_axis_y_);
        audio_event_chart_ = new QChartView(audio_event_chart_object_, page);
        audio_event_chart_->setMinimumHeight(180);
        audio_event_chart_->setRenderHint(QPainter::Antialiasing);
        pl->addWidget(audio_event_chart_);

        QLabel* clip_hint = new QLabel(tr("点击任意一行跳转到该位置。"), page);
        pl->addWidget(clip_hint);
        audio_clip_table_ = new QTableWidget(0, 5, page);
        audio_clip_table_->setHorizontalHeaderLabels(
            {tr("起始"), tr("结束"), tr("声道"), tr("削波样本"), tr("峰值 dBFS")});
        audio_clip_table_->verticalHeader()->setVisible(false);
        audio_clip_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        audio_clip_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        audio_clip_table_->horizontalHeader()->setStretchLastSection(true);
        audio_clip_table_->setMinimumHeight(120);
        connect(audio_clip_table_, &QTableWidget::cellClicked,
                this, &AnalysisPanel::OnAudioQcClipCellClicked);
        pl->addWidget(audio_clip_table_);

        audio_silence_table_ = new QTableWidget(0, 4, page);
        audio_silence_table_->setHorizontalHeaderLabels(
            {tr("起始"), tr("结束"), tr("时长(s)"), tr("平均 RMS dBFS")});
        audio_silence_table_->verticalHeader()->setVisible(false);
        audio_silence_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        audio_silence_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        audio_silence_table_->horizontalHeader()->setStretchLastSection(true);
        audio_silence_table_->setMinimumHeight(120);
        connect(audio_silence_table_, &QTableWidget::cellClicked,
                this, &AnalysisPanel::OnAudioQcSilenceCellClicked);
        pl->addWidget(audio_silence_table_);

        audio_qc_sub_tabs_->addTab(page, tr("静音与削波"));
    }

    // ---- 子页 2: 声道与相位 ----
    {
        QWidget* page = new QWidget(audio_qc_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        audio_channel_chart_object_ = new QChart();
        audio_channel_chart_object_->setTitle(tr("声道能量（RMS / 峰值 dBFS）"));
        audio_channel_series_ = new QBarSeries();
        audio_channel_chart_object_->addSeries(audio_channel_series_);
        audio_channel_axis_x_ = new QBarCategoryAxis();
        audio_channel_chart_object_->addAxis(audio_channel_axis_x_, Qt::AlignBottom);
        audio_channel_axis_y_ = new QValueAxis();
        audio_channel_axis_y_->setTitleText(tr("dBFS"));
        audio_channel_chart_object_->addAxis(audio_channel_axis_y_, Qt::AlignLeft);
        audio_channel_series_->attachAxis(audio_channel_axis_x_);
        audio_channel_series_->attachAxis(audio_channel_axis_y_);
        audio_channel_chart_ = new QChartView(audio_channel_chart_object_, page);
        audio_channel_chart_->setMinimumHeight(200);
        audio_channel_chart_->setRenderHint(QPainter::Antialiasing);
        pl->addWidget(audio_channel_chart_);

        audio_corr_chart_object_ = new QChart();
        audio_corr_chart_object_->setTitle(tr("声道相关性（最差声道对，1=同相 / -1=反相）"));
        audio_corr_series_ = new QLineSeries();
        audio_corr_series_->setName(tr("相关性"));
        audio_corr_chart_object_->addSeries(audio_corr_series_);
        audio_corr_axis_x_ = new QValueAxis();
        audio_corr_axis_x_->setTitleText(tr("时间 (s)"));
        audio_corr_axis_y_ = new QValueAxis();
        audio_corr_axis_y_->setRange(-1.05, 1.05);
        audio_corr_chart_object_->addAxis(audio_corr_axis_x_, Qt::AlignBottom);
        audio_corr_chart_object_->addAxis(audio_corr_axis_y_, Qt::AlignLeft);
        audio_corr_series_->attachAxis(audio_corr_axis_x_);
        audio_corr_series_->attachAxis(audio_corr_axis_y_);
        audio_corr_chart_ = new QChartView(audio_corr_chart_object_, page);
        audio_corr_chart_->setMinimumHeight(200);
        audio_corr_chart_->setRenderHint(QPainter::Antialiasing);
        pl->addWidget(audio_corr_chart_);

        audio_metadata_table_ = new QTableWidget(0, 2, page);
        audio_metadata_table_->setHorizontalHeaderLabels({tr("项目"), tr("值")});
        audio_metadata_table_->verticalHeader()->setVisible(false);
        audio_metadata_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        audio_metadata_table_->horizontalHeader()->setStretchLastSection(true);
        audio_metadata_table_->setMinimumHeight(150);
        pl->addWidget(audio_metadata_table_);

        audio_qc_sub_tabs_->addTab(page, tr("声道与相位"));
    }

    // ---- 子页 3: 规则结果 ----
    {
        QWidget* page = new QWidget(audio_qc_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        QLabel* hint = new QLabel(
            tr("判定阈值取自「诊断与报告 → 规则与阈值」，改完会立即重算；"
               "这些音频问题也会一并进入诊断报告的评分。"), page);
        hint->setWordWrap(true);
        pl->addWidget(hint);

        audio_verdict_table_ = new QTableWidget(0, 4, page);
        audio_verdict_table_->setHorizontalHeaderLabels(
            {tr("规则"), tr("判定"), tr("实测值"), tr("说明")});
        audio_verdict_table_->verticalHeader()->setVisible(false);
        audio_verdict_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        audio_verdict_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        audio_verdict_table_->horizontalHeader()->setStretchLastSection(true);
        audio_verdict_table_->setMinimumHeight(220);
        pl->addWidget(audio_verdict_table_);

        audio_qc_sub_tabs_->addTab(page, tr("规则结果"));
    }

    layout->addWidget(audio_qc_sub_tabs_);
    AddPageWithScroll(audio_qc_tab_, tr("音频 QC"));
}

void AnalysisPanel::ApplyAudioQcOptionsFromUi() {
    audio_qc_options_.silence_threshold_dbfs = audio_qc_silence_spin_->value();
    audio_qc_options_.min_silence_seconds = audio_qc_min_silence_spin_->value();
    audio_qc_options_.clip_threshold = audio_qc_clip_spin_->value();
    audio_qc_options_.enable_loudness = audio_qc_loudness_check_->isChecked();
    audio_qc_options_.enable_true_peak = audio_qc_true_peak_check_->isChecked();
    audio_qc_options_.enable_correlation = audio_qc_correlation_check_->isChecked();
    diagnostics_options_.audio_qc_options = audio_qc_options_;
    diagnostics_options_.analyze_audio_qc = true;
}

void AnalysisPanel::OnStartAudioQcAnalysis() {
    ApplyAudioQcOptionsFromUi();
    StartDiagnosticsScan(diagnostics_options_);
}

void AnalysisPanel::OnCancelAudioQcAnalysis() { OnCancelDiagnostics(); }

void AnalysisPanel::OnAudioQcOptionChanged() {
    if (!audio_qc_silence_spin_) return;
    ApplyAudioQcOptionsFromUi();
    if (audio_qc_target_lufs_spin_ && has_diagnostics_result_) UpdateAudioQcCharts();
}

void AnalysisPanel::OnAudioQcClipCellClicked(int row, int) {
    if (!has_diagnostics_result_ || row < 0) return;
    const auto& events = diagnostics_result_.audio_qc.clipping_events;
    if (row >= static_cast<int>(events.size())) return;
    emit SeekRequested(events[static_cast<size_t>(row)].start_seconds);
}

void AnalysisPanel::OnAudioQcSilenceCellClicked(int row, int) {
    if (!has_diagnostics_result_ || row < 0) return;
    const auto& ranges = diagnostics_result_.audio_qc.silence_ranges;
    if (row >= static_cast<int>(ranges.size())) return;
    emit SeekRequested(ranges[static_cast<size_t>(row)].start_seconds);
}

void AnalysisPanel::UpdateAudioQcUi() {
    UpdateAudioQcSummary();
    UpdateAudioQcCharts();
    RebuildAudioQcClipTable();
    RebuildAudioQcSilenceTable();
    RebuildAudioQcVerdictTable();
    RebuildAudioQcMetadataTable();
}

void AnalysisPanel::UpdateAudioQcSummary() {
    if (!audio_qc_summary_label_) return;
    const auto& qc = diagnostics_result_.audio_qc;
    if (!has_diagnostics_result_ || !qc.analyzed) {
        audio_qc_summary_label_->setText(
            tr("暂无音频 QC 结果。点击「开始分析」扫描当前文件（需要有音频流且能解码）。"));
        return;
    }

    const double target = audio_qc_target_lufs_spin_ ? audio_qc_target_lufs_spin_->value() : -23.0;
    const double deviation = qc.integrated_lufs - target;
    QString verdict;
    QColor verdict_color;
    if (std::abs(deviation) <= 1.0) {
        verdict = tr("响度达标");
        verdict_color = QColor("#43a047");
    } else if (std::abs(deviation) <= 3.0) {
        verdict = tr("响度偏离");
        verdict_color = QColor("#fb8c00");
    } else {
        verdict = tr("响度超标");
        verdict_color = QColor("#e53935");
    }

    QString text;
    text += QStringLiteral("<b>%1</b>: %2 ｜ ").arg(tr("布局"), QString::fromStdString(qc.metadata.channel_layout));
    text += QStringLiteral("%1 Hz ｜ %2 s ｜ ").arg(qc.metadata.sample_rate).arg(qc.duration_seconds, 0, 'f', 2);
    text += QStringLiteral("<font color='%1'><b>%2</b></font><br>").arg(verdict_color.name(), verdict);
    text += tr("Integrated %1 LUFS（目标 %2，偏差 %3 LU）｜ Short-term 最大 %4 ｜ Momentary 最大 %5 ｜ LRA %6 LU<br>")
                .arg(AudioFormatDb(qc.integrated_lufs, model::kSilenceLufs))
                .arg(target, 0, 'f', 1)
                .arg(deviation, 0, 'f', 2)
                .arg(AudioFormatDb(qc.short_term_max_lufs, model::kSilenceLufs))
                .arg(AudioFormatDb(qc.momentary_max_lufs, model::kSilenceLufs))
                .arg(qc.loudness_range_lu, 0, 'f', 1);
    text += tr("真峰值 %1 dBTP ｜ 采样峰值 %2 dBFS ｜ RMS %3 dBFS ｜ DC %4<br>")
                .arg(AudioFormatDb(qc.true_peak_dbtp, model::kSilenceLevelDb))
                .arg(AudioFormatDb(qc.sample_peak_dbfs, model::kSilenceLevelDb))
                .arg(AudioFormatDb(qc.rms_dbfs, model::kSilenceLevelDb))
                .arg(qc.max_dc_offset, 0, 'f', 5);
    text += tr("削波 %1 样本 / %2 段 ｜ 静音 %3 段（占比 %4%，最长 %5 s）")
                .arg(qc.clipping_sample_count)
                .arg(qc.clipping_event_count)
                .arg(qc.silence_ranges.size())
                .arg(qc.silence_ratio * 100.0, 0, 'f', 1)
                .arg(qc.longest_silence_seconds, 0, 'f', 2);
    if (qc.correlation_available) {
        text += tr(" ｜ 相关性 min %1 / mean %2")
                    .arg(qc.correlation_min, 0, 'f', 3)
                    .arg(qc.correlation_mean, 0, 'f', 3);
    }
    for (const auto& note : qc.notes) {
        text += QStringLiteral("<br><font color='#888888'>%1</font>")
                    .arg(QString::fromStdString(note).toHtmlEscaped());
    }
    if (!qc.metadata.inconsistencies.empty()) {
        text += QStringLiteral("<br><font color='#e53935'>%1: %2</font>")
                    .arg(tr("metadata 不一致"),
                         QString::fromStdString(qc.metadata.inconsistencies.front()).toHtmlEscaped());
    }
    audio_qc_summary_label_->setText(text);
}

void AnalysisPanel::UpdateAudioQcCharts() {
    if (!audio_lufs_chart_object_) return;
    audio_momentary_series_->clear();
    audio_short_term_series_->clear();
    audio_integrated_series_->clear();
    audio_target_series_->clear();
    audio_rms_series_->clear();
    audio_peak_series_->clear();
    audio_true_peak_series_->clear();
    audio_silence_series_->clear();
    audio_clip_series_->clear();
    audio_corr_series_->clear();

    const auto& qc = diagnostics_result_.audio_qc;
    if (!has_diagnostics_result_ || !qc.analyzed) return;

    constexpr int kMaxPoints = 4000;
    const auto& points = qc.loudness_points;
    AppendAudioPoints(audio_momentary_series_, points, kMaxPoints,
                      &model::LoudnessPoint::momentary_lufs, model::kSilenceLufs);
    AppendAudioPoints(audio_short_term_series_, points, kMaxPoints,
                      &model::LoudnessPoint::short_term_lufs, model::kSilenceLufs);
    AppendAudioPoints(audio_integrated_series_, points, kMaxPoints,
                      &model::LoudnessPoint::integrated_lufs, model::kSilenceLufs);
    AppendAudioPoints(audio_rms_series_, points, kMaxPoints,
                      &model::LoudnessPoint::rms_dbfs, model::kSilenceLevelDb);
    AppendAudioPoints(audio_peak_series_, points, kMaxPoints,
                      &model::LoudnessPoint::sample_peak_dbfs, model::kSilenceLevelDb);
    AppendAudioPoints(audio_true_peak_series_, points, kMaxPoints,
                      &model::LoudnessPoint::true_peak_dbtp, model::kSilenceLevelDb);
    AppendAudioPoints(audio_corr_series_, points, kMaxPoints,
                      &model::LoudnessPoint::correlation, -2.0);

    // 目标响度参考线
    const double target = audio_qc_target_lufs_spin_ ? audio_qc_target_lufs_spin_->value() : -23.0;
    if (!points.empty()) {
        audio_target_series_->append(points.front().timestamp_seconds, target);
        audio_target_series_->append(points.back().timestamp_seconds, target);
    }

    // 静音段画成方波；削波点画在 y=1.5
    {
        SeriesBatch silence(audio_silence_series_);
        silence.Reserve(static_cast<int>(qc.silence_ranges.size() * 4));
        for (const auto& range : qc.silence_ranges) {
            silence.Add(range.start_seconds, 0.0);
            silence.Add(range.start_seconds, 1.0);
            silence.Add(range.end_seconds, 1.0);
            silence.Add(range.end_seconds, 0.0);
        }
    }
    {
        SeriesBatch clip(audio_clip_series_);
        clip.Reserve(static_cast<int>(qc.clipping_events.size()));
        for (const auto& event : qc.clipping_events) {
            clip.Add(event.start_seconds, 1.5);
        }
    }

    const double span = std::max(1.0, qc.duration_seconds);
    audio_lufs_axis_x_->setRange(0.0, span);
    audio_level_axis_x_->setRange(0.0, span);
    audio_event_axis_x_->setRange(0.0, span);
    audio_corr_axis_x_->setRange(0.0, span);
    audio_lufs_axis_y_->setRange(-60.0, 0.0);
    audio_level_axis_y_->setRange(-90.0, 6.0);

    // 声道能量柱状图
    audio_channel_series_->clear();
    while (audio_channel_series_->barSets().size() > 0) {
        audio_channel_series_->remove(audio_channel_series_->barSets().first());
    }
    if (!qc.channels.empty()) {
        QBarSet* rms_set = new QBarSet(tr("RMS dBFS"), audio_channel_series_);
        QBarSet* peak_set = new QBarSet(tr("峰值 dBFS"), audio_channel_series_);
        QStringList categories;
        for (const auto& ch : qc.channels) {
            const QString name = ch.name.empty() ? QString("Ch%1").arg(ch.index + 1)
                                                 : QString::fromStdString(ch.name);
            categories << name;
            *rms_set << (ch.rms_dbfs <= model::kSilenceLevelDb + 1.0 ? -120.0 : ch.rms_dbfs);
            *peak_set << (ch.peak_dbfs <= model::kSilenceLevelDb + 1.0 ? -120.0 : ch.peak_dbfs);
        }
        audio_channel_series_->append(rms_set);
        audio_channel_series_->append(peak_set);
        audio_channel_axis_x_->clear();
        audio_channel_axis_x_->append(categories);
        audio_channel_axis_y_->setRange(-90.0, 6.0);
    }
}

void AnalysisPanel::RebuildAudioQcClipTable() {
    if (!audio_clip_table_) return;
    audio_clip_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;
    const auto& qc = diagnostics_result_.audio_qc;
    const int rows = std::min<int>(static_cast<int>(qc.clipping_events.size()), 2000);
    audio_clip_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& e = qc.clipping_events[static_cast<size_t>(i)];
        SetTableItemText(audio_clip_table_, i, 0, QString::number(e.start_seconds, 'f', 3));
        SetTableItemText(audio_clip_table_, i, 1, QString::number(e.end_seconds, 'f', 3));
        QString channel = (e.channel < 0) ? tr("全部")
                                          : QString::fromStdString(
                                                e.channel < static_cast<int>(qc.channels.size())
                                                    ? qc.channels[static_cast<size_t>(e.channel)].name
                                                    : std::string());
        SetTableItemText(audio_clip_table_, i, 2, channel);
        SetTableItemText(audio_clip_table_, i, 3, QString::number(e.sample_count));
        SetTableItemText(audio_clip_table_, i, 4, QString::number(
            e.peak > 0.0 ? 20.0 * std::log10(e.peak) : model::kSilenceLevelDb, 'f', 2));
    }
}

void AnalysisPanel::RebuildAudioQcSilenceTable() {
    if (!audio_silence_table_) return;
    audio_silence_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;
    const auto& qc = diagnostics_result_.audio_qc;
    const int rows = std::min<int>(static_cast<int>(qc.silence_ranges.size()), 2000);
    audio_silence_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& r = qc.silence_ranges[static_cast<size_t>(i)];
        SetTableItemText(audio_silence_table_, i, 0, QString::number(r.start_seconds, 'f', 3));
        SetTableItemText(audio_silence_table_, i, 1, QString::number(r.end_seconds, 'f', 3));
        SetTableItemText(audio_silence_table_, i, 2, QString::number(r.duration_seconds, 'f', 3));
        SetTableItemText(audio_silence_table_, i, 3, QString::number(r.rms_dbfs, 'f', 1));
    }
}

void AnalysisPanel::RebuildAudioQcVerdictTable() {
    if (!audio_verdict_table_) return;
    audio_verdict_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;

    const size_t count = sizeof(kAudioQcRuleIds) / sizeof(kAudioQcRuleIds[0]);
    audio_verdict_table_->setRowCount(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i) {
        const QString id = QString::fromUtf8(kAudioQcRuleIds[i]);
        QString name = id;
        QString threshold_text;
        for (const auto& rule : current_qc_report_.rules) {
            if (rule.id == id.toStdString()) {
                name = QString::fromStdString(rule.name);
                threshold_text = QString::fromStdString(
                    rule.unit.empty() ? QString::number(rule.threshold, 'f', 2).toStdString()
                                      : (QString::number(rule.threshold, 'f', 2) +
                                         QString::fromStdString(rule.unit)).toStdString());
                break;
            }
        }
        QString detail;
        const QString verdict = AudioVerdictText(current_qc_report_, id, &detail);
        SetTableItemText(audio_verdict_table_, static_cast<int>(i), 0, name);
        SetTableItemText(audio_verdict_table_, static_cast<int>(i), 1, verdict);
        SetTableItemText(audio_verdict_table_, static_cast<int>(i), 2, threshold_text);
        SetTableItemText(audio_verdict_table_, static_cast<int>(i), 3, detail);
        QTableWidgetItem* item = audio_verdict_table_->item(static_cast<int>(i), 1);
        if (item != nullptr) {
            item->setForeground(verdict == tr("失败")   ? QColor("#e53935")
                                : verdict == tr("警告") ? QColor("#fb8c00")
                                : verdict == tr("提示") ? QColor("#1e88e5")
                                                        : QColor("#43a047"));
        }
    }
}

void AnalysisPanel::RebuildAudioQcMetadataTable() {
    if (!audio_metadata_table_) return;
    audio_metadata_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;
    const auto& meta = diagnostics_result_.audio_qc.metadata;

    auto add_row = [this](const QString& key, const QString& value) {
        const int row = audio_metadata_table_->rowCount();
        audio_metadata_table_->insertRow(row);
        SetTableItemText(audio_metadata_table_, row, 0, key);
        SetTableItemText(audio_metadata_table_, row, 1, value);
    };

    add_row(tr("声道布局"), QString::fromStdString(meta.channel_layout));
    add_row(tr("声道数"), QString::number(meta.channels));
    add_row(tr("采样率"), QString::number(meta.sample_rate) + tr(" Hz"));
    add_row(tr("采样格式"), meta.sample_format.empty() ? tr("未知")
                                                       : QString::fromStdString(meta.sample_format));
    add_row(tr("位深"), meta.bits_per_sample > 0 ? QString::number(meta.bits_per_sample) + tr(" bit")
                                                 : tr("未知"));
    add_row(tr("音频时长"), QString::number(meta.stream_duration_seconds, 'f', 3) + tr(" s"));
    add_row(tr("容器时长"), QString::number(meta.container_duration_seconds, 'f', 3) + tr(" s"));
    add_row(tr("视频时长"), meta.has_video
                                ? QString::number(meta.video_duration_seconds, 'f', 3) + tr(" s")
                                : tr("无视频流"));
    add_row(tr("与容器时差"), QString::number(meta.container_delta_seconds, 'f', 3) + tr(" s"));
    if (meta.has_video) {
        add_row(tr("与视频时差"), QString::number(meta.video_delta_seconds, 'f', 3) + tr(" s"));
    }
    add_row(tr("一致性问题"), meta.inconsistencies.empty()
                                 ? tr("无")
                                 : QString::fromStdString(meta.inconsistencies.front()));
}

void AnalysisPanel::OnExportAudioQcCsv() {
    if (!has_diagnostics_result_ || !diagnostics_result_.audio_qc.analyzed) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次音频 QC 分析。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出响度曲线 CSV"),
        QString::fromStdString(current_video_path_) + QStringLiteral("_audioqc.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件: %1").arg(path));
        return;
    }
    QTextStream out(&file);
    out << "time_s,momentary_lufs,short_term_lufs,integrated_lufs,rms_dbfs,"
           "sample_peak_dbfs,true_peak_dbtp,correlation,silent\n";
    for (const auto& p : diagnostics_result_.audio_qc.loudness_points) {
        out << QString::number(p.timestamp_seconds, 'f', 3) << ','
            << QString::number(p.momentary_lufs, 'f', 2) << ','
            << QString::number(p.short_term_lufs, 'f', 2) << ','
            << QString::number(p.integrated_lufs, 'f', 2) << ','
            << QString::number(p.rms_dbfs, 'f', 2) << ','
            << QString::number(p.sample_peak_dbfs, 'f', 2) << ','
            << QString::number(p.true_peak_dbtp, 'f', 2) << ','
            << QString::number(p.correlation, 'f', 4) << ','
            << (p.silent ? 1 : 0) << '\n';
    }
    file.close();
    QMessageBox::information(this, tr("导出完成"),
                             tr("已导出 %1 个采样点。")
                                 .arg(diagnostics_result_.audio_qc.loudness_points.size()));
}

// ==========================================================================
// 色彩与 HDR 分析页
// 与「码率与 GOP」「音频 QC」「诊断与报告」共用同一次全文件扫描结果
// ==========================================================================
void AnalysisPanel::SetupColorHdrTab() {
    color_hdr_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(color_hdr_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 第一行: 标题 + 开始/取消/导出
    {
        QWidget* row = new QWidget(color_hdr_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("色彩与 HDR 元数据"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        color_hdr_start_button_ = new QPushButton(tr("开始分析"), row);
        color_hdr_start_button_->setToolTip(
            tr("读取视频流的 primaries / transfer / matrix / range / bit depth / 色度采样，"
               "并汇总 HDR10 静态元数据（母版显示、MaxCLL/MaxFALL）与 Dolby Vision 配置记录。"
               "与「码率与 GOP」「音频 QC」「诊断与报告」共用同一次扫描。"));
        connect(color_hdr_start_button_, &QPushButton::clicked,
                this, &AnalysisPanel::OnStartColorHdrAnalysis);
        rl->addWidget(color_hdr_start_button_);

        color_hdr_cancel_button_ = new QPushButton(tr("取消"), row);
        color_hdr_cancel_button_->setEnabled(false);
        connect(color_hdr_cancel_button_, &QPushButton::clicked,
                this, &AnalysisPanel::OnCancelColorHdrAnalysis);
        rl->addWidget(color_hdr_cancel_button_);

        QPushButton* export_btn = new QPushButton(tr("导出 CSV"), row);
        connect(export_btn, &QPushButton::clicked, this, &AnalysisPanel::OnExportColorHdrCsv);
        rl->addWidget(export_btn);
        layout->addWidget(row);
    }

    color_hdr_progress_bar_ = new QProgressBar(color_hdr_tab_);
    color_hdr_progress_bar_->setRange(0, 100);
    color_hdr_progress_bar_->setValue(0);
    color_hdr_progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(color_hdr_progress_bar_);

    // 第二行: 选项
    {
        QWidget* row = new QWidget(color_hdr_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        color_hdr_probe_frame_check_ = new QCheckBox(tr("解码首帧读取动态元数据"), row);
        color_hdr_probe_frame_check_->setChecked(color_hdr_options_.probe_decoded_frame);
        color_hdr_probe_frame_check_->setToolTip(
            tr("容器/码流层没有 HDR 静态元数据时，解码首帧读取 AVFrame side data "
               "（HDR10+ / DV RPU / HDR Vivid）。关闭后只依赖容器标注，速度最快。"));
        connect(color_hdr_probe_frame_check_, &QCheckBox::toggled,
                this, &AnalysisPanel::OnColorHdrOptionChanged);
        rl->addWidget(color_hdr_probe_frame_check_);
        rl->addStretch();
        layout->addWidget(row);
    }

    color_hdr_summary_label_ = new QLabel(
        tr("点击「开始分析」读取当前视频流的色彩与 HDR 元数据。"
           "正确的组合应为：BT.709 + BT.709 + BT.709（SDR）或 BT.2020 + PQ/HLG + BT.2020 NCL（HDR）。"),
        color_hdr_tab_);
    color_hdr_summary_label_->setWordWrap(true);
    layout->addWidget(color_hdr_summary_label_);

    color_hdr_sub_tabs_ = new QTabWidget(color_hdr_tab_);
    color_hdr_sub_tabs_->setMinimumHeight(340);

    auto make_info_table = [](QWidget* parent) {
        QTableWidget* table = new QTableWidget(0, 3, parent);
        table->setHorizontalHeaderLabels({tr("项目"), tr("值"), tr("说明")});
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->horizontalHeader()->setStretchLastSection(true);
        table->setMinimumHeight(260);
        return table;
    };

    // ---- 子页 0: 色彩信息 ----
    {
        QWidget* page = new QWidget(color_hdr_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        color_info_table_ = make_info_table(page);
        pl->addWidget(color_info_table_);
        color_hdr_sub_tabs_->addTab(page, tr("色彩信息"));
    }

    // ---- 子页 1: HDR 元数据 ----
    {
        QWidget* page = new QWidget(color_hdr_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        hdr_info_table_ = make_info_table(page);
        pl->addWidget(hdr_info_table_);
        color_hdr_sub_tabs_->addTab(page, tr("HDR 元数据"));
    }

    // ---- 子页 2: 异常组合 ----
    {
        QWidget* page = new QWidget(color_hdr_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        QLabel* hint = new QLabel(
            tr("下面列出的是由色彩/HDR 规则判定的异常组合，判定阈值取自"
               "「诊断与报告 → 规则与阈值」，这些项目同样计入诊断报告评分。"), page);
        hint->setWordWrap(true);
        pl->addWidget(hint);

        color_issue_table_ = new QTableWidget(0, 4, page);
        color_issue_table_->setHorizontalHeaderLabels(
            {tr("严重度"), tr("规则"), tr("说明"), tr("建议")});
        color_issue_table_->verticalHeader()->setVisible(false);
        color_issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        color_issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        color_issue_table_->horizontalHeader()->setStretchLastSection(true);
        color_issue_table_->setMinimumHeight(240);
        pl->addWidget(color_issue_table_);

        color_hdr_sub_tabs_->addTab(page, tr("异常组合"));
    }

    layout->addWidget(color_hdr_sub_tabs_);
    AddPageWithScroll(color_hdr_tab_, tr("色彩与 HDR"));
}

void AnalysisPanel::ApplyColorHdrOptionsFromUi() {
    color_hdr_options_.probe_decoded_frame = color_hdr_probe_frame_check_ &&
                                             color_hdr_probe_frame_check_->isChecked();
    diagnostics_options_.color_hdr_options = color_hdr_options_;
    diagnostics_options_.analyze_color_hdr = true;
}

void AnalysisPanel::OnStartColorHdrAnalysis() {
    ApplyColorHdrOptionsFromUi();
    StartDiagnosticsScan(diagnostics_options_);
}

void AnalysisPanel::OnCancelColorHdrAnalysis() { OnCancelDiagnostics(); }

void AnalysisPanel::OnColorHdrOptionChanged() {
    if (!color_hdr_probe_frame_check_) return;
    ApplyColorHdrOptionsFromUi();
}

void AnalysisPanel::UpdateColorHdrUi() {
    UpdateColorHdrSummary();
    RebuildColorHdrTables();
    RebuildColorHdrIssueTable();
}

void AnalysisPanel::UpdateColorHdrSummary() {
    if (!color_hdr_summary_label_) return;
    const auto& analysis = diagnostics_result_.color_hdr;
    if (!has_diagnostics_result_ || !analysis.analyzed) {
        color_hdr_summary_label_->setText(
            tr("暂无色彩/HDR 结果。点击「开始分析」扫描当前文件（需要有视频流）。"));
        return;
    }

    const model::ColorInfo& color = analysis.color;
    const model::HdrMetadataInfo& hdr = analysis.hdr;

    auto piece = [](const std::string& text) {
        return text.empty() ? QObject::tr("未标注") : QString::fromStdString(text);
    };

    QColor verdict_color = QColor("#43a047");   // 绿
    QString verdict = tr("色彩标注完整");
    int missing_count = 0;
    if (!color.PrimariesSpecified()) ++missing_count;
    if (!color.TransferSpecified()) ++missing_count;
    if (!color.MatrixSpecified()) ++missing_count;
    if (!color.RangeSpecified()) ++missing_count;
    if (missing_count > 0 || (hdr.hdr && !hdr.mastering_display.Complete() &&
                              color.transfer == model::TransferKind::Pq)) {
        verdict_color = QColor("#fb8c00");      // 橙
        verdict = tr("存在待核查项");
    }

    QString text;
    text += QStringLiteral("<b>%1</b>: %2 ｜ ").arg(tr("HDR 格式"), piece(hdr.format_name));
    text += QStringLiteral("%1 ｜ %2 ｜ %3 ｜ %4<br>")
                .arg(piece(color.primaries_name), piece(color.transfer_name),
                     piece(color.matrix_name), piece(color.range_name));
    text += QStringLiteral("%1: %2 ｜ %3 ｜ ")
                .arg(tr("像素格式"), piece(color.pixel_format.name),
                     color.EffectiveBitDepth() > 0
                         ? (QString::number(color.EffectiveBitDepth()) + QLatin1String(" bit"))
                         : tr("未标注"));
    text += QStringLiteral("%1<br>").arg(
        color.pixel_format.chroma_subsampling.empty()
            ? QString::fromStdString(color.codec_name + " / " + color.profile_name)
            : piece(color.pixel_format.chroma_subsampling));
    text += QStringLiteral("<font color='%1'><b>%2</b></font>")
                .arg(verdict_color.name(), verdict);

    if (hdr.mastering_display.has_luminance) {
        text += QStringLiteral(" ｜ MaxCLL %1 / MaxFALL %2 cd/m²")
                    .arg(hdr.content_light.max_cll)
                    .arg(hdr.content_light.max_fall);
    }
    if (hdr.dolby_vision.present) {
        text += QStringLiteral(" ｜ DV %1").arg(
            QString::fromStdString(hdr.dolby_vision.ProfileText()));
    }
    for (const auto& note : analysis.notes) {
        text += QStringLiteral("<br><font color='#e53935'>%1</font>")
                    .arg(QString::fromStdString(note).toHtmlEscaped());
    }
    color_hdr_summary_label_->setText(text);
}

void AnalysisPanel::RebuildColorHdrTables() {
    if (!color_info_table_ || !hdr_info_table_) return;

    const auto fill = [this](QTableWidget* table,
                             const std::vector<analyzer::ColorKeyValueRow>& rows) {
        table->setRowCount(0);
        table->setRowCount(static_cast<int>(rows.size()));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& row = rows[static_cast<size_t>(i)];
            table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(row.key)));
            QTableWidgetItem* value_item = new QTableWidgetItem(QString::fromStdString(row.value));
            // 缺失/异常项用醒目颜色标记
            const QString lower = QString::fromStdString(row.value).toLower();
            if (row.value == "缺失" || row.value == "未标注" || lower.startsWith("unknown")) {
                value_item->setForeground(QColor("#fb8c00"));
                value_item->setFont([value_item] {
                    QFont f = value_item->font();
                    f.setBold(true);
                    return f;
                }());
            }
            table->setItem(i, 1, value_item);
            table->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(row.note)));
        }
        table->resizeColumnsToContents();
    };

    const auto& analysis = diagnostics_result_.color_hdr;
    if (!has_diagnostics_result_ || !analysis.analyzed) {
        color_info_table_->setRowCount(0);
        hdr_info_table_->setRowCount(0);
        return;
    }
    fill(color_info_table_, analyzer::BuildColorRows(analysis));
    fill(hdr_info_table_, analyzer::BuildHdrRows(analysis));
}

void AnalysisPanel::RebuildColorHdrIssueTable() {
    if (!color_issue_table_) return;
    color_issue_table_->setRowCount(0);
    if (!has_diagnostics_result_) return;

    auto severity_color = [](model::IssueSeverity severity) -> QColor {
        switch (severity) {
            case model::IssueSeverity::Critical: return QColor("#c62828");
            case model::IssueSeverity::Error:    return QColor("#e53935");
            case model::IssueSeverity::Warning:  return QColor("#fb8c00");
            case model::IssueSeverity::Info:     return QColor("#1e88e5");
        }
        return QColor("#888888");
    };

    int row = 0;
    for (const auto& issue : current_qc_report_.issues) {
        if (issue.category != model::IssueCategory::ColorHdr) continue;
        color_issue_table_->insertRow(row);
        QTableWidgetItem* severity_item =
            new QTableWidgetItem(QString::fromStdString(issue.SeverityText()));
        severity_item->setForeground(severity_color(issue.severity));
        {
            QFont f = severity_item->font();
            f.setBold(true);
            severity_item->setFont(f);
        }
        color_issue_table_->setItem(row, 0, severity_item);
        color_issue_table_->setItem(row, 1, new QTableWidgetItem(
                                                QString::fromStdString(issue.rule_id)));
        color_issue_table_->setItem(row, 2, new QTableWidgetItem(
                                                QString::fromStdString(issue.detail)));
        color_issue_table_->setItem(row, 3, new QTableWidgetItem(
                                                QString::fromStdString(issue.suggestion)));
        ++row;
    }
    if (row == 0) {
        color_issue_table_->insertRow(0);
        color_issue_table_->setItem(0, 0, new QTableWidgetItem(tr("无")));
        color_issue_table_->setItem(0, 1,
                                    new QTableWidgetItem(tr("未发现异常的色彩/HDR 组合")));
        color_issue_table_->setItem(0, 2, new QTableWidgetItem(tr("")));
        color_issue_table_->setItem(0, 3, new QTableWidgetItem(tr("")));
    }
    color_issue_table_->resizeColumnsToContents();
}

void AnalysisPanel::OnExportColorHdrCsv() {
    if (!has_diagnostics_result_ || !diagnostics_result_.color_hdr.analyzed) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次色彩/HDR 分析。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出色彩与 HDR 信息 CSV"),
        QString::fromStdString(current_video_path_) + QStringLiteral("_colorhdr.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件: %1").arg(path));
        return;
    }

    auto csv_field = [](const QString& text) {
        QString out = text;
        out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + out + QLatin1Char('"');
    };

    const auto& analysis = diagnostics_result_.color_hdr;
    QTextStream out(&file);
    out << "\xEF\xBB\xBF";  // UTF-8 BOM for Excel
    out << "分组,项目,值,说明\n";
    auto write_rows = [&](const char* /*unused*/, const QString& group,
                          const std::vector<analyzer::ColorKeyValueRow>& rows) {
        for (const auto& row : rows) {
            out << csv_field(group) << ','
                << csv_field(QString::fromStdString(row.key)) << ','
                << csv_field(QString::fromStdString(row.value)) << ','
                << csv_field(QString::fromStdString(row.note)) << '\n';
        }
    };
    write_rows(nullptr, tr("色彩信息"), analyzer::BuildColorRows(analysis));
    write_rows(nullptr, tr("HDR 元数据"), analyzer::BuildHdrRows(analysis));
    file.close();

    const int rows = static_cast<int>(analyzer::BuildColorRows(analysis).size() +
                                      analyzer::BuildHdrRows(analysis).size());
    QMessageBox::information(this, tr("导出完成"), tr("已导出 %1 行。").arg(rows));
}

void AnalysisPanel::SetupDiagnosticsTab() {
    diagnostics_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(diagnostics_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 第一行: 标题 + 控制按钮
    {
        QWidget* row = new QWidget(diagnostics_tab_);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("诊断与报告（全文件扫描）"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        qc_start_button_ = new QPushButton(tr("开始分析"), row);
        qc_start_button_->setToolTip(tr("对当前文件做一次完整 demux 扫描并按规则生成诊断报告"));
        connect(qc_start_button_, &QPushButton::clicked, this, &AnalysisPanel::OnStartDiagnostics);
        rl->addWidget(qc_start_button_);

        qc_cancel_button_ = new QPushButton(tr("取消"), row);
        qc_cancel_button_->setEnabled(false);
        connect(qc_cancel_button_, &QPushButton::clicked, this, &AnalysisPanel::OnCancelDiagnostics);
        rl->addWidget(qc_cancel_button_);

        qc_export_button_ = new QPushButton(tr("导出报告"), row);
        qc_export_button_->setEnabled(false);
        connect(qc_export_button_, &QPushButton::clicked, this, &AnalysisPanel::OnExportQcReport);
        rl->addWidget(qc_export_button_);
        layout->addWidget(row);
    }

    qc_progress_bar_ = new QProgressBar(diagnostics_tab_);
    qc_progress_bar_->setRange(0, 100);
    qc_progress_bar_->setValue(0);
    qc_progress_bar_->setTextVisible(true);
    qc_progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(qc_progress_bar_);

    qc_summary_label_ = new QLabel(
        tr("点击「开始分析」对当前文件做一次完整扫描，将按内置 QC 规则输出问题清单与评分。"));
    qc_summary_label_->setWordWrap(true);
    layout->addWidget(qc_summary_label_);

    qc_sub_tabs_ = new QTabWidget(diagnostics_tab_);
    qc_sub_tabs_->setMinimumHeight(360);

    // ---- 子页 0: 问题清单 ----
    {
        QWidget* page = new QWidget(qc_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        qc_chart_object_ = new QChart();
        qc_chart_object_->setTitle(tr("逐秒码率 / 帧率"));
        qc_bitrate_series_ = new QLineSeries();
        qc_bitrate_series_->setName(tr("码率 (kbps)"));
        qc_fps_series_ = new QLineSeries();
        qc_fps_series_->setName(tr("帧率 (fps)"));
        qc_chart_object_->addSeries(qc_bitrate_series_);
        qc_chart_object_->addSeries(qc_fps_series_);
        qc_axis_x_ = new QValueAxis();
        qc_axis_x_->setTitleText(tr("时间 (s)"));
        qc_axis_bitrate_ = new QValueAxis();
        qc_axis_bitrate_->setTitleText(tr("kbps"));
        qc_axis_fps_ = new QValueAxis();
        qc_axis_fps_->setTitleText(tr("fps"));
        qc_chart_object_->addAxis(qc_axis_x_, Qt::AlignBottom);
        qc_chart_object_->addAxis(qc_axis_bitrate_, Qt::AlignLeft);
        qc_chart_object_->addAxis(qc_axis_fps_, Qt::AlignRight);
        qc_bitrate_series_->attachAxis(qc_axis_x_);
        qc_bitrate_series_->attachAxis(qc_axis_bitrate_);
        qc_fps_series_->attachAxis(qc_axis_x_);
        qc_fps_series_->attachAxis(qc_axis_fps_);
        qc_chart_view_ = new QChartView(qc_chart_object_, page);
        qc_chart_view_->setMinimumHeight(220);
        qc_chart_view_->setRenderHint(QPainter::Antialiasing);
        pl->addWidget(qc_chart_view_);

        qc_issue_table_ = new QTableWidget(0, 6, page);
        qc_issue_table_->setHorizontalHeaderLabels(
            {tr("严重度"), tr("类别"), tr("问题"), tr("位置"), tr("说明"), tr("建议")});
        qc_issue_table_->verticalHeader()->setVisible(false);
        qc_issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        qc_issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        qc_issue_table_->horizontalHeader()->setStretchLastSection(true);
        qc_issue_table_->setMinimumHeight(200);
        pl->addWidget(qc_issue_table_);

        qc_sub_tabs_->addTab(page, tr("问题清单"));
    }

    // ---- 子页 1: 规则与阈值 ----
    {
        QWidget* page = new QWidget(qc_sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        QLabel* hint = new QLabel(
            tr("勾选启用列可开关规则，双击阈值可直接修改；修改后会立即用当前扫描结果重算报告。"),
            page);
        hint->setWordWrap(true);
        pl->addWidget(hint);

        qc_rule_table_ = new QTableWidget(0, 6, page);
        qc_rule_table_->setHorizontalHeaderLabels(
            {tr("启用"), tr("规则"), tr("类别"), tr("严重度"), tr("判定"), tr("阈值")});
        qc_rule_table_->verticalHeader()->setVisible(false);
        qc_rule_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        qc_rule_table_->horizontalHeader()->setStretchLastSection(true);
        pl->addWidget(qc_rule_table_);
        connect(qc_rule_table_, &QTableWidget::itemChanged,
                this, &AnalysisPanel::OnQcRuleItemChanged);

        QPushButton* reset_btn = new QPushButton(tr("恢复默认规则"), page);
        connect(reset_btn, &QPushButton::clicked, this, &AnalysisPanel::OnResetQcRules);
        pl->addWidget(reset_btn, 0, Qt::AlignRight);

        qc_sub_tabs_->addTab(page, tr("规则与阈值"));
    }

    layout->addWidget(qc_sub_tabs_);

    // 时间轴与同步子页
    SetupTimelineDiagnosticsSubPage();

    // 规则表初始内容
    RebuildRuleTable();

    // 扫描线程回调 -> UI 线程
    connect(&diagnostics_coordinator_, &analyzer::AnalysisCoordinator::ProgressReported,
            this, &AnalysisPanel::OnDiagnosticsProgress);
    connect(&diagnostics_coordinator_, &analyzer::AnalysisCoordinator::AnalysisFinished,
            this, &AnalysisPanel::OnDiagnosticsFinished);
    connect(&diagnostics_coordinator_, &analyzer::AnalysisCoordinator::AnalysisFailed,
            this, &AnalysisPanel::OnDiagnosticsFailed);

    AddPageWithScroll(diagnostics_tab_, tr("诊断与报告"));
}

// 「诊断与报告」与「码率与 GOP」共用同一次全文件扫描，避免重复 I/O
void AnalysisPanel::StartDiagnosticsScan(const analyzer::AnalysisOptions& options) {
    if (current_video_path_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("请先打开一个媒体文件。"));
        return;
    }
    if (diagnostics_coordinator_.IsRunning()) {
        QMessageBox::information(this, tr("提示"), tr("分析正在进行中。"));
        return;
    }

    diagnostics_start_time_ = std::chrono::steady_clock::now();
    has_diagnostics_result_ = false;
    qc_issue_table_->setRowCount(0);
    qc_export_button_->setEnabled(false);
    qc_start_button_->setEnabled(false);
    qc_cancel_button_->setEnabled(true);
    qc_progress_bar_->setValue(0);
    qc_progress_bar_->setFormat(tr("准备中 %p%"));
    qc_summary_label_->setText(tr("正在扫描: %1").arg(QString::fromStdString(current_video_path_)));

    // 两个页面共用同一次扫描：同步「码率与 GOP」页的控件状态
    bitrate_gop_start_button_->setEnabled(false);
    bitrate_gop_cancel_button_->setEnabled(true);
    bitrate_gop_progress_bar_->setValue(0);
    bitrate_gop_progress_bar_->setFormat(tr("准备中 %p%"));

    // 「音频 QC」与「色彩与 HDR」也共用同一次扫描：同步这两页的控件状态
    audio_qc_start_button_->setEnabled(false);
    audio_qc_cancel_button_->setEnabled(true);
    audio_qc_progress_bar_->setValue(0);
    audio_qc_progress_bar_->setFormat(tr("准备中 %p%"));

    color_hdr_start_button_->setEnabled(false);
    color_hdr_cancel_button_->setEnabled(true);
    color_hdr_progress_bar_->setValue(0);
    color_hdr_progress_bar_->setFormat(tr("准备中 %p%"));

    diagnostics_generation_ = diagnostics_coordinator_.StartAnalysis(current_video_path_, options);
}

void AnalysisPanel::OnStartDiagnostics() {
    StartDiagnosticsScan(diagnostics_options_);
}

void AnalysisPanel::StartDiagnosticsScanForCurrentFile() {
    // 播放器打开失败时的自动扫描入口: 静默失败路径, 不弹"请先打开文件"提示框
    if (current_video_path_.empty() || diagnostics_coordinator_.IsRunning()) return;
    StartDiagnosticsScan(diagnostics_options_);
}

void AnalysisPanel::OnCancelDiagnostics() {
    diagnostics_coordinator_.Cancel();
    qc_cancel_button_->setEnabled(false);
    bitrate_gop_cancel_button_->setEnabled(false);
    audio_qc_cancel_button_->setEnabled(false);
    color_hdr_cancel_button_->setEnabled(false);
    qc_progress_bar_->setFormat(tr("取消中..."));
    bitrate_gop_progress_bar_->setFormat(tr("取消中..."));
    audio_qc_progress_bar_->setFormat(tr("取消中..."));
    color_hdr_progress_bar_->setFormat(tr("取消中..."));
}

void AnalysisPanel::OnDiagnosticsProgress(quint64 generation, double percent, const QString& stage) {
    if (generation != diagnostics_generation_) return;  // 旧任务的回调直接丢弃
    qc_progress_bar_->setValue(static_cast<int>(percent));
    qc_progress_bar_->setFormat(stage + " %p%");
    bitrate_gop_progress_bar_->setValue(static_cast<int>(percent));
    bitrate_gop_progress_bar_->setFormat(stage + " %p%");
    audio_qc_progress_bar_->setValue(static_cast<int>(percent));
    audio_qc_progress_bar_->setFormat(stage + " %p%");
    color_hdr_progress_bar_->setValue(static_cast<int>(percent));
    color_hdr_progress_bar_->setFormat(stage + " %p%");
}

void AnalysisPanel::OnDiagnosticsFinished(quint64 generation, bool completed,
                                          const analyzer::AnalysisResult& result) {
    if (generation != diagnostics_generation_) return;

    diagnostics_result_ = result;
    has_diagnostics_result_ = true;

    const double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - diagnostics_start_time_)
                                  .count();

    {
        VE_PERF("EvaluateDiagnostics(QC 规则 + 时间轴 + 图表)");
        EvaluateDiagnostics();
    }
    current_qc_report_.analysis_elapsed_ms = elapsed_ms;

    qc_start_button_->setEnabled(true);
    qc_cancel_button_->setEnabled(false);
    qc_export_button_->setEnabled(true);
    qc_progress_bar_->setValue(100);
    qc_progress_bar_->setFormat(completed ? tr("分析完成") : tr("已取消（结果不完整）"));

    bitrate_gop_start_button_->setEnabled(true);
    bitrate_gop_cancel_button_->setEnabled(false);
    bitrate_gop_progress_bar_->setValue(100);
    bitrate_gop_progress_bar_->setFormat(completed ? tr("分析完成") : tr("已取消（结果不完整）"));
    {
        VE_PERF("UpdateBitrateGopUi");
        UpdateBitrateGopUi();
    }

    UpdateQcSummary();
    {
        VE_PERF("UpdateAudioQcUi");
        UpdateAudioQcUi();   // 音频 QC 页与码率/GOP、诊断报告共用同一次扫描结果
    }
    {
        VE_PERF("UpdateColorHdrUi");
        UpdateColorHdrUi();  // 色彩与 HDR 页同样共用同一次扫描结果
    }

    // MP4 样本表：扫描跑过就顺带刷新容器页（与打开文件时那次解析结果一致）
    if (result.mp4_samples_analyzed && result.mp4_samples.valid) {
        VE_PERF("诊断后刷新 MP4 样本表");
        mp4_samples_ = result.mp4_samples;
        UpdateMp4SampleSummary();
        RebuildMp4SampleTable();
        RebuildMp4IssueTable();
        RebuildMp4FragmentTable();
    }
}

void AnalysisPanel::OnDiagnosticsFailed(quint64 generation, const QString& message) {
    if (generation != diagnostics_generation_) return;
    qc_start_button_->setEnabled(true);
    qc_cancel_button_->setEnabled(false);
    qc_progress_bar_->setValue(0);
    qc_progress_bar_->setFormat(tr("失败"));
    qc_summary_label_->setText(message);

    bitrate_gop_start_button_->setEnabled(true);
    bitrate_gop_cancel_button_->setEnabled(false);
    bitrate_gop_progress_bar_->setValue(0);
    bitrate_gop_progress_bar_->setFormat(tr("失败"));

    // 不再弹模态框: 失败原因已写入上方汇总标签, 在诊断页内直接可见
    // (异常文件打开时也会自动触发扫描, 弹窗会打断"打开即分析"的流程)。
}

void AnalysisPanel::EvaluateDiagnostics() {
    if (!has_diagnostics_result_) return;
    current_qc_report_ = qc_rule_engine_.Evaluate(diagnostics_result_);

    // 时间轴与同步诊断：直接复用全文件扫描的 demux 层结果
    timeline_result_ = diagnostics_result_.timeline;
    timeline_offline_ = true;
    RefreshTimelineUi();

    RebuildIssueTable();
    UpdateQcChart();
    UpdateQcSummary();
    RebuildAudioQcVerdictTable();   // 音频 QC 页的判定表复用同一份报告
    RebuildColorHdrIssueTable();    // 色彩与 HDR 页的异常组合同样来自这份报告
}

void AnalysisPanel::RebuildIssueTable() {
    if (!qc_issue_table_) return;
    qc_issue_table_->setRowCount(0);
    qc_issue_table_->setRowCount(static_cast<int>(current_qc_report_.issues.size()));

    for (int i = 0; i < static_cast<int>(current_qc_report_.issues.size()); ++i) {
        const auto& issue = current_qc_report_.issues[i];
        SetTableItemText(qc_issue_table_, i, 0, QString::fromStdString(issue.SeverityText()));
        SetTableItemText(qc_issue_table_, i, 1, QString::fromStdString(issue.CategoryText()));
        SetTableItemText(qc_issue_table_, i, 2, QString::fromStdString(issue.title));
        SetTableItemText(qc_issue_table_, i, 3, QString::fromStdString(issue.range.ToString()));
        SetTableItemText(qc_issue_table_, i, 4, QString::fromStdString(issue.detail));
        SetTableItemText(qc_issue_table_, i, 5, QString::fromStdString(issue.suggestion));

        QColor color = QColor("#1565c0");
        switch (issue.severity) {
            case model::IssueSeverity::Critical: color = QColor("#c62828"); break;
            case model::IssueSeverity::Error:    color = QColor("#e53935"); break;
            case model::IssueSeverity::Warning:  color = QColor("#ef6c00"); break;
            case model::IssueSeverity::Info:     color = QColor("#1565c0"); break;
        }
        if (QTableWidgetItem* cell = qc_issue_table_->item(i, 0)) {
            cell->setForeground(color);
        }
    }
    qc_issue_table_->resizeColumnsToContents();
}

void AnalysisPanel::UpdateQcSummary() {
    if (!qc_summary_label_) return;
    if (!has_diagnostics_result_) {
        qc_summary_label_->setText(
            tr("点击「开始分析」对当前文件做一次完整扫描，将按内置 QC 规则输出问题清单与评分。"));
        return;
    }

    const auto& report = current_qc_report_;
    const auto& result = diagnostics_result_;
    const int critical = report.CountBySeverity(model::IssueSeverity::Critical);
    const int error = report.CountBySeverity(model::IssueSeverity::Error);
    const int warning = report.CountBySeverity(model::IssueSeverity::Warning);
    const int info = report.CountBySeverity(model::IssueSeverity::Info);

    qc_summary_label_->setText(
        tr("评分 <b>%1</b>/100（%2）｜ 致命 %3 · 错误 %4 · 警告 %5 · 提示 %6<br>"
           "容器 %7 ｜ 时长 %8 s ｜ 平均码率 %9 kbps ｜ 视频流 %10 · 音频流 %11 ｜ 包 %12 ｜ 关键帧 %13")
            .arg(QString::number(report.score, 'f', 1))
            .arg(QString::fromStdString(report.verdict))
            .arg(critical).arg(error).arg(warning).arg(info)
            .arg(QString::fromStdString(result.container_format))
            .arg(QString::number(result.duration_seconds, 'f', 3))
            .arg(result.overall_bitrate_bps / 1000)
            .arg(result.VideoStreamCount())
            .arg(result.AudioStreamCount())
            .arg(static_cast<qlonglong>(result.total_packets))
            .arg(static_cast<qlonglong>(result.key_frame_count)));
}

void AnalysisPanel::UpdateQcChart() {
    if (!qc_bitrate_series_ || !has_diagnostics_result_) return;
    qc_bitrate_series_->clear();
    qc_fps_series_->clear();

    const auto& bitrate = diagnostics_result_.video_bitrate_kbps.IsEmpty()
                              ? diagnostics_result_.total_bitrate_kbps
                              : diagnostics_result_.video_bitrate_kbps;
    {
        SeriesBatch batch(qc_bitrate_series_);
        batch.Reserve(static_cast<int>(bitrate.samples.size()));
        for (const auto& sample : bitrate.samples) {
            batch.Add(sample.timestamp_seconds, sample.value);
        }
    }
    {
        SeriesBatch batch(qc_fps_series_);
        batch.Reserve(static_cast<int>(diagnostics_result_.video_fps.samples.size()));
        for (const auto& sample : diagnostics_result_.video_fps.samples) {
            batch.Add(sample.timestamp_seconds, sample.value);
        }
    }

    double max_t = 1.0;
    if (!bitrate.samples.empty()) max_t = std::max(max_t, bitrate.samples.back().timestamp_seconds);
    if (!diagnostics_result_.video_fps.samples.empty()) {
        max_t = std::max(max_t, diagnostics_result_.video_fps.samples.back().timestamp_seconds);
    }
    qc_axis_x_->setRange(0, max_t);
    qc_axis_bitrate_->setRange(0, std::max(1.0, bitrate.Max() * 1.2));
    qc_axis_fps_->setRange(0, std::max(1.0, diagnostics_result_.video_fps.Max() * 1.2));
}

void AnalysisPanel::RebuildRuleTable() {
    if (!qc_rule_table_) return;
    qc_rule_table_updating_ = true;
    qc_rule_table_->setRowCount(0);

    const auto& rules = qc_rule_engine_.rules();
    qc_rule_table_->setRowCount(static_cast<int>(rules.size()));
    for (int i = 0; i < static_cast<int>(rules.size()); ++i) {
        const auto& rule = rules[i];

        QTableWidgetItem* enable_item = new QTableWidgetItem();
        enable_item->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
        enable_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        qc_rule_table_->setItem(i, 0, enable_item);

        QTableWidgetItem* name_item = new QTableWidgetItem(QString::fromStdString(rule.name));
        name_item->setToolTip(QString::fromStdString(rule.description));
        name_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        qc_rule_table_->setItem(i, 1, name_item);

        SetTableItemText(qc_rule_table_, i, 2, QString::fromStdString(ToString(rule.category)));
        SetTableItemText(qc_rule_table_, i, 3, QString::fromStdString(ToString(rule.severity)));
        SetTableItemText(qc_rule_table_, i, 4, QString::fromStdString(ToString(rule.op)));

        QTableWidgetItem* threshold_item =
            new QTableWidgetItem(QString::number(rule.threshold, 'f', 2) +
                                 QString::fromStdString(rule.unit));
        threshold_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        threshold_item->setToolTip(tr("双击修改阈值（仅需填写数值部分）"));
        qc_rule_table_->setItem(i, 5, threshold_item);
    }
    qc_rule_table_->resizeColumnsToContents();
    qc_rule_table_updating_ = false;
}

void AnalysisPanel::OnQcRuleItemChanged(QTableWidgetItem* item) {
    if (!item || qc_rule_table_updating_) return;
    const QString rule_id = item->data(Qt::UserRole).toString();
    if (rule_id.isEmpty()) return;

    model::QcRule* rule = model::FindQcRule(qc_rule_engine_.rules(), rule_id.toStdString());
    if (!rule) return;

    if (item->column() == 0) {
        rule->enabled = (item->checkState() == Qt::Checked);
    } else if (item->column() == 5) {
        // 允许 "12.5s" / "12.5" 这类输入，取前导数字部分
        const QString text = item->text().trimmed();
        int end = 0;
        while (end < text.size() &&
               (text.at(end).isDigit() || text.at(end) == QLatin1Char('.') ||
                text.at(end) == QLatin1Char('-') || text.at(end) == QLatin1Char('+'))) {
            ++end;
        }
        bool ok = false;
        const double value = text.left(end).toDouble(&ok);
        if (!ok) {
            // 还原显示
            qc_rule_table_updating_ = true;
            item->setText(QString::number(rule->threshold, 'f', 2) +
                          QString::fromStdString(rule->unit));
            qc_rule_table_updating_ = false;
            return;
        }
        rule->threshold = value;
    } else {
        return;
    }

    if (has_diagnostics_result_) EvaluateDiagnostics();
}

void AnalysisPanel::OnResetQcRules() {
    qc_rule_engine_.SetRules(model::DefaultQcRules());
    RebuildRuleTable();
    if (has_diagnostics_result_) EvaluateDiagnostics();
}

void AnalysisPanel::OnExportQcReport() {
    if (!has_diagnostics_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先执行一次分析。"));
        return;
    }

    const QString default_name =
        QString::fromStdString(current_qc_report_.file_name.empty()
                                   ? std::string("diagnostics")
                                   : current_qc_report_.file_name) + "_diagnostics.html";
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出诊断报告"), default_name,
        tr("HTML 报告 (*.html);;JSON 报告 (*.json);;CSV 报告 (*.csv);;文本报告 (*.txt)"));
    if (filename.isEmpty()) return;

    if (utils::ReportExporter::ExportQcReport(filename.toStdString(), current_qc_report_)) {
        QMessageBox::information(this, tr("成功"),
            tr("诊断报告已导出到:\n%1").arg(filename));
    } else {
        QMessageBox::warning(this, tr("失败"), tr("导出诊断报告失败。"));
    }
}

// ===========================================================================
// 时间轴与同步诊断（诊断与报告页的子页）
// ===========================================================================

void AnalysisPanel::SetupTimelineDiagnosticsSubPage() {
    timeline_sub_ = new QWidget(qc_sub_tabs_);
    QVBoxLayout* layout = new QVBoxLayout(timeline_sub_);
    layout->setContentsMargins(2, 2, 2, 2);

    timeline_diag_summary_label_ = new QLabel(
        tr("开启「事件与时间轴」分析并播放，或在本页执行一次全文件扫描，"
           "将输出 PTS/DTS、音视频同步、帧间隔与 VFR/CFR 判定。"));
    timeline_diag_summary_label_->setWordWrap(true);
    layout->addWidget(timeline_diag_summary_label_);

    // 帧间隔曲线 + 问题标记散点
    timeline_issue_chart_object_ = new QChart();
    timeline_issue_chart_object_->setTitle(tr("帧间隔与问题分布"));
    timeline_interval_series_ = new QLineSeries();
    timeline_interval_series_->setName(tr("帧间隔 (ms)"));
    timeline_marker_series_ = new QScatterSeries();
    timeline_marker_series_->setName(tr("问题"));
    timeline_marker_series_->setMarkerSize(10.0);
    timeline_issue_chart_object_->addSeries(timeline_interval_series_);
    timeline_issue_chart_object_->addSeries(timeline_marker_series_);
    timeline_chart_axis_x_ = new QValueAxis();
    timeline_chart_axis_x_->setTitleText(tr("时间 (s)"));
    timeline_chart_axis_y_ = new QValueAxis();
    timeline_chart_axis_y_->setTitleText(tr("间隔 (ms)"));
    timeline_issue_chart_object_->addAxis(timeline_chart_axis_x_, Qt::AlignBottom);
    timeline_issue_chart_object_->addAxis(timeline_chart_axis_y_, Qt::AlignLeft);
    timeline_interval_series_->attachAxis(timeline_chart_axis_x_);
    timeline_interval_series_->attachAxis(timeline_chart_axis_y_);
    timeline_marker_series_->attachAxis(timeline_chart_axis_x_);
    timeline_marker_series_->attachAxis(timeline_chart_axis_y_);
    timeline_issue_chart_ = new QChartView(timeline_issue_chart_object_, timeline_sub_);
    timeline_issue_chart_->setMinimumHeight(220);
    timeline_issue_chart_->setRenderHint(QPainter::Antialiasing);
    layout->addWidget(timeline_issue_chart_);

    connect(timeline_marker_series_, &QScatterSeries::hovered,
            this, &AnalysisPanel::OnTimelineMarkerHovered);

    timeline_issue_table_ = new QTableWidget(0, 6, timeline_sub_);
    timeline_issue_table_->setHorizontalHeaderLabels(
        {tr("类型"), tr("严重度"), tr("流"), tr("时间码"), tr("次数"), tr("说明")});
    timeline_issue_table_->verticalHeader()->setVisible(false);
    timeline_issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline_issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    timeline_issue_table_->horizontalHeader()->setStretchLastSection(true);
    timeline_issue_table_->setMinimumHeight(200);
    layout->addWidget(timeline_issue_table_);

    QWidget* button_row = new QWidget(timeline_sub_);
    QHBoxLayout* bl = new QHBoxLayout(button_row);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addStretch();
    QPushButton* jump_button = new QPushButton(tr("跳转到问题帧"), button_row);
    jump_button->setToolTip(tr("按当前选中问题的位置执行 seek"));
    connect(jump_button, &QPushButton::clicked, this, &AnalysisPanel::OnJumpToTimelineIssue);
    bl->addWidget(jump_button);
    layout->addWidget(button_row);

    qc_sub_tabs_->addTab(timeline_sub_, tr("时间轴与同步"));
}

void AnalysisPanel::OnTimelinePacket(const model::PacketTiming& timing) {
    timeline_analyzer_.OnPacket(timing);
    timeline_dirty_ = true;
}

void AnalysisPanel::OnFrameTiming(const model::FrameTimingInfo& timing) {
    timeline_analyzer_.OnFrame(timing);
    timeline_dirty_ = true;
}

void AnalysisPanel::RefreshTimelineUi() {
    UpdateTimelineDiagnosticSummary();
    UpdateTimelineDiagnosticChart();

    if (!timeline_issue_table_) return;
    timeline_issue_table_->setRowCount(0);
    const auto& issues = timeline_result_.issues;
    timeline_issue_table_->setRowCount(static_cast<int>(issues.size()));
    for (int i = 0; i < static_cast<int>(issues.size()); ++i) {
        const auto& issue = issues[i];
        SetTableItemText(timeline_issue_table_, i, 0, QString::fromStdString(issue.title));
        SetTableItemText(timeline_issue_table_, i, 1, QString::fromStdString(issue.SeverityText()));
        SetTableItemText(timeline_issue_table_, i, 2,
                         issue.stream_index >= 0 ? QString::number(issue.stream_index)
                                                 : tr("文件级"));
        SetTableItemText(timeline_issue_table_, i, 3,
                         QString::fromStdString(model::FormatTimestamp(issue.range.start_seconds)));
        SetTableItemText(timeline_issue_table_, i, 4, QString::number(issue.occurrence_count));
        SetTableItemText(timeline_issue_table_, i, 5, QString::fromStdString(issue.detail));

        QColor color = QColor("#1565c0");
        switch (issue.severity) {
            case model::IssueSeverity::Critical: color = QColor("#c62828"); break;
            case model::IssueSeverity::Error:    color = QColor("#e53935"); break;
            case model::IssueSeverity::Warning:  color = QColor("#ef6c00"); break;
            case model::IssueSeverity::Info:     color = QColor("#1565c0"); break;
        }
        if (QTableWidgetItem* cell = timeline_issue_table_->item(i, 1)) {
            cell->setForeground(color);
        }
    }
    timeline_issue_table_->resizeColumnsToContents();
}

void AnalysisPanel::UpdateTimelineDiagnosticSummary() {
    if (!timeline_diag_summary_label_) return;
    const auto& r = timeline_result_;
    if (!r.has_data) {
        timeline_diag_summary_label_->setText(
            tr("开启「时间轴与同步」分析并播放，或在本页执行一次全文件扫描，"
               "将输出 PTS/DTS、音视频同步、帧间隔与 VFR/CFR 判定。"));
        return;
    }

    timeline_diag_summary_label_->setText(
        tr("数据源: %1 ｜ 最大音视频偏移 <b>%2</b> ms ｜ 首帧偏移 %3 ms ｜ 平均帧间隔 %4 ms"
           "（σ %5 ms, %6~%7 ms）｜ 帧率判定 <b>%8</b> ｜ 视频 %9 ms / 音频 %10 ms"
           " ｜ 帧 %11 ｜ 关键帧 %12 ｜ 问题 %13")
            .arg(timeline_offline_ ? tr("全文件扫描") : tr("播放实时"))
            .arg(QString::number(r.max_av_offset_ms, 'f', 1))
            .arg(QString::number(r.av_start_offset_ms, 'f', 1))
            .arg(QString::number(r.avg_frame_interval_ms, 'f', 2))
            .arg(QString::number(r.frame_interval_stddev_ms, 'f', 2))
            .arg(QString::number(r.min_frame_interval_ms, 'f', 2))
            .arg(QString::number(r.max_frame_interval_ms, 'f', 2))
            .arg(QString::fromStdString(r.FrameRateVerdict()))
            .arg(QString::number(r.video_duration_ms, 'f', 0))
            .arg(QString::number(r.audio_duration_ms, 'f', 0))
            .arg(r.frame_count)
            .arg(r.key_frame_count)
            .arg(static_cast<int>(r.issues.size())));
}

void AnalysisPanel::UpdateTimelineDiagnosticChart() {
    if (!timeline_interval_series_) return;
    timeline_interval_series_->clear();
    timeline_marker_series_->clear();
    timeline_marker_issue_index_.clear();

    const auto& r = timeline_result_;
    double max_interval = 1.0;
    double max_time = 1.0;
    {
        SeriesBatch batch(timeline_interval_series_);
        batch.Reserve(static_cast<int>(r.frame_interval_ms.samples.size()));
        for (const auto& sample : r.frame_interval_ms.samples) {
            const double t = sample.timestamp_seconds / 1000.0;   // ms -> s
            batch.Add(t, sample.value);
            max_interval = std::max(max_interval, sample.value);
            max_time = std::max(max_time, t);
        }
    }

    // 问题标记：按严重度着色，y 取该时刻的帧间隔（无数据则取 0）
    for (int i = 0; i < static_cast<int>(r.issues.size()); ++i) {
        const auto& issue = r.issues[i];
        const double t = issue.range.start_seconds;
        if (t < 0.0) continue;
        const double y = r.frame_interval_ms.ValueAt(issue.range.start_seconds * 1000.0);
        *timeline_marker_series_ << QPointF(t, y);
        timeline_marker_issue_index_.append(i);

        QColor color = QColor("#1565c0");
        switch (issue.severity) {
            case model::IssueSeverity::Critical: color = QColor("#c62828"); break;
            case model::IssueSeverity::Error:    color = QColor("#e53935"); break;
            case model::IssueSeverity::Warning:  color = QColor("#ef6c00"); break;
            case model::IssueSeverity::Info:     color = QColor("#1565c0"); break;
        }
        timeline_marker_series_->setColor(color);  // 颜色以最后一组为准（散点整体着色）
    }

    timeline_chart_axis_x_->setRange(0, max_time);
    timeline_chart_axis_y_->setRange(0, max_interval * 1.2);
}

void AnalysisPanel::OnTimelineMarkerHovered(const QPointF& point, bool state) {
    if (!state || !timeline_marker_series_) return;

    // 命中点定位到问题序号（按下标顺序一一对应）
    const QList<QPointF> points = timeline_marker_series_->points();
    int index = -1;
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i < points.size(); ++i) {
        const double dx = points[i].x() - point.x();
        const double dy = points[i].y() - point.y();
        const double dist = dx * dx + dy * dy;
        if (dist < best) { best = dist; index = i; }
    }
    if (index < 0 || index >= timeline_marker_issue_index_.size()) return;

    const int issue_index = timeline_marker_issue_index_[index];
    if (issue_index < 0 || issue_index >= static_cast<int>(timeline_result_.issues.size())) return;
    const auto& issue = timeline_result_.issues[issue_index];

    QToolTip::showText(QCursor::pos(),
        tr("流 #%1\n时间码: %2\n%3 (%4)\n实测: %5 ms / 阈值: %6 ms\n%7")
            .arg(issue.stream_index)
            .arg(QString::fromStdString(model::FormatTimestamp(issue.range.start_seconds)))
            .arg(QString::fromStdString(issue.title))
            .arg(QString::fromStdString(issue.SeverityText()))
            .arg(QString::number(issue.metric_value, 'f', 2))
            .arg(QString::number(issue.threshold, 'f', 2))
            .arg(QString::fromStdString(issue.detail)));
}

void AnalysisPanel::OnJumpToTimelineIssue() {
    if (!timeline_issue_table_) return;
    const int row = timeline_issue_table_->currentRow();
    if (row < 0 || row >= static_cast<int>(timeline_result_.issues.size())) {
        QMessageBox::information(this, tr("提示"), tr("请先在问题列表中选择一条记录。"));
        return;
    }
    const double seconds = timeline_result_.issues[row].range.start_seconds;
    if (seconds < 0.0) {
        QMessageBox::information(this, tr("提示"), tr("该问题为文件级问题，没有可跳转的时间点。"));
        return;
    }
    emit SeekRequested(seconds);
}

} // namespace ui
} // namespace videoeye
