#pragma once

// 「问题清单」子页：筛选 + 问题表 + 详情（说明 / 证据 / 建议）+ 逐秒码率帧率曲线。
//
// 从 DiagnosticsPage 拆出的展示型组件。它只认"一次扫描的结果" ——
// SetReport() 进来，重画曲线与问题表；跳转按钮只发 SeekRequested，
// 由页面把信号接到播放器。扫描状态机（开始/取消/终态/看门狗）仍留在 DiagnosticsPage，
// 本视图不知道扫描在跑还是在停。
//
// 为什么主表只有四列（严重度 / 问题 / 位置 / 类别）：
// 以前是六列，"说明"与"建议"两段长文本和结构化字段抢横向空间 —— 表格被撑到
// 必须横向滚动，而真正需要快速扫读的四列反而被挤成细缝。现在长文本进右侧
// 详情区（跟着选中行走），主表只留可扫读的字段，定位动作也放在详情区。
//
// 首页「异常直达」的落点就是 SelectIssue()：按问题身份（rule_id + 起点 + 流）
// 选中对应行并刷新详情，用户不必再自己翻表找人。

#include <QString>
#include <QWidget>

#include <QTableWidget>

#include <vector>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "ui/charts/MetricChartWidget.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTextBrowser;

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

    // 按问题身份选中并展开详情（首页「异常直达」的落点）。
    // 命中返回 true；未命中返回 false，界面保持原样，由调用方决定要不要提示。
    // 目标被当前筛选挡住时会先把筛选重置为「全部」——"一步直达"不能因为
    // 用户几轮之前选过某个筛选条件就失效。
    bool SelectIssue(const QString& rule_id, double start_seconds, int stream_index);

    // 当前选中行对应的问题在报告 issues 里的下标（无选中返回 -1）
    int CurrentIssueIndex() const;

signals:
    // 「跳转到问题位置」：参数是选中问题的起始秒数（文件级问题不会发）。
    void SeekRequested(double seconds);

private:
    void SetupUi();
    void RebuildIssueTable();
    void UpdateChart();
    void UpdateDetail();
    void OnJumpToIssue();
    int IssueIndexForRow(int row) const;
    void RebuildFilterOptions();
    // 当前筛选是否放行这条问题（severity_filter_ / category_filter_ 的 data 为 -1 表示全部）
    bool PassesFilter(const model::DiagnosticIssue& issue) const;

    MetricChartWidget* chart_ = nullptr;
    ChartSeries* bitrate_series_ = nullptr;
    ChartSeries* fps_series_ = nullptr;
    ChartAxis* axis_x_ = nullptr;
    ChartAxis* axis_bitrate_ = nullptr;
    ChartAxis* axis_fps_ = nullptr;

    QComboBox* severity_filter_ = nullptr;
    QComboBox* category_filter_ = nullptr;
    QLabel* count_label_ = nullptr;
    QTableWidget* issue_table_ = nullptr;
    QTextBrowser* detail_text_ = nullptr;
    QPushButton* jump_button_ = nullptr;

    // 只留问题清单（跳转需要按行取问题位置），整份报告由页面持有。
    std::vector<model::DiagnosticIssue> issues_;
    // 显示行 -> issues_ 下标。筛选之后行号与问题下标不再相等，
    // 所有"按行取问题"的地方都必须走这张表。
    std::vector<std::size_t> visible_issues_;
    // 曲线数据来源：页面保证该引用的生命周期覆盖到下一次 SetReport/Clear
    // （AnalysisFacade 的结果快照在下次扫描前不会变）。
    const model::AnalysisResult* result_ = nullptr;
};

} // namespace ui
} // namespace videoeye
