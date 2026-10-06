#pragma once

// 文件结构页（容器结构）：通用结构树 + MP4/MOV 的 box 表与样本表 + EBML 详情 + 导出。
//
// 从 AnalysisPanel 拆出来的独立页面组件：自己持有控件与当前结果，不再碰
// AnalysisPanel 的任何状态。对外的接缝只有三个：
//   - SetResult()        打开文件时由容器解析喂进来
//   - ApplySampleTable() 全文件诊断扫描顺带跑过样本表时刷新
//   - FeatureToggled()   页内「启用分析」开关，由面板转成统一的 feature 信号
// 页面本体就是 QWidget（外部 QStackedWidget 的一页），不需要再包一层 tab widget。

#include <QWidget>
#include <QString>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeWidget>

#include "core/domain/model/ContainerStructureInfo.h"

#include "ui/analysis_panel/Mp4SampleTableWidget.h"

namespace videoeye {
namespace ui {

class ContainerStructurePage : public QWidget {
    Q_OBJECT

public:
    // feature_checked: 「启用分析」开关的初值（面板按 AnalysisFeature 表给进来）
    explicit ContainerStructurePage(bool feature_checked, QWidget* parent = nullptr);

    // 打开文件时容器解析完成：刷新结构树 / 详情表 / 样本表
    void SetResult(const model::ContainerStructureResult& result);

    // 结构分析没出结果（解析失败 / 分析器抛异常）。与 SetResult 走同一套"空页面"表现，
    // 只是把原因换成调用方给的文字 —— 否则页面会一直停在"打开媒体文件后将自动分析"，
    // 用户看不出是没分析还是分析崩了。
    void SetError(const QString& message);

    // 全文件诊断扫描也跑了样本表时，只刷新样本相关的四张表（不重建结构树）
    void ApplySampleTable(const model::Mp4SampleTableResult& samples);

    const model::ContainerStructureResult& result() const { return result_; }
    bool feature_checked() const { return feature_checked_; }

signals:
    // 页内开关变化：面板负责写回 feature 表并对外发射 AnalysisFeatureToggled
    void FeatureToggled(bool enabled);

private:
    void SetupUi();
    void OnContainerTreeSelectionChanged();
    void OnExportContainerStructure();

    bool feature_checked_ = true;

    QLabel* title_label_ = nullptr;          // 动态标题（格式名 + 结构分析）
    QLabel* summary_label_ = nullptr;
    QTreeWidget* tree_ = nullptr;            // 通用结构树
    QStackedWidget* detail_stack_ = nullptr; // 右侧详情区（通用 / MP4 / EBML）

    // Page 0: 通用信息
    QTableWidget* stream_table_ = nullptr;
    QTableWidget* metadata_table_ = nullptr;

    // Page 1: MP4 专用
    QTabWidget* mp4_detail_tabs_ = nullptr;
    QTableWidget* stts_table_ = nullptr;
    QTableWidget* stco_table_ = nullptr;
    QTableWidget* stsc_table_ = nullptr;
    QTableWidget* stsz_table_ = nullptr;
    QTableWidget* co64_table_ = nullptr;
    QTableWidget* stss_table_ = nullptr;

    // Sample Table 子页（MP4/fMP4 样本级一致性）—— 抽到独立组件
    Mp4SampleTableWidget* mp4_sample_widget_ = nullptr;

    // Page 2: EBML 专用
    QTabWidget* ebml_detail_tabs_ = nullptr;
    QTableWidget* ebml_track_table_ = nullptr;
    QTableWidget* ebml_cue_table_ = nullptr;
    QTableWidget* ebml_block_table_ = nullptr;

    QPushButton* export_button_ = nullptr;

    model::ContainerStructureResult result_;
};

} // namespace ui
} // namespace videoeye
