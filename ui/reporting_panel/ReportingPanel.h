#pragma once

// 「报告与批量 QC」页
//
// 这一页把功能 12 的能力凑到一个地方：选模板 → 分析（单文件 / 目录批量）→
// 看总体结论 → 选导出格式 → 落盘。UI 只做编排，真正的分析逻辑在
// core/qc/{QcRunner,BatchQcRunner,QcProfile}，导出在 core/reporting/QcReportExporter.h。
//
// 版面已按职责拆成三个视图组件（见 ui/reporting_panel/{QcProfileView,SingleReportView,
// BatchScanView}.h）；本页只保留模板与文件的语义、分析执行、结果新鲜度与导出编排。
//
// 线程模型（与项目其余部分一致，不用 QtConcurrent）：
//   * 后台用 std::thread 跑分析，回调在 worker 线程触发；
//   * 所有 UI 更新都经 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 投递回主线程；
//   * 析构走严格 Cooperative（评审 P2-4 方案 A）：先请求取消，再**等待线程体自己收尾并
//     join** —— 不 detach。任务体捕获了 this 并往面板上排队 UI 更新，detach 之后那些投递
//     会落到已销毁的 QWidget 上；在"给出硬性时间上限"与"不悬空访问"之间，本页明确选后者。
//     代价要说清楚：**析构不保证硬时间上限**，取消链（令牌 → QcRunner → 引擎 → FFmpeg
//     中断回调）必须一路通到任务体，否则关停可能长时间等待。回收预算只是告警线。
//     （见 ui/reporting_panel/analysis_task.h 的 RecycleTask 与 ReportingTaskCoordinator。）

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QString>

#include <QTextEdit>
#include <QWidget>

#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"
#include "core/qc/QcRunner.h"

#include "ui/reporting_panel/ReportExportController.h"
#include "ui/reporting_panel/ReportingTaskCoordinator.h"
#include "ui/reporting_panel/analysis_task.h"
#include "ui/reporting_panel/result_stamp.h"

namespace videoeye {
namespace ui {

class QcProfileView;
class SingleReportView;
class BatchScanView;

class ReportingPanel : public QWidget {
    Q_OBJECT

public:
    explicit ReportingPanel(QWidget* parent = nullptr);
    ~ReportingPanel() override;

    // 报告页 slot 上是否仍有任务在跑。
    //
    // 正常完成后必须为 false —— 终态已经归还（见 ReportingTaskCoordinator::Finish）。
    // 若为 true 说明还留在
    // Running，或有一次后台任务从未归还终态。供测试与关闭诊断查询。
    bool IsTaskRunning() const;

    // 当前是否持有一份"与当前文件 + 当前模板都匹配"的分析结果。
    // 切换文件或模板会让结果过期（返回 false），导出按钮也随之禁用；重新分析
    // 成功后才恢复。导出永远只允许导出这份"新鲜"结果，杜绝把上一次结果导出去。
    bool HasFreshResult() const;

    // 导出按钮当前是否可用（供状态回归测试查询，等价于 !busy_ && HasFreshResult()）。
    bool IsExportEnabled() const;

public slots:
    // 主窗口打开文件/切换文件时同步过来
    void SetCurrentFile(const QString& path);

signals:
    // 让主窗口把消息打到状态栏
    void StatusMessage(const QString& text);

private slots:
    void OnProfileSelected(const QString& profile_id);
    void OnLoadProfileFile();
    void OnSaveProfile();
    void OnAnalyzeCurrentFile();
    void OnExportSingleReport();
    void OnStartBatch();
    void OnCancelBatch();
    void OnExportBatchSummary();

private:
    // 后台任务的明确所有权载体（线程 / 取消令牌 / 生命周期守卫）定义在
    // ui/reporting_panel/analysis_task.h，并配套 WaitTaskBody() 的预算等待逻辑，
    // 避免对仍 joinable 的 std::thread 直接赋值导致 std::terminate（连续分析两次的根因）。

    void BuildUi();
    void AppendSummary(const QString& text);
    // 把一次结果刷到结论标签与分析摘要（分析完成、模板复用两条路径共用）。
    void ShowResult(const qc::QcRunResult& result);
    void SetBusy(bool busy);

    // 模板切换后的处置：结果仍可复用时按其原始 AnalysisResult 重算规则并保持导出可用，
    // 否则标为过期。复用条件见 result_stamp.h 的 CanReuseAnalysis。
    void RefreshResultForProfile();

    // 把 last_result_ 标记为不可用（切换文件、模板不可复用、分析失败时调用）：
    // 清 verdict 与问题计数、禁用导出按钮。真正的 last_result_ 数据仍留在内存里，
    // 只是不再被 HasFreshResult() 认可，避免误导出上一次的结果。
    void InvalidateResult();
    // 依据 busy_ 与 HasFreshResult() 刷新导出按钮使能，SetBusy 与结果变化都走它。
    void UpdateExportEnabled();
    // 构造时建好导出控制器：单文件报告、批量汇总两条导出路径连同"先选目录、分析完
    // 再导出"的挂起票据都在它里面（见 ReportExportController.h）。
    void SetupExportController();
    // 构造时建好任务协调器：起线程 / 回收 / 归还终态 / 异常收尾都在它里面
    // （见 ReportingTaskCoordinator.h）。本页只留 RunSingle / RunBatch 的业务本体。
    void SetupTaskCoordinator();

    // 仅当任务仍存活时把更新投递回主线程；否则静默丢弃（面板正在销毁）。
    // 实际实现在任务协调器里（它持有 alive 标志与投递目标）。
    void PostToUi(const std::shared_ptr<AnalysisTask>& task, const std::function<void()>& updater) const;

    void StartSingleAnalysis(const std::string& path);
    void StartBatchScan(const std::string& directory);
    void RunSingle(std::shared_ptr<AnalysisTask> task, const std::string& path);

    // 批量请求的所有参数。必须在主线程里采集完再交给 worker ——
    // Qt 的控件只能在创建它的线程上访问，worker 里碰 QSpinBox / QCheckBox 是未定义行为。
    struct BatchRequest {
        std::string directory;
        std::string out_dir;
        std::vector<std::string> extensions;
        std::vector<qc::QcReportFormat> formats;
        qc::QcProfile profile;
        int jobs = 4;
        bool recursive = true;
    };

    void RunBatch(std::shared_ptr<AnalysisTask> task, const BatchRequest& request);

    qc::QcProfile CurrentProfile() const;
    std::vector<qc::QcReportFormat> SelectedFormats() const;

    QcProfileView* profile_view_ = nullptr;
    SingleReportView* single_view_ = nullptr;
    BatchScanView* batch_view_ = nullptr;
    QTextEdit* log_view_ = nullptr;

    // ---- 状态 ----
    // 报告页后台任务调度器：任务身份（TaskId）、取消令牌（CancelToken）、终态都在这里，
    // 与诊断页 / QcRunner 是同一个 TaskManager 协议，只是 slot 不同。
    // unique_ptr 而不是 QObject 子对象：它必须在**面板成员之前**析构，不能留
    // "子对象比宿主晚死"的窗口。
    std::unique_ptr<ReportingTaskCoordinator> task_runner_;

    qc::QcProfile profile_;       // 当前模板（可能是从文件加载的自定义模板）
    qc::QcRunResult last_result_; // 最近一次单文件分析结果，导出按钮用它
    std::string current_path_;
    // 结果"身份戳"：记录 last_result_ 属于哪个文件、按哪个模板算出（见 result_stamp.h）。
    // 与当前 (current_path_, profile_.id) 一致才算新鲜 —— 切换文件/模板后自然过期。
    ResultStamp result_stamp_;
    // 导出控制器（本页拥有）：挂起票据与两条导出路径都搬进去了，本页只留按钮转发。
    ReportExportController* export_controller_ = nullptr;
    bool busy_ = false;
};

} // namespace ui
} // namespace videoeye
