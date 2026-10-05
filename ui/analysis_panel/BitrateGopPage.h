#pragma once

// 码率与 GOP 页：滑动窗口码率曲线（叠加 I 帧 / 场景切换 / 异常峰值标记）+ GOP 列表
// + 异常表 + 优化建议。
//
// 从 AnalysisPanel 拆出来的独立页面组件。与「诊断与报告」「音频 QC」「色彩与 HDR」
// 共用同一次全文件扫描，所以接缝是：
//   - 数据进来：SetResult() / SetSceneChanges() / SetScanActive() / SetProgress()
//   - 意图出去：ScanRequested() / CancelRequested() / SeekRequested() /
//               SceneLinkRequested()
// 「关联场景切换」要动 QC 报告并重算诊断页的问题表，那一步仍在面板编排 —— 本页不持有
// facade，也不知道诊断页的问题表。执行完由面板回调 ShowSceneLinkSummary()。
// 页面本体就是 QWidget（外部 QStackedWidget 的一页），不需要再包一层 tab widget。

#include <QWidget>
#include <QString>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>

#include <cstddef>
#include <vector>

#include "core/domain/model/SceneChangeResult.h"
#include "ui/AnalysisFacade.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class BitrateGopPage : public QWidget {
    Q_OBJECT

public:
    explicit BitrateGopPage(QWidget* parent = nullptr);

    // 导出 CSV 的默认文件名用当前文件路径拼，面板换文件时同步一次
    void SetSourcePath(const QString& path) { source_path_ = path; }

    // 扫描结束（或规则重算后）由面板喂结果
    void SetResult(const model::AnalysisResult& result);

    // 「场景切换」页的检测记录：曲线上画标记，「关联场景切换」按钮要用
    void SetSceneChanges(const std::vector<model::SceneChangeResult>& records);

    // 面板发起扫描前调用：把 UI 上的窗口 / 阈值同步进 AnalysisOptions
    void FillScanOptions(videoeye::AnalysisOptions& options);

    // 扫描生命周期由面板驱动（与诊断页、音频 QC、色彩/HDR 保持同步）
    void SetScanActive(bool active);
    // 与 SetScanActive 配对的状态查询。三页共用同一次扫描，任何一页卡在 active
    // 都是 bug（取消按钮亮着、"开始分析"永久禁用），所以这个状态必须能被观察到。
    bool IsScanActive() const { return scan_active_; }
    void SetProgress(int percent);
    void SetProgressFormat(const QString& format);

    // 面板在 facade 上执行完 ApplySceneChanges 后回调：刷新本页并弹结果提示
    void ShowSceneLinkSummary();

signals:
    void ScanRequested();
    void CancelRequested();
    // 点 GOP / 异常行跳到对应时间点
    void SeekRequested(double seconds);
    // 用「场景切换」页的记录检查附近是否有关键帧
    void SceneLinkRequested(const std::vector<model::SceneChangeResult>& records,
                            const videoeye::BitrateGopOptions& options);

public slots:
    void Refresh();

private:
    // 扫描态（由 SetScanActive 写入；面板成功/取消/失败三种终态都必须把它复位）
    bool scan_active_ = false;

    void SetupUi();
    void ApplyOptionsFromUi();
    void OnStartAnalysis();
    void OnCancelAnalysis();
    void OnWindowChanged();
    void OnOptionChanged();
    void OnLinkSceneChanges();
    void OnGopCellClicked(int row, int column);
    void OnAnomalyCellClicked(int row, int column);
    void UpdateSummary();
    void UpdateChart();
    void RebuildGopTable();
    void RebuildAnomalyTable();
    void UpdateSuggestions();
    void OnExportGopCsv();
    void OnExportCurveCsv();
    void OnExportAnomalyCsv();

    QString source_path_;
    bool has_result_ = false;
    model::AnalysisResult result_;
    videoeye::BitrateGopOptions options_;
    bool decode_frame_types_ = false;
    std::vector<model::SceneChangeResult> scene_changes_;
    double display_window_ = 1.0;   // 当前图表显示的窗口长度

    QLabel* summary_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QComboBox* window_combo_ = nullptr;
    QDoubleSpinBox* target_peak_spin_ = nullptr;
    QDoubleSpinBox* max_gop_seconds_spin_ = nullptr;
    QSpinBox* max_gop_frames_spin_ = nullptr;
    QCheckBox* decode_types_check_ = nullptr;

    MetricChartWidget* chart_ = nullptr;
    ChartSeries* bitrate_series_ = nullptr;
    ChartSeries* target_series_ = nullptr;
    ChartSeries* iframe_series_ = nullptr;
    ChartSeries* scene_series_ = nullptr;
    ChartSeries* anomaly_series_ = nullptr;
    ChartAxis* axis_x_ = nullptr;
    ChartAxis* axis_y_ = nullptr;

    QTabWidget* sub_tabs_ = nullptr;
    QTableWidget* gop_table_ = nullptr;
    QTableWidget* anomaly_table_ = nullptr;
    QListWidget* suggestion_list_ = nullptr;
};

} // namespace ui
} // namespace videoeye
