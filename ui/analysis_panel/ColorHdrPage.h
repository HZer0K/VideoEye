#pragma once

// 色彩与 HDR 页：primaries / transfer / matrix / range / bit depth / HDR 元数据。
//
// 从 AnalysisPanel 拆出来的独立页面组件。与音频 QC 页一样共用同一次全文件扫描，
// 扫描请求通过 ScanRequested() 交给面板编排。

#include <QWidget>
#include <QString>

#include <QCheckBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include "ui/AnalysisFacade.h"

namespace videoeye {
namespace ui {

class ColorHdrPage : public QWidget {
    Q_OBJECT

public:
    explicit ColorHdrPage(QWidget* parent = nullptr);

    void SetSourcePath(const QString& path) { source_path_ = path; }
    // 异常表来自 QC 规则引擎（category=色彩/HDR），所以要连报告一起给
    void SetResult(const analyzer::AnalysisResult& result, const model::QcReport& qc_report);

    // 只换报告（用户在「规则与阈值」页改了规则后重新评估）再刷一次
    void SetQcReport(const model::QcReport& qc_report);

    void FillScanOptions(analyzer::AnalysisOptions& options);

    void SetScanActive(bool active);
    void SetProgress(int percent);
    void SetProgressFormat(const QString& format);

signals:
    void ScanRequested();
    void CancelRequested();

public slots:
    void Refresh();

private:
    void SetupUi();
    void ApplyOptionsFromUi();
    void OnStartAnalysis();
    void OnCancelAnalysis();
    void OnOptionChanged();
    void UpdateSummary();
    void RebuildTables();
    void RebuildIssueTable();
    void OnExportCsv();

    QString source_path_;
    bool has_result_ = false;
    analyzer::AnalysisResult result_;
    model::QcReport qc_report_;
    analyzer::ColorHdrOptions options_;

    QLabel* summary_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QCheckBox* probe_frame_check_ = nullptr;
    QTabWidget* sub_tabs_ = nullptr;
    QTableWidget* color_info_table_ = nullptr;   // 色彩信息（项目/值/说明）
    QTableWidget* hdr_info_table_ = nullptr;     // HDR 元数据（项目/值/说明）
    QTableWidget* issue_table_ = nullptr;        // 异常组合（QC 规则 category=色彩/HDR）
};

} // namespace ui
} // namespace videoeye
