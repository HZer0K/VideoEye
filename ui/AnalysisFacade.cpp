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
    model::AnalysisResult result;
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
    // 过期事件在 facade 这一层就丢掉，不往下游传。
    //
    // 以前只挡住了写入 impl_->result，信号照样发出去 —— 过滤责任被推给了每个监听者。
    // 页面只要有一处忘了判代际，旧任务的完成/失败就会把当前任务的状态顶掉：
    // 界面显示"分析完成"但 result() 是新任务的（或反过来弹一条上一个文件的错误）。
    // 统一在源头拦，所有监听者就都只剩"收到的一定是新鲜事件"这一条假设。
    connect(&impl_->coordinator, &qt::QtAnalysisController::AnalysisFinished,
            this, [this](quint64 generation, bool completed, const model::AnalysisResult& result) {
                if (generation != impl_->coordinator.generation())
                    return;
                impl_->result = result;
                emit AnalysisFinished(generation, completed, result);
            });
    // 失败同理：on_failed 带的是**失败那次任务**的代际号，切文件后来迟到的那条
    // 不该弹给现在的用户。
    connect(&impl_->coordinator, &qt::QtAnalysisController::AnalysisFailed,
            this, [this](quint64 generation, const QString& message) {
                if (generation != impl_->coordinator.generation())
                    return;
                emit AnalysisFailed(generation, message);
            });
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

const model::AnalysisResult& AnalysisFacade::result() const {
    return impl_->result;
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

model::QcReport AnalysisFacade::Evaluate(const model::AnalysisResult& r) const {
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
