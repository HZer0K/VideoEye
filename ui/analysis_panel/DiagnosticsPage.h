#pragma once

// 诊断与报告页（功能 11）：全文件扫描 + QC 规则引擎 + 问题清单 / 规则与阈值。
//
// 从 AnalysisPanel 拆出来的独立页面组件，同时是「扫描总控」的落点：
// AnalysisFacade、当前 QC 报告、扫描代数都归本页所有，面板只做两件事 ——
// 把 current_video_path_ 与扫描选项喂进来，以及把 ScanStarted / ProgressChanged /
// ScanEnded 转发给共用同一次扫描的其它几页（码率与 GOP / 音频 QC / 色彩与 HDR）。
//
// 时间轴的**样本 / 曲线 / 导出**统一由「事件与时间轴」页（EventTimelineView）承载。
// 本页只呈现 QC 报告里的**问题**（时间轴 / 同步类问题由 QcRuleEngine 在 Evaluate 时
// 从扫描结果的 timeline 并入报告，见 QcRuleEngine::Evaluate）、规则判定与评分，
// 并提供「跳转到问题位置」。以前这里另有一份与时间轴页重复的实时曲线/问题表，同一
// 份同步样本被同时喂给两页 —— 已收敛：本页不再累积实时时间轴，也不再有第二套视图。
//
// 页面内的 5 个子页已各自拆成独立组件（DiagnosticsIssueView / QcRuleTableView /
// EvidenceCoverageView / ParameterTimelineView / FrameOrderView），本页只保留
// 「扫描总控 + 汇总」：把 facade 结果分发给各视图、维护扫描状态机与导出。
//
// 面板侧要拿结果时用 result() / qcReport() / hasResult() 取只读快照，
// 不要反过来碰本页成员（与 ContainerStructurePage / AudioQcPage 一致的约定）。

#include <QList>
#include <QString>
#include <QWidget>

#include <chrono>
#include <functional>

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include "infrastructure/logging/PerfMetrics.h"
#include "ui/AnalysisFacade.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/analysis_panel/ScanEndReason.h"

#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/SceneChangeResult.h"

namespace videoeye {
namespace ui {

class DiagnosticsIssueView;
class QcRuleTableView;
class EvidenceCoverageView;
class ParameterTimelineView;
class FrameOrderView;

class DiagnosticsPage : public QWidget {
    Q_OBJECT

public:
    // 一次扫描的**终态**。
    //
    // 为什么是一个三值枚举而不是"成功一个信号、取消一个信号"：本页编排的这次扫描被
    // 码率 GOP / 音频 QC / 色彩 HDR / 字幕辅助 几页共用，它们必须在同一时刻一起回到
    // Idle。以前只在"成功"和"取消"时通知，失败时其它几页就一直停在扫描态
    // （取消按钮还亮着、"开始分析"永久禁用），和诊断页的"失败"状态互相矛盾。
    //
    // 定义已提到 ui/analysis_panel/ScanEndReason.h：面板的公共 slot 也要用它，
    // 不该为此 include 整个页面头。这里保留类内别名，既有写法不变。
    using ScanEndReason = videoeye::ui::ScanEndReason;

    explicit DiagnosticsPage(QWidget* parent = nullptr);

    // 面板换文件时同步：本页自己持有默认扫描选项与当前文件路径（StartScan 用）
    void SetSourcePath(const QString& path) {
        source_path_ = path;
    }

    // 默认扫描选项：其它页 FillScanOptions(options) 的基线，也允许就地改单项开关
    // （「流媒体包」「参数集解析」页的「开始分析」就是改一个 analyze_* 开关）
    videoeye::AnalysisOptions& options() {
        return options_;
    }
    const videoeye::AnalysisOptions& options() const {
        return options_;
    }

    // 全文件扫描（一次 demux 供多页共用：诊断页 / 码率 GOP / 音频 QC /
    // 色彩 HDR / 字幕辅助 / 流媒体包 / 参数集）
    // silent=true 时不弹 QMessageBox（打开失败自动补扫这类静默/批量入口），
    // 提示一律写进本页的汇总标签，免得弹窗打断流程。
    void StartScan(const videoeye::AnalysisOptions& options, bool silent = false);
    void CancelScan();

    // 每次扫描前的钩子：面板挂上「字幕阈值从规则表同步」这类跨页逻辑，
    // 页面自己不认识 SubtitleAuxPage（避免页面之间互相 include）
    using ScanHook = std::function<void(videoeye::AnalysisOptions&)>;
    void SetBeforeScanHook(ScanHook hook) {
        before_scan_ = std::move(hook);
    }

    // 「关联场景切换」：报告重算留在诊断页，码率页只负责展示自己的结果
    void ApplySceneLink(const std::vector<model::SceneChangeResult>& records,
                        const videoeye::BitrateGopOptions& gop_options);

    // 换文件：清掉上一文件的扫描结果
    void ResetForNewFile();

    // 只读快照：面板分发给共用同一次扫描的其它页。
    // 返回共享指针：各页面共用同一份结果，不再各自整份拷贝 AnalysisResult。
    const std::vector<model::QcRule>& rules() const;
    std::shared_ptr<const model::AnalysisResult> result() const;
    const model::QcReport& qcReport() const {
        return report_;
    }
    bool hasResult() const {
        return has_result_;
    }

    // 最近一次全文件扫描的失败原因（打开/探测/读取失败时由 OnFacadeFailed 写入，
    // StartScan 时清空；无失败返回空串）。首页「分析状态」区用它解释为什么没结果。
    const QString& lastError() const {
        return last_error_;
    }

signals:
    // 扫描生命周期：面板收到后同步其它几页的按钮 / 进度条
    void ScanStarted();
    void ProgressChanged(double percent, const QString& stage);
    // 扫描终态，**每次扫描恰好发一次**。共用同一次扫描的几页只认这个事件来恢复
    // 按钮与进度状态 —— 成功、取消、失败都走同一条路径，不会再出现"诊断页失败了、
    // 其它页还在扫描中"这种页面之间状态不一致。
    // (以前用户点取消时还会额外发一个"取消"，而那一刻任务其实还在跑 ——
    //  那个"假终态"和"真终态"混在一起，正是状态收不干净的原因。)
    void ScanEnded(videoeye::ui::DiagnosticsPage::ScanEndReason reason);

    // 报告重算（改规则 / 关联场景切换）后通知面板再刷一遍音频 QC 与色彩 HDR
    void QcReportChanged(const model::QcReport& report);
    // 点「跳转到问题位置」（按问题清单里选中那一行的位置 seek）
    void SeekRequested(double seconds);

private slots:
    void OnCancelClicked();
    void OnCancelWatchdog();
    void OnExportClicked();
    void OnFacadeProgress(quint64 generation, double percent, const QString& stage);
    void OnFacadeFinished(quint64 generation, bool completed, std::shared_ptr<const model::AnalysisResult> result);
    void OnFacadeFailed(quint64 generation, const QString& message);

private:
    void SetupUi();
    // 用 facade 结果 + QC 规则引擎重算报告并刷新各视图（同时外发 QcReportChanged）
    void Evaluate();
    void UpdateQcSummary();
    void SetScanButtonState(bool running);
    // 终态到达（成功 / 取消 / 失败共用）：停掉取消看门狗、结算取消延迟
    void ClearCancelPending();

    QString source_path_;
    videoeye::AnalysisOptions options_;
    model::QcReport report_;
    quint64 generation_ = 0;
    bool has_result_ = false;
    // 最近一次扫描的失败原因（空 = 没有失败过或已被新的扫描清掉）
    QString last_error_;
    std::chrono::steady_clock::time_point scan_start_time_;

    // ---- 取消延迟 ----
    //
    // 「用户点了取消」到「界面承认取消了」是两件事，中间隔着后台线程退出：
    //   * cancel_probe_ 记下请求时刻，终态到达时结算 cancel_settle_ms；
    //   * cancel_watchdog_ 是**收敛兜底**：后台一旦不按预期退出，界面不能永远停在
    //     "取消中…"却毫无进展。它只改进度条文案、绝不弹框 —— 弹框会把
    //     "打开即分析"这类自动流程卡在模态循环里等不到人点。
    //   * cancel_pending_ 标记"已请求但还没收到终态"。
    CancelLatencyProbe cancel_probe_;
    QTimer* cancel_watchdog_ = nullptr;
    bool cancel_pending_ = false;

    AnalysisFacade* facade_ = nullptr;
    ScanHook before_scan_; // 扫描前的跨页钩子（由面板注入）

    QLabel* summary_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QTabWidget* sub_tabs_ = nullptr;

    // 5 个子页各自独立成组件：本页只负责建页与分发结果
    DiagnosticsIssueView* issue_view_ = nullptr;      // 子页 0：问题清单（曲线 + 问题表 + 跳转）
    QcRuleTableView* rule_view_ = nullptr;            // 子页 1：规则与阈值
    EvidenceCoverageView* coverage_view_ = nullptr;   // 子页 2：证据与覆盖
    ParameterTimelineView* parameter_view_ = nullptr; // 子页 3：参数变化
    FrameOrderView* frame_order_view_ = nullptr;      // 子页 4：帧顺序（逐帧显示顺序值）
};

} // namespace ui
} // namespace videoeye
