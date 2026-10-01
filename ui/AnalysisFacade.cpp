#include "ui/AnalysisFacade.h"

// 具体分析器的头文件只出现在这里 —— 它们是实现细节，不进 facade 的公开头。
#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/qt/QtAnalysisController.h"

namespace videoeye {
namespace ui {

// 分析机件的实际持有者。全部按值存放：它们不是 QObject（QtAnalysisController
// 除外，但它挂在 facade 的 QObject 树之外，由 Impl 的析构顺序负责释放），
// 不设 Qt 父对象以免与 unique_ptr 双重析构。
struct AnalysisFacade::Impl {
    qt::QtAnalysisController coordinator;
    analyzer::QcRuleEngine qc_rule_engine;
    analyzer::TimelineAnalyzer timeline_analyzer;
    analyzer::AnalysisResult result;
};

AnalysisFacade::AnalysisFacade(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    // 底层协调器的信号原样转发给本 facade 的信号，UI 只连 facade。
    connect(&impl_->coordinator, &qt::QtAnalysisController::ProgressReported,
            this, &AnalysisFacade::ProgressReported);
    // AnalysisFinished 不是"原样转发"，而是 facade 自己先落库再转发。
    //
    // 为什么放在这里: 扫描结果必须先写进 facade，任何监听 AnalysisFinished 的人
    // （页面、报告导出）才可能读到。以前这一步散在 DiagnosticsPage::OnFacadeFinished 里，
    // 结果就是"页面忘了写"成了整个功能的单点故障 —— 信号发了、result() 还是默认空结果，
    // 问题清单/评分/报告全空。编排层自己保存，页面只负责展示，才不会再漏。
    connect(&impl_->coordinator, &qt::QtAnalysisController::AnalysisFinished,
            this, [this](quint64 generation, bool completed, const analyzer::AnalysisResult& result) {
                // 只认当前代际: 旧任务迟到的回包不能覆盖新结果（与页面的过滤规则一致）。
                if (generation == impl_->coordinator.generation()) {
                    impl_->result = result;
                }
                emit AnalysisFinished(generation, completed, result);
            });
    connect(&impl_->coordinator, &qt::QtAnalysisController::AnalysisFailed,
            this, &AnalysisFacade::AnalysisFailed);
}

AnalysisFacade::~AnalysisFacade() = default;

quint64 AnalysisFacade::StartAnalysis(const std::string& file_path,
                                      const analyzer::AnalysisOptions& options) {
    return impl_->coordinator.StartAnalysis(file_path, options);
}

void AnalysisFacade::Cancel() {
    impl_->coordinator.Cancel();
}

bool AnalysisFacade::IsRunning() const {
    return impl_->coordinator.IsRunning();
}

quint64 AnalysisFacade::generation() const {
    return impl_->coordinator.generation();
}

const analyzer::AnalysisResult& AnalysisFacade::result() const {
    return impl_->result;
}

void AnalysisFacade::SetResult(const analyzer::AnalysisResult& r) {
    impl_->result = r;
}

const std::vector<model::QcRule>& AnalysisFacade::rules() const {
    return impl_->qc_rule_engine.rules();
}

std::vector<model::QcRule>& AnalysisFacade::rules() {
    return impl_->qc_rule_engine.rules();
}

void AnalysisFacade::SetRules(const std::vector<model::QcRule>& rules) {
    impl_->qc_rule_engine.SetRules(rules);
}

model::QcReport AnalysisFacade::Evaluate(const analyzer::AnalysisResult& r) const {
    return impl_->qc_rule_engine.Evaluate(r);
}

void AnalysisFacade::OnSyncSample(double audio_ms, double video_ms) {
    impl_->timeline_analyzer.OnSyncSample(audio_ms, video_ms);
}

void AnalysisFacade::OnPacket(const model::PacketTiming& packet) {
    impl_->timeline_analyzer.OnPacket(packet);
}

void AnalysisFacade::OnFrame(const model::FrameTimingInfo& frame) {
    impl_->timeline_analyzer.OnFrame(frame);
}

model::TimelineAnalysisResult AnalysisFacade::Snapshot() const {
    return impl_->timeline_analyzer.Snapshot();
}

void AnalysisFacade::Reset() {
    impl_->timeline_analyzer.Reset();
}

void AnalysisFacade::ApplySceneChanges(const std::vector<model::SceneChangeResult>& changes,
                                       const analyzer::BitrateGopOptions& options) {
    analyzer::BitrateGopAnalyzer::ApplySceneChanges(impl_->result.bitrate_gop, changes, options);
}

} // namespace ui
} // namespace videoeye
