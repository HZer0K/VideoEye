#include "ui/analysis_panel/EventTimelineView.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QHeaderView>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTextStream>

#include <deque>
#include <algorithm>
#include <cmath>

namespace videoeye {
namespace ui {

namespace {
constexpr size_t kMaxEventRecords = 5000;
constexpr size_t kMaxSyncRecords = 5000;
constexpr size_t kMaxTimelineRecords = 5000;
constexpr int kMaxChartSamples = 300;

// 裁剪记录向量到指定上限，移除最早的多余记录并重置表格（与 AnalysisPanel 内那份同语义）。
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

EventTimelineView::EventTimelineView(QWidget* parent)
    : QWidget(parent) {
    event_analysis_sub_tabs_ = new QTabWidget(this);
    event_analysis_sub_tabs_->setDocumentMode(true);

    // 子页顺序沿用原 AnalysisPanel::SetupEventAnalysisTab：异常事件 / 时间轴 / 同步分析
    SetupEventTab();
    SetupTimelineTab();
    SetupSyncTab();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(event_analysis_sub_tabs_);
}

EventTimelineView::~EventTimelineView() = default;

void EventTimelineView::SetFeatureHooks(std::function<bool(int)> is_enabled,
                                        std::function<void(int, bool)> set_enabled) {
    is_enabled_ = std::move(is_enabled);
    set_enabled_ = std::move(set_enabled);
    // 控件建得比钩子早，这里必须补一次回写（评审 P1-4）。
    SyncTogglesFromHooks();
}

void EventTimelineView::SyncTogglesFromHooks() {
    // 三个 QCheckBox 在构造函数里就已建好，那时钩子还不存在，只能按默认值创建。
    // 不回写的话界面显示"启用分析"，而面板 feature_enabled_ 里其实是关的 ——
    // 数据处理被静默过滤，用户看到的界面与实际行为不一致。
    const struct {
        QCheckBox* box;
        ViewFeature feature;
    } toggles[] = {
        {event_toggle_, ViewFeature::Event},
        {sync_toggle_, ViewFeature::Sync},
        {timeline_toggle_, ViewFeature::Timeline},
    };

    for (const auto& t : toggles) {
        if (!t.box) continue;
        // 没有钩子（独立使用本视图时）就保持控件现状，不做无意义的改写。
        const bool enabled =
            is_enabled_ ? is_enabled_(static_cast<int>(t.feature)) : t.box->isChecked();
        // 必须屏蔽信号：setChecked() 会触发 toggled，而那个槽会回调 set_enabled_
        // 改写面板的 feature_enabled_ 并转发 AnalysisFeatureToggled —— 那是"用户操作"
        // 的语义，不该由一次初始化同步伪造出来。
        const QSignalBlocker blocker(t.box);
        t.box->setChecked(enabled);
    }
}

// ---------------------------------------------------------------------------
// 建页
// ---------------------------------------------------------------------------

void EventTimelineView::SetupEventTab() {
    event_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(event_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    event_summary_label_ = new QLabel(tr("总事件数: 0 | 错误: 0 | 警告: 0 | 信息: 0"), event_tab_);
    toolbar_layout->addWidget(event_summary_label_, 1);

    export_event_csv_button_ = new QPushButton(tr("导出 CSV"), event_tab_);
    toolbar_layout->addWidget(export_event_csv_button_);

    event_toggle_ = new QCheckBox(tr("启用分析"), event_tab_);
    event_toggle_->setChecked(is_enabled_ ? is_enabled_(static_cast<int>(ViewFeature::Event)) : true);
    connect(event_toggle_, &QCheckBox::toggled, this, [this](bool checked) {
        if (set_enabled_) set_enabled_(static_cast<int>(ViewFeature::Event), checked);
    });
    toolbar_layout->addWidget(event_toggle_);
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

    connect(export_event_csv_button_, &QPushButton::clicked, this, &EventTimelineView::OnExportEventCsv);

    event_analysis_sub_tabs_->addTab(event_tab_, tr("异常事件"));
}

void EventTimelineView::SetupSyncTab() {
    sync_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(sync_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    sync_summary_label_ = new QLabel(tr("样本数: 0 | 平均偏移: 0.00 ms | 最大偏移: 0.00 ms"), sync_tab_);
    toolbar_layout->addWidget(sync_summary_label_, 1);

    export_sync_csv_button_ = new QPushButton(tr("导出 CSV"), sync_tab_);
    toolbar_layout->addWidget(export_sync_csv_button_);

    sync_toggle_ = new QCheckBox(tr("启用分析"), sync_tab_);
    sync_toggle_->setChecked(is_enabled_ ? is_enabled_(static_cast<int>(ViewFeature::Sync)) : true);
    connect(sync_toggle_, &QCheckBox::toggled, this, [this](bool checked) {
        if (set_enabled_) set_enabled_(static_cast<int>(ViewFeature::Sync), checked);
    });
    toolbar_layout->addWidget(sync_toggle_);
    layout->addLayout(toolbar_layout);

    QGroupBox* chart_group = new QGroupBox(tr("音视频时间差"), sync_tab_);
    QVBoxLayout* chart_layout = new QVBoxLayout(chart_group);
    sync_chart_ = new MetricChartWidget(sync_tab_);
    sync_chart_->setMinimumHeight(220);
    sync_chart_->setMinimumWidth(280);
    sync_chart_->SetTitle(tr("A-V 差值 (ms)"));
    sync_chart_->SetLegendVisible(false);
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

    sync_series_ = sync_chart_->AddLineSeries(QString(), QColor("#42a5f5"));
    sync_axis_x_ = sync_chart_->AxisX();
    sync_axis_y_ = sync_chart_->AxisY();
    sync_axis_x_->SetLabelFormat("%d");
    sync_axis_y_->SetLabelFormat("%.0f");
    sync_axis_y_->SetRange(-1.0, 1.0);
    sync_axis_x_->SetTitleText(tr("样本序号"));
    sync_axis_y_->SetTitleText(tr("ms"));

    connect(export_sync_csv_button_, &QPushButton::clicked, this, &EventTimelineView::OnExportSyncCsv);

    event_analysis_sub_tabs_->addTab(sync_tab_, tr("同步分析"));
}

void EventTimelineView::SetupTimelineTab() {
    timeline_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(timeline_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    timeline_summary_label_ = new QLabel(tr("事件数: 0 | 视频关键帧: 0 | 音频采样: 0 | 异常事件: 0"), timeline_tab_);
    toolbar_layout->addWidget(timeline_summary_label_, 1);

    export_timeline_csv_button_ = new QPushButton(tr("导出 CSV"), timeline_tab_);
    toolbar_layout->addWidget(export_timeline_csv_button_);

    timeline_toggle_ = new QCheckBox(tr("启用分析"), timeline_tab_);
    timeline_toggle_->setChecked(is_enabled_ ? is_enabled_(static_cast<int>(ViewFeature::Timeline)) : true);
    connect(timeline_toggle_, &QCheckBox::toggled, this, [this](bool checked) {
        if (set_enabled_) set_enabled_(static_cast<int>(ViewFeature::Timeline), checked);
    });
    toolbar_layout->addWidget(timeline_toggle_);
    layout->addLayout(toolbar_layout);

    QGroupBox* chart_group = new QGroupBox(tr("统一时间轴"), timeline_tab_);
    QVBoxLayout* chart_layout = new QVBoxLayout(chart_group);
    timeline_chart_ = new MetricChartWidget(timeline_tab_);
    timeline_chart_->setMinimumHeight(220);
    timeline_chart_->setMinimumWidth(280);
    timeline_chart_->SetTitle(tr("统一时间轴"));
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

    timeline_video_series_ = timeline_chart_->AddLineSeries(tr("视频关键帧"), QColor("#42a5f5"));
    timeline_audio_series_ = timeline_chart_->AddLineSeries(tr("音频采样"), QColor("#66bb6a"));
    timeline_event_series_ = timeline_chart_->AddLineSeries(tr("异常事件"), QColor("#e53935"));
    timeline_video_series_->SetPointsVisible(true);
    timeline_audio_series_->SetPointsVisible(true);
    timeline_event_series_->SetPointsVisible(true);

    timeline_axis_x_ = timeline_chart_->AxisX();
    timeline_axis_y_ = timeline_chart_->AxisY();
    timeline_axis_x_->SetLabelFormat("%.2f");
    timeline_axis_y_->SetRange(0.5, 3.5);
    timeline_axis_y_->SetTickCount(4);
    timeline_axis_x_->SetTitleText(tr("时间 (s)"));

    connect(export_timeline_csv_button_, &QPushButton::clicked, this, &EventTimelineView::OnExportTimelineCsv);

    event_analysis_sub_tabs_->addTab(timeline_tab_, tr("时间轴"));
}

// ---------------------------------------------------------------------------
// 实时数据接入（由 AnalysisPanel 薄转发）
// ---------------------------------------------------------------------------

void EventTimelineView::ResetAnalysisEventList() {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Event))) return;
    analysis_event_records_.clear();
    event_table_synced_record_count_ = 0;
    event_table_dirty_ = false;
    event_summary_dirty_ = true;
    if (event_table_) {
        event_table_->setRowCount(0);
    }
    UpdateEventSummary();
}

void EventTimelineView::AppendAnalysisEvent(const model::AnalysisEvent& event_info) {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Event))) return;
    if (!event_table_) {
        return;
    }

    AnalysisEventRecord record;
    record.index = event_info.index;
    record.severity = QString::fromStdString(event_info.severity);
    record.type = QString::fromStdString(event_info.type);
    record.stream_index = event_info.stream_index;
    record.pts = event_info.pts;
    record.timestamp_seconds = event_info.timestamp_seconds;
    record.summary = QString::fromStdString(event_info.summary);
    record.detail = QString::fromStdString(event_info.detail);

    analysis_event_records_.push_back(record);
    event_table_dirty_ = true;
    event_summary_dirty_ = true;
    TrimRecords(analysis_event_records_, event_table_synced_record_count_, event_table_, event_table_dirty_, kMaxEventRecords);
}

void EventTimelineView::ResetSyncSampleList() {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Sync))) return;
    sync_sample_records_.clear();
    sync_table_synced_record_count_ = 0;
    sync_table_dirty_ = false;
    sync_summary_dirty_ = true;
    sync_chart_values_.clear();
    if (sync_table_) {
        sync_table_->setRowCount(0);
    }
    if (sync_series_) {
        sync_series_->Clear();
    }
    if (sync_axis_x_) {
        sync_axis_x_->SetRange(0, 1);
    }
    if (sync_axis_y_) {
        sync_axis_y_->SetRange(-1.0, 1.0);
    }
    UpdateSyncSummary();
}

void EventTimelineView::AppendSyncSample(const model::SyncSample& sample) {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Sync))) return;
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

    // 同步采样同时喂给时间轴分析器（构建音视频偏移曲线）。
    // 广播信号而不是直调诊断页：谁来消费由 AnalysisPanel 决定，本视图不再认识
    // DiagnosticsPage，两者可以各自单独构造与测试。
    emit SyncSampleReceived(sample.audio_timestamp_seconds * 1000.0,
                            sample.video_timestamp_seconds * 1000.0);
}

void EventTimelineView::ResetTimelineEventList() {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Timeline))) return;
    timeline_event_records_.clear();
    timeline_table_synced_record_count_ = 0;
    timeline_table_dirty_ = false;
    timeline_summary_dirty_ = true;
    if (timeline_table_) {
        timeline_table_->setRowCount(0);
    }
    if (timeline_video_series_) {
        timeline_video_series_->Clear();
    }
    if (timeline_audio_series_) {
        timeline_audio_series_->Clear();
    }
    if (timeline_event_series_) {
        timeline_event_series_->Clear();
    }
    if (timeline_axis_x_) {
        timeline_axis_x_->SetRange(0.0, 1.0);
    }
    if (timeline_axis_y_) {
        timeline_axis_y_->SetRange(0.5, 3.5);
    }
    UpdateTimelineSummary();
}

void EventTimelineView::AppendTimelineEvent(const model::TimelineEvent& event) {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Timeline))) return;
    if (!timeline_table_) {
        return;
    }

    TimelineEventRecord record;
    record.index = event.index;
    record.category = QString::fromStdString(event.category);
    record.timestamp_seconds = event.timestamp_seconds;
    record.label = QString::fromStdString(event.label);
    record.detail = QString::fromStdString(event.detail);

    timeline_event_records_.push_back(record);
    timeline_table_dirty_ = true;
    timeline_summary_dirty_ = true;
    TrimRecords(timeline_event_records_, timeline_table_synced_record_count_, timeline_table_, timeline_table_dirty_, kMaxTimelineRecords);
}

// ---------------------------------------------------------------------------
// 重建 / 摘要 / 导出 / 刷新
// ---------------------------------------------------------------------------

void EventTimelineView::RebuildEventTable() {
    if (!event_table_) return;
    event_table_->setUpdatesEnabled(false);
    event_table_->setRowCount(0);
    for (const auto& record : analysis_event_records_) {
        AppendEventRowToTable(record);
    }
    event_table_->setUpdatesEnabled(true);
    event_table_synced_record_count_ = analysis_event_records_.size();
}

void EventTimelineView::RebuildSyncTable() {
    if (!sync_table_) return;
    sync_table_->setUpdatesEnabled(false);
    sync_table_->setRowCount(0);
    for (const auto& record : sync_sample_records_) {
        AppendSyncRowToTable(record);
    }
    sync_table_->setUpdatesEnabled(true);
    sync_table_synced_record_count_ = sync_sample_records_.size();
    UpdateSyncChart();
}

void EventTimelineView::RebuildTimelineTable() {
    if (!timeline_table_) return;
    timeline_table_->setUpdatesEnabled(false);
    timeline_table_->setRowCount(0);
    for (const auto& record : timeline_event_records_) {
        AppendTimelineRowToTable(record);
    }
    timeline_table_->setUpdatesEnabled(true);
    timeline_table_synced_record_count_ = timeline_event_records_.size();
    UpdateTimelineChart();
}

void EventTimelineView::UpdateEventSummary() {
    if (!event_summary_label_) return;

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

void EventTimelineView::UpdateSyncSummary() {
    if (!sync_summary_label_) return;

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

void EventTimelineView::UpdateTimelineSummary() {
    if (!timeline_summary_label_) return;

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

void EventTimelineView::OnExportEventCsv() {
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

void EventTimelineView::OnExportSyncCsv() {
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

void EventTimelineView::OnExportTimelineCsv() {
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

void EventTimelineView::AppendEventRowToTable(const AnalysisEventRecord& record) {
    const int row = event_table_->rowCount();
    event_table_->insertRow(row);
    auto cell = [&](int col, const QString& text) {
        event_table_->setItem(row, col, new QTableWidgetItem(text));
    };
    cell(0, QString::number(record.index));
    cell(1, record.severity);
    cell(2, record.type);
    cell(3, QString::number(record.stream_index));
    cell(4, QString::number(record.timestamp_seconds, 'f', 6));
    cell(5, QString::number(record.pts));
    cell(6, record.summary);
    cell(7, record.detail);
}

void EventTimelineView::AppendSyncRowToTable(const SyncSampleRecord& record) {
    const int row = sync_table_->rowCount();
    sync_table_->insertRow(row);
    auto cell = [&](int col, const QString& text) {
        sync_table_->setItem(row, col, new QTableWidgetItem(text));
    };
    cell(0, QString::number(record.index));
    cell(1, QString::number(record.audio_timestamp_seconds, 'f', 6));
    cell(2, QString::number(record.video_timestamp_seconds, 'f', 6));
    cell(3, QString::number(record.diff_ms, 'f', 3));
    cell(4, record.audio_anchor ? tr("音频") : tr("视频"));
}

void EventTimelineView::AppendTimelineRowToTable(const TimelineEventRecord& record) {
    const int row = timeline_table_->rowCount();
    timeline_table_->insertRow(row);
    auto cell = [&](int col, const QString& text) {
        timeline_table_->setItem(row, col, new QTableWidgetItem(text));
    };
    cell(0, QString::number(record.index));
    cell(1, record.category);
    cell(2, QString::number(record.timestamp_seconds, 'f', 6));
    cell(3, record.label);
    cell(4, record.detail);
}

void EventTimelineView::UpdateSyncChart() {
    if (!sync_series_ || !sync_axis_x_ || !sync_axis_y_) {
        return;
    }

    sync_series_->Clear();
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
    sync_axis_x_->SetRange(x_min, std::max(x_min + 1, x_max));

    qreal max_abs = 1.0;
    for (qreal value : sync_chart_values_) {
        max_abs = std::max(max_abs, std::abs(value));
    }
    sync_axis_y_->SetRange(-max_abs * 1.1, max_abs * 1.1);
}

void EventTimelineView::UpdateTimelineChart() {
    if (!timeline_video_series_ || !timeline_audio_series_ || !timeline_event_series_ ||
        !timeline_axis_x_ || !timeline_axis_y_) {
        return;
    }

    timeline_video_series_->Clear();
    timeline_audio_series_->Clear();
    timeline_event_series_->Clear();

    if (timeline_event_records_.empty()) {
        timeline_axis_x_->SetRange(0.0, 1.0);
        timeline_axis_y_->SetRange(0.5, 3.5);
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
    timeline_axis_x_->SetRange(min_ts, max_ts);
    timeline_axis_y_->SetRange(0.5, 3.5);
}

void EventTimelineView::FlushPendingEventTableUpdates() {
    if (!event_table_) return;
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

void EventTimelineView::FlushPendingSyncTableUpdates() {
    if (!sync_table_) return;
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

void EventTimelineView::FlushPendingTimelineTableUpdates() {
    if (!timeline_table_) return;
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

void EventTimelineView::FlushPendingUiUpdates() {
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

} // namespace ui
} // namespace videoeye
