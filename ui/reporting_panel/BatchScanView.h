#pragma once

// 「批量扫描」区：目录 / 扩展名 / 并发 / 输出目录参数，进度与结论行，结果表 +
// 状态/严重度筛选，选中行详情与「打开报告文件」。
//
// 从 ReportingPanel 拆出的展示型组件。扫描与导出的执行仍在面板里（BatchQcRunner、
// BatchQcCallbacks、导出路径计算）；这里只负责把条目铺进表格、维护行与
// batch_results_ 下标的绑定，以及两个筛选口径的行可见性。汇总导出入口统一在
// 面板页头的「导出」菜单里（见 ReportingPanel），本区不再挂按钮。

#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

#include "core/qc/BatchQcRunner.h"

namespace videoeye {
namespace ui {

class BatchScanView : public QWidget {
    Q_OBJECT

public:
    explicit BatchScanView(QWidget* parent = nullptr);

    // 批量请求里"从控件读"的那部分。主线程采集完再交给 worker ——
    // Qt 控件只能在创建它的线程上访问。导出格式属于单文件区，不在这里。
    struct Settings {
        std::string out_dir;
        std::vector<std::string> extensions;
        int jobs = 4;
        bool recursive = true;
        // 单文件分析超时；0 表示不限。内置模板会开启全解码画质 QC，
        // 长视频用固定的小超时会把正常文件误判为 TimedOut。
        int timeout_seconds = 0;
        // 连续无数据进度超过该时长则跳过当前文件；0 表示不限。
        int idle_timeout_seconds = 120;
        // 批量场景默认关闭全解码画质 QC：逐帧解码是批量吞吐的主要成本，
        // 需要时由用户显式勾选。
        bool visual_qc = false;
    };

    // 目录输入框原文（空白的判空由面板做，与原实现一致）
    QString DirectoryText() const;
    Settings CurrentSettings() const;

    // 一次扫描开始：清表、清结果、进度归零、汇总与详情回到初始文案
    void BeginScan();
    // 扫描中每完成一个文件调一次：追加一行，并按当前筛选口径决定可见性
    void AppendResult(const qc::BatchQcItemResult& item);
    int RowCount() const;
    const std::vector<qc::BatchQcItemResult>& Results() const {
        return batch_results_;
    }

    // 进度条：范围按发现到的条目数，值按已完成数
    void SetProgress(int finished, int total);
    void SetSummaryText(const QString& text);
    // 在汇总行后面追加一条提示（例如"批量结果基于旧模板"），不清掉原有统计。
    void AppendSummaryNote(const QString& text);
    QString SummaryText() const;
    void ResetDetail();

    // 扫描态：开始/停止互斥 + 排序开关（扫描中排序会让每插一行整表重排）
    void SetBusy(bool busy);

    // 参数持久化。由面板在构造/析构时调用 —— 不放进本组件构造函数，
    // 这样直接 new BatchScanView 的单测仍拿到干净的默认值。
    void LoadPersistedSettings();
    void SavePersistedSettings() const;

signals:
    void StartRequested();
    void CancelRequested();
    // 只重跑上一批里的失败/超时/导出失败/被取消项，路径由面板从 Results() 里取。
    void RerunFailedRequested();
    // 用"主界面当前打开的文件"作左侧、选中的批量行作右侧做逐字段对比。
    // 批量结果不驻留原始分析数据，右侧会在面板里按同一模板重新分析。
    void CompareWithCurrentRequested(const QString& path);

private slots:
    void OnFilterChanged();
    void OnSelectionChanged();
    void OnOpenSelectedReport();
    void OnCompareSelectedWithCurrent();
    void OnPickDirectory();
    void OnPickOutputDirectory();
    void OnOpenOutputDirectory();

private:
    void SetupUi();
    bool RowVisible(const qc::BatchQcItemResult& item) const;
    void ReapplyFilter();
    void ShowDetail(int result_index);
    // 是否存在可重跑的条目（失败 / 超时 / 导出失败 / 被取消）。
    bool HasFailedResults() const;

    QLineEdit* batch_dir_edit_ = nullptr;
    QLineEdit* batch_ext_edit_ = nullptr;
    QLineEdit* batch_out_edit_ = nullptr;
    QSpinBox* jobs_spin_ = nullptr;
    QSpinBox* timeout_spin_ = nullptr;
    QSpinBox* idle_timeout_spin_ = nullptr;
    QCheckBox* visual_qc_check_ = nullptr;
    QCheckBox* recursive_check_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* rerun_failed_button_ = nullptr;
    QPushButton* open_out_button_ = nullptr;
    QProgressBar* batch_progress_ = nullptr;
    QLabel* batch_summary_label_ = nullptr;
    // 批量表格筛选: 状态 / 严重度两个口径（itemData 见 batch_filter.h 的常量）
    QComboBox* batch_status_filter_ = nullptr;
    QComboBox* batch_severity_filter_ = nullptr;
    QTableWidget* batch_table_ = nullptr;
    // 选中行详情: 表格十列装不下失败原因/问题清单，选中后在这里展开
    QLabel* batch_detail_ = nullptr;
    QPushButton* open_report_button_ = nullptr;
    QPushButton* compare_current_button_ = nullptr;

    std::vector<qc::BatchQcItemResult> batch_results_;
};

} // namespace ui
} // namespace videoeye
