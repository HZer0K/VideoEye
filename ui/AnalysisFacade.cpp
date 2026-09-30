#include "ui/AnalysisFacade.h"

namespace videoeye {
namespace ui {

AnalysisFacade::AnalysisFacade(QObject* parent) : QObject(parent) {
    // 底层协调器的信号原样转发给本 facade 的信号，UI 只连 facade。
    connect(&coordinator_, &qt::QtAnalysisController::ProgressReported,
            this, &AnalysisFacade::ProgressReported);
    connect(&coordinator_, &qt::QtAnalysisController::AnalysisFinished,
            this, &AnalysisFacade::AnalysisFinished);
    connect(&coordinator_, &qt::QtAnalysisController::AnalysisFailed,
            this, &AnalysisFacade::AnalysisFailed);
}

AnalysisFacade::~AnalysisFacade() = default;

quint64 AnalysisFacade::StartAnalysis(const std::string& file_path,
                                      const analyzer::AnalysisOptions& options) {
    return coordinator_.StartAnalysis(file_path, options);
}

void AnalysisFacade::Cancel() {
    coordinator_.Cancel();
}

bool AnalysisFacade::IsRunning() const {
    return coordinator_.IsRunning();
}

quint64 AnalysisFacade::generation() const {
    return coordinator_.generation();
}

} // namespace ui
} // namespace videoeye
