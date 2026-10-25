#pragma once

// 画面质量页（人眼可见缺陷）：采样帧指标曲线（亮度 / 黑场比例 / 锐度 / 帧间差异）
// + 缺陷列表 + 证据缩略图 + CSV / 证据图导出。
//
// 从 AnalysisPanel 拆出来的独立页面组件。与面板的接缝只有这几个：
//   - AppendFrame()/AppendDefect()  播放期逐帧回调喂数据（只入队 + 置脏）
//   - ApplyStats()                  播放器回报已分析帧数 / 丢弃帧数 / 有效画面区域
//   - FlushPending()                面板的批量刷新定时器到点时重绘
//   - ResetAll()                    换文件 / 重新分析清空
//   - FeatureToggled()              页内「启用分析」开关，由面板转成 feature 信号
//   - OptionsChanged()              采样档位 / 阈值改动，由面板转发给 MediaPlayer
//   - SeekRequested()               点缺陷行跳到时间点，转发给播放器
// 页面本体就是 QWidget（外部 QStackedWidget 的一页），不需要再包一层 tab widget。

#include <QImage>
#include <QString>
#include <QWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QTableWidget>

#include <cstddef>
#include <vector>

#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/VisualDefect.h"
#include "core/domain/model/VisualDefectOptions.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class VisualDefectPage : public QWidget {
    Q_OBJECT

public:
    // feature_checked: 「启用分析」开关初值（面板按 AnalysisFeature 表给进来）
    // options:         面板当前持有的视觉缺陷选项（默认档位 / 阈值）
    explicit VisualDefectPage(bool feature_checked, const model::VisualDefectOptions& options,
                              QWidget* parent = nullptr);

    // 播放/分析回调喂数据：只入队 + 置脏，真正重绘等面板的批量定时器
    void AppendFrame(const model::FrameQualityMetric& metric);
    // 按值传入、内部 move 进缓存：一条缺陷带着一张 RGB 证据图（几十 KB），
    // const& 再 push_back 等于每检出一条就整幅拷一遍图 —— 长视频里这些拷贝
    // 堆在一次播放里就是几十 MB 的无谓搬运。
    void AppendDefect(model::VisualDefect defect);

    // 供测试/诊断观察：是否发生过证据图释放（决定"无证据图"的提示文案）
    bool evidence_trimmed() const {
        return evidence_trimmed_;
    }

    // 播放器回报的统计（已分析帧数 / 因压力丢弃的分析帧数 / 有效画面区域）
    void ApplyStats(int analyzed_frames, int dropped_frames, const model::ActivePictureArea& effective_area);

    bool HasPending() const {
        return dirty_;
    }
    void FlushPending();

    void ResetAll();

    const model::VisualDefectOptions& options() const {
        return options_;
    }

    // 缺陷记录（唯一数据源）。外露给 CSV 导出之外的两件事：
    // ① 报告页要按缺陷类型汇总；② 测试要验证"证据图限额"真的释放了内存 ——
    //    内存这件事只能从外部观察到"第 N 条还有没有图"，页面自己说不算数。
    const std::vector<model::VisualDefect>& DefectRecords() const {
        return records_;
    }

signals:
    void FeatureToggled(bool enabled);
    void OptionsChanged(const model::VisualDefectOptions& options);
    void SeekRequested(double seconds);

private:
    void SetupUi();
    void ApplyOptionsFromUi();
    void UpdateSummary();
    void UpdateCharts();
    void RebuildTable();
    // 释放超出保留额度的证据图（只保留最近若干张）
    void TrimEvidenceFrames();
    void UpdateEvidencePreview(int defect_index);
    void OnOptionChanged();
    void OnCellClicked(int row, int column);
    void OnSelectionChanged();
    void OnExportCsv();
    void OnExportEvidence();

    // 把模型里的 RGB 证据转成 QImage（空证据返回 null 图）
    static QImage EvidenceImage(const model::VisualDefect& defect);

    bool feature_checked_ = false;
    model::VisualDefectOptions options_;
    bool updating_options_ = false;
    bool dirty_ = false;

    int analyzed_frames_ = 0;
    int dropped_frames_ = 0;
    model::ActivePictureArea effective_area_;

    QLabel* summary_label_ = nullptr;
    QComboBox* preset_combo_ = nullptr;
    QDoubleSpinBox* blur_spin_ = nullptr;
    QDoubleSpinBox* freeze_spin_ = nullptr;
    QCheckBox* rgb_check_ = nullptr;

    MetricChartWidget* luma_chart_ = nullptr;
    ChartSeries* luma_series_ = nullptr;
    ChartSeries* black_series_ = nullptr;
    MetricChartWidget* sharp_chart_ = nullptr;
    ChartSeries* blur_series_ = nullptr;
    ChartSeries* diff_series_ = nullptr;

    QTableWidget* table_ = nullptr;
    QLabel* evidence_label_ = nullptr;

    std::vector<model::FrameQualityMetric> samples_;
    std::vector<model::VisualDefect> records_;
    // 是否发生过"证据图超限被释放"。只影响提示文案：用户看到"无证据图"时，
    // 原因可能是没采集（勾选项关着），也可能是采集过但被限额释放了 ——
    // 这两种情况给同一句提示，会让人以为是功能坏了。
    bool evidence_trimmed_ = false;
};

} // namespace ui
} // namespace videoeye
