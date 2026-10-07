#include "ui/analysis_panel/BitrateGopPage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include <QColor>
#include <QDateTime>

#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/theme/AppTheme.h"

// BitrateAnomalyType 与 model::ToString(BitrateAnomalyType) 住在 domain 的
// BitrateGopResult.h 里。以前它们在 BitrateGopAnalyzer.h，UI 为此不得不 include 分析器，
// 把具体表达式实现拖进页面的编译图；现在直接 include 结果头就够了。
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/MetricSeries.h"
#include "core/domain/model/TimeRange.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace videoeye {
namespace ui {

namespace {
constexpr int kMaxBitrateChartPoints = 4000;   // 折线图最多绘制的点数
constexpr int kMaxBitrateChartMarkers = 1500;  // 每类标记最多绘制的点数
constexpr int kMaxGopTableRows = 5000;
constexpr int kMaxAnomalyTableRows = 2000;
}  // namespace

BitrateGopPage::BitrateGopPage(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void BitrateGopPage::SetupUi() {
    // 页面本体就是 QWidget（外部 QStackedWidget 的一页），不再多包一层
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 开始/取消
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("码率与 GOP 深度分析"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        start_button_ = new QPushButton(tr("开始分析"), row);
        start_button_->setToolTip(
            tr("对当前文件做一次完整扫描：滑动窗口码率、I/P/B 分布、GOP 列表与异常识别。"
               "与「诊断与报告」共用同一次扫描结果，不会重复读文件。"));
        connect(start_button_, &QPushButton::clicked,
                this, &BitrateGopPage::OnStartAnalysis);
        rl->addWidget(start_button_);

        cancel_button_ = new QPushButton(tr("取消"), row);
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked,
                this, &BitrateGopPage::OnCancelAnalysis);
        rl->addWidget(cancel_button_);
        layout->addWidget(row);
    }

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(progress_bar_);

    // 参数行
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);

        rl->addWidget(new QLabel(tr("滑动窗口"), row));
        window_combo_ = new QComboBox(row);
        for (double w : options_.windows_seconds) {
            window_combo_->addItem(QString::number(w, 'f', 2) + tr(" 秒"), w);
        }
        window_combo_->setCurrentIndex(0);
        connect(window_combo_, &QComboBox::currentIndexChanged,
                this, &BitrateGopPage::OnWindowChanged);
        rl->addWidget(window_combo_);

        rl->addWidget(new QLabel(tr("目标峰值 kbps（0=自动）"), row));
        target_peak_spin_ = new QDoubleSpinBox(row);
        target_peak_spin_->setRange(0.0, 1.0e7);
        target_peak_spin_->setDecimals(0);
        target_peak_spin_->setSingleStep(100.0);
        target_peak_spin_->setValue(0.0);
        connect(target_peak_spin_, &QDoubleSpinBox::valueChanged,
                this, &BitrateGopPage::OnOptionChanged);
        rl->addWidget(target_peak_spin_);

        rl->addWidget(new QLabel(tr("GOP 上限 秒"), row));
        max_gop_seconds_spin_ = new QDoubleSpinBox(row);
        max_gop_seconds_spin_->setRange(0.5, 600.0);
        max_gop_seconds_spin_->setDecimals(1);
        max_gop_seconds_spin_->setSingleStep(1.0);
        max_gop_seconds_spin_->setValue(options_.max_gop_seconds);
        connect(max_gop_seconds_spin_, &QDoubleSpinBox::valueChanged,
                this, &BitrateGopPage::OnOptionChanged);
        rl->addWidget(max_gop_seconds_spin_);

        rl->addWidget(new QLabel(tr("帧"), row));
        max_gop_frames_spin_ = new QSpinBox(row);
        max_gop_frames_spin_->setRange(1, 100000);
        max_gop_frames_spin_->setSingleStep(10);
        max_gop_frames_spin_->setValue(options_.max_gop_frames);
        connect(max_gop_frames_spin_, &QSpinBox::valueChanged,
                this, &BitrateGopPage::OnOptionChanged);
        rl->addWidget(max_gop_frames_spin_);

        decode_types_check_ = new QCheckBox(tr("精确帧类型（解码，较慢）"), row);
        decode_types_check_->setChecked(false);
        decode_types_check_->setToolTip(
            tr("默认用 codec parser 判定帧类型（几乎零成本）；勾选后会完整解码一遍视频，"
               "I/P/B 比例最准确，但长文件明显变慢。"));
        connect(decode_types_check_, &QCheckBox::toggled,
                this, &BitrateGopPage::OnOptionChanged);
        rl->addWidget(decode_types_check_);

        QPushButton* link_btn = new QPushButton(tr("关联场景切换"), row);
        link_btn->setToolTip(tr("用「场景切换」页检测到的切换点，检查其附近是否有关键帧"));
        connect(link_btn, &QPushButton::clicked, this, &BitrateGopPage::OnLinkSceneChanges);
        rl->addWidget(link_btn);

        rl->addStretch();
        layout->addWidget(row);
    }

    summary_label_ = new QLabel(
        tr("点击「开始分析」扫描当前文件：输出滑动窗口码率曲线、I/P/B 比例、GOP 列表与异常峰值，"
           "并可与场景切换结果关联给出编码优化建议。"), this);
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    // 码率曲线（叠加 I 帧 / 场景切换 / 异常峰值标记）
    {
        chart_ = new MetricChartWidget(this);
        chart_->SetTitle(tr("滑动窗口码率（含 I 帧 / 场景切换 / 峰值标记）"));
        bitrate_series_ = chart_->AddLineSeries(tr("码率"), QColor("#1e88e5"));
        target_series_ = chart_->AddLineSeries(tr("目标峰值"), QColor("#fb8c00"));

        iframe_series_ = chart_->AddScatterSeries(tr("I 帧"), QColor("#43a047"));
        iframe_series_->SetMarkerSize(6.0);
        iframe_series_->SetBorderColor(QColor("#43a047"));

        scene_series_ = chart_->AddScatterSeries(tr("场景切换"), QColor("#8e24aa"));
        scene_series_->SetMarkerSize(9.0);
        scene_series_->SetMarkerShape(ChartMarkerShape::Rectangle);
        scene_series_->SetBorderColor(QColor("#8e24aa"));

        anomaly_series_ = chart_->AddScatterSeries(tr("异常峰值"), QColor("#e53935"));
        anomaly_series_->SetMarkerSize(11.0);
        anomaly_series_->SetMarkerShape(ChartMarkerShape::Triangle);
        anomaly_series_->SetBorderColor(QColor("#e53935"));

        axis_x_ = chart_->AxisX();
        axis_y_ = chart_->AxisY();
        axis_x_->SetTitleText(tr("时间 (s)"));
        axis_y_->SetTitleText(tr("kbps"));
        chart_->setMinimumHeight(240);
        layout->addWidget(chart_);
    }

    // 子页: GOP 列表 / 异常 / 建议
    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setMinimumHeight(320);
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        QLabel* hint = new QLabel(tr("点击任意一行即跳转到该 GOP 起始位置。"), page);
        pl->addWidget(hint);
        gop_table_ = new QTableWidget(0, 11, page);
        gop_table_->setHorizontalHeaderLabels(
            {tr("序号"), tr("起始"), tr("结束"), tr("时长(s)"), tr("帧数"), tr("大小(KB)"),
             tr("平均码率(kbps)"), tr("I / P / B"), tr("最大帧(KB)"), tr("类型"), tr("状态")});
        gop_table_->verticalHeader()->setVisible(false);
        gop_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        gop_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        gop_table_->horizontalHeader()->setStretchLastSection(true);
        gop_table_->setMinimumHeight(220);
        connect(gop_table_, &QTableWidget::cellClicked,
                this, &BitrateGopPage::OnGopCellClicked);
        pl->addWidget(gop_table_);
        QPushButton* export_btn = new QPushButton(tr("导出 GOP CSV"), page);
        connect(export_btn, &QPushButton::clicked, this, &BitrateGopPage::OnExportGopCsv);
        pl->addWidget(export_btn, 0, Qt::AlignRight);
        sub_tabs_->addTab(page, tr("GOP 列表"));
    }
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        anomaly_table_ = new QTableWidget(0, 5, page);
        anomaly_table_->setHorizontalHeaderLabels(
            {tr("类型"), tr("位置"), tr("实测值"), tr("阈值"), tr("说明")});
        anomaly_table_->verticalHeader()->setVisible(false);
        anomaly_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        anomaly_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        anomaly_table_->horizontalHeader()->setStretchLastSection(true);
        anomaly_table_->setMinimumHeight(220);
        connect(anomaly_table_, &QTableWidget::cellClicked,
                this, &BitrateGopPage::OnAnomalyCellClicked);
        pl->addWidget(anomaly_table_);
        QPushButton* export_btn = new QPushButton(tr("导出异常 CSV"), page);
        connect(export_btn, &QPushButton::clicked, this, &BitrateGopPage::OnExportAnomalyCsv);
        pl->addWidget(export_btn, 0, Qt::AlignRight);
        sub_tabs_->addTab(page, tr("异常"));
    }
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);
        suggestion_list_ = new QListWidget(page);
        suggestion_list_->setWordWrap(true);
        suggestion_list_->setMinimumHeight(220);
        pl->addWidget(suggestion_list_);
        QPushButton* export_btn = new QPushButton(tr("导出码率曲线 CSV"), page);
        connect(export_btn, &QPushButton::clicked, this, &BitrateGopPage::OnExportCurveCsv);
        pl->addWidget(export_btn, 0, Qt::AlignRight);
        sub_tabs_->addTab(page, tr("优化建议"));
    }
    layout->addWidget(sub_tabs_);

    // 建好之后由 AnalysisPanel::AddPageWithScroll 收进外部 stack
}

void BitrateGopPage::ApplyOptionsFromUi() {
    options_.target_peak_kbps = target_peak_spin_->value();
    options_.max_gop_seconds = max_gop_seconds_spin_->value();
    options_.max_gop_frames = max_gop_frames_spin_->value();
    decode_frame_types_ = decode_types_check_->isChecked();
}

void BitrateGopPage::OnStartAnalysis() {
    ApplyOptionsFromUi();
    emit ScanRequested();
}

void BitrateGopPage::OnCancelAnalysis() {
    // 真正取消 + 按钮/进度条状态由面板统一驱动（本页与诊断页共用同一次扫描）
    emit CancelRequested();
}

void BitrateGopPage::OnWindowChanged() {
    const double w = window_combo_->currentData().toDouble();
    if (w <= 0.0) return;
    display_window_ = w;
    UpdateChart();
    UpdateSummary();
}

void BitrateGopPage::OnOptionChanged() {
    // 阈值类参数在下一次「开始分析」时生效（重新扫描代价大，不自动触发）
    ApplyOptionsFromUi();
}

void BitrateGopPage::OnLinkSceneChanges() {
    if (!has_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先在本页点击「开始分析」完成一次扫描。"));
        return;
    }
    if (scene_changes_.empty()) {
        QMessageBox::information(this, tr("提示"),
            tr("当前没有场景切换数据。请先在「场景切换」页启用检测并播放一段视频。"));
        return;
    }
    ApplyOptionsFromUi();
    // 真正在 facade 上执行、重算 QC 报告、刷新诊断页问题表都由面板编排：
    // 本页不持有 facade，也不知道诊断页的问题表。
    emit SceneLinkRequested(scene_changes_, options_);
}

void BitrateGopPage::OnGopCellClicked(int row, int) {
    if (!gop_table_ || row < 0) return;
    const auto& gops = result_.bitrate_gop.gops;
    if (row >= static_cast<int>(gops.size())) return;
    emit SeekRequested(gops[row].start_seconds);
}

void BitrateGopPage::OnAnomalyCellClicked(int row, int) {
    if (!anomaly_table_ || row < 0) return;
    const auto& anomalies = result_.bitrate_gop.anomalies;
    if (row >= static_cast<int>(anomalies.size())) return;
    emit SeekRequested(anomalies[row].start_seconds);
}

void BitrateGopPage::Refresh() {
    UpdateSummary();
    UpdateChart();
    RebuildGopTable();
    RebuildAnomalyTable();
    UpdateSuggestions();
}

void BitrateGopPage::UpdateSummary() {
    if (!summary_label_) return;
    if (!has_result_) return;

    const auto& bg = result_.bitrate_gop;
    if (bg.total_frames == 0) {
        summary_label_->setText(tr("未检测到视频帧，无法进行码率与 GOP 分析。"));
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

    summary_label_->setText(
        tr("窗口 <b>%1</b> 秒 ｜ 平均 <b>%2</b> kbps ｜ 峰值 <b>%3</b> kbps ｜ 最低 %4 kbps ｜ "
           "中位 %5 kbps ｜ P95 %6 kbps ｜ 峰均比 <b>%7</b><br>"
           "目标峰值 %8 kbps（%9） ｜ 帧类型 %10 ｜ 平均帧 %11 KB ｜ 平均 I 帧 %12 KB<br>"
           "GOP <b>%13</b> 个 ｜ 平均时长 %14 s（%15~%16 帧）｜ 最长 %17 s ｜ 超长 %18 个 ｜ "
           "closed %19 / open %20 ｜ 关键帧间隔 %21 ± %22 s<br>"
           "异常 %23 条")
            .arg(QString::number(display_window_, 'f', 2))
            .arg(QString::number(bg.avg_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.peak_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.min_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.median_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.p95_bitrate_kbps, 'f', 0))
            .arg(QString::number(bg.peak_to_mean_ratio, 'f', 2))
            .arg(QString::number(bg.target_peak_kbps, 'f', 0))
            .arg(options_.target_peak_kbps > 0.0 ? tr("手动") : tr("自动=均值×2"))
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

void BitrateGopPage::UpdateChart() {
    if (!bitrate_series_) return;
    bitrate_series_->Clear();
    target_series_->Clear();
    iframe_series_->Clear();
    scene_series_->Clear();
    anomaly_series_->Clear();
    if (!has_result_) return;

    const auto& bg = result_.bitrate_gop;
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
                          }(display_window_) + "s") {
            curve = &c;
            break;
        }
    }
    if (curve != nullptr) {
        AppendDecimated(bitrate_series_, *curve, kMaxBitrateChartPoints);
    } else {
        model::MetricSeries fallback;
        fallback.samples.reserve(bg.bitrate_points.size());
        for (const auto& p : bg.bitrate_points) {
            fallback.Add(p.timestamp_seconds, p.bitrate_kbps);
        }
        AppendDecimated(bitrate_series_, fallback, kMaxBitrateChartPoints);
    }

    const double duration = std::max(bg.duration_seconds, 1.0);
    double y_max = std::max(bg.peak_bitrate_kbps, bg.target_peak_kbps) * 1.15;
    if (y_max <= 0.0) y_max = 1.0;

    if (bg.target_peak_kbps > 0.0) {
        target_series_->Append(0.0, bg.target_peak_kbps);
        target_series_->Append(duration, bg.target_peak_kbps);
    }

    // I 帧标记（画在基线）
    {
        const int iframe_step =
            std::max(1, static_cast<int>(bg.i_frame_seconds.size()) / kMaxBitrateChartMarkers);
        SeriesBatch batch(iframe_series_);
        for (size_t i = 0; i < bg.i_frame_seconds.size(); i += iframe_step) {
            batch.Add(bg.i_frame_seconds[i], 0.0);
        }
    }

    // 场景切换点（基线，用关联结果；未关联时直接用检测记录）
    {
        SeriesBatch batch(scene_series_);
        if (!bg.scene_matches.empty()) {
            const int scene_step =
                std::max(1, static_cast<int>(bg.scene_matches.size()) / kMaxBitrateChartMarkers);
            for (size_t i = 0; i < bg.scene_matches.size(); i += scene_step) {
                batch.Add(bg.scene_matches[i].timestamp_seconds, 0.0);
            }
        } else {
            const int scene_step =
                std::max(1, static_cast<int>(scene_changes_.size()) / kMaxBitrateChartMarkers);
            for (size_t i = 0; i < scene_changes_.size(); i += scene_step) {
                batch.Add(scene_changes_[i].timestamp, 0.0);
            }
        }
    }

    // 异常峰值（标在实际码率高度）
    {
        SeriesBatch batch(anomaly_series_);
        for (const auto& a : bg.anomalies) {
            if (a.type != model::BitrateAnomalyType::PeakOvershoot) continue;
            batch.Add((a.start_seconds + a.end_seconds) * 0.5, a.value);
            y_max = std::max(y_max, a.value * 1.1);
        }
    }

    axis_x_->SetRange(0.0, duration);
    axis_y_->SetRange(0.0, y_max);
}

void BitrateGopPage::RebuildGopTable() {
    if (!gop_table_) return;
    gop_table_->setRowCount(0);
    if (!has_result_) return;

    const auto& gops = result_.bitrate_gop.gops;
    const int rows = std::min(static_cast<int>(gops.size()), kMaxGopTableRows);
    gop_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& g = gops[i];
        SetTableItemText(gop_table_, i, 0, QString::number(g.index));
        SetTableItemText(gop_table_, i, 1,
                         theme::font::formatTime(static_cast<int>(g.start_seconds * 1000)));
        SetTableItemText(gop_table_, i, 2,
                         theme::font::formatTime(static_cast<int>(g.end_seconds * 1000)));
        SetTableItemText(gop_table_, i, 3, QString::number(g.DurationSeconds(), 'f', 3));
        SetTableItemText(gop_table_, i, 4, QString::number(g.frame_count));
        SetTableItemText(gop_table_, i, 5, FormatKb(static_cast<double>(g.byte_count)));
        SetTableItemText(gop_table_, i, 6,
                         QString::number(g.AverageBitrateKbps(), 'f', 0));
        SetTableItemText(gop_table_, i, 7, QString::fromStdString(g.FrameTypeSummary()));
        SetTableItemText(gop_table_, i, 8,
                         FormatKb(static_cast<double>(g.max_frame_bytes)));
        SetTableItemText(gop_table_, i, 9, g.closed_gop ? tr("closed") : tr("open"));
        SetTableItemText(gop_table_, i, 10, g.complete ? tr("已收尾") : tr("未收尾"));

        if (options_.max_gop_seconds > 0.0 &&
            g.DurationSeconds() > options_.max_gop_seconds) {
            if (QTableWidgetItem* cell = gop_table_->item(i, 3)) {
                cell->setForeground(QColor("#ef6c00"));
            }
        }
    }
    gop_table_->resizeColumnsToContents();
}

void BitrateGopPage::RebuildAnomalyTable() {
    if (!anomaly_table_) return;
    anomaly_table_->setRowCount(0);
    if (!has_result_) return;

    const auto& anomalies = result_.bitrate_gop.anomalies;
    const int rows = std::min(static_cast<int>(anomalies.size()), kMaxAnomalyTableRows);
    anomaly_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& a = anomalies[i];
        const QString unit = QString::fromStdString(a.unit);
        SetTableItemText(anomaly_table_, i, 0,
                         QString::fromStdString(model::ToString(a.type)));
        SetTableItemText(anomaly_table_, i, 1,
                         QString::fromStdString(
                             model::TimeRange::Between(a.start_seconds, a.end_seconds).ToString()));
        SetTableItemText(anomaly_table_, i, 2, FormatMetricValue(a.value, unit));
        SetTableItemText(anomaly_table_, i, 3, FormatMetricValue(a.threshold, unit));
        SetTableItemText(anomaly_table_, i, 4, QString::fromStdString(a.detail));
    }
    anomaly_table_->resizeColumnsToContents();
}

void BitrateGopPage::UpdateSuggestions() {
    if (!suggestion_list_) return;
    suggestion_list_->clear();
    if (!has_result_) return;
    for (const auto& s : result_.bitrate_gop.suggestions) {
        suggestion_list_->addItem(QString::fromStdString(s));
    }
}

void BitrateGopPage::OnExportGopCsv() {
    if (!has_result_ || result_.bitrate_gop.gops.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的 GOP 数据。"));
        return;
    }
    ExportCsvStream(this, tr("导出 GOP 列表 CSV"),
        source_path_.section('/', -1) + "_gop.csv",
        [this](QTextStream& stream) {
            stream << "index,start_seconds,end_seconds,duration_seconds,frame_count,byte_count,"
                      "avg_bitrate_kbps,i_count,p_count,b_count,unknown_count,max_frame_bytes,"
                      "closed_gop,complete\n";
            for (const auto& g : result_.bitrate_gop.gops) {
                stream << g.index << "," << QString::number(g.start_seconds, 'f', 3) << ","
                       << QString::number(g.end_seconds, 'f', 3) << ","
                       << QString::number(g.DurationSeconds(), 'f', 3) << "," << g.frame_count << ","
                       << g.byte_count << "," << QString::number(g.AverageBitrateKbps(), 'f', 3) << ","
                       << g.i_count << "," << g.p_count << "," << g.b_count << "," << g.unknown_count << ","
                       << g.max_frame_bytes << "," << (g.closed_gop ? 1 : 0) << ","
                       << (g.complete ? 1 : 0) << "\n";
            }
        },
        static_cast<int>(result_.bitrate_gop.gops.size()));
}

void BitrateGopPage::OnExportCurveCsv() {
    if (!has_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先完成一次扫描。"));
        return;
    }
    const auto& bg = result_.bitrate_gop;
    if (bg.window_curves.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的码率曲线数据。"));
        return;
    }
    ExportCsvStream(this, tr("导出码率曲线 CSV"),
        source_path_.section('/', -1) + "_bitrate.csv",
        [this, &bg](QTextStream& stream) {
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
        },
        static_cast<int>(bg.window_curves.front().samples.size()));
}

void BitrateGopPage::OnExportAnomalyCsv() {
    if (!has_result_ || result_.bitrate_gop.anomalies.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的异常数据。"));
        return;
    }
    ExportCsvStream(this, tr("导出异常 CSV"),
        source_path_.section('/', -1) + "_bitrate_anomaly.csv",
        [this](QTextStream& stream) {
            stream << "type,start_seconds,end_seconds,value,threshold,unit,detail,suggestion\n";
            for (const auto& a : result_.bitrate_gop.anomalies) {
                stream << QString::fromStdString(model::ToString(a.type)) << ","
                       << QString::number(a.start_seconds, 'f', 3) << ","
                       << QString::number(a.end_seconds, 'f', 3) << ","
                       << QString::number(a.value, 'f', 3) << ","
                       << QString::number(a.threshold, 'f', 3) << ","
                       << QString::fromStdString(a.unit) << ",\""
                       << QString::fromStdString(a.detail).replace('"', "'") << "\",\""
                       << QString::fromStdString(a.suggestion).replace('"', "'") << "\"\n";
            }
        },
        static_cast<int>(result_.bitrate_gop.anomalies.size()));
}

void BitrateGopPage::SetResult(const model::AnalysisResult& result) {
    result_ = result;
    has_result_ = true;
    Refresh();
}

void BitrateGopPage::SetSceneChanges(const std::vector<model::SceneChangeResult>& records) {
    scene_changes_ = records;
    UpdateChart();
}

void BitrateGopPage::FillScanOptions(videoeye::AnalysisOptions& options) {
    ApplyOptionsFromUi();
    options.bitrate_gop_options = options_;
    options.analyze_bitrate_gop = true;
    options.decode_frame_types = decode_frame_types_;
}

void BitrateGopPage::SetScanActive(bool active) {
    scan_active_ = active;
    if (start_button_) start_button_->setEnabled(!active);
    if (cancel_button_) cancel_button_->setEnabled(active);
}

void BitrateGopPage::SetProgress(int percent) {
    if (progress_bar_) progress_bar_->setValue(percent);
}

void BitrateGopPage::SetProgressFormat(const QString& format) {
    if (progress_bar_) progress_bar_->setFormat(format);
}

void BitrateGopPage::ShowSceneLinkSummary() {
    Refresh();
    const size_t matched = result_.bitrate_gop.scene_matches.size();
    size_t missing = 0;
    for (const auto& m : result_.bitrate_gop.scene_matches) {
        if (!m.has_nearby_keyframe) ++missing;
    }
    QMessageBox::information(this, tr("已关联"),
        tr("共关联 %1 个场景切换点，其中 %2 处附近没有关键帧。")
            .arg(matched).arg(missing));
}


} // namespace ui
} // namespace videoeye
