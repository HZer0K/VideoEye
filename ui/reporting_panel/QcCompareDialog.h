#pragma once

// 双文件 QC 对比结果视图。
//
// 数据来自 core/qc/QcComparator.h 的 QcComparison（一行一个字段），本类只负责
// 展示、按容差重算与导出，不做任何二次分析。导出复用 QcReportExporter 的对比报告
// 入口，支持 HTML / JSON / CSV / TXT / PDF。
//
// 容忍度输入直接改 QcCompareTolerance：容器时间基不同、音频一帧对齐差，都会让
// 时长/码率出现毫秒级或 0.1% 级差异；不给调节入口时，用户只能看到"处处不同"。
// 左右两侧的原始结果需要留着，容差变化时用 CompareRuns 重算而不是二次分析。

#include <QDialog>

#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcComparator.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace videoeye {
namespace ui {

class QcCompareDialog : public QDialog {
    Q_OBJECT

public:
    QcCompareDialog(qc::QcRunResult left, qc::QcRunResult right, QWidget* parent = nullptr);

    // 供面板/测试读取当前口径下的统计
    int DifferentCount() const {
        return comparison_.DifferentCount();
    }
    int AvailableRowCount() const {
        return comparison_.AvailableRowCount();
    }
    int RowCount() const;

private slots:
    void OnExport();
    void OnToleranceChanged();
    void OnFilterChanged();

private:
    void BuildUi();
    void RebuildComparison();
    void FillTable();
    void UpdateSummary();
    void ApplyFilter();

    qc::QcRunResult left_;
    qc::QcRunResult right_;
    qc::QcCompareTolerance tolerance_;
    qc::QcComparison comparison_;

    QLabel* summary_ = nullptr;
    QCheckBox* only_diff_check_ = nullptr;
    QDoubleSpinBox* tolerance_spin_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* export_button_ = nullptr;
};

} // namespace ui
} // namespace videoeye
