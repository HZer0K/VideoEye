#pragma once

// 诊断与报告页（功能 11）：全文件扫描 + QC 规则引擎 + 时间轴/同步诊断。
//
// 从 AnalysisPanel 拆出来的独立页面组件，同时是「扫描总控」的落点：
// AnalysisFacade、当前 QC 报告、扫描代数、时间轴（离线/实时）状态都归本页所有，
// 面板只做两件事 —— 把 current_video_path_ 与播放期的包/帧/同步采样喂进来，
// 以及把 ScanStarted / ProgressChanged / ScanFinished 转发给共用同一次扫描的
// 其它几页（码率与 GOP / 音频 QC / 色彩与 HDR）。
//
// 面板侧要拿结果时用 result() / qcReport() / hasResult() 取只读快照，
// 不要反过来碰本页成员（与 ContainerStructurePage / AudioQcPage 一致的约定）。

#include <QWidget>
#include <QString>
#include <QVector>
#include <QList>

#include <chrono>
#include <functional>

#include <QProgressBar>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>

#include "ui/AnalysisFacade.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/charts/MetricChartWidget.h"

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/FrameTimingInfo.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/TimelineDiagnostic.h"

namespace videoeye {
namespace ui {

class DiagnosticsPage : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);

    // 面板换文件时同步：本页自己持有默认扫描选项与当前文件路径（StartScan 用）
    void SetSourcePath(const QString& path) { source_path_ = path; }

    // 默认扫描选项：其它页 FillScanOptions(options) 的基线，也允许就地改单项开关
    // （「流媒体包」「参数集解析」页的「重新扫描」就是改一个 analyze_* 开关）
    analyzer::AnalysisOptions& options() { return options_; }
    const analyzer::AnalysisOptions& options() const { return options_; }

    // 全文件扫描（一次 demux 供多页共用：诊断页 / 码率 GOP / 音频 QC /
    // 色彩 HDR / 字幕辅助 / 流媒体包 / 参数集）
    void StartScan(const analyzer::AnalysisOptions& options);
    void CancelScan();

    // 每次扫描前的钩子：面板挂上「字幕阈值从规则表同步」这类跨页逻辑，
    // 页面自己不认识 SubtitleAuxPage（避免页面之间互相 include）
    using ScanHook = std::function<void(analyzer::AnalysisOptions&)>;
    void SetBeforeScanHook(ScanHook hook) { before_scan_ = std::move(hook); }

    // 播放期实时统计入口（MediaPlayer 回调 → 面板转发 → 本页）
    void OnSyncSample(double audio_ms, double video_ms);
    void OnPacketTiming(const model::PacketTiming& timing);
    void OnFrameTiming(const model::FrameTimingInfo& timing);

    // 「关联场景切换」：报告重算留在诊断页，码率页只负责展示自己的结果
    void ApplySceneLink(const std::vector<model::SceneChangeResult>& records,
                        const analyzer::BitrateGopOptions& gop_options);

    // 换文件：清掉上一文件的扫描结果 / 时间轴累计状态
    void ResetForNewFile();

    // 只读快照：面板分发给共用同一次扫描的其它页
    const std::vector<model::QcRule>& rules() const;
    const analyzer::AnalysisResult& result() const;
    const model::QcReport& qcReport() const { return report_; }
    bool hasResult() const { return has_result_; }

signals:
    // 扫描生命周期：面板收到后同步其它几页的按钮 / 进度条
    void ScanStarted();
    void ProgressChanged(double percent, const QString& stage);
    void ScanFinished(bool completed);
    // 用户点了取消：面板要把共用扫描的其它几页一起退出扫描态
    void ScanCancelled();
    // 报告重算（改规则 / 关联场景切换）后通知面板再刷一遍音频 QC 与色彩 HDR
    void QcReportChanged(const model::QcReport& report);
    // 点「跳转到问题帧」
    void SeekRequested(double seconds);

private slots:
    void OnCancelClicked();
    void OnExportClicked();
    void OnRuleItemChanged(QTableWidgetItem* item);
    void OnResetRulesClicked();
    void OnFacadeProgress(quint64 generation, double percent, const QString& stage);
    void OnFacadeFinished(quint64 generation, bool completed,
                          const analyzer::AnalysisResult& result);
    void OnFacadeFailed(quint64 generation, const QString& message);
    void OnTimelineMarkerHovered(const QPointF& point, bool state);
    void OnJumpToIssue();
    void FlushTimeline();

private:
    void SetupUi();
    void SetupRuleTab();
    void SetupTimelineSubTab();
    // 用 facade 结果 + QC 规则引擎重算报告并刷新本页（同时外发 QcReportChanged）
    void Evaluate();
    void RebuildIssueTable();
    void UpdateQcSummary();
    void UpdateQcChart();
    void RebuildRuleTable();
    // 时间轴与同步（离线=全文件扫描结果，在线=播放实时累积）
    void RefreshTimelineUi();
    void UpdateTimelineSummary();
    void UpdateTimelineChart();
    void SetScanButtonState(bool running);

    QString source_path_;
    analyzer::AnalysisOptions options_;
    model::QcReport report_;
    quint64 generation_ = 0;
    bool has_result_ = false;
    bool rule_table_updating_ = false;
    std::chrono::steady_clock::time_point scan_start_time_;

    // 时间轴状态：播放期逐包/逐帧累积，扫描完成后换成离线结果
    model::TimelineAnalysisResult timeline_result_;
    bool timeline_dirty_ = false;
    bool timeline_offline_ = false;
    QVector<int> timeline_marker_issue_index_;   // 散点序号 -> 问题序号

    AnalysisFacade* facade_ = nullptr;
    ScanHook before_scan_;   // 扫描前的跨页钩子（由面板注入）

    QLabel* summary_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QTabWidget* sub_tabs_ = nullptr;

    // 子页 0：问题清单（逐秒码率/帧率曲线 + 问题表）
    MetricChartWidget* qc_chart_ = nullptr;
    ChartSeries* qc_bitrate_series_ = nullptr;
    ChartSeries* qc_fps_series_ = nullptr;
    ChartAxis* qc_axis_x_ = nullptr;
    ChartAxis* qc_axis_bitrate_ = nullptr;
    ChartAxis* qc_axis_fps_ = nullptr;
    QTableWidget* issue_table_ = nullptr;

    // 子页 1：规则与阈值
    QTableWidget* rule_table_ = nullptr;

    // 子页 2：时间轴与同步
    QLabel* timeline_summary_label_ = nullptr;
    MetricChartWidget* timeline_chart_ = nullptr;
    ChartSeries* timeline_interval_series_ = nullptr;
    ChartSeries* timeline_marker_series_ = nullptr;
    ChartAxis* timeline_axis_x_ = nullptr;
    ChartAxis* timeline_axis_y_ = nullptr;
    QTableWidget* timeline_issue_table_ = nullptr;

    QTimer* flush_timer_ = nullptr;   // 播放期实时时间轴的批量刷新节拍
};

} // namespace ui
} // namespace videoeye
