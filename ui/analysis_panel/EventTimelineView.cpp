#include "ui/analysis_panel/EventTimelineView.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <deque>

namespace videoeye {
namespace ui {

namespace {
constexpr size_t kMaxEventRecords = 5000;
constexpr size_t kMaxSyncRecords = 5000;
constexpr size_t kMaxTimelineRecords = 5000;
// 页头聚合开关覆盖的采集项个数：异常事件 / 同步样本 / 时间轴（内部编号 0/1/2）。
constexpr int kFeatureCount = 3;
// 曲线窗口上限不再本地定义：统一用 ui/charts/ChartDecimation.h 的 kMaxRecentChartItems。
} // namespace
// 注：TrimRecords 原本在这里有一份匿名命名空间副本（与 FramePacketView 那份同语义），
// 现已统一提到 AnalysisPageSupport.h，本文件直接用共享版本，不再各抄一份。

EventTimelineView::EventTimelineView(QWidget* parent) : QWidget(parent) {
    event_analysis_sub_tabs_ = new QTabWidget(this);
    event_analysis_sub_tabs_->setDocumentMode(true);

    // 子页顺序沿用原 AnalysisPanel::SetupEventAnalysisTab：异常事件 / 时间轴 / 同步分析
    SetupEventTab();
    SetupTimelineTab();
    SetupSyncTab();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    // 页头：统一标题 + 唯一的「启用分析」（聚合三项采集）+ 一个「导出」菜单。
    // 三个子页的 CSV 都收进导出菜单，三个同名勾选框也收成这一个开关 —— 其它页的
    // 分析入口都在页头的同一位置，只有本页把开关散在子页里，用户要先找按钮再猜它管哪项。
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(4, 2, 4, 0);
        rl->setSpacing(8);
        QLabel* title = new QLabel(tr("事件与时间轴"), row);
        title->setObjectName(QStringLiteral("PageTitle"));
        rl->addWidget(title);
        rl->addStretch();
        rl->addWidget(toggle_.Create(row,
                                     tr("播放时实时采集异常事件、音视频同步样本与统一时间轴。部分开启时显示为半选；"
                                        "点击可一键全开或全关。"),
                                     kFeatureCount));
        // 清空：与帧表/包表的"清空"同口径，只清本地已采集列表，不影响正在进行的采集
        // （控制器侧的序号继续递增，新数据不会被旧数据污染）。
        auto* clear_button = new QPushButton(tr("清空"), row);
        clear_button->setToolTip(tr("清空本页已采集的异常事件、同步样本与时间轴数据（不影响正在进行的采集）"));
        connect(clear_button, &QPushButton::clicked, this, [this]() {
            ResetAnalysisEventList();
            ResetSyncSampleList();
            ResetTimelineEventList();
        });
        rl->addWidget(clear_button);
        rl->addWidget(MakeExportMenuButton(
            row, {{tr("异常事件 CSV"), [this]() { OnExportEventCsv(); }, tr("导出异常事件列表")},
                  {tr("同步分析 CSV"), [this]() { OnExportSyncCsv(); }, tr("导出音视频同步样本与偏移")},
                  {tr("统一时间轴 CSV"), [this]() { OnExportTimelineCsv(); }, tr("导出统一时间轴事件")}}));
        root->addWidget(row);
    }
    root->addWidget(event_analysis_sub_tabs_);
}

EventTimelineView::~EventTimelineView() = default;

void EventTimelineView::SetFeatureHooks(std::function<bool(int)> is_enabled,
                                        std::function<void(int, bool)> set_enabled) {
    // 数据过滤仍按各项自己的开关走（见 Append*），所以这份 is_enabled 要留着；
    // 界面只剩页头一个聚合勾选框，交给 AggregateFeatureToggle 维护三态。
    is_enabled_ = is_enabled;
    set_enabled_ = set_enabled;
    toggle_.SetHooks(std::move(is_enabled), std::move(set_enabled), kFeatureCount);
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
    // 采集开关统一在页头（聚合三项），子页只留摘要 —— 子页不再挂同名勾选框。
    event_summary_label_ = new QLabel(tr("总事件数: 0 | 错误: 0 | 警告: 0 | 信息: 0"), event_tab_);
    event_summary_label_->setObjectName(QStringLiteral("HintLabel"));
    toolbar_layout->addWidget(event_summary_label_, 1);
    layout->addLayout(toolbar_layout);

    QGroupBox* table_group = new QGroupBox(tr("异常事件"), event_tab_);
    QVBoxLayout* table_layout = new QVBoxLayout(table_group);

    event_table_ = new QTableWidget(0, 8, table_group);
    event_table_->setHorizontalHeaderLabels(
        {tr("序号"), tr("级别"), tr("类型"), tr("流索引"), tr("时间戳(s)"), tr("PTS"), tr("摘要"), tr("详情")});
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
    // 双击跳到该事件发生的位置：异常事件最有用的动作就是"过去看看"。
    // 行与 analysis_event_records_ 严格一一对应（裁剪时同步删行），直接用行号取时间戳。
    connect(event_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && static_cast<size_t>(row) < analysis_event_records_.size()) {
            emit SeekRequested(analysis_event_records_[static_cast<size_t>(row)].timestamp_seconds);
        }
    });

    table_layout->addWidget(event_table_);
    layout->addWidget(table_group);

    event_analysis_sub_tabs_->addTab(event_tab_, tr("异常事件"));
}

void EventTimelineView::SetupSyncTab() {
    sync_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(sync_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    // 采集开关统一在页头（聚合三项），子页只留摘要。
    sync_summary_label_ = new QLabel(tr("样本数: 0 | 平均偏移: 0.00 ms | 最大偏移: 0.00 ms"), sync_tab_);
    sync_summary_label_->setObjectName(QStringLiteral("HintLabel"));
    toolbar_layout->addWidget(sync_summary_label_, 1);
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
    sync_table_->setHorizontalHeaderLabels(
        {tr("序号"), tr("音频时间(s)"), tr("视频时间(s)"), tr("差值(ms)"), tr("锚点")});
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
    // 双击跳到该同步样本的时间；取音频时间戳，偏移异常时优先看"声音在哪"。
    connect(sync_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && static_cast<size_t>(row) < sync_sample_records_.size()) {
            emit SeekRequested(sync_sample_records_[static_cast<size_t>(row)].audio_timestamp_seconds);
        }
    });
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

    event_analysis_sub_tabs_->addTab(sync_tab_, tr("同步分析"));
}

void EventTimelineView::SetupTimelineTab() {
    timeline_tab_ = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(timeline_tab_);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    QHBoxLayout* toolbar_layout = new QHBoxLayout();
    // 采集开关统一在页头（聚合三项），子页只留摘要。
    timeline_summary_label_ = new QLabel(tr("总数: 0 | 视频关键帧: 0 | 音频采样: 0 | 异常事件: 0"), timeline_tab_);
    timeline_summary_label_->setObjectName(QStringLiteral("HintLabel"));
    toolbar_layout->addWidget(timeline_summary_label_, 1);
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
    timeline_table_->setHorizontalHeaderLabels({tr("序号"), tr("类别"), tr("时间戳(s)"), tr("标签"), tr("详情")});
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
    // 双击跳到该时间轴标记的位置（关键帧/音频采样/异常事件通用）。
    connect(timeline_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && static_cast<size_t>(row) < timeline_event_records_.size()) {
            emit SeekRequested(timeline_event_records_[static_cast<size_t>(row)].timestamp_seconds);
        }
    });
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

    event_analysis_sub_tabs_->addTab(timeline_tab_, tr("统一时间轴"));
}

// ---------------------------------------------------------------------------
// 实时数据接入（由 AnalysisPanel 薄转发）
// ---------------------------------------------------------------------------

void EventTimelineView::ResetAnalysisEventList() {
    // 清空列表不受采集开关约束：控制器换媒体时无条件发 Reset，
    // 这里若按开关提前返回，"A 文件采集过 -> 关开关 -> 换 B -> 再开开关"
    // 会把 B 的数据接在 A 的旧行后面。按开关过滤只发生在 Append*。
    analysis_event_records_.clear();
    event_table_synced_record_count_ = 0;
    event_table_dirty_ = false;
    event_summary_dirty_ = false;
    event_records_trimmed_ = false;
    if (event_table_) {
        event_table_->setRowCount(0);
    }
    UpdateEventSummary();
}

void EventTimelineView::AppendAnalysisEvent(const model::AnalysisEvent& event_info) {
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Event)))
        return;
    if (!event_table_) {
        return;
    }

    AnalysisEventRecord record;
    record.index = event_info.index;
    record.severity = event_info.severity;
    record.type = QString::fromStdString(event_info.type);
    record.stream_index = event_info.stream_index;
    record.pts = event_info.pts;
    record.timestamp_seconds = event_info.timestamp_seconds;
    record.summary = QString::fromStdString(event_info.summary);
    record.detail = QString::fromStdString(event_info.detail);

    analysis_event_records_.push_back(record);
    event_table_dirty_ = true;
    event_summary_dirty_ = true;
    if (analysis_event_records_.size() > kMaxEventRecords) {
        event_records_trimmed_ = true;
    }
    // 增量裁剪：到上限后只移除表头一行，不再整表清空重填（见 AnalysisPageSupport.h）。
    TrimRecordsIncremental(analysis_event_records_, event_table_synced_record_count_, event_table_, kMaxEventRecords);
}

void EventTimelineView::ResetSyncSampleList() {
    // 同上：清空无条件执行，开关只管收不收新数据。
    sync_sample_records_.clear();
    sync_table_synced_record_count_ = 0;
    sync_table_dirty_ = false;
    sync_summary_dirty_ = false;
    sync_records_trimmed_ = false;
    sync_chart_values_.clear();
    sync_total_sample_count_ = 0;
    sync_abs_sum_ms_ = 0.0;
    sync_max_abs_ms_ = 0.0;
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
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Sync)))
        return;
    if (!sync_table_) {
        return;
    }

    SyncSampleRecord record;
    record.index = sample.index;
    record.audio_timestamp_seconds = sample.audio_timestamp_seconds;
    record.video_timestamp_seconds = sample.video_timestamp_seconds;
    record.diff_ms = sample.diff_ms;
    record.audio_anchor = sample.audio_anchor;

    // 全时段累计（含随后被裁剪掉的旧样本），摘要不能用"本地保留窗口"冒充全程。
    ++sync_total_sample_count_;
    const double abs_diff = std::abs(record.diff_ms);
    sync_abs_sum_ms_ += abs_diff;
    sync_max_abs_ms_ = std::max(sync_max_abs_ms_, abs_diff);

    sync_sample_records_.push_back(record);
    sync_table_dirty_ = true;
    sync_summary_dirty_ = true;
    if (sync_sample_records_.size() > kMaxSyncRecords) {
        sync_records_trimmed_ = true;
    }
    TrimRecordsIncremental(sync_sample_records_, sync_table_synced_record_count_, sync_table_, kMaxSyncRecords);
}

void EventTimelineView::ResetTimelineEventList() {
    // 同上：清空无条件执行，开关只管收不收新数据。
    timeline_event_records_.clear();
    timeline_table_synced_record_count_ = 0;
    timeline_table_dirty_ = false;
    timeline_summary_dirty_ = false;
    timeline_records_trimmed_ = false;
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
    if (!is_enabled_ || !is_enabled_(static_cast<int>(ViewFeature::Timeline)))
        return;
    if (!timeline_table_) {
        return;
    }

    TimelineEventRecord record;
    record.index = event.index;
    record.category = event.category;
    record.timestamp_seconds = event.timestamp_seconds;
    record.label = QString::fromStdString(event.label);
    record.detail = QString::fromStdString(event.detail);

    timeline_event_records_.push_back(record);
    timeline_table_dirty_ = true;
    timeline_summary_dirty_ = true;
    if (timeline_event_records_.size() > kMaxTimelineRecords) {
        timeline_records_trimmed_ = true;
    }
    TrimRecordsIncremental(timeline_event_records_, timeline_table_synced_record_count_, timeline_table_,
                           kMaxTimelineRecords);
}

QString EventTimelineView::CategoryDisplayName(model::TimelineEventCategory category) {
    switch (category) {
    case model::TimelineEventCategory::VideoKeyframe:
        return tr("视频关键帧");
    case model::TimelineEventCategory::AudioSample:
        return tr("音频采样");
    case model::TimelineEventCategory::Event:
        break;
    }
    return tr("事件");
}

QString EventTimelineView::SeverityDisplayName(model::AnalysisEventSeverity severity) {
    switch (severity) {
    case model::AnalysisEventSeverity::Error:
        return tr("错误");
    case model::AnalysisEventSeverity::Warning:
        return tr("警告");
    case model::AnalysisEventSeverity::Info:
        break;
    }
    return tr("信息");
}

QString EventTimelineView::TrimNote(bool trimmed, int max_count) {
    if (!trimmed)
        return QString();
    // 与画面质量页同一口径：列表被上限裁剪时必须明示，
    // 否则用户会把"只保留最近 N 条"误读成"分析漏了数据"。
    return tr("；本地列表已达上限（仅保留最近 %1 条）").arg(max_count);
}

// ---------------------------------------------------------------------------
// 摘要 / 导出 / 刷新
// ---------------------------------------------------------------------------

void EventTimelineView::UpdateEventSummary() {
    if (!event_summary_label_)
        return;

    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    for (const auto& record : analysis_event_records_) {
        if (record.severity == model::AnalysisEventSeverity::Error) {
            error_count++;
        } else if (record.severity == model::AnalysisEventSeverity::Warning) {
            warning_count++;
        } else {
            info_count++;
        }
    }

    event_summary_label_->setText(tr("总事件数: %1 | 错误: %2 | 警告: %3 | 信息: %4%5")
                                      .arg(analysis_event_records_.size())
                                      .arg(error_count)
                                      .arg(warning_count)
                                      .arg(info_count)
                                      .arg(TrimNote(event_records_trimmed_, static_cast<int>(kMaxEventRecords))));
}

void EventTimelineView::UpdateSyncSummary() {
    if (!sync_summary_label_)
        return;

    const QString trim_note = TrimNote(sync_records_trimmed_, static_cast<int>(kMaxSyncRecords));
    if (sync_total_sample_count_ == 0) {
        // 零样本时也沿用同一套文案，避免出现两套标签。
        sync_summary_label_->setText(
            tr("样本数: 0 | 全时段平均偏移: 0.00 ms | 全时段最大偏移: 0.00 ms%1").arg(trim_note));
        return;
    }

    const double mean_abs = sync_abs_sum_ms_ / static_cast<double>(sync_total_sample_count_);
    sync_summary_label_->setText(tr("样本数: %1（本地保留 %2 条） | 全时段平均偏移: %3 ms | 全时段最大偏移: %4 ms%5")
                                     .arg(sync_total_sample_count_)
                                     .arg(sync_sample_records_.size())
                                     .arg(mean_abs, 0, 'f', 2)
                                     .arg(sync_max_abs_ms_, 0, 'f', 2)
                                     .arg(trim_note));
}

void EventTimelineView::UpdateTimelineSummary() {
    if (!timeline_summary_label_)
        return;

    int video_count = 0;
    int audio_count = 0;
    int event_count = 0;
    for (const auto& record : timeline_event_records_) {
        switch (record.category) {
        case model::TimelineEventCategory::VideoKeyframe:
            video_count++;
            break;
        case model::TimelineEventCategory::AudioSample:
            audio_count++;
            break;
        case model::TimelineEventCategory::Event:
            event_count++;
            break;
        }
    }

    timeline_summary_label_->setText(
        tr("总数: %1 | 视频关键帧: %2 | 音频采样: %3 | 异常事件: %4%5")
            .arg(timeline_event_records_.size())
            .arg(video_count)
            .arg(audio_count)
            .arg(event_count)
            .arg(TrimNote(timeline_records_trimmed_, static_cast<int>(kMaxTimelineRecords))));
}

void EventTimelineView::OnExportEventCsv() {
    if (analysis_event_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的异常事件数据。"));
        return;
    }

    ExportCsvStream(
        this, tr("导出异常事件 CSV"),
        QString("videoeye_events_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,severity,type,stream_index,timestamp_seconds,pts,summary,detail\n";
            for (const auto& record : analysis_event_records_) {
                out << record.index << ',' << '"' << SeverityDisplayName(record.severity) << '"' << ',' << '"'
                    << record.type << '"' << ',' << record.stream_index << ','
                    << QString::number(record.timestamp_seconds, 'f', 6) << ',' << record.pts << ',' << '"'
                    << record.summary << '"' << ',' << '"' << record.detail << '"' << '\n';
            }
        },
        static_cast<int>(analysis_event_records_.size()));
}

void EventTimelineView::OnExportSyncCsv() {
    if (sync_sample_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的同步分析数据。"));
        return;
    }

    ExportCsvStream(
        this, tr("导出同步分析 CSV"),
        QString("videoeye_sync_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,audio_timestamp_seconds,video_timestamp_seconds,diff_ms,anchor\n";
            for (const auto& record : sync_sample_records_) {
                out << record.index << ',' << QString::number(record.audio_timestamp_seconds, 'f', 6) << ','
                    << QString::number(record.video_timestamp_seconds, 'f', 6) << ','
                    << QString::number(record.diff_ms, 'f', 3) << ',' << '"'
                    << (record.audio_anchor ? tr("音频") : tr("视频")) << '"' << '\n';
            }
        },
        static_cast<int>(sync_sample_records_.size()));
}

void EventTimelineView::OnExportTimelineCsv() {
    if (timeline_event_records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的时间轴数据。"));
        return;
    }

    ExportCsvStream(
        this, tr("导出统一时间轴 CSV"),
        QString("videoeye_timeline_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        [this](QTextStream& out) {
            out << "index,category,timestamp_seconds,label,detail\n";
            for (const auto& record : timeline_event_records_) {
                out << record.index << ',' << '"' << CategoryDisplayName(record.category) << '"' << ','
                    << QString::number(record.timestamp_seconds, 'f', 6) << ',' << '"' << record.label << '"' << ','
                    << '"' << record.detail << '"' << '\n';
            }
        },
        static_cast<int>(timeline_event_records_.size()));
}

void EventTimelineView::AppendEventRowToTable(const AnalysisEventRecord& record) {
    const int row = event_table_->rowCount();
    event_table_->insertRow(row);
    auto cell = [&](int col, const QString& text) { event_table_->setItem(row, col, new QTableWidgetItem(text)); };
    cell(0, QString::number(record.index));
    cell(1, SeverityDisplayName(record.severity));
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
    auto cell = [&](int col, const QString& text) { sync_table_->setItem(row, col, new QTableWidgetItem(text)); };
    cell(0, QString::number(record.index));
    cell(1, QString::number(record.audio_timestamp_seconds, 'f', 6));
    cell(2, QString::number(record.video_timestamp_seconds, 'f', 6));
    cell(3, QString::number(record.diff_ms, 'f', 3));
    cell(4, record.audio_anchor ? tr("音频") : tr("视频"));
}

void EventTimelineView::AppendTimelineRowToTable(const TimelineEventRecord& record) {
    const int row = timeline_table_->rowCount();
    timeline_table_->insertRow(row);
    auto cell = [&](int col, const QString& text) { timeline_table_->setItem(row, col, new QTableWidgetItem(text)); };
    cell(0, QString::number(record.index));
    cell(1, CategoryDisplayName(record.category));
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
    const int start = std::max(0, static_cast<int>(sync_sample_records_.size()) - kMaxRecentChartItems);
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
    if (!timeline_video_series_ || !timeline_audio_series_ || !timeline_event_series_ || !timeline_axis_x_ ||
        !timeline_axis_y_) {
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

    const int start = std::max(0, static_cast<int>(timeline_event_records_.size()) - kMaxRecentChartItems);
    double min_ts = timeline_event_records_[start].timestamp_seconds;
    double max_ts = timeline_event_records_[start].timestamp_seconds;
    {
        SeriesBatch video_batch(timeline_video_series_);
        SeriesBatch audio_batch(timeline_audio_series_);
        SeriesBatch event_batch(timeline_event_series_);
        for (int i = start; i < static_cast<int>(timeline_event_records_.size()); ++i) {
            const auto& record = timeline_event_records_[i];
            switch (record.category) {
            case model::TimelineEventCategory::VideoKeyframe:
                video_batch.Add(record.timestamp_seconds, 3.0);
                break;
            case model::TimelineEventCategory::AudioSample:
                audio_batch.Add(record.timestamp_seconds, 2.0);
                break;
            case model::TimelineEventCategory::Event:
                event_batch.Add(record.timestamp_seconds, 1.0);
                break;
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
    if (!event_table_)
        return;
    // 跟随策略与帧表/包表一致：只有用户本来就停在底部时才继续跟随，
    // 往上翻看历史时把控制权交还给用户，不再每 120ms 拽回底部。
    const bool pinned = IsTablePinnedToBottom(event_table_);
    event_table_->setUpdatesEnabled(false);
    for (size_t i = event_table_synced_record_count_; i < analysis_event_records_.size(); ++i) {
        AppendEventRowToTable(analysis_event_records_[i]);
    }
    event_table_->setUpdatesEnabled(true);
    event_table_synced_record_count_ = analysis_event_records_.size();
    FollowTableBottom(event_table_, pinned);
}

void EventTimelineView::FlushPendingSyncTableUpdates() {
    if (!sync_table_)
        return;
    const bool pinned = IsTablePinnedToBottom(sync_table_);
    sync_table_->setUpdatesEnabled(false);
    for (size_t i = sync_table_synced_record_count_; i < sync_sample_records_.size(); ++i) {
        AppendSyncRowToTable(sync_sample_records_[i]);
    }
    sync_table_->setUpdatesEnabled(true);
    sync_table_synced_record_count_ = sync_sample_records_.size();
    UpdateSyncChart();
    FollowTableBottom(sync_table_, pinned);
}

void EventTimelineView::FlushPendingTimelineTableUpdates() {
    if (!timeline_table_)
        return;
    const bool pinned = IsTablePinnedToBottom(timeline_table_);
    timeline_table_->setUpdatesEnabled(false);
    for (size_t i = timeline_table_synced_record_count_; i < timeline_event_records_.size(); ++i) {
        AppendTimelineRowToTable(timeline_event_records_[i]);
    }
    timeline_table_->setUpdatesEnabled(true);
    timeline_table_synced_record_count_ = timeline_event_records_.size();
    UpdateTimelineChart();
    FollowTableBottom(timeline_table_, pinned);
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
