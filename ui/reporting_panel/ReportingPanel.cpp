#include "ui/reporting_panel/ReportingPanel.h"

#include <algorithm>

#include <QFileDialog>
#include <QMessageBox>
#include <QStringList>
#include <QVBoxLayout>

#include "core/domain/task/ThreadBody.h"
#include "core/qc/QcProfileMapper.h"
#include "core/qc/QcRuleEngine.h"
#include "core/reporting/QcReportExporter.h"

#include "infrastructure/logging/Logger.h"

#include "ui/reporting_panel/BatchScanView.h"
#include "ui/reporting_panel/QcProfileView.h"
#include "ui/reporting_panel/SingleReportView.h"
#include "ui/reporting_panel/batch_result_model.h"
#include "ui/reporting_panel/report_path.h"

namespace videoeye {
namespace ui {

ReportingPanel::ReportingPanel(QWidget* parent) : QWidget(parent) {
    profile_ = qc::BuiltinQcProfiles().front(); // 通用
    BuildUi();
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
                                            tr("CSV (*.csv);;JSON (*.json);;HTML (*.html)"));
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
    connect(single_view_, &SingleReportView::ExportRequested, this, &ReportingPanel::OnExportSingleReport);
    connect(batch_view_, &BatchScanView::StartRequested, this, &ReportingPanel::OnStartBatch);
    connect(batch_view_, &BatchScanView::CancelRequested, this, &ReportingPanel::OnCancelBatch);
    connect(batch_view_, &BatchScanView::ExportSummaryRequested, this, &ReportingPanel::OnExportBatchSummary);

    // 初始没有结果：导出按钮必须是禁用的（BuildUi 里创建的控件默认 enabled）。
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
        if (changed)
            RefreshResultForProfile();
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

    // 等待策略：正常分析**不限时**（评审 P1-1 —— 以前把 5 秒的关停回收预算当运行时长
    // 传进来，长视频 / 多音轨 / 深度档一律被判超时）。关停不再靠这个预算兜底：
    // RetireAll 先关回报通道再置取消令牌，QcRunner 见到取消后按 cancel_wait_ms 收尾，
    // 到点就把线程转入受控回收 —— 关界面因此只有一个预算，不再是两段相加。
    qc::QcRunResult result = runner.AnalyzeFile(path, profile, analyzed_options, callbacks, SingleAnalysisBudgets());
    // 终态由结果 + 取消令牌共同判定：用户取消 -> Canceled；分析跑完 -> Succeeded；
    // 其余（打开失败 / 超时 / 引擎报错）-> Failed。
    const task::TaskState terminal = task->cancel.IsCanceled()
                                         ? task::TaskState::Canceled
                                         : (result.ok ? task::TaskState::Succeeded : task::TaskState::Failed);
    // 结果的新鲜度要素在发起分析时（主线程已冻结 profile_ 与 path）就固定下来，
    // 完成回调里按同一份身份登记，避免分析期间状态被改导致"结果对不上文件/模板"。
    const std::string analyzed_path = path;
    const std::string analyzed_profile_id = profile.id;
    // 输入身份在**打开文件之前**冻结（评审 P2-5）：完成回调里若再取一次版本，取到的是
    // "分析结束那一刻的文件"，而结果对应的是分析期间读到的那份内容 —— 文件在这段时间
    // 被替换时，旧结果会被登记到新文件上。冻结的版本才是这份结果的真实身份。
    // （放在 worker 线程刚起来、还没交给 FFmpeg 的这一刻，正是"分析开始时"。）
    const InputIdentity analyzed_identity = FreezeInputIdentity(analyzed_path);
    PostUiUpdate(task->report, [this, task, result, terminal, analyzed_path, analyzed_profile_id, analyzed_options,
                                analyzed_identity]() {
        if (result.ok) {
            last_result_ = result;
            result_stamp_ = ResultStamp{};
            result_stamp_.path = analyzed_path;
            result_stamp_.profile_id = analyzed_profile_id;
            result_stamp_.options = analyzed_options;
            result_stamp_.scan_status = result.analysis.scan_status;
            // 登记**冻结时**的版本，再核对一次：文件在分析期间被替换 -> 这份结果不属于
            // 当前文件，标记无效（导出随之禁用），而不是把它当成当前文件的结果。
            result_stamp_.version = analyzed_identity.version;
            const bool identity_holds = analyzed_identity.Matches(analyzed_path, QueryFileVersion(analyzed_path));
            result_stamp_.valid = identity_holds;
            if (!identity_holds) {
                emit StatusMessage(tr("分析期间文件已被替换，本次结果不对应当前文件，请重新分析"));
            }
        } else {
            // 失败结果不作数：导出继续禁用，等待用户重新分析。
            result_stamp_ = ResultStamp{};
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

void ReportingPanel::StartBatchScan(const std::string& directory) {
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
    auto items = qc::BatchQcRunner::Discover(request.directory, request.recursive, request.extensions);
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

    const auto analyze = qc::QcRunner::MakeAnalyzeFunction(profile, qc::OptionsForDepth(profile.depth));
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
    profile_view_->SetBusy(busy);
    single_view_->SetBusy(busy);
    batch_view_->SetBusy(busy);
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
    single_view_->SetExportEnabled(!busy_ && HasFreshResult());
}

bool ReportingPanel::HasFreshResult() const {
    return IsResultFresh(result_stamp_, current_path_, profile_.id);
}

bool ReportingPanel::IsExportEnabled() const {
    return single_view_ && single_view_->IsExportEnabled();
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

void ReportingPanel::AppendSummary(const QString& text) {
    log_view_->append(text);
}

} // namespace ui
} // namespace videoeye
