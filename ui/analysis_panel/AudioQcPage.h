#pragma once

// 音频 QC 页：响度 / 真峰值 / 削波 / 静音 / 声道相位 / metadata 一致性。
//
// 从 AnalysisPanel 拆出来的独立页面组件。数据与「诊断与报告」共用同一次全文件
// 扫描，所以本页不自己发扫描请求 —— 只发 ScanRequested()，由面板统一编排
// （面板才知道 current_video_path_ / diagnostics_options_ / 进度条总控）。

#include <QWidget>
#include <QString>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include "ui/AnalysisFacade.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class AudioQcPage : public QWidget {
    Q_OBJECT

public:
    explicit AudioQcPage(QWidget* parent = nullptr);

    // 导出 CSV 的默认文件名用当前文件路径拼，面板换文件时同步一次
    void SetSourcePath(const QString& path) { source_path_ = path; }

    // 扫描结束后由面板喂结果（qc_report 用于「规则判定」列）
    void SetResult(const analyzer::AnalysisResult& result, const model::QcReport& qc_report);

    // 只换报告（用户在「规则与阈值」页改了规则后重新评估）再刷一次
    void SetQcReport(const model::QcReport& qc_report);

    // 面板发起扫描前调用：把 UI 上的阈值同步进 AnalysisOptions
    void FillScanOptions(analyzer::AnalysisOptions& options);

    // 扫描生命周期由面板驱动（与码率/GOP、色彩/HDR、诊断页保持同步）
    void SetScanActive(bool active);
    // 同 BitrateGopPage::IsScanActive: 状态必须可观察，否则"某页卡在扫描中"只能靠肉眼发现
    bool IsScanActive() const { return scan_active_; }
    void SetProgress(int percent);
    void SetProgressFormat(const QString& format);

signals:
    void ScanRequested();
    void CancelRequested();
    // 点削波/静音行跳到对应时间点
    void SeekRequested(double seconds);

public slots:
    void Refresh();

private:
    // 扫描态（由 SetScanActive 写入；面板成功/取消/失败三种终态都必须把它复位）
    bool scan_active_ = false;

    void SetupUi();
    void ApplyOptionsFromUi();
    void OnStartAnalysis();
    void OnCancelAnalysis();
    void OnOptionChanged();
    void OnClipCellClicked(int row, int column);
    void OnSilenceCellClicked(int row, int column);
    void UpdateSummary();
    void UpdateCharts();
    void RebuildClipTable();
    void RebuildSilenceTable();
    void RebuildVerdictTable();
    void RebuildMetadataTable();
    void OnExportCsv();

    QString source_path_;
    bool has_result_ = false;
    analyzer::AnalysisResult result_;
    model::QcReport qc_report_;
    analyzer::AudioQcOptions options_;

    QLabel* summary_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QDoubleSpinBox* target_lufs_spin_ = nullptr;
    QDoubleSpinBox* silence_spin_ = nullptr;
    QDoubleSpinBox* min_silence_spin_ = nullptr;
    QDoubleSpinBox* clip_spin_ = nullptr;
    QCheckBox* loudness_check_ = nullptr;
    QCheckBox* true_peak_check_ = nullptr;
    QCheckBox* correlation_check_ = nullptr;
    QTabWidget* sub_tabs_ = nullptr;

    // 响度曲线
    MetricChartWidget* lufs_chart_ = nullptr;
    ChartSeries* momentary_series_ = nullptr;
    ChartSeries* short_term_series_ = nullptr;
    ChartSeries* integrated_series_ = nullptr;
    ChartSeries* target_series_ = nullptr;
    ChartAxis* lufs_axis_x_ = nullptr;
    ChartAxis* lufs_axis_y_ = nullptr;
    // 电平曲线
    MetricChartWidget* level_chart_ = nullptr;
    ChartSeries* rms_series_ = nullptr;
    ChartSeries* peak_series_ = nullptr;
    ChartSeries* true_peak_series_ = nullptr;
    ChartAxis* level_axis_x_ = nullptr;
    ChartAxis* level_axis_y_ = nullptr;
    // 静音段 / 削波点时间轴
    MetricChartWidget* event_chart_ = nullptr;
    ChartSeries* silence_series_ = nullptr;
    ChartSeries* clip_series_ = nullptr;
    ChartAxis* event_axis_x_ = nullptr;
    ChartAxis* event_axis_y_ = nullptr;
    // 声道能量柱状图
    MetricChartWidget* channel_chart_ = nullptr;
    ChartSeries* channel_series_ = nullptr;        // RMS dBFS
    ChartSeries* channel_peak_series_ = nullptr;   // 峰值 dBFS
    ChartAxis* channel_axis_x_ = nullptr;
    ChartAxis* channel_axis_y_ = nullptr;
    // 声道相关性曲线
    MetricChartWidget* corr_chart_ = nullptr;
    ChartSeries* corr_series_ = nullptr;
    ChartAxis* corr_axis_x_ = nullptr;
    ChartAxis* corr_axis_y_ = nullptr;

    QTableWidget* clip_table_ = nullptr;
    QTableWidget* silence_table_ = nullptr;
    QTableWidget* verdict_table_ = nullptr;
    QTableWidget* metadata_table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
