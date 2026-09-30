#pragma once

// 分析流程的 Qt 接线层。
//
// 它自己不做任何分析：执行在 AnalysisEngine，这里只负责
//   * 起后台线程
//   * generation 机制（快速切换文件时丢弃旧任务的回包）
//   * 把引擎的回调转发成 Qt 信号
//
// 回调是在工作线程里激发的，信号会在接收者所在线程排队执行 —— 这也是为什么本类
// 只发信号、不直接碰界面控件。

#include <atomic>
#include <memory>
#include <thread>

#include <QObject>
#include <QString>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/AnalysisResult.h"
#include "core/analysis/orchestration/AnalysisEngine.h"

namespace videoeye {
namespace qt {

class QtAnalysisController : public QObject {
    Q_OBJECT

public:
    explicit QtAnalysisController(QObject* parent = nullptr);
    ~QtAnalysisController() override;

    // 启动一次分析，返回本次 generation
    quint64 StartAnalysis(const std::string& file_path,
                          const analyzer::AnalysisOptions& options = analyzer::AnalysisOptions{});

    // 请求取消当前分析（异步，结果仍会以 completed=false 返回）
    void Cancel();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    quint64 generation() const { return generation_.load(std::memory_order_acquire); }

signals:
    // percent: 0..100
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const analyzer::AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

private:
    std::atomic<bool> running_{false};
    std::atomic<quint64> generation_{0};
    std::thread worker_;
    analyzer::AnalysisEngine engine_;
};

} // namespace qt
} // namespace videoeye
