#include "ui/reporting_panel/ReportingPanel.h"

#include <algorithm>

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStringList>
#include <QVBoxLayout>

#include "core/domain/task/ThreadBody.h"
#include "core/qc/QcComparator.h"
#include "core/qc/QcProfileMapper.h"
#include "core/qc/QcRuleEngine.h"
#include "core/reporting/QcReportExporter.h"

#include "infrastructure/logging/Logger.h"

#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/reporting_panel/BatchScanView.h"
#include "ui/reporting_panel/QcCompareDialog.h"
#include "ui/reporting_panel/QcProfileView.h"
#include "ui/reporting_panel/SingleReportView.h"
#include "ui/reporting_panel/batch_result_model.h"
#include "ui/reporting_panel/report_path.h"

namespace videoeye {
namespace ui {

ReportingPanel::ReportingPanel(QWidget* parent) : QWidget(parent) {
    profile_ = qc::BuiltinQcProfiles().front(); // 通用
    BuildUi();
    // 批量页参数（目录/并发/超时/输出目录）跨会话保留。放在 BuildUi 之后、
    // 控件已就绪时恢复；单测直接 new BatchScanView 不会触发这里，默认值仍然干净。
    batch_view_->LoadPersistedSettings();
    // 控制器要读批量目录等控件状态，必须在 BuildUi 之后建。
    SetupExportController();
    SetupTaskCoordinator();
}

void ReportingPanel::SetupTaskCoordinator() {
    ReportingTaskCoordinator::Deps deps;
    deps.ui = this;
    deps.show_failure = [this](const QString& text) {
        single_view_->ShowFailure(text);
        batch_view_->SetSummaryText(text);
    };
    deps.append = [this](const QString& text) { AppendSummary(text); };
    deps.set_busy = [this](bool busy) { SetBusy(busy); };
    deps.abort_pending_export = [this]() {
        if (export_controller_)
            export_controller_->AbortPendingExport();
    };
    task_runner_ = std::make_unique<ReportingTaskCoordinator>(std::move(deps));
}

void ReportingPanel::SetupExportController() {
    // 依赖全部以 lambda 注入：控制器只拿到"能不能导 / 导什么"，看不到控件，
    // 于是那三条决策（新鲜就直接导 / 挂起等分析 / 汇总重算）可以脱离 QWidget 单测。
    ReportExportController::Deps deps;
    deps.has_file = [this]() { return !current_path_.empty(); };
    deps.busy = [this]() { return busy_; };
    deps.has_fresh_result = [this]() { return HasFreshResult(); };
    deps.current_path = [this]() { return current_path_; };
    deps.single_result = [this]() { return last_result_; };
    deps.profile = [this]() { return profile_; };
    deps.formats = [this]() { return SelectedFormats(); };
    deps.batch_results = [this]() { return batch_view_->Results(); };
    deps.batch_root = [this]() { return batch_view_->DirectoryText().toStdString(); };
    deps.start_single_analysis = [this]() { StartSingleAnalysis(current_path_); };
    deps.pick_output_directory = [this]() { return QFileDialog::getExistingDirectory(this, tr("选择报告输出目录")); };
    deps.pick_summary_file = [this]() {
        return QFileDialog::getSaveFileName(this, tr("导出批量汇总"), tr("qc-summary.csv"),
                                            tr("CSV (*.csv);;JSON (*.json);;HTML (*.html);;文本 (*.txt);;PDF (*.pdf)"));
    };
    deps.notice = [this](const QString& title, const QString& body) { QMessageBox::information(this, title, body); };
    export_controller_ = new ReportExportController(std::move(deps), this);
    connect(export_controller_, &ReportExportController::StatusMessage, this, &ReportingPanel::StatusMessage);
}

ReportingPanel::~ReportingPanel() {
    // 取消 + 置 alive=false + **带预算的**回收，全部由任务协调器完成：worker 在预算
    // 返回前结束，其回调看到 alive=false 不会再触碰 this；已入队的 UI 事件由 Qt 在对象
    // 销毁时自动移除，不会跑到销毁后的 this。
    // （P0：原先对仍 joinable 的 std::thread 直接 detach，且第二次启动会对其赋值导致 terminate。）
    //
    // 预算这件事是评审 P1-3 的要求：关界面不能再无限等下去。
    if (batch_view_)
        batch_view_->SavePersistedSettings();
    if (task_runner_)
        task_runner_->RetireAll();
}

// ===========================================================================
// 布局
// ===========================================================================

void ReportingPanel::BuildUi() {
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(8);

    // ---- 页头: 标题 + 统一「导出」菜单 ----
    // 这一页的两条导出路径（单文件报告 / 批量汇总）在数据形态上不同，但入口
    // 统一收进这一个菜单，不再由两个视图组件各挂一个导出按钮。
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("报告与批量 QC"), row);
        title->setObjectName(QStringLiteral("PageTitle"));
        rl->addWidget(title);
        rl->addStretch();
        cancel_button_ = new QPushButton(tr("取消任务"), row);
        cancel_button_->setToolTip(tr("停止当前正在运行的单文件分析、批量扫描或双文件对比"));
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked, this, &ReportingPanel::OnCancelActiveTask);
        rl->addWidget(cancel_button_);
        compare_button_ = new QPushButton(tr("双文件对比…"), row);
        compare_button_->setToolTip(
            tr("用当前分析结果与另一个文件逐字段对比（容器 / 视频 / 码率与 GOP / 音频 / 色彩）"));
        compare_button_->setEnabled(false);
        connect(compare_button_, &QPushButton::clicked, this, &ReportingPanel::OnCompareWithFile);
        rl->addWidget(compare_button_);
        export_button_ =
            MakeExportMenuButton(row, {{tr("单文件分析报告（按上方所选格式）…"), [this]() { OnExportSingleReport(); },
                                        tr("把当前文件的 QC 报告按所选 JSON / CSV / HTML / TXT / PDF 格式导出")},
                                       {tr("批量扫描汇总…"), [this]() { OnExportBatchSummary(); },
                                        tr("把批量扫描结果导出为 CSV / JSON / HTML 汇总")}});
        // 第一项（单文件报告）的可用性跟着结果新鲜度走，第二项由控制器判空
        single_export_action_ = export_button_->menu()->actions().value(0);
        rl->addWidget(export_button_);
        root->addWidget(row);
    }

    // 三个视图组件与原来的三段独立版面一一对应：模板区（含说明）、单文件报告、批量扫描。
    profile_view_ = new QcProfileView(this);
    profile_view_->SetDescription(QString::fromStdString(profile_.description));
    root->addWidget(profile_view_);

    single_view_ = new SingleReportView(this);
    root->addWidget(single_view_);

    batch_view_ = new BatchScanView(this);
    root->addWidget(batch_view_);

    // ---- 文本摘要 ----
    log_view_ = new QTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMinimumHeight(120);
    log_view_->setPlaceholderText(tr("分析报告的纯文本摘要会显示在这里"));
    root->addWidget(log_view_, 1);

    connect(profile_view_, &QcProfileView::ProfileChanged, this, &ReportingPanel::OnProfileSelected);
    connect(profile_view_, &QcProfileView::LoadRequested, this, &ReportingPanel::OnLoadProfileFile);
    connect(profile_view_, &QcProfileView::SaveRequested, this, &ReportingPanel::OnSaveProfile);
    connect(single_view_, &SingleReportView::AnalyzeRequested, this, &ReportingPanel::OnAnalyzeCurrentFile);
    connect(batch_view_, &BatchScanView::StartRequested, this, &ReportingPanel::OnStartBatch);
    connect(batch_view_, &BatchScanView::CancelRequested, this, &ReportingPanel::OnCancelBatch);
    connect(batch_view_, &BatchScanView::RerunFailedRequested, this, &ReportingPanel::OnRerunFailedBatch);
    connect(batch_view_, &BatchScanView::CompareWithCurrentRequested, this,
            [this](const QString& path) { StartComparisonWith(path.toStdString()); });

    // 初始没有结果：单文件报告那一项必须是禁用的（菜单项默认 enabled）。
    UpdateExportEnabled();
}

// ===========================================================================
// 槽
// ===========================================================================

void ReportingPanel::SetCurrentFile(const QString& path) {
    // 这里存什么就是什么 —— 主窗口可能同步过来一个网络源（http/rtmp/rtsp）。归类不在
    // 这一层做：统一交给 qc::ClassifyMediaInput（QcRunner::AnalyzeFile 内部调用），
    // 于是单文件 QC 能据 Kind 走 FFmpeg 网络输入，本地批量扫描则只接受本地文件。
    const std::string new_path = path.toStdString();
    const bool changed = (new_path != current_path_);
    current_path_ = new_path;
    single_view_->SetCurrentFile(path);
    // 换文件后上一次的结果不再对应当前文件 —— 标为过期并禁用导出，避免把旧结果导出去。
    // 同一个路径重复同步（主窗口可能不止一处发）不算切换，保留结果。
    if (changed)
        InvalidateResult();
}

void ReportingPanel::OnProfileSelected(const QString& profile_id) {
    if (const qc::QcProfile* builtin = qc::FindBuiltinQcProfile(profile_id.toStdString())) {
        // 模板变了：上一次按旧模板算的结论不再作数，但原始 AnalysisResult 在满足
        // 复用条件时可以直接拿来按新模板重算规则（见 RefreshResultForProfile）。
        const bool changed = (profile_.id != builtin->id);
        profile_ = *builtin;
        profile_view_->SetDescription(QString::fromStdString(profile_.description));
        if (changed) {
            RefreshResultForProfile();
            NotifyBatchProfileMismatch();
        }
    }
}

void ReportingPanel::OnLoadProfileFile() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("选择 QC 模板"), QString(), tr("JSON 模板 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    std::string error;
    qc::QcProfile loaded;
    if (!qc::LoadQcProfileFromFile(path.toStdString(), loaded, error)) {
        QMessageBox::warning(this, tr("模板加载失败"), QString::fromStdString(error));
        return;
    }
    const auto unknown = qc::UnknownRuleIds(loaded);
    if (!unknown.empty()) {
        QStringList ids;
        for (const auto& id : unknown)
            ids << QString::fromStdString(id);
        QMessageBox::information(this, tr("模板已加载"), tr("以下规则 id 未识别，已忽略:\n%1").arg(ids.join("\n")));
    }

    profile_ = loaded;
    profile_view_->SetDescription(
        tr("自定义模板: %1（%2）")
            .arg(QString::fromStdString(profile_.name), QString::fromStdString(profile_.description)));
    // 自定义模板同样只决定"重算规则"：满足复用条件（同文件版本 + 选项覆盖 + 完整成功）
    // 时按它重算，否则标为过期。
    RefreshResultForProfile();
    NotifyBatchProfileMismatch();
    emit StatusMessage(tr("已加载模板 %1").arg(path));
}

void ReportingPanel::OnSaveProfile() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出 QC 模板为 JSON"), QString::fromStdString(profile_.id + ".json"), tr("JSON 模板 (*.json)"));
    if (path.isEmpty())
        return;
    if (!qc::SaveQcProfileToFile(path.toStdString(), profile_)) {
        QMessageBox::warning(this, tr("保存失败"), tr("无法写入: %1").arg(path));
        return;
    }
    emit StatusMessage(tr("模板已保存到 %1").arg(path));
}

void ReportingPanel::OnAnalyzeCurrentFile() {
    if (current_path_.empty()) {
        QMessageBox::information(this, tr("未选择文件"), tr("请先在主界面打开一个媒体文件。"));
        return;
    }
    if (busy_) {
        QMessageBox::information(this, tr("任务进行中"), tr("有任务正在运行，请先等待或停止。"));
        return;
    }
    StartSingleAnalysis(current_path_);
}

void ReportingPanel::OnExportSingleReport() {
    // 完整决策（新鲜就直接导 / 不新鲜先挂起再分析 / 弹什么提示）都在导出控制器里。
    if (export_controller_)
        export_controller_->ExportSingleReport();
}

void ReportingPanel::OnStartBatch() {
    const QString directory = batch_view_->DirectoryText().trimmed();
    if (directory.isEmpty()) {
        QMessageBox::information(this, tr("未选择目录"), tr("请先选择要批量扫描的目录。"));
        return;
    }
    if (busy_) {
        QMessageBox::information(this, tr("任务进行中"), tr("有任务正在运行，请先等待或停止。"));
        return;
    }
    StartBatchScan(directory.toStdString());
}

void ReportingPanel::OnCancelBatch() {
    // 报告页的"取消"也只剩这一颗标志：就是 slot 上那个任务的 task::CancelToken，
    // 与诊断页、QcRunner 内部是同一个类型、同一套语义。
    task_runner_->RequestCancel(ReportingTaskCoordinator::Slot::Batch);
    batch_view_->SetSummaryText(tr("正在停止…（已经完成的文件保留结果）"));
}

void ReportingPanel::OnRerunFailedBatch() {
    if (busy_)
        return;
    // 口径与 BatchScanView::HasFailedResults 保持一致：失败 / 超时 / 导出失败 / 被取消。
    std::vector<std::string> paths;
    for (const auto& item : batch_view_->Results()) {
        switch (item.status) {
        case qc::BatchItemStatus::Failed:
        case qc::BatchItemStatus::TimedOut:
        case qc::BatchItemStatus::ExportFailed:
        case qc::BatchItemStatus::Cancelled:
            paths.push_back(item.path);
            break;
        default:
            break;
        }
    }
    if (paths.empty()) {
        AppendSummary(tr("没有可重跑的失败项"));
        return;
    }
    emit StatusMessage(tr("只重跑 %1 个失败项").arg(paths.size()));
    StartBatchScan(batch_view_->DirectoryText().toStdString(), paths);
}

void ReportingPanel::OnCancelActiveTask() {
    if (!task_runner_ || !active_slot_)
        return;
    // 三类任务共用页头按钮：按当前占用的槽位发给对应的取消令牌。
    // 终态仍由任务体自己归还（见 ReportingTaskCoordinator::RequestCancel 注释），
    // 这里只负责请求，不抢着 Finish/回收。
    task_runner_->RequestCancel(*active_slot_);
    if (cancel_button_)
        cancel_button_->setEnabled(false);
    emit StatusMessage(tr("已请求取消当前任务，等待收尾…"));
}

void ReportingPanel::OnExportBatchSummary() {
    if (export_controller_)
        export_controller_->ExportBatchSummary();
}

// ===========================================================================
// 分析执行
// ===========================================================================

// ❗ 后台线程请直接用 PostUiUpdate(task->report, ...)：本函数是**面板成员**，
// 从后台线程调用它等于先解引用 this —— 面板销毁后那条路径就是访问已释放对象
// （评审 P1-2）。这里只留给主线程上那些已经持有任务的场合。
void ReportingPanel::PostToUi(const std::shared_ptr<AnalysisTask>& task, const std::function<void()>& updater) const {
    PostUiUpdate(task ? task->report : nullptr, updater);
}

void ReportingPanel::StartSingleAnalysis(const std::string& path) {
    active_slot_ = ReportingTaskCoordinator::Slot::Single;
    SetBusy(true);
    single_view_->ShowAnalyzing();

    // 起线程（回收上一次 / 分配句柄 / 异常收口）全在任务协调器里。
    const std::shared_ptr<AnalysisTask> task =
        task_runner_->Start(ReportingTaskCoordinator::Slot::Single,
                            [this, path](const std::shared_ptr<AnalysisTask>& t) { RunSingle(t, path); });
    // 并发满。busy_ 本该拦住，这里兜一次：宁可提示，别静默不跑。
    if (!task) {
        SetBusy(false);
        single_view_->ShowFailure(tr("任务并发已满，请稍后再试"));
    }
}

void ReportingPanel::RunSingle(std::shared_ptr<AnalysisTask> task, const std::string& path) {
    const qc::QcProfile profile = profile_;
    // 冻结这次分析实际用的参数（模板决定深度，见 OptionsForDepth）。它既是引擎的输入，
    // 也是之后"换模板能否复用这份结果"的判据（见 result_stamp.h 的 AnalysisOptionsCovers）。
    const videoeye::AnalysisOptions analyzed_options = qc::OptionsForDepth(profile.depth);
    qc::QcRunner runner;
    qc::QcRunCallbacks callbacks;
    // 后台线程在这条 lambda 里**不解引用**面板指针：它只把任务（内含自持有的回报通道）
    // 交给 PostUiUpdate。this 只被**转发**给内层 updater，而后者只会在接收端所在的
    // 线程上、且通道仍然开着时才执行 —— QcRunner 判定放弃后 detach 出去的那条线程
    // 可能比面板活得久，但它碰到的是通道，不是 this。
    callbacks.progress = [this, task](double percent, const std::string& stage) {
        PostUiUpdate(task->report, [this, percent, stage]() {
            batch_view_->SetProgress(static_cast<int>(percent), 100);
            emit StatusMessage(tr("分析进度 %1% %2").arg(static_cast<int>(percent)).arg(QString::fromStdString(stage)));
        });
    };
    // 取消只看令牌这一颗 —— 与批量扫描、诊断页、QcRunner 内部轮询的是同一个来源。
    callbacks.should_cancel = [task]() { return task->cancel.IsCanceled(); };

    // 结果的新鲜度要素在发起分析时（主线程已冻结 profile_ 与 path）就固定下来，
    // 完成回调里按同一份身份登记，避免分析期间状态被改导致"结果对不上文件/模板"。
    const std::string analyzed_path = path;
    const std::string analyzed_profile_id = profile.id;

    // 等待策略：正常分析**不限时**（评审 P1-1 —— 以前把 5 秒的关停回收预算当运行时长
    // 传进来，长视频 / 多音轨 / 深度档一律被判超时）。关停不再靠这个预算兜底：
    // RetireAll 先关回报通道再置取消令牌，QcRunner 见到取消后按 cancel_wait_ms 收尾，
    // 到点就把线程转入受控回收 —— 关界面因此只有一个预算，不再是两段相加。
    //
    // 输入身份在**打开文件之前**冻结（评审 P2-5）：若等分析结束再取版本，取到的是
    // "分析结束那一刻的文件"，而结果对应的是分析期间读到的那份内容 —— 文件在这段时间
    // 被替换时，旧结果会被登记到新文件上。冻结的版本才是这份结果的真实身份。
    //
    // ❗ 顺序由 AnalyzeWithFrozenInputIdentity 钉住：冻结发生在 AnalyzeFile() **之前**
    // （以前这里是先跑完同步的 AnalyzeFile 再冻结，于是"分析期间被替换"既挡不住也测
    // 不出来 —— 冻到的就是替换后的版本，复核自然通过）。放在 worker 线程刚起来、还没
    // 交给 FFmpeg 的这一刻，正是"分析开始时"。
    const auto chain = AnalyzeWithFrozenInputIdentity(analyzed_path, [&](const InputIdentity&) {
        return runner.AnalyzeFile(path, profile, analyzed_options, callbacks, SingleAnalysisBudgets());
    });
    const qc::QcRunResult result = chain.result;
    const InputIdentity analyzed_identity = chain.frozen;
    // 终态由结果 + 取消令牌共同判定：用户取消 -> Canceled；分析跑完 -> Succeeded；
    // 其余（打开失败 / 超时 / 引擎报错）-> Failed。
    const task::TaskState terminal = task->cancel.IsCanceled()
                                         ? task::TaskState::Canceled
                                         : (result.ok ? task::TaskState::Succeeded : task::TaskState::Failed);
    PostUiUpdate(task->report, [this, task, result, terminal, analyzed_path, analyzed_profile_id, analyzed_options,
                                analyzed_identity]() {
        if (result.ok)
            last_result_ = result;
        // 按**冻结时**的身份登记，并复核一次（登记细节见 result_stamp.h）。
        const bool identity_holds =
            CommitAnalysisResult(result_stamp_, analyzed_path, analyzed_profile_id, analyzed_options,
                                 result.analysis.scan_status, analyzed_identity, result.ok);
        if (result.ok && !identity_holds) {
            emit StatusMessage(tr("分析期间文件已被替换，本次结果不对应当前文件，请重新分析"));
        }
        ShowResult(result);
        SetBusy(false);
        // 正常完成路径必须归还终态：否则 TaskManager::State() 会一直停在 Running，
        // 槽位长期持有过期句柄（见 ReportingTaskCoordinator::Finish 注释）。
        task_runner_->Finish(task, terminal);

        // 无结果时点导出 -> 分析成功后在同一目录继续导出；失败则丢弃挂起的目录，
        // 避免悄悄导出空报告（票据取走即作废，见 ReportExportController）。
        if (export_controller_)
            export_controller_->OnSingleAnalysisFinished(result.ok);
    });
}

void ReportingPanel::OnCompareWithFile() {
    const QString initial_dir = QString::fromStdString(current_path_);
    const QString right = QFileDialog::getOpenFileName(this, tr("选择要对比的文件"), initial_dir, tr("媒体文件 (*.*)"));
    if (right.isEmpty())
        return;
    StartComparisonWith(right.toStdString());
}

void ReportingPanel::StartComparisonWith(const std::string& right_path) {
    if (busy_) {
        QMessageBox::information(this, tr("任务进行中"), tr("有任务正在运行，请先等待或停止。"));
        return;
    }
    if (!HasFreshResult()) {
        QMessageBox::information(this, tr("暂无可用结果"), tr("请先分析主界面当前打开的文件，再与所选文件对比。"));
        return;
    }

    active_slot_ = ReportingTaskCoordinator::Slot::Compare;
    SetBusy(true);
    // 左侧结果与模板在主线程冻结；worker 不读取面板状态。
    const qc::QcRunResult left = last_result_;
    const qc::QcProfile profile = profile_;
    const std::shared_ptr<AnalysisTask> task =
        task_runner_->Start(ReportingTaskCoordinator::Slot::Compare,
                            [this, left, profile, right_path](const std::shared_ptr<AnalysisTask>& t) {
                                RunComparison(t, left, profile, right_path);
                            });
    if (!task) {
        SetBusy(false);
        QMessageBox::warning(this, tr("无法对比"), tr("任务并发已满，请稍后再试。"));
    }
}

void ReportingPanel::RunComparison(std::shared_ptr<AnalysisTask> task, qc::QcRunResult left,
                                   const qc::QcProfile& profile, const std::string& right_path) {
    const videoeye::AnalysisOptions analyzed_options = qc::OptionsForDepth(profile.depth);
    qc::QcRunner runner;
    qc::QcRunCallbacks callbacks;
    callbacks.progress = [this, task](double percent, const std::string& stage) {
        PostUiUpdate(task->report, [this, percent, stage]() {
            batch_view_->SetProgress(static_cast<int>(percent), 100);
            emit StatusMessage(tr("对比分析 %1% %2").arg(static_cast<int>(percent)).arg(QString::fromStdString(stage)));
        });
    };
    callbacks.should_cancel = [task]() { return task->cancel.IsCanceled(); };

    const qc::QcRunResult right =
        runner.AnalyzeFile(right_path, profile, analyzed_options, callbacks, SingleAnalysisBudgets());
    const task::TaskState terminal = task->cancel.IsCanceled()
                                         ? task::TaskState::Canceled
                                         : (right.ok ? task::TaskState::Succeeded : task::TaskState::Failed);

    PostUiUpdate(task->report, [this, task, left, right, terminal]() {
        SetBusy(false);
        task_runner_->Finish(task, terminal);
        if (!right.ok) {
            const QString reason = QString::fromStdString(right.error);
            emit StatusMessage(tr("对比失败：%1").arg(reason));
            if (terminal != task::TaskState::Canceled)
                QMessageBox::warning(this, tr("对比失败"), tr("无法分析对比文件：%1").arg(reason));
            return;
        }
        // 双方使用同一模板与同一套规则，差异才有意义。左右原始结果交给对话框：
        // 用户调容忍度时它用 CompareRuns 重算，不重新分析文件。
        auto* dialog = new QcCompareDialog(left, right, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        emit StatusMessage(
            tr("对比完成：差异 %1 项 / 可比 %2 项").arg(dialog->DifferentCount()).arg(dialog->AvailableRowCount()));
    });
}

void ReportingPanel::StartBatchScan(const std::string& directory, const std::vector<std::string>& rerun_paths) {
    active_slot_ = ReportingTaskCoordinator::Slot::Batch;
    SetBusy(true);
    batch_view_->BeginScan();

    // 所有控件读取都在这一帧（主线程）里完成
    const BatchScanView::Settings settings = batch_view_->CurrentSettings();
    BatchRequest request;
    request.directory = directory;
    request.out_dir = settings.out_dir;
    request.extensions = settings.extensions;
    request.formats = SelectedFormats();
    request.profile = profile_;
    request.jobs = settings.jobs;
    request.recursive = settings.recursive;
    request.timeout_seconds = settings.timeout_seconds;
    request.idle_timeout_seconds = settings.idle_timeout_seconds;
    request.visual_qc = settings.visual_qc;
    request.rerun_paths = rerun_paths;

    // 记下这批结果按哪个模板算：之后切换模板时据此提示"结果已过期"。
    batch_profile_id_ = request.profile.id;
    batch_profile_name_ = request.profile.name;
    batch_stale_notified_ = false;

    const std::shared_ptr<AnalysisTask> task =
        task_runner_->Start(ReportingTaskCoordinator::Slot::Batch,
                            [this, request](const std::shared_ptr<AnalysisTask>& t) { RunBatch(t, request); });
    // 并发满：busy_ 本该拦住，兜一次别静默不跑
    if (!task) {
        SetBusy(false);
        batch_view_->SetSummaryText(tr("任务并发已满，请稍后再试"));
    }
}

void ReportingPanel::RunBatch(std::shared_ptr<AnalysisTask> task, const BatchRequest& request) {
    // 重跑失败项：直接使用上一批留下的路径，不再遍历目录（否则会把已成功的也重跑一遍）。
    std::vector<qc::BatchQcItem> items;
    if (!request.rerun_paths.empty()) {
        items.reserve(request.rerun_paths.size());
        for (const auto& path : request.rerun_paths) {
            qc::BatchQcItem item;
            item.path = path;
            items.push_back(item);
        }
    } else {
        items = qc::BatchQcRunner::Discover(request.directory, request.recursive, request.extensions);
    }
    if (items.empty()) {
        PostUiUpdate(task->report, [this]() {
            batch_view_->SetSummaryText(tr("目录下没有匹配的文件"));
            SetBusy(false);
        });
        return;
    }

    qc::BatchQcOptions options;
    options.recursive = request.recursive;
    options.extensions = request.extensions;
    options.max_parallel = request.jobs;
    options.keep_reports = false;
    // 0 = 不限；由批量页的「单文件超时」决定，默认不限，避免长视频在全量画质 QC 下被误判超时。
    options.per_item_timeout_ms = request.timeout_seconds > 0 ? request.timeout_seconds * 1000 : 0;
    options.io_idle_timeout_ms = request.idle_timeout_seconds > 0 ? request.idle_timeout_seconds * 1000 : 0;
    const std::string directory = request.directory;
    const std::string out_dir = request.out_dir;
    const std::vector<qc::QcReportFormat> formats = request.formats;
    const qc::QcProfile profile = request.profile;

    const int total = static_cast<int>(items.size());

    qc::BatchQcCallbacks callbacks;
    callbacks.is_cancelled = [task]() { return task->cancel.IsCanceled(); };
    callbacks.progress = [this, task, total](int finished, int) {
        PostUiUpdate(task->report, [this, finished, total]() { batch_view_->SetProgress(finished, total); });
    };
    callbacks.item_finished = [this, task](const qc::BatchQcItemResult& item) {
        // 注意：item 是 worker 线程的栈对象，跨线程投递前必须先拷一份
        const qc::BatchQcItemResult copy = item;
        PostUiUpdate(task->report, [this, copy]() { batch_view_->AppendResult(copy); });
    };

    // 画质 QC 由批量页显式控制：勾选即开启、未勾选关闭。
    // ❗ 不能只做"未勾选 -> false"：快速模板本身 analyze_visual_qc=false，
    // 那样勾选框在快速模板下等于没生效。快速模板勾选时用 Standard 采样档
    // （2 fps / 256 宽），避免批量场景退化成逐帧全解码；标准/深度模板已经预置了
    // 自己的采样档（OfflineFull），不要在这里降级。
    const videoeye::AnalysisOptions profile_options = qc::OptionsForDepth(profile.depth);
    videoeye::AnalysisOptions batch_options = profile_options;
    batch_options.analyze_visual_qc = request.visual_qc;
    if (request.visual_qc && !profile_options.analyze_visual_qc)
        batch_options.visual_qc_options.preset = model::VisualSamplingPreset::Standard;
    const auto analyze = qc::QcRunner::MakeAnalyzeFunction(profile, batch_options);
    // 每个文件分析完立刻按选中格式落盘（keep_reports=false，报告本体没留内存）。
    // 关键修复（P0）：导出与 UI 共用同一组"保留相对子目录"的预定路径（ApplyExportPaths），
    // 避免不同子目录同名文件互相覆盖、且表格路径 = 真实落盘路径；导出失败时也能区分
    // "分析成功 / 导出失败"。
    const qc::QcAnalyzeFn analyze_with_export = [analyze, out_dir, directory, formats,
                                                 profile](const qc::QcAnalyzeRequest& req) {
        qc::QcRunResult result = analyze(req);
        if (!out_dir.empty() && result.ok) {
            ApplyExportPaths(result, out_dir, directory, req.path, formats, profile);
        }
        return result;
    };

    qc::BatchQcRunner runner;
    const auto run = runner.Run(items, options, analyze_with_export, callbacks);

    // 终态与汇总文本的口径都在 batch_result_model.h（有单测）：
    // 取消 > 失败/超时 > 成功；"共 N 个"用发现到的条目数（跳过/未跑到的也占一行）。
    const task::TaskState terminal = BatchTerminalState(run.summary, task->cancel.IsCanceled());
    PostUiUpdate(task->report, [this, task, run, total, terminal]() {
        batch_view_->SetProgress(batch_view_->RowCount(), total);
        batch_view_->SetSummaryText(BatchSummaryText(run.summary, total));
        // 扫描期间可能换过模板：结果仍按开跑时冻结的 request.profile 计算，
        // 收尾时补一条过期提示（若确实不一致）。
        NotifyBatchProfileMismatch();
        SetBusy(false);
        // 同 RunSingle：整批正常结束后归还终态并清掉该槽位句柄。
        task_runner_->Finish(task, terminal);
    });
}

void ReportingPanel::ShowResult(const qc::QcRunResult& result) {
    single_view_->ShowResult(result);
    if (!result.ok) {
        log_view_->setPlainText(tr("分析失败: %1").arg(QString::fromStdString(result.error)));
        return;
    }
    reporting::QcExportBundle bundle;
    bundle.profile_id = profile_.id;
    bundle.profile_name = profile_.name;
    bundle.run = result;
    log_view_->setPlainText(QString::fromStdString(reporting::QcReportExporter::BuildText(bundle)));
}

void ReportingPanel::RefreshResultForProfile() {
    const videoeye::AnalysisOptions required = qc::OptionsForDepth(profile_.depth);
    const FileVersionStamp current_version = QueryFileVersion(current_path_);
    if (!CanReuseAnalysis(result_stamp_, current_path_, current_version, required)) {
        // 文件版本变了 / 新模板要求更全的分析 / 上次没跑完 —— 都不能靠旧数据糊过去。
        InvalidateResult();
        return;
    }

    // 复用缓存的原始 AnalysisResult，只按新模板重算规则（不重跑 FFmpeg / 不解码）。
    videoeye::qc::QcRuleEngine engine(qc::BuildRulesForProfile(profile_));
    last_result_.report = engine.Evaluate(last_result_.analysis);
    last_result_.profile_id = profile_.id;
    // 重算后这份结果对应的就是新模板 —— 身份戳同步更新，导出保持可用。
    result_stamp_.profile_id = profile_.id;
    ShowResult(last_result_);
    UpdateExportEnabled();
    emit StatusMessage(tr("已复用现有分析结果，按模板 %1 重算规则").arg(QString::fromStdString(profile_.name)));
}

void ReportingPanel::SetBusy(bool busy) {
    busy_ = busy;
    // 任务结束（包括启动失败）就清掉槽位；否则取消按钮会在空闲时仍然可用。
    if (!busy)
        active_slot_.reset();
    profile_view_->SetBusy(busy);
    single_view_->SetBusy(busy);
    batch_view_->SetBusy(busy);
    if (cancel_button_) {
        cancel_button_->setText(tr("取消任务"));
        cancel_button_->setEnabled(busy && active_slot_.has_value());
    }
    // 导出使能 = 空闲 && 结果新鲜。不能只看 busy_ —— 否则切换文件/模板后仍可点导出。
    UpdateExportEnabled();
}

void ReportingPanel::InvalidateResult() {
    result_stamp_ = ResultStamp{};
    // 有文件时明确提示"过期需重分析"，没文件时回到初始文案。
    single_view_->ShowInvalidated(!current_path_.empty());
    UpdateExportEnabled();
}

void ReportingPanel::UpdateExportEnabled() {
    // 只有「单文件分析报告」这一项依赖结果新鲜度；「批量扫描汇总」在控制器里判空。
    const bool fresh_and_idle = !busy_ && HasFreshResult();
    if (single_export_action_)
        single_export_action_->setEnabled(fresh_and_idle);
    if (compare_button_)
        compare_button_->setEnabled(fresh_and_idle);
}

bool ReportingPanel::HasFreshResult() const {
    return IsResultFresh(result_stamp_, current_path_, profile_.id);
}

bool ReportingPanel::IsExportEnabled() const {
    return single_export_action_ && single_export_action_->isEnabled();
}

qc::QcProfile ReportingPanel::CurrentProfile() const {
    return profile_;
}

bool ReportingPanel::IsTaskRunning() const {
    return task_runner_ && task_runner_->IsRunning();
}

std::vector<qc::QcReportFormat> ReportingPanel::SelectedFormats() const {
    return single_view_ ? single_view_->SelectedFormats() : std::vector<qc::QcReportFormat>{qc::QcReportFormat::Json};
}

void ReportingPanel::NotifyBatchProfileMismatch() {
    if (batch_stale_notified_ || batch_profile_id_.empty() || !batch_view_)
        return;
    if (batch_profile_id_ == profile_.id || batch_view_->RowCount() <= 0)
        return;
    batch_stale_notified_ = true;
    const QString note = tr("注意：上方批量结果按模板「%1」计算，当前模板已切换为「%2」；如需新模板结论，请重新扫描。")
                             .arg(QString::fromStdString(batch_profile_name_), QString::fromStdString(profile_.name));
    batch_view_->AppendSummaryNote(note);
    AppendSummary(note);
    emit StatusMessage(note);
}

void ReportingPanel::AppendSummary(const QString& text) {
    log_view_->append(text);
}

} // namespace ui
} // namespace videoeye
