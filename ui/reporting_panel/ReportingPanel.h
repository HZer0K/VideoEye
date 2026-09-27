#pragma once

// 「报告与批量 QC」页
//
// 这一页把功能 12 的能力凑到一个地方：选模板 → 分析（单文件 / 目录批量）→
// 看总体结论 → 选导出格式 → 落盘。UI 只做编排，真正的分析逻辑在
// core/qc/{QcRunner,BatchQcRunner,QcProfile}，导出在 utils/QcReportExporter。
//
// 线程模型（与项目其余部分一致，不用 QtConcurrent）：
//   * 后台用 std::thread 跑分析，回调在 worker 线程触发；
//   * 所有 UI 更新都经 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 投递回主线程；
//   * 析构时先置取消标记再 detach —— 队列里没跑完的回调会随 QObject 销毁自动丢弃，
//     不会跑到已经析构的 this 上。

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

#include "ui/reporting_panel/analysis_task.h"

namespace videoeye {
namespace ui {

class ReportingPanel : public QWidget {
    Q_OBJECT

public:
    explicit ReportingPanel(QWidget* parent = nullptr);
    ~ReportingPanel() override;

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
    // 后台任务的明确所有权载体（线程 / 取消标记 / 生命周期守卫）定义在
    // ui/reporting_panel/analysis_task.h，并配套 RecycleTask() 回收逻辑，
    // 避免对仍 joinable 的 std::thread 直接赋值导致 std::terminate（连续分析两次的根因）。

    void BuildUi();
    void AppendSummary(const QString& text);
    void UpdateVerdictLabel(const qc::QcRunResult& result);
    void SetBusy(bool busy);

    // 仅当任务仍存活时把更新投递回主线程；否则静默丢弃（面板正在销毁）
    void PostToUi(std::shared_ptr<AnalysisTask> task, const std::function<void()>& updater);

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
    qc::QcProfile profile_;                     // 当前模板（可能是从文件加载的自定义模板）
    qc::QcRunResult last_result_;               // 最近一次单文件分析结果，导出按钮用它
    std::string current_path_;
    std::shared_ptr<AnalysisTask> single_task_;
    std::shared_ptr<AnalysisTask> batch_task_;
    std::vector<qc::BatchQcItemResult> batch_results_;
    bool busy_ = false;
};

}  // namespace ui
}  // namespace videoeye
