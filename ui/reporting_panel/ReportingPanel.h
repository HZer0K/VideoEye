#pragma once

// 「报告与批量 QC」页
//
// 这一页把功能 12 的能力凑到一个地方：选模板 → 分析（单文件 / 目录批量）→
// 看总体结论 → 选导出格式 → 落盘。UI 只做编排，真正的分析逻辑在
// core/qc/{QcRunner,BatchQcRunner,QcProfile}，导出在 core/reporting/QcReportExporter.h。
//
// 线程模型（与项目其余部分一致，不用 QtConcurrent）：
//   * 后台用 std::thread 跑分析，回调在 worker 线程触发；
//   * 所有 UI 更新都经 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 投递回主线程；
//   * 析构走严格 Cooperative（评审 P2-4 方案 A）：先请求取消，再**等待线程体自己收尾并
//     join** —— 不 detach。任务体捕获了 this 并往面板上排队 UI 更新，detach 之后那些投递
//     会落到已销毁的 QWidget 上；在"给出硬性时间上限"与"不悬空访问"之间，本页明确选后者。
//     代价要说清楚：**析构不保证硬时间上限**，取消链（令牌 → QcRunner → 引擎 → FFmpeg
//     中断回调）必须一路通到任务体，否则关停可能长时间等待。回收预算只是告警线。
//     （见 ui/reporting_panel/analysis_task.h 的 RecycleTask 与 ReportingPanel::RetireTask。）

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QString>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>

#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"
#include "core/qc/QcRunner.h"
#include "infrastructure/concurrency/TaskManager.h"

#include "ui/reporting_panel/analysis_task.h"
#include "ui/reporting_panel/result_stamp.h"

namespace videoeye {
namespace ui {

class ReportingPanel : public QWidget {
    Q_OBJECT

public:
    explicit ReportingPanel(QWidget* parent = nullptr);
    ~ReportingPanel() override;

    // 报告页 slot 上是否仍有任务在跑。
    //
    // 正常完成后必须为 false —— 终态已经归还（见 FinishTask）。若为 true 说明还留在
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
    void OnProfileChanged(int index);
    void OnLoadProfileFile();
    void OnSaveProfile();
    void OnAnalyzeCurrentFile();
    void OnExportSingleReport();
    void OnPickBatchDirectory();
    void OnPickOutputDirectory();
    void OnStartBatch();
    void OnCancelBatch();
    void OnExportBatchSummary();

private:
    // 后台任务的明确所有权载体（线程 / 取消令牌 / 生命周期守卫）定义在
    // ui/reporting_panel/analysis_task.h，并配套 WaitTaskBody() 的预算等待逻辑，
    // 避免对仍 joinable 的 std::thread 直接赋值导致 std::terminate（连续分析两次的根因）。

    void BuildUi();
    void AppendSummary(const QString& text);
    void UpdateVerdictLabel(const qc::QcRunResult& result);
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
    // 用 last_result_ + 当前模板把单文件报告导出到 directory（调用前须保证结果新鲜）。
    void ExportSingleResult(const QString& directory);

    // 仅当任务仍存活时把更新投递回主线程；否则静默丢弃（面板正在销毁）
    void PostToUi(std::shared_ptr<AnalysisTask> task, const std::function<void()>& updater);

    void StartSingleAnalysis(const std::string& path);
    void StartBatchScan(const std::string& directory);
    void RunSingle(std::shared_ptr<AnalysisTask> task, const std::string& path);

    // 报告页任务（单文件 / 批量）的槽位。取消与终态都归到这个 slot 上，与诊断页、
    // QcRunner 共用同一套 task::TaskHandle 协议，不再各写一份。
    //
    // 归还终态只能发生在宿主线程：任务体捕获了 this（要往面板上刷结果）。报告页走的是
    // 严格 Cooperative（见 RecycleTask）：超预算也继续 join、不 detach，所以 tasks_ 一定
    // 比任务体活得久；即便如此，终态也统一在宿主线程归还，不把调度器交到任务体手里。
    void RetireTask(std::shared_ptr<AnalysisTask>& task);

    // 任务**正常完成**时由 UI 线程归还终态并清掉当前任务句柄：
    //   * EndHandle 把 slot 从 Running 写成 Succeeded/Canceled/Failed（终态一次性）；
    //   * 命中 single_task_ / batch_task_ 时一并置空，避免句柄过期后仍被当成"在跑"。
    // 与 RetireTask 的区别: RetireTask 是取消 + 回收（关停/换任务），这里只做"跑完了"。
    // 不归还的话 slot 会一直停在 Running，任务切换时 single_task_/batch_task_ 长期持有
    // 过期句柄，RunningCount() 也会长时间不准确。
    void FinishTask(const std::shared_ptr<AnalysisTask>& task, task::TaskState terminal);

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

    // ---- 模板区 ----
    QComboBox* profile_combo_ = nullptr;
    QLabel* profile_description_ = nullptr;

    // ---- 单文件区 ----
    QLabel* current_file_label_ = nullptr;
    QLabel* verdict_label_ = nullptr;
    QLabel* issue_count_label_ = nullptr;
    QCheckBox* format_json_ = nullptr;
    QCheckBox* format_csv_ = nullptr;
    QCheckBox* format_html_ = nullptr;
    QCheckBox* format_pdf_ = nullptr;
    QCheckBox* format_txt_ = nullptr;
    QPushButton* analyze_button_ = nullptr;
    QPushButton* export_button_ = nullptr;

    // ---- 批量区 ----
    QLineEdit* batch_dir_edit_ = nullptr;
    QLineEdit* batch_ext_edit_ = nullptr;
    QLineEdit* batch_out_edit_ = nullptr;
    QSpinBox* jobs_spin_ = nullptr;
    QCheckBox* recursive_check_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QProgressBar* batch_progress_ = nullptr;
    QLabel* batch_summary_label_ = nullptr;
    QTableWidget* batch_table_ = nullptr;
    QPushButton* summary_button_ = nullptr;

    QTextEdit* log_view_ = nullptr;

    // ---- 状态 ----
    // 报告页后台任务调度器：任务身份（TaskId）、取消令牌（CancelToken）、终态都在这里，
    // 与诊断页 / QcRunner 是同一个 TaskManager 协议，只是 slot 不同。
    task::TaskManager tasks_{2};
    static constexpr const char* kTaskSlot = "reporting-panel";

    qc::QcProfile profile_;                     // 当前模板（可能是从文件加载的自定义模板）
    qc::QcRunResult last_result_;               // 最近一次单文件分析结果，导出按钮用它
    std::string current_path_;
    // 结果"身份戳"：记录 last_result_ 属于哪个文件、按哪个模板算出（见 result_stamp.h）。
    // 与当前 (current_path_, profile_.id) 一致才算新鲜 —— 切换文件/模板后自然过期。
    ResultStamp result_stamp_;
    // 无结果时点导出：先取输出目录挂在这里，分析成功后在完成回调里继续导出。
    QString pending_export_dir_;
    std::shared_ptr<AnalysisTask> single_task_;
    std::shared_ptr<AnalysisTask> batch_task_;
    std::vector<qc::BatchQcItemResult> batch_results_;
    bool busy_ = false;
};

}  // namespace ui
}  // namespace videoeye
