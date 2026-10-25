#pragma once

// 「问题清单」子页：逐秒码率 / 帧率曲线 + QC 问题表 + 跳转到问题位置。
//
// 从 DiagnosticsPage 拆出的展示型组件。它只认"一次扫描的结果" ——
// SetReport() 进来，重画曲线与问题表；跳转按钮只发 SeekRequested，
// 由页面把信号接到播放器。扫描状态机（开始/取消/终态/看门狗）仍留在 DiagnosticsPage，
// 本视图不知道扫描在跑还是在停。

#include <QString>
#include <QWidget>

#include <QTableWidget>

#include <vector>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class DiagnosticsIssueView : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticsIssueView(QWidget* parent = nullptr);

    // 一次扫描的两个产物：result 提供逐秒曲线，report 提供问题清单。
    // 两者必须来自同一次扫描（调用方保证），本视图不做一致性校验。
    void SetReport(const model::AnalysisResult& result, const model::QcReport& report);

    // 换文件 / 新扫描开始：清掉上一份结果（曲线与问题表一起清）。
    // 只清展示，不影响调用方持有的报告快照。
    void Clear();

signals:
    // 「跳转到问题位置」：参数是选中问题的起始秒数（文件级问题不会发）。
    void SeekRequested(double seconds);

private:
    void SetupUi();
    void RebuildIssueTable();
    void UpdateChart();
    void OnJumpToIssue();

    MetricChartWidget* chart_ = nullptr;
    ChartSeries* bitrate_series_ = nullptr;
    ChartSeries* fps_series_ = nullptr;
    ChartAxis* axis_x_ = nullptr;
    ChartAxis* axis_bitrate_ = nullptr;
    ChartAxis* axis_fps_ = nullptr;
    QTableWidget* issue_table_ = nullptr;

    // 只留问题清单（跳转需要按行取问题位置），整份报告由页面持有。
    std::vector<model::DiagnosticIssue> issues_;
    // 曲线数据来源：页面保证该引用的生命周期覆盖到下一次 SetReport/Clear
    // （AnalysisFacade 的结果快照在下次扫描前不会变）。
    const model::AnalysisResult* result_ = nullptr;
};

} // namespace ui
} // namespace videoeye
