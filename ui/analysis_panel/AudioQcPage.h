#pragma once

// 音频 QC 页：响度 / 真峰值 / 削波 / 静音 / 声道相位 / metadata 一致性。
//
// 从 AnalysisPanel 拆出来的独立页面组件。数据与「诊断与报告」共用同一次全文件
// 扫描，所以本页不自己发扫描请求 —— 只发 ScanRequested()，由面板统一编排
// （面板才知道 current_video_path_ / diagnostics_options_ / 进度条总控）。
// 与码率 GOP / 色彩 HDR 共有的生命周期动作（换文件路径 / 扫描态 / 进度 / 选项
// 并入）实现在 ScanClient 接口里，由面板按注册表统一驱动。
//
// 5 个子页已各自拆成独立组件（AudioLoudnessView / AudioEventView /
// AudioChannelView / AudioVerdictView / AudioTrackView）；本页保留工具栏、
// 扫描选项、汇总与扫描生命周期。

#include <QString>
#include <QWidget>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>

#include "ui/AnalysisFacade.h"
#include "ui/analysis_panel/ScanClient.h"

namespace videoeye {
namespace ui {

class AudioLoudnessView;
class AudioEventView;
class AudioChannelView;
class AudioVerdictView;
class AudioTrackView;

class AudioQcPage : public QWidget, public ScanClient {
    Q_OBJECT

public:
    explicit AudioQcPage(QWidget* parent = nullptr);

    // 导出 CSV 的默认文件名用当前文件路径拼，面板换文件时同步一次
    void SetSourcePath(const QString& path) override {
        source_path_ = path;
    }

    // 扫描结束后由面板喂结果（qc_report 用于「规则判定」列）
    void SetResult(const model::AnalysisResult& result, const model::QcReport& qc_report);

    // 只换报告（用户在「规则与阈值」页改了规则后重新评估）再刷一次
    void SetQcReport(const model::QcReport& qc_report);

    // 判定表的固定规则条数（= 实现里 kAudioQcRuleIds 的长度）。
    // 行序是页面契约，测试与侧栏都靠它对齐：新增一条 audio.* 规则只需改实现一处，
    // 不必再去测试里改魔法数字。
    static int VerdictRuleCount();

    // 面板发起扫描前调用：把 UI 上的阈值同步进 AnalysisOptions
    void FillScanOptions(videoeye::AnalysisOptions& options) override;

    // 扫描生命周期由面板驱动（与码率/GOP、色彩/HDR、诊断页保持同步）
    void SetScanActive(bool active) override;
    // 侧栏导航标记：削波 + 静音两表的行数
    int IssueCount() const override;
    // 同 BitrateGopPage::IsScanActive: 状态必须可观察，否则"某页卡在扫描中"只能靠肉眼发现。
    // 不在 ScanClient 接口里：它是给人/测试看的，不是面板驱动页面要用的。
    bool IsScanActive() const {
        return scan_active_;
    }
    void SetProgress(int percent) override;
    void SetProgressFormat(const QString& format) override;

signals:
    void ScanRequested();
    void CancelRequested();
    // 点削波/静音行跳到对应时间点
    void SeekRequested(double seconds);

public slots:
    void Refresh();

private:
    // 扫描态（由 SetScanActive 写入；面板成功/取消/失败三种终态都必须把它复位）
    bool scan_active_ = false;

    void SetupUi();
    void ApplyOptionsFromUi();
    void OnStartAnalysis();
    void OnCancelAnalysis();
    void OnOptionChanged();
    void UpdateSummary();
    void OnExportCsv();

    QString source_path_;
    bool has_result_ = false;
    model::AnalysisResult result_;
    model::QcReport qc_report_;
    videoeye::AudioQcOptions options_;

    QLabel* summary_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QDoubleSpinBox* target_lufs_spin_ = nullptr;
    QDoubleSpinBox* silence_spin_ = nullptr;
    QDoubleSpinBox* min_silence_spin_ = nullptr;
    QDoubleSpinBox* clip_spin_ = nullptr;
    QCheckBox* loudness_check_ = nullptr;
    QCheckBox* true_peak_check_ = nullptr;
    QCheckBox* correlation_check_ = nullptr;
    QTabWidget* sub_tabs_ = nullptr;

    // 5 个子页各自独立成组件：本页只负责建页与分发结果
    AudioLoudnessView* loudness_view_ = nullptr; // 子页 0：响度与电平
    AudioEventView* event_view_ = nullptr;       // 子页 1：静音与削波
    AudioChannelView* channel_view_ = nullptr;   // 子页 2：声道与相位
    AudioVerdictView* verdict_view_ = nullptr;   // 子页 3：规则结果
    AudioTrackView* track_view_ = nullptr;       // 子页 4：音轨对照
};

} // namespace ui
} // namespace videoeye
