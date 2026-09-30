#pragma once

// AnalysisPanel 与核心分析层的唯一接缝（UI facade）。
//
// 之前 AnalysisPanel 直接持有 QtAnalysisController / QcRuleEngine / TimelineAnalyzer
// 并 include 一堆 core/analysis 头；现在这些分析机件全部收口到本 facade 的 Impl，
// UI 只看到一个对象：编排（Start/Cancel/IsRunning）、QC 规则引擎、时间轴实时分析、
// 全文件扫描结果，以及 3 个转发自底层协调器的 Qt 信号。
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
#include "core/analysis/AnalysisResult.h"
#include "core/domain/model/FrameTimingInfo.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/TimelineDiagnostic.h"

namespace videoeye {
namespace ui {

// 分析流程的 UI 接缝：把"跑分析 / QC 评估 / 时间轴实时统计"三件事收口成一个对象。
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
                          const analyzer::AnalysisOptions& options = analyzer::AnalysisOptions{});
    void Cancel();
    bool IsRunning() const;
    quint64 generation() const;

    // ---- 全文件扫描结果（诊断唯一数据源）----
    const analyzer::AnalysisResult& result() const;
    void SetResult(const analyzer::AnalysisResult& r);

    // ---- QC 规则引擎 ----
    const std::vector<model::QcRule>& rules() const;
    std::vector<model::QcRule>& rules();
    void SetRules(const std::vector<model::QcRule>& rules);
    model::QcReport Evaluate(const analyzer::AnalysisResult& r) const;

    // ---- 时间轴实时分析（播放逐包 / 逐帧 / 音视频偏移）----
    void OnSyncSample(double audio_ms, double video_ms);
    void OnPacket(const model::PacketTiming& packet);
    void OnFrame(const model::FrameTimingInfo& frame);
    model::TimelineAnalysisResult Snapshot() const;
    void Reset();

    // ---- 场景切换关联（就地刷新 result().bitrate_gop）----
    void ApplySceneChanges(const std::vector<model::SceneChangeResult>& changes,
                           const analyzer::BitrateGopOptions& options);

signals:
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const analyzer::AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ui
} // namespace videoeye
