#pragma once

// 「单文件报告」区：当前文件、导出格式、结论与问题计数、重新分析 / 导出。
//
// 从 ReportingPanel 拆出的展示型组件。分析怎么跑、导出的三条决策（新鲜就直接导 /
// 不新鲜先挂起再分析 / 汇总重算）仍在面板与 ReportExportController 里；这里只把
// qc::QcRunResult 翻译成"分数 + 结论 + 四类计数"的展示口径，并转发两个按钮请求。

#include <vector>

#include <QString>
#include <QWidget>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

#include "core/qc/QcAnalyzeRequest.h" // qc::QcRunResult
#include "core/qc/QcReportFormat.h"

namespace videoeye {
namespace ui {

class SingleReportView : public QWidget {
    Q_OBJECT

public:
    explicit SingleReportView(QWidget* parent = nullptr);

    void SetCurrentFile(const QString& path);

    // 分析结论（失败时把原因放进计数行），口径与原 UpdateVerdictLabel 一致
    void ShowResult(const qc::QcRunResult& result);
    // 分析进行中：结论行先写「分析中…」，计数行清空
    void ShowAnalyzing();
    // 任务协调器给出的失败文案：只替换结论行（不改配色、不动计数），与原实现一致
    void ShowFailure(const QString& text);
    // 结果过期 / 尚无结果：清掉结论配色与计数，文案按"有没有打开文件"区分
    void ShowInvalidated(bool has_file);

    void SetBusy(bool busy);
    void SetExportEnabled(bool enabled);
    bool IsExportEnabled() const;

    // 勾选的导出格式；一个都没勾退回 JSON（与原实现一致）
    std::vector<qc::QcReportFormat> SelectedFormats() const;

signals:
    void AnalyzeRequested();
    void ExportRequested();

private:
    void SetupUi();

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
};

} // namespace ui
} // namespace videoeye
