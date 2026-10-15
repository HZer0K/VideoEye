#pragma once

// 首页「分析状态」区块的组装口径（从 MainWindow::RefreshOverviewAnalysis 抽出）。
//
// 抽出来的理由和 ui/reporting_panel/batch_filter.h 一样：**这一段有业务口径，而口径
// 原本只活在 UI 事件回调里，测不到**。具体是三条：
//   1. 「扫描失败」的判定是「没在扫描 且 有错误文本」——扫描重启后旧错误必须让位
//      给「进行中」，否则首页同时挂一个转圈和一个红字；
//   2. 「最重要的几条异常」是严重度降序、**同严重度保持规则引擎的输出顺序**
//      （stable_sort，不是 sort）——规则引擎按流/规则 id 排过一遍，再按严重度
//      重排时不能把同一档里的顺序洗掉，否则用户前后两次看到的清单顺序会跳；
//   3. 取前 N 条而不是全量——首页只放摘要，全量在质量诊断页。
//
// 依赖上只有 QString / QVector（显示层用的容器）+ 纯 C++ 的 model::QcReport，
// 不碰 QWidget，所以单测不需要 QApplication。

#include <cstddef>
#include <vector>

#include <QString>
#include <QVector>

#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/QcReport.h"

namespace videoeye {
namespace ui {

// 首页摘要里展示的异常条数上限。
constexpr std::size_t kOverviewTopIssueCount = 5;

// 严重度排序键：越大越严重。返回值只用于相互比较，具体数值没有外部含义。
inline int IssueSeverityRank(model::IssueSeverity severity) {
    switch (severity) {
        case model::IssueSeverity::Critical: return 3;
        case model::IssueSeverity::Error: return 2;
        case model::IssueSeverity::Warning: return 1;
        case model::IssueSeverity::Info: return 0;
    }
    return 0;
}

// 严重度降序 + 同级稳定（保持入参顺序），返回**值拷贝**的新数组。
// 不改动入参，也不返回指针 —— 摘要要跨事件存活，指向 report.issues 的指针会
// 在下一次扫描落地的瞬间悬空。
inline std::vector<model::DiagnosticIssue> RankIssuesBySeverity(
    const std::vector<model::DiagnosticIssue>& issues) {
    std::vector<model::DiagnosticIssue> ranked = issues;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const model::DiagnosticIssue& a, const model::DiagnosticIssue& b) {
                         return IssueSeverityRank(a.severity) > IssueSeverityRank(b.severity);
                     });
    return ranked;
}

// 取排好序的前 limit 条。limit=0 表示不展示（返回空），不是"不限制"——
// 调用方想全量就直接用 RankIssuesBySeverity。
inline std::vector<model::DiagnosticIssue> TopIssuesBySeverity(
    const std::vector<model::DiagnosticIssue>& issues,
    std::size_t limit = kOverviewTopIssueCount) {
    if (limit == 0) return {};
    std::vector<model::DiagnosticIssue> ranked = RankIssuesBySeverity(issues);
    if (ranked.size() > limit) ranked.resize(limit);
    return ranked;
}

// 首页分析状态摘要。MainWindow 拉完 AnalysisPanel 的只读快照后填 Input，
// 由 BuildOverviewAnalysisSummary 一次性算出，再交给 OverviewPage 显示。
struct OverviewAnalysisSummary {
    bool has_file = false;       // 是否打开了文件（未打开时只显示空态提示）
    bool scan_running = false;   // 扫描进行中
    bool failed = false;         // 最近一次扫描失败（与 has_result 互斥展示）
    QString error;               // 失败原因（failed 时展示）
    bool has_result = false;     // 是否有一份可用结果
    bool partial = false;        // 结果是否不完整（被取消）
    double score = 0.0;          // 0..100
    QString verdict;             // 通过 / 警告 / 不通过
    int critical = 0;
    int error_count = 0;
    int warning = 0;
    int info = 0;
    // 最重要的几条异常（严重度降序取前 kOverviewTopIssueCount 条，值拷贝）
    QVector<model::DiagnosticIssue> top_issues;
};

// BuildOverviewAnalysisSummary 的输入：全是 MainWindow 从 AnalysisPanel 拉到的只读快照。
struct OverviewAnalysisInput {
    bool has_file = false;
    bool scan_running = false;
    QString error;                          // 最近一次扫描的失败原因，空 = 没失败
    bool has_result = false;                // has_result 为真时 report 必须非空
    const model::QcReport* report = nullptr; // 借用，函数内不持有
};

// 组装摘要。纯函数：同样的输入必然给同样的输出，且不触碰任何 UI 对象。
inline OverviewAnalysisSummary BuildOverviewAnalysisSummary(const OverviewAnalysisInput& in) {
    OverviewAnalysisSummary summary;
    summary.has_file = in.has_file;
    summary.scan_running = in.scan_running;
    summary.error = in.error;
    // 「失败」的口径：只在**没有正在扫描**时才成立。扫描一旦重启，上一次的错误文本
    // 就该让位给「进行中」——两个状态同时为真会让首页既转圈又报红。
    summary.failed = !in.scan_running && !in.error.isEmpty();

    if (!in.has_result || in.report == nullptr) return summary;

    const model::QcReport& report = *in.report;
    summary.has_result = true;
    // 沿用抽出来之前的口径：以 completed 为准（取消/中断会让 completed=false）。
    // 注：QcReport::partial 记的是"命中包数上限的抽样扫描"，目前不参与本判定。
    summary.partial = !report.completed;
    summary.score = report.score;
    summary.verdict = QString::fromStdString(report.verdict);
    summary.critical = report.CountBySeverity(model::IssueSeverity::Critical);
    summary.error_count = report.CountBySeverity(model::IssueSeverity::Error);
    summary.warning = report.CountBySeverity(model::IssueSeverity::Warning);
    summary.info = report.CountBySeverity(model::IssueSeverity::Info);

    const std::vector<model::DiagnosticIssue> top =
        TopIssuesBySeverity(report.issues, kOverviewTopIssueCount);
    summary.top_issues = QVector<model::DiagnosticIssue>(top.begin(), top.end());
    return summary;
}

}  // namespace ui
}  // namespace videoeye
