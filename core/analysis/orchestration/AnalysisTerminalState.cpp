#include "core/analysis/orchestration/AnalysisTerminalState.h"

namespace videoeye {

void NotifyProgress(const AnalysisCallbacks& callbacks, double percent, const std::string& stage) {
    if (callbacks.on_progress) callbacks.on_progress(percent, stage);
}

void NotifyFailed(const AnalysisCallbacks& callbacks, const std::string& message) {
    if (callbacks.on_failed) callbacks.on_failed(message);
}

void NotifyFinished(const AnalysisCallbacks& callbacks, bool completed,
                    const model::AnalysisResult& result) {
    if (callbacks.on_finished) callbacks.on_finished(completed, result);
}

void NotifyCancelled(const AnalysisCallbacks& callbacks, model::AnalysisResult& result) {
    result.scan_status = model::AnalysisStatus::Cancelled;
    NotifyProgress(callbacks, 100.0, "已取消");
    NotifyFinished(callbacks, false, result);
}

void MarkFailed(model::AnalysisResult& result, const std::string& message) {
    result.scan_status = model::AnalysisStatus::Failed;
    result.error_message = message;
    if (result.scan_error_code == 0) result.scan_error_code = kNoFfmpegErrorCode;
}

}  // namespace videoeye
