#pragma once

// 「参考帧质量对比」页：选一段参考视频 + 一段待测视频，按采样栅格逐帧对齐算 PSNR / SSIM。
//
// 与其他分析页的关系
// ------------------
// 它**不**实现 ScanClient、也不共用「诊断与报告」那次全文件扫描 —— 那次扫描只有
// 一个输入（当前文件），而这里必须成对。所以本页自己起一次后台任务：
//
//   * 解码 / 对齐 / 指标计算全在 core/analysis/quality/ReferenceQualityRunner，
//     页面只负责收集参数、显示进度与结果（引擎可以脱离 UI 单测）；
//   * 线程模型与项目其余部分一致：std::thread，不用 QtConcurrent；
//   * 进度与终态都经 QMetaObject::invokeMethod 以本页为上下文对象排队回主线程 ——
//     页面销毁后挂起的调用不会再触发；析构与「换文件」都是先取消再 join，绝不把
//     仍可 join 的 std::thread 留给析构（那会直接 std::terminate）。
//
// 一次运行的身份用 run_generation_ 记：换文件 / 作废上一份结果时自增，于是
// "上一次运行迟到的那条终态消息"会被丢弃，而不是把旧文件的数字画到新文件上。

#include <QString>
#include <QWidget>

#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>

#include <atomic>
#include <thread>

#include "core/domain/model/ReferenceQualityResult.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {

// 解码执行层的选项（core/analysis/quality/ReferenceQualityRunner.h）。这里只做前向
// 声明：页面头被 AnalysisPanel.h → MainWindow.h 一路包含，把 Runner 头（含 FFmpeg
// 侧实现声明）扇出去会把每个碰主窗口的 TU 都拖进去。
struct ReferenceQualityRunOptions;

namespace ui {

class ReferenceQualityPage : public QWidget {
    Q_OBJECT

public:
    explicit ReferenceQualityPage(QWidget* parent = nullptr);
    ~ReferenceQualityPage() override;

    // 当前打开的文件当「待测视频」的默认值（参考视频必须由用户另选）。
    void SetSourcePath(const QString& path);

    // 换文件 / 重新开始时作废上一份结果（进行中的运行会被取消并回收）。
    void ClearResult();

    // 后台任务是否仍在跑。与 SetBusy 的状态互为表里，测试用它观察 "取消后是否复位"。
    bool IsRunning() const {
        return worker_.joinable();
    }
    bool HasResult() const {
        return has_result_;
    }
    const model::ReferenceQualityResult& Result() const {
        return result_;
    }
    const QString& ReferencePath() const {
        return reference_path_;
    }
    const QString& DistortedPath() const {
        return distorted_path_;
    }

    // 展示一份已有结果（后台任务终态与测试都走这里，不重复一遍刷新逻辑）。
    void SetResult(const model::ReferenceQualityResult& result);

public slots:
    void StartComparison();
    // 只请求取消：终态仍由任务自己归还（已完成的部分保留，见 Runner 的说明）。
    void CancelComparison();

private:
    void SetupUi();
    void SetBusy(bool busy);
    void RefreshSummary();
    void RebuildFrameTable();
    void UpdateChart();
    // 参数控件 -> 引擎选项
    ReferenceQualityRunOptions CollectRunOptions() const;
    void OnRunFinished(quint64 run_id, const model::ReferenceQualityResult& result);
    void PickReference();
    void PickDistorted();
    void OnExportCsv();
    void OnExportJson();
    QString DefaultExportBaseName() const;

    QString source_path_;
    QString reference_path_;
    QString distorted_path_;
    bool has_result_ = false;
    // 非空默认值（一份空结果）：成员在 SetResult 之前也会被解引用，避免判空铺满全页。
    model::ReferenceQualityResult result_;

    std::atomic<bool> cancel_{false};
    std::thread worker_;
    // 只由 UI 线程读写；后台线程只会把它当作常量带回来比较。
    quint64 run_generation_ = 0;

    QLabel* summary_label_ = nullptr;
    QLabel* frame_hint_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;

    QLineEdit* reference_edit_ = nullptr;
    QLineEdit* distorted_edit_ = nullptr;
    QDoubleSpinBox* fps_spin_ = nullptr;
    QDoubleSpinBox* offset_spin_ = nullptr;
    QDoubleSpinBox* tolerance_spin_ = nullptr;
    QSpinBox* sample_width_spin_ = nullptr;
    QSpinBox* max_frames_spin_ = nullptr;

    QTabWidget* sub_tabs_ = nullptr;
    MetricChartWidget* chart_ = nullptr;
    ChartSeries* psnr_series_ = nullptr;
    ChartSeries* ssim_series_ = nullptr;
    QTableWidget* frame_table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
