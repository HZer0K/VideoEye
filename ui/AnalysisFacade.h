#pragma once

// AnalysisPanel 与核心分析层的唯一接缝（UI facade）。
//
// 之前 AnalysisPanel 直接持有 AnalysisCoordinator / QcRuleEngine / TimelineAnalyzer
// 并 include 8 个 core/analysis 头；现在这些分析机件全部收口到本 facade，UI 只看到一个
// 对象：编排（Start/Cancel/IsRunning）、QC 规则引擎、时间轴实时分析、全文件扫描结果，
// 以及 3 个转发自底层协调器的 Qt 信号。
//
// 这样 AnalysisPanel 的头文件不再直接依赖 core/analysis/*，UI 的依赖面收敛到本文件。

#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "core/qt/QtAnalysisController.h"    // QtAnalysisController + AnalysisResult + AnalysisOptions + 信号
#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/QcRule.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/SceneChangeResult.h"

namespace videoeye {
namespace ui {

// 分析流程的 UI 接缝：把"跑分析 / QC 评估 / 时间轴实时统计"三件事收口成一个对象。
//
// 所有分析机件都是本类的直接成员（不挂 Qt 父对象，避免双重析构）；底层协调器的信号
// 原样转发，UI 侧的连接代码无需改动语义。
class AnalysisFacade : public QObject {
    Q_OBJECT

public:
    explicit AnalysisFacade(QObject* parent = nullptr);
    ~AnalysisFacade() override;

    // ---- 编排（转发自 QtAnalysisController）----
    quint64 StartAnalysis(const std::string& file_path,
                          const analyzer::AnalysisOptions& options = analyzer::AnalysisOptions{});
    void Cancel();
    bool IsRunning() const;
    quint64 generation() const;

    // ---- 全文件扫描结果（诊断唯一数据源）----
    const analyzer::AnalysisResult& result() const { return result_; }
    void SetResult(const analyzer::AnalysisResult& r) { result_ = r; }

    // ---- QC 规则引擎 ----
    const std::vector<model::QcRule>& rules() const { return qc_rule_engine_.rules(); }
    std::vector<model::QcRule>& rules() { return qc_rule_engine_.rules(); }
    void SetRules(const std::vector<model::QcRule>& rules) { qc_rule_engine_.SetRules(rules); }
    model::QcReport Evaluate(const analyzer::AnalysisResult& r) const {
        return qc_rule_engine_.Evaluate(r);
    }

    // ---- 时间轴实时分析（播放逐包 / 逐帧 / 音视频偏移）----
    void OnSyncSample(double audio_ms, double video_ms) {
        timeline_analyzer_.OnSyncSample(audio_ms, video_ms);
    }
    void OnPacket(const model::PacketTiming& packet) { timeline_analyzer_.OnPacket(packet); }
    void OnFrame(const model::FrameTimingInfo& frame) { timeline_analyzer_.OnFrame(frame); }
    model::TimelineAnalysisResult Snapshot() const { return timeline_analyzer_.Snapshot(); }
    void Reset() { timeline_analyzer_.Reset(); }

    // ---- 场景切换关联（原 BitrateGopAnalyzer::ApplySceneChanges，就地刷新结果）----
    void ApplySceneChanges(const std::vector<model::SceneChangeResult>& changes,
                           const analyzer::BitrateGopOptions& options) {
        analyzer::BitrateGopAnalyzer::ApplySceneChanges(result_.bitrate_gop, changes, options);
    }

signals:
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const analyzer::AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

private:
    qt::QtAnalysisController coordinator_;
    analyzer::QcRuleEngine qc_rule_engine_;
    analyzer::TimelineAnalyzer timeline_analyzer_;
    analyzer::AnalysisResult result_;
};

} // namespace ui
} // namespace videoeye
