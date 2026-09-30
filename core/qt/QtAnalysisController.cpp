#include "core/qt/QtAnalysisController.h"

#include <utility>

#include <QMetaType>

namespace videoeye {
namespace qt {

QtAnalysisController::QtAnalysisController(QObject* parent) : QObject(parent) {
    // AnalysisResult 要跨线程走队列连接，必须先在元类型系统里注册，
    // 否则排队时会报 "QObject::connect: Cannot queue arguments" 而槽永远不被调用。
    qRegisterMetaType<analyzer::AnalysisResult>("videoeye::analyzer::AnalysisResult");
    qRegisterMetaType<analyzer::AnalysisResult>("AnalysisResult");
}

QtAnalysisController::~QtAnalysisController() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}

quint64 QtAnalysisController::StartAnalysis(const std::string& file_path,
                                            const analyzer::AnalysisOptions& options) {
    Cancel();
    if (worker_.joinable()) worker_.join();

    const quint64 gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine_.Reset();
    running_.store(true, std::memory_order_release);

    analyzer::AnalysisCallbacks callbacks;
    callbacks.on_progress = [this, gen](double percent, const std::string& stage) {
        emit ProgressReported(gen, percent, QString::fromStdString(stage));
    };
    callbacks.on_failed = [this, gen](const std::string& message) {
        running_.store(false, std::memory_order_release);
        emit AnalysisFailed(gen, QString::fromStdString(message));
    };
    callbacks.on_finished = [this, gen](bool completed, const analyzer::AnalysisResult& result) {
        running_.store(false, std::memory_order_release);
        emit AnalysisFinished(gen, completed, result);
    };

    worker_ = std::thread([this, gen, file_path, options, callbacks]() {
        engine_.Run(file_path, options, callbacks);
    });
    return gen;
}

void QtAnalysisController::Cancel() {
    engine_.Cancel();
}

} // namespace qt
} // namespace videoeye
