#pragma once

// 字幕 / 时码 / 辅助数据页：字幕 cue、SMPTE 时码、章节、SCTE-35、metadata。
//
// 从 AnalysisPanel 拆出来的独立页面组件。数据全部来自「诊断与报告」那一次全文件
// 扫描（AnalysisResult 的 subtitle / timecode / aux_data），本页不再单独发起
// demux —— 字幕包本来就在同一次 demux 里过了一遍，重复扫一遍纯属浪费 I/O。

#include <QWidget>
#include <QString>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include "ui/AnalysisFacade.h"
#include "ui/analysis_panel/ScanClient.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class SubtitleAuxPage : public QWidget, public ScanClient {
    Q_OBJECT

public:
    explicit SubtitleAuxPage(QWidget* parent = nullptr);

    // 扫描结束后由面板喂结果并整体刷新（顺带把起始时码冒泡给播放器）
    void SetResult(const model::AnalysisResult& result);

    // ScanClient：本页与码率/音频/HDR 共用诊断页发起的那次全文件扫描，
    // 扫描中「开始分析」按钮跟着禁用，终态一起复位。
    void SetScanActive(bool active) override;
    bool IsScanActive() const { return scan_active_; }

    // 字幕阈值以「规则与阈值」页那张可编辑规则表为准，扫描前同步一次，
    // 避免选项与规则两处阈值各说各话。
    void ApplyRuleThresholds(const std::vector<model::QcRule>& rules,
                             videoeye::SubtitleOptions& options) const;

signals:
    void ScanRequested();
    // 全文件扫描拿到素材自带起始时码（tmcd 轨 / metadata timecode tag）时发出，
    // 由 AnalysisPanel 转给 MainWindow，让播放器时间轴旁显示真实 SMPTE 时码。
    void StartTimecodeReady(const QString& timecode, double fps);
    // 点 cue 行 / 章节行跳转到对应时间点
    void SeekRequested(double seconds);

private:
    void SetupUi();
    void OnStartAnalysis();
    void OnSubtitleStreamChanged(int index);
    // 汇总 + 6 张表 + SCTE-35 标记图 一次刷新（SetResult 进来时调）
    void RefreshUi();
    void UpdateSummary();
    void RebuildSubtitleStreamTable();
    void RebuildSubtitleCueTable();
    void RebuildTimecodeTable();
    void RebuildChapterTable();
    void RebuildAuxStreamTable();
    void RebuildScte35Table();
    void RebuildMetadataTable();
    void UpdateScte35MarkerChart();
    void OnCueCellClicked(int row, int column);
    void OnExportSubtitleCsv();
    void OnExportTimecodeCsv();
    void OnExportScte35Csv();
    void OnExportMetadataCsv();
    // 当前 cue 表要展示的流（-1 = 全部字幕流）
    int CurrentSubtitleStreamIndex() const;

    model::AnalysisResult result_;
    bool scan_active_ = false;

    QLabel* summary_label_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QTabWidget* sub_tabs_ = nullptr;
    QComboBox* stream_combo_ = nullptr;
    QCheckBox* issues_only_check_ = nullptr;
    QTableWidget* stream_table_ = nullptr;
    QTableWidget* cue_table_ = nullptr;
    QTableWidget* timecode_table_ = nullptr;
    QTableWidget* chapter_table_ = nullptr;
    QTableWidget* aux_stream_table_ = nullptr;
    QTableWidget* scte35_table_ = nullptr;
    QTableWidget* metadata_table_ = nullptr;
    MetricChartWidget* scte35_marker_chart_ = nullptr;
    ChartSeries* scte35_marker_series_ = nullptr;
    ChartAxis* scte35_marker_axis_x_ = nullptr;
    ChartAxis* scte35_marker_axis_y_ = nullptr;
};

} // namespace ui
} // namespace videoeye
