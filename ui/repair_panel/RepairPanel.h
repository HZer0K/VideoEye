#pragma once

// 「视频修复」页：配置 → 进度 → 结论 / 证据 → 报告。
//
// 界面只负责收集参数、展示结果和落报告；诊断 / 执行 / 验证全在 core/repair。
// 报告写入放在这里，是因为 core/qt 与 core/reporting 之间不允许建边。

#include <future>
#include <string>
#include <vector>

#include <QWidget>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/qt/RepairController.h"
#include "core/reporting/RepairReportExporter.h"

class QComboBox;
class QCheckBox;
class QTimer;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace videoeye {
namespace ui {

class RepairPanel : public QWidget {
    Q_OBJECT

public:
    explicit RepairPanel(QWidget* parent = nullptr);
    ~RepairPanel() override;

    // 主窗口打开新文件时同步过来（不影响正在运行的修复）。
    void SetCurrentFile(const QString& path);

private slots:
    void OnBrowseSource();
    void OnBrowseReference();
    void OnBrowseOutput();
    void OnStart();
    void OnCancel();
    void OnOpenOutput();
    void OnOpenReport();
    void OnAddBatchFiles();
    void OnRemoveBatchSelection();
    void OnClearBatch();
    void OnProgress(quint64 task_id, int percent, const QString& stage, const QString& detail);
    void OnFinished(quint64 task_id, const QString& status_text, const QString& summary);

private:
    void BuildUi();
    void SetRunning(bool running);
    void ResetForNewRun();
    void ShowResult();
    model::RepairRequest BuildRequest(const QString& source) const;
    void StartReportWrite(const std::shared_ptr<const model::RepairResult>& result);
    void PollReportWrite();
    void ClearPendingReport();
    // P2-14c：串行批量队列。任务在同一 controller 槽上排队，前一项结束（含报告写完）
    // 才启动下一项；取消只作用于当前项，并把剩余项标记为未执行。
    void StartNextBatchItem();
    void AdvanceBatch();
    // first_unstarted：从第几行开始标"未执行"。当前项还在渲染结果时不能覆盖它。
    void FinishBatch(int first_unstarted);
    void MarkBatchRow(int row, const QString& status, const QString& color);
    void UpdateOutputs(const model::RepairResult& result);
    void SetMetric(int index, const QString& value, const QString& color);
    void PopulateFindings(const std::vector<model::DamageFinding>& findings);

    qt::RepairController* controller_ = nullptr;

    QLineEdit* source_edit_ = nullptr;
    QLineEdit* reference_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QComboBox* mode_combo_ = nullptr;
    QComboBox* container_combo_ = nullptr;
    QComboBox* level_combo_ = nullptr;
    QComboBox* gap_combo_ = nullptr;
    // 切片 F：源侧无帧率证据时的显式帧率（数据存 "num/den"，"0/0" = 不推定）。
    QComboBox* frame_rate_combo_ = nullptr;
    QCheckBox* segment_check_ = nullptr;
    QCheckBox* timeline_check_ = nullptr;
    QCheckBox* fill_missing_check_ = nullptr;
    QCheckBox* single_track_check_ = nullptr;
    QCheckBox* keep_partial_check_ = nullptr;

    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* open_output_button_ = nullptr;
    QPushButton* open_report_button_ = nullptr;

    // 批量队列：表格是待执行清单（行 = 源文件），batch_requests_ 是开始时的配置快照。
    QTableWidget* batch_table_ = nullptr;
    QPushButton* batch_add_button_ = nullptr;
    QPushButton* batch_remove_button_ = nullptr;
    QPushButton* batch_clear_button_ = nullptr;
    std::vector<model::RepairRequest> batch_requests_;
    int batch_index_ = -1;
    bool batch_running_ = false;
    bool batch_canceled_ = false;

    QProgressBar* progress_bar_ = nullptr;
    QLabel* stage_label_ = nullptr;
    QLabel* status_badge_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QLabel* metric_labels_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    QLabel* report_label_ = nullptr;
    QTableWidget* findings_table_ = nullptr;

    // R13：报告落盘放到后台线程，UI 线程只轮询 future；结果写入与产物发布分开显示。
    QTimer* report_timer_ = nullptr;
    std::future<reporting::RepairReportWriteResult> report_future_;
    bool last_write_report_ = true;
    QString pending_report_html_;
    QString pending_report_json_;

    QString last_output_path_;
    QString last_report_html_path_;
    QString last_report_json_path_;
};

} // namespace ui
} // namespace videoeye
