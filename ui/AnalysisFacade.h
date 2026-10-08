#pragma once

// AnalysisPanel 与核心分析层的唯一接缝（UI facade）。
//
// 之前 AnalysisPanel 直接持有 QtAnalysisController / QcRuleEngine（以及已被移除的
// 播放期 TimelineAnalyzer）并 include 一堆 core/analysis 头；现在这些分析机件全部
// 收口到本 facade 的 Impl，UI 只看到一个对象：编排（Start/Cancel/IsRunning）、
// QC 规则引擎、全文件扫描结果，以及 3 个转发自底层协调器的 Qt 信号。
//
// 为什么用 Impl（pimpl）而不是直接成员变量：直接成员意味着这些分析器的头文件
// 必须出现在 facade 的公开头里 —— 依赖只是从 AnalysisPanel 转移到了 facade，
// 改一个分析器照样会让所有 include 本文件的 UI 代码重编。收进 Impl 之后，本文件
// 只暴露三类类型：AnalysisOptions、AnalysisResult、domain model，
// 分析器实现的改动不再外溢到 UI 的编译图。
//
// 本文件不再 include 任何 core/analysis/*Analyzer.h 与 core/qt/*。

#include <memory>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/domain/model/SceneChangeResult.h"

namespace videoeye {
namespace ui {

// 分析流程的 UI 接缝：把"跑分析 / QC 评估"两件事（外加场景切换关联）收口成一个对象。
//
// 所有分析机件都在 Impl 里（不挂 Qt 父对象，避免双重析构）；底层协调器的信号
// 原样转发，UI 侧的连接代码无需改动语义。
class AnalysisFacade : public QObject {
    Q_OBJECT

public:
    explicit AnalysisFacade(QObject* parent = nullptr);
    ~AnalysisFacade() override;

    // ---- 编排（转发自底层 Qt 协调器）----
    quint64 StartAnalysis(const std::string& file_path,
                          const videoeye::AnalysisOptions& options = videoeye::AnalysisOptions{});
    void Cancel();
    bool IsRunning() const;
    quint64 generation() const;

    // ---- 全文件扫描结果（诊断唯一数据源）----
    //
    // 只读快照: 结果的唯一写入者是本 facade 自己 —— 它在发出 AnalysisFinished
    // **之前**先落库（见 AnalysisFacade.cpp 构造函数里的连接），之后任何页面拿到的
    // 都是同一份不可变快照。
    //
    // 以前这里还有一个 SetResult()，把"写回结果"的责任推给了页面：
    // DiagnosticsPage 漏调一次，问题清单/评分/各页面与报告导出就全部基于空结果（P0）。
    // 修好之后它就没有生产调用者了，只剩一条回归测试在用 —— 留着等于继续对外
    // 提供"页面可以改写分析结果"的口子，与"结果存储是只读快照来源"冲突，故删除。
    const model::AnalysisResult& result() const;

    // ---- QC 规则引擎 ----
    const std::vector<model::QcRule>& rules() const;
    std::vector<model::QcRule>& rules();
    void SetRules(const std::vector<model::QcRule>& rules);
    model::QcReport Evaluate(const model::AnalysisResult& r) const;

    // ---- 时间轴实时分析（播放逐包 / 逐帧 / 音视频偏移）----
    //
    // 播放期的时间轴样本 / 曲线已收敛到「事件与时间轴」页（EventTimelineView）自持，
    // facade 不再维护第二份累积状态（播放热路径上每包/每帧都推一份进来、却无人消费）。
    // 全文件扫描结果里的时间轴问题仍由 QcRuleEngine 在 Evaluate 时并入 QC 报告。

    // ---- 场景切换关联（就地刷新 result().bitrate_gop）----
    //
    // 这是**唯一**允许改动已落库结果的入口（DiagnosticsPage 用它把播放期检测到的
    // 场景切换点关联到扫描结果上）。约束:
    //   * 只允许改 result().bitrate_gop 这一支 —— 扫描事实（包数/时长/码率曲线/流摘要）
    //     一旦落库就是历史，页面改它不是"刷新"而是篡改诊断结论；
    //   * 幂等: 同一批 changes 反复应用结果不变（BitrateGopAnalyzer 侧有回归守住）；
    //   * 不重新发 AnalysisFinished —— 页面自己负责刷新，别让"改了一支字段"
    //     伪装成"跑完了一次扫描"。
    void ApplySceneChanges(const std::vector<model::SceneChangeResult>& changes,
                           const videoeye::BitrateGopOptions& options);

signals:
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const model::AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ui
} // namespace videoeye
