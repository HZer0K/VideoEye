#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <QObject>
#include <QString>

#include "core/analyzer/AnalysisTask.h"

namespace videoeye {
namespace analyzer {

// 全文件分析调度器
//
// 职责边界:
//   - 只做"离线全文件 demux 扫描"，与播放解耦（播放中的实时统计仍由 StreamAnalyzer 负责）
//   - 后台线程执行，通过信号把进度/结果抛回 UI 线程
//   - generation 机制: 每次 StartAnalysis 递增，UI 只接受与当前 generation 匹配的结果，
//     避免快速切换文件时旧任务的回调覆盖新结果（与 MediaPlayer 的 generation 思路一致）
class AnalysisCoordinator : public QObject {
    Q_OBJECT

public:
    explicit AnalysisCoordinator(QObject* parent = nullptr);
    ~AnalysisCoordinator() override;

    // 启动一次分析，返回本次 generation
    quint64 StartAnalysis(const std::string& file_path,
                          const AnalysisOptions& options = AnalysisOptions{});

    // 请求取消当前分析（异步，结果仍会以 completed=false 返回）
    void Cancel();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    quint64 generation() const { return generation_.load(std::memory_order_acquire); }

signals:
    // percent: 0..100
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

private:
    void Run(quint64 generation, std::string file_path, AnalysisOptions options);

    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<quint64> generation_{0};
    std::thread worker_;
};

} // namespace analyzer
} // namespace videoeye
