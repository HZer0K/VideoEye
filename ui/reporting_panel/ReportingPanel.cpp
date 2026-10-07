#include "ui/reporting_panel/ReportingPanel.h"

#include <algorithm>
#include <filesystem>

#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMetaObject>
#include <QMessageBox>
#include <QVBoxLayout>

#include "core/qc/QcProfileMapper.h"
#include "core/reporting/QcReportExporter.h"

#include "infrastructure/logging/Logger.h"

#include "ui/reporting_panel/report_path.h"

namespace videoeye {
namespace ui {
namespace {

QStringList BatchTableHeaders() {
    return QStringList() << QObject::tr("文件") << QObject::tr("状态") << QObject::tr("评分")
                         << QObject::tr("结论") << QObject::tr("致命") << QObject::tr("错误")
                         << QObject::tr("警告") << QObject::tr("提示") << QObject::tr("耗时(ms)")
                         << QObject::tr("输出");
}

std::vector<std::string> ParseExtensionList(const QString& text) {
    std::vector<std::string> extensions;
    std::string current;
    const std::string raw = text.toStdString();
    for (const char c : raw) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t') {
            if (!current.empty()) {
                extensions.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) extensions.push_back(current);
    return extensions;
}

// BatchItemStatus 的文本是字符串常量（不是 tr 字面量），这里只做转码不做翻译
QString StatusText(qc::BatchItemStatus status) {
    return QString::fromStdString(qc::ToString(status));
}

std::string BaseNameOf(const std::string& path) {
    std::filesystem::path file(path);
    return file.stem().string();
}

}  // namespace

ReportingPanel::ReportingPanel(QWidget* parent) : QWidget(parent) {
    profile_ = qc::BuiltinQcProfiles().front();  // 通用
    BuildUi();
}

ReportingPanel::~ReportingPanel() {
    // 取消 + 置 alive=false + **带预算的**回收，全部由 RetireTask 完成：worker 在预算
    // 返回前结束，其回调看到 alive=false 不会再触碰 this；已入队的 UI 事件由 Qt 在对象
    // 销毁时自动移除，不会跑到销毁后的 this。
    // （P0：原先对仍 joinable 的 std::thread 直接 detach，且第二次启动会对其赋值导致 terminate。）
    //
    // 预算这件事是评审 P1-3 的要求：关界面不能再无限等下去。
    RetireTask(single_task_);
    RetireTask(batch_task_);
}

// ===========================================================================
// 布局
// ===========================================================================

void ReportingPanel::BuildUi() {
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(8);

    // ---- 模板选择 ----
    QGroupBox* profile_box = new QGroupBox(tr("QC 模板"), this);
    QHBoxLayout* profile_row = new QHBoxLayout(profile_box);
    profile_combo_ = new QComboBox(profile_box);
    for (const auto& profile : qc::BuiltinQcProfiles()) {
        profile_combo_->addItem(QString::fromStdString(profile.name),
                                QString::fromStdString(profile.id));
    }
    profile_combo_->setToolTip(tr("内置模板按交付场景调整了阈值与严重度；也可加载自定义 JSON 模板"));
    profile_row->addWidget(profile_combo_);

    QPushButton* load_button = new QPushButton(tr("加载模板文件…"), profile_box);
    connect(load_button, &QPushButton::clicked, this, &ReportingPanel::OnLoadProfileFile);
    profile_row->addWidget(load_button);

    QPushButton* save_button = new QPushButton(tr("另存为…"), profile_box);
    connect(save_button, &QPushButton::clicked, this, &ReportingPanel::OnSaveProfile);
    profile_row->addWidget(save_button);

    root->addWidget(profile_box);

    profile_description_ = new QLabel(QString::fromStdString(profile_.description), this);
    profile_description_->setWordWrap(true);
    profile_description_->setStyleSheet("color: #6b7280;");
    root->addWidget(profile_description_);

    // ---- 单文件报告 ----
    QGroupBox* single_box = new QGroupBox(tr("单文件报告"), this);
    QVBoxLayout* single_layout = new QVBoxLayout(single_box);

    QHBoxLayout* file_row = new QHBoxLayout();
    file_row->addWidget(new QLabel(tr("当前文件:"), single_box));
    current_file_label_ = new QLabel(tr("未选择"), single_box);
    current_file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file_row->addWidget(current_file_label_, 1);

    analyze_button_ = new QPushButton(tr("按模板重新分析"), single_box);
    connect(analyze_button_, &QPushButton::clicked, this, &ReportingPanel::OnAnalyzeCurrentFile);
    file_row->addWidget(analyze_button_);

    export_button_ = new QPushButton(tr("导出报告…"), single_box);
    connect(export_button_, &QPushButton::clicked, this, &ReportingPanel::OnExportSingleReport);
    file_row->addWidget(export_button_);
    single_layout->addLayout(file_row);

    QHBoxLayout* format_row = new QHBoxLayout();
    format_row->addWidget(new QLabel(tr("导出格式:"), single_box));
    format_json_ = new QCheckBox(tr("JSON"), single_box);
    format_csv_ = new QCheckBox(tr("CSV"), single_box);
    format_html_ = new QCheckBox(tr("HTML"), single_box);
    format_txt_ = new QCheckBox(tr("TXT"), single_box);
    format_pdf_ = new QCheckBox(tr("PDF"), single_box);
    format_json_->setChecked(true);
    for (auto* check : {format_json_, format_csv_, format_html_, format_txt_, format_pdf_}) {
        format_row->addWidget(check);
    }
    format_pdf_->setToolTip(tr("PDF 不嵌入中文字体，中文会显示成 '?'，需要完整中文请用 HTML"));
    format_row->addStretch();
    single_layout->addLayout(format_row);

    QHBoxLayout* verdict_row = new QHBoxLayout();
    verdict_label_ = new QLabel(tr("尚无结果"), single_box);
    QFont verdict_font = verdict_label_->font();
    verdict_font.setPointSize(verdict_font.pointSize() + 6);
    verdict_font.setBold(true);
    verdict_label_->setFont(verdict_font);
    verdict_row->addWidget(verdict_label_);
    issue_count_label_ = new QLabel(QString(), single_box);
    verdict_row->addWidget(issue_count_label_);
    verdict_row->addStretch();
    single_layout->addLayout(verdict_row);

    root->addWidget(single_box);

    // ---- 批量扫描 ----
    QGroupBox* batch_box = new QGroupBox(tr("批量扫描"), this);
    QVBoxLayout* batch_layout = new QVBoxLayout(batch_box);

    QHBoxLayout* dir_row = new QHBoxLayout();
    dir_row->addWidget(new QLabel(tr("目录:"), batch_box));
    batch_dir_edit_ = new QLineEdit(batch_box);
    batch_dir_edit_->setPlaceholderText(tr("选择要扫描的目录"));
    dir_row->addWidget(batch_dir_edit_, 1);
    QPushButton* pick_dir = new QPushButton(tr("浏览…"), batch_box);
    connect(pick_dir, &QPushButton::clicked, this, &ReportingPanel::OnPickBatchDirectory);
    dir_row->addWidget(pick_dir);
    batch_layout->addLayout(dir_row);

    QHBoxLayout* ext_row = new QHBoxLayout();
    ext_row->addWidget(new QLabel(tr("扩展名:"), batch_box));
    batch_ext_edit_ = new QLineEdit(batch_box);
    batch_ext_edit_->setPlaceholderText(tr("留空表示全部文件，如 mp4,mov,mxf"));
    ext_row->addWidget(batch_ext_edit_, 1);

    ext_row->addWidget(new QLabel(tr("并发:"), batch_box));
    jobs_spin_ = new QSpinBox(batch_box);
    jobs_spin_->setRange(1, 16);
    jobs_spin_->setValue(4);
    jobs_spin_->setToolTip(tr("并发上限，机械盘建议 2~4，SSD 可以更高"));
    ext_row->addWidget(jobs_spin_);

    recursive_check_ = new QCheckBox(tr("递归子目录"), batch_box);
    recursive_check_->setChecked(true);
    ext_row->addWidget(recursive_check_);
    batch_layout->addLayout(ext_row);

    QHBoxLayout* out_row = new QHBoxLayout();
    out_row->addWidget(new QLabel(tr("报告输出目录:"), batch_box));
    batch_out_edit_ = new QLineEdit(batch_box);
    batch_out_edit_->setPlaceholderText(tr("留空则不落盘，只在表里看结果"));
    out_row->addWidget(batch_out_edit_, 1);
    QPushButton* pick_out = new QPushButton(tr("浏览…"), batch_box);
    connect(pick_out, &QPushButton::clicked, this, &ReportingPanel::OnPickOutputDirectory);
    out_row->addWidget(pick_out);
    batch_layout->addLayout(out_row);

    QHBoxLayout* action_row = new QHBoxLayout();
    start_button_ = new QPushButton(tr("开始扫描"), batch_box);
    connect(start_button_, &QPushButton::clicked, this, &ReportingPanel::OnStartBatch);
    action_row->addWidget(start_button_);

    cancel_button_ = new QPushButton(tr("停止"), batch_box);
    cancel_button_->setEnabled(false);
    connect(cancel_button_, &QPushButton::clicked, this, &ReportingPanel::OnCancelBatch);
    action_row->addWidget(cancel_button_);

    batch_progress_ = new QProgressBar(batch_box);
    batch_progress_->setRange(0, 100);
    action_row->addWidget(batch_progress_, 1);

    summary_button_ = new QPushButton(tr("导出汇总…"), batch_box);
    connect(summary_button_, &QPushButton::clicked, this, &ReportingPanel::OnExportBatchSummary);
    action_row->addWidget(summary_button_);
    batch_layout->addLayout(action_row);

    batch_summary_label_ = new QLabel(tr("尚未扫描"), batch_box);
    batch_layout->addWidget(batch_summary_label_);

    batch_table_ = new QTableWidget(0, BatchTableHeaders().size(), batch_box);
    batch_table_->setHorizontalHeaderLabels(BatchTableHeaders());
    batch_table_->verticalHeader()->setVisible(false);
    batch_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    batch_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    batch_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    batch_table_->setColumnWidth(0, 260);
    batch_table_->setMinimumHeight(220);
    batch_layout->addWidget(batch_table_);

    root->addWidget(batch_box);

    // ---- 文本摘要 ----
    log_view_ = new QTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMinimumHeight(120);
    log_view_->setPlaceholderText(tr("分析报告的纯文本摘要会显示在这里"));
    root->addWidget(log_view_, 1);

    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ReportingPanel::OnProfileChanged);
}

// ===========================================================================
// 槽
// ===========================================================================

void ReportingPanel::SetCurrentFile(const QString& path) {
    // 这里存什么就是什么 —— 主窗口可能同步过来一个网络源（http/rtmp/rtsp）。归类不在
    // 这一层做：统一交给 qc::ClassifyMediaInput（QcRunner::AnalyzeFile 内部调用），
    // 于是单文件 QC 能据 Kind 走 FFmpeg 网络输入，本地批量扫描则只接受本地文件。
    current_path_ = path.toStdString();
    current_file_label_->setText(path.isEmpty() ? tr("未选择") : path);
    if (path.isEmpty()) {
        verdict_label_->setText(tr("尚无结果"));
        issue_count_label_->setText(QString());
    }
}

void ReportingPanel::OnProfileChanged(int index) {
    Q_UNUSED(index);
    const QString id = profile_combo_->currentData().toString();
    if (const qc::QcProfile* builtin = qc::FindBuiltinQcProfile(id.toStdString())) {
        profile_ = *builtin;
        profile_description_->setText(QString::fromStdString(profile_.description));
    }
}

void ReportingPanel::OnLoadProfileFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择 QC 模板"), QString(), tr("JSON 模板 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    std::string error;
    qc::QcProfile loaded;
    if (!qc::LoadQcProfileFromFile(path.toStdString(), loaded, error)) {
        QMessageBox::warning(this, tr("模板加载失败"), QString::fromStdString(error));
        return;
    }
    const auto unknown = qc::UnknownRuleIds(loaded);
    if (!unknown.empty()) {
        QStringList ids;
        for (const auto& id : unknown) ids << QString::fromStdString(id);
        QMessageBox::information(this, tr("模板已加载"),
                                 tr("以下规则 id 未识别，已忽略:\n%1").arg(ids.join("\n")));
    }

    profile_ = loaded;
    profile_description_->setText(tr("自定义模板: %1（%2）")
                                      .arg(QString::fromStdString(profile_.name),
                                           QString::fromStdString(profile_.description)));
    emit StatusMessage(tr("已加载模板 %1").arg(path));
}

void ReportingPanel::OnSaveProfile() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出 QC 模板为 JSON"), QString::fromStdString(profile_.id + ".json"),
        tr("JSON 模板 (*.json)"));
    if (path.isEmpty()) return;
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
    // 导出的是最近一次分析结果；没跑过就先跑一次，避免导出空报告
    if (last_result_.report.rules.empty() && !last_result_.ok) {
        OnAnalyzeCurrentFile();
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(this, tr("选择报告输出目录"));
    if (directory.isEmpty()) return;

    reporting::QcExportBundle bundle;
    bundle.profile_id = profile_.id;
    bundle.profile_name = profile_.name;
    bundle.run = last_result_;

    bool ok = true;
    const std::string base = BaseNameOf(last_result_.report.file_path.empty()
                                            ? current_path_
                                            : last_result_.report.file_path);
    for (const auto format : SelectedFormats()) {
        const std::string target = reporting::QcReportOutputPath(directory.toStdString(), base, format);
        if (format == qc::QcReportFormat::Pdf) {
            const auto pdf = reporting::QcReportExporter::ExportPdf(target, bundle);
            ok &= pdf.ok;
            if (pdf.text_loss) {
                QMessageBox::information(
                    this, tr("PDF 提示"),
                    tr("PDF 使用非嵌入字体，报告里的中文已被替换为 '?'。需要完整中文请勾选 HTML。"));
            }
        } else {
            ok &= reporting::QcReportExporter::Export(target, bundle, format);
        }
    }
    emit StatusMessage(ok ? tr("报告已导出到 %1").arg(directory)
                          : tr("报告导出失败，请检查目录权限"));
}

void ReportingPanel::OnPickBatchDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择批量扫描目录"));
    if (!path.isEmpty()) batch_dir_edit_->setText(path);
}

void ReportingPanel::OnPickOutputDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择报告输出目录"));
    if (!path.isEmpty()) batch_out_edit_->setText(path);
}

void ReportingPanel::OnStartBatch() {
    const QString directory = batch_dir_edit_->text().trimmed();
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
    if (batch_task_)
        batch_task_->cancel.RequestCancel();
    batch_summary_label_->setText(tr("正在停止…（已经完成的文件保留结果）"));
}

void ReportingPanel::OnExportBatchSummary() {
    if (batch_results_.empty()) {
        QMessageBox::information(this, tr("没有结果"), tr("请先跑一次批量扫描。"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出批量汇总"), tr("qc-summary.csv"),
        tr("CSV (*.csv);;JSON (*.json);;HTML (*.html)"));
    if (path.isEmpty()) return;

    qc::BatchQcRun run;
    run.items = batch_results_;
    // 汇总只需要前面表格里已经算过的东西，这里按同样口径重算一遍
    for (const auto& item : run.items) {
        run.summary.total += 1;
        switch (item.status) {
            case qc::BatchItemStatus::Succeeded: run.summary.succeeded += 1; break;
            case qc::BatchItemStatus::Failed:    run.summary.failed += 1;    break;
            case qc::BatchItemStatus::ExportFailed: run.summary.failed += 1; break;
            case qc::BatchItemStatus::TimedOut:  run.summary.timed_out += 1; break;
            case qc::BatchItemStatus::Cancelled: run.summary.cancelled += 1; break;
            case qc::BatchItemStatus::Skipped:   run.summary.skipped += 1;   break;
            default: break;
        }
        run.summary.critical_count += item.critical_count;
        run.summary.error_count += item.error_count;
        run.summary.warning_count += item.warning_count;
        run.summary.info_count += item.info_count;
    }
    run.summary.completed = (run.summary.cancelled == 0);

    const auto input = reporting::QcReportExporter::MakeBatchSummary(
        batch_dir_edit_->text().toStdString(), run, profile_);
    emit StatusMessage(reporting::QcReportExporter::ExportBatchSummaryAuto(path.toStdString(), input)
                           ? tr("汇总已导出到 %1").arg(path)
                           : tr("汇总导出失败"));
}

// ===========================================================================
// 分析执行
// ===========================================================================

void ReportingPanel::PostToUi(std::shared_ptr<AnalysisTask> task,
                               const std::function<void()>& updater) {
    // 任务已不再存活（面板正在销毁）时丢弃，避免触碰已销毁的 UI 对象
    if (!task || !task->alive.load(std::memory_order_acquire)) return;
    QMetaObject::invokeMethod(this, updater, Qt::QueuedConnection);
}

void ReportingPanel::RetireTask(std::shared_ptr<AnalysisTask>& task) {
    if (!task)
        return;
    // 先要一份句柄副本：下面的 RecycleTask 会把 task 置空，句柄得先拿出来。
    const task::TaskHandle handle = task->handle;

    // 运行时诊断（评审 P2-4 方案 A）：报告页走严格 Cooperative，**析构没有硬时间上限**。
    // 这条日志把"开始回收、正在等任务体响应取消"这件事留痕 —— 万一关停久等，日志能区分
    // 是"任务体在收尾"还是"取消没生效"。
    LOG_INFO("报告页开始回收任务 #" + std::to_string(handle.id) +
             "（严格 Cooperative：请求取消后等待线程体收尾，无硬时间上限）");

    // 请求取消并封死"还允许往面板上刷结果"这条路。顺序不能反 —— 先取消，任务体
    // 才可能在预算内自己收尾；先置 alive=false 也行，但取消必须做，否则取消按钮
    // 点了没反应（这条路径就是取消按钮 / 析构两条路共用的一份收尾逻辑）。
    task->cancel.RequestCancel();
    task->alive.store(false, std::memory_order_release);

    // 终态只在这里（宿主线程）归还：任务体不能碰 tasks_（它捕获了 this，关停时
    // tasks_ 比它先析构，从线程里 EndHandle 就是悬空访问）。于是 slot 上的终态在
    // 下一次启动 / 析构时补记 —— 正常流程下 join 已返回，这里一定是干净的
    // Running -> Canceled/Succeeded；超预算时记 Canceled，slot 不会永远挂在 Running。
    if (handle.valid())
        tasks_.EndHandle(handle, handle.cancel.IsCanceled() ? task::TaskState::Canceled
                                                           : task::TaskState::Succeeded);

    // 回收：等线程体返回（观测预算）→ join 收句柄 → 置空。报告页归到协议的 Cooperative
    // 一边，而且是**严格** Cooperative：任务体承诺响应取消令牌（取消链一路通到引擎与
    // FFmpeg 中断回调），所以这里的 join 没有上界 —— 预算只是"预期多快收回来"的告警线。
    // 任务体捕获了 this 并往面板上排队 UI 更新，detach 之后那些投递会落到已销毁的
    // QWidget 上；在"硬性时间上限"与"不悬空访问"之间，报告页明确选后者。
    if (!RecycleTask(task)) {
        LOG_WARN("报告页任务回收超出预期预算 " + std::to_string(kDefaultRecycleBudgetMs) +
                 "ms，按严格 Cooperative 继续等待其响应取消（不 detach）");
    }
}

void ReportingPanel::FinishTask(const std::shared_ptr<AnalysisTask>& task,
                                task::TaskState terminal) {
    if (!task) return;
    // 终态一次性写入：EndHandle 只在 id 仍与 slot 当前任务匹配时才生效，否则静默丢弃
    // （说明这个任务已被取代 / 句柄已失效）。写完之后 slot 不再停在 Running。
    tasks_.EndHandle(task->handle, terminal);

    // 归还句柄前必须先把线程收回（join）：任务体按值捕获了 task（shared_ptr），而
    // AnalysisTask 又持有 std::thread —— 二者构成引用环。若这里只 reset 不 join，环
    // 没人打破，AnalysisTask（连同尚可 join 的 std::thread）会永久泄漏。此时
    // RunSingle / RunBatch 已经返回、body_done 已置位，join 几乎瞬间返回。
    // 不归还句柄的话 slot 会一直停在 Running，任务切换时 single_task_ / batch_task_
    // 长期持有过期句柄，RunningCount() 也会长时间不准确。
    if (single_task_ == task) {
        RecycleTask(single_task_);
    } else if (batch_task_ == task) {
        RecycleTask(batch_task_);
    }
}

void ReportingPanel::StartSingleAnalysis(const std::string& path) {
    RetireTask(single_task_);  // 回收上一次（busy_ 已拦住重复启动，这里兜底）
    SetBusy(true);
    verdict_label_->setText(tr("分析中…"));
    issue_count_label_->setText(QString());

    // 任务身份与取消令牌一并向协议要：id 由 slot 分配（"第几次分析"这个号不再自己
    // 另起一套），取消只有 handle.cancel 这一颗，任务体与 QcRunner 轮询的是同一颗。
    const task::TaskHandle handle =
        tasks_.BeginHandle(kTaskSlot, 0, task::TaskKind::Cooperative);
    if (!handle.valid()) {  // 并发满。busy_ 本该拦住，这里兜一次：宁可提示，别静默不跑
        SetBusy(false);
        verdict_label_->setText(tr("任务并发已满，请稍后再试"));
        return;
    }

    auto task = std::make_shared<AnalysisTask>();
    task->handle = handle;
    task->cancel = handle.cancel;
    single_task_ = task;
    task->body_done.store(false, std::memory_order_release);
    // body_done 在**包裹层**置位而不是塞进 RunSingle 的每条 return 路径：RunSingle
    // 里有提前返回（空目录之类），靠函数末尾那一行总会漏，回收就变成干等满预算。
    task->thread = std::thread([this, task, path]() {
        RunSingle(task, path);
        task->body_done.store(true, std::memory_order_release);
    });
}

void ReportingPanel::RunSingle(std::shared_ptr<AnalysisTask> task, const std::string& path) {
    const qc::QcProfile profile = profile_;
    qc::QcRunner runner;
    qc::QcRunCallbacks callbacks;
    callbacks.progress = [this, task](double percent, const std::string& stage) {
        PostToUi(task, [this, percent, stage]() {
            batch_progress_->setValue(static_cast<int>(percent));
            emit StatusMessage(tr("分析进度 %1% %2")
                                   .arg(static_cast<int>(percent))
                                   .arg(QString::fromStdString(stage)));
        });
    };
    // 取消只看令牌这一颗 —— 与批量扫描、诊断页、QcRunner 内部轮询的是同一个来源。
    callbacks.should_cancel = [task]() { return task->cancel.IsCanceled(); };

    // 把这次分析的等待预算压到"回收预算"量级：面板关停时 RetireTask 先置取消令牌，
    // QcRunner 在等待循环里把它转成引擎的 cancel_source（FFmpeg 中断回调也能看见），
    // 所以正常情况下任务体会很快自己收尾；这个预算只是兜底 —— 免得引擎卡在一次不响应
    // 中断的调用里时，关界面变成"5s 等任务体 + 30s 等引擎"两段预算相加。超预算时
    // QcRunner 只持 Box 的 worker 会被放弃(detach)，RunSingle 照常返回。
    qc::QcRunResult result = runner.AnalyzeFile(
        path, profile, qc::OptionsForDepth(profile.depth), callbacks, kDefaultRecycleBudgetMs);
    // 终态由结果 + 取消令牌共同判定：用户取消 -> Canceled；分析跑完 -> Succeeded；
    // 其余（打开失败 / 超时 / 引擎报错）-> Failed。
    const task::TaskState terminal =
        task->cancel.IsCanceled() ? task::TaskState::Canceled
                                  : (result.ok ? task::TaskState::Succeeded
                                               : task::TaskState::Failed);
    PostToUi(task, [this, task, result, terminal]() {
        last_result_ = result;
        UpdateVerdictLabel(result);
        if (result.ok) {
            log_view_->setPlainText(QString::fromStdString(reporting::QcReportExporter::BuildText(
                [&result, this]() {
                    reporting::QcExportBundle bundle;
                    bundle.profile_id = profile_.id;
                    bundle.profile_name = profile_.name;
                    bundle.run = result;
                    return bundle;
                }())));
        } else {
            log_view_->setPlainText(tr("分析失败: %1").arg(QString::fromStdString(result.error)));
        }
        SetBusy(false);
        // 正常完成路径必须归还终态：否则 TaskManager::State() 会一直停在 Running，
        // single_task_ / batch_task_ 长期持有过期句柄（见 FinishTask 注释）。
        FinishTask(task, terminal);
    });
}

void ReportingPanel::StartBatchScan(const std::string& directory) {
    SetBusy(true);
    batch_results_.clear();
    batch_table_->setRowCount(0);
    batch_progress_->setValue(0);
    batch_summary_label_->setText(tr("正在扫描目录…"));

    // 所有控件读取都在这一帧（主线程）里完成
    BatchRequest request;
    request.directory = directory;
    request.out_dir = batch_out_edit_->text().trimmed().toStdString();
    request.extensions = ParseExtensionList(batch_ext_edit_->text().trimmed());
    request.formats = SelectedFormats();
    request.profile = profile_;
    request.jobs = jobs_spin_->value();
    request.recursive = recursive_check_->isChecked();

    RetireTask(batch_task_);  // 回收上一次（busy_ 已拦住重复启动，这里兜底）

    const task::TaskHandle handle =
        tasks_.BeginHandle(kTaskSlot, 0, task::TaskKind::Cooperative);
    if (!handle.valid()) {  // 并发满：busy_ 本该拦住，兜一次别静默不跑
        SetBusy(false);
        batch_summary_label_->setText(tr("任务并发已满，请稍后再试"));
        return;
    }

    auto task = std::make_shared<AnalysisTask>();
    task->handle = handle;
    task->cancel = handle.cancel;
    batch_task_ = task;
    task->body_done.store(false, std::memory_order_release);
    // 同 RunSingle：body_done 在包裹层置位，别漏 RunBatch 里那些提前 return。
    task->thread = std::thread([this, task, request]() {
        RunBatch(task, request);
        task->body_done.store(true, std::memory_order_release);
    });
}

void ReportingPanel::RunBatch(std::shared_ptr<AnalysisTask> task, const BatchRequest& request) {
    auto items = qc::BatchQcRunner::Discover(request.directory, request.recursive,
                                             request.extensions);
    if (items.empty()) {
        PostToUi(task, [this]() {
            batch_summary_label_->setText(tr("目录下没有匹配的文件"));
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
        PostToUi(task, [this, finished, total]() {
            batch_progress_->setRange(0, std::max(1, total));
            batch_progress_->setValue(finished);
        });
    };
    callbacks.item_finished = [this, task](const qc::BatchQcItemResult& item) {
        // 注意：item 是 worker 线程的栈对象，跨线程投递前必须先拷一份
        const qc::BatchQcItemResult copy = item;
        PostToUi(task, [this, copy]() {
            const int row = batch_table_->rowCount();
            batch_table_->insertRow(row);
            batch_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(copy.path)));
            batch_table_->setItem(row, 1, new QTableWidgetItem(StatusText(copy.status)));
            batch_table_->setItem(row, 2, new QTableWidgetItem(QString::number(copy.score, 'f', 1)));
            batch_table_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(copy.verdict)));
            batch_table_->setItem(row, 4, new QTableWidgetItem(QString::number(copy.critical_count)));
            batch_table_->setItem(row, 5, new QTableWidgetItem(QString::number(copy.error_count)));
            batch_table_->setItem(row, 6, new QTableWidgetItem(QString::number(copy.warning_count)));
            batch_table_->setItem(row, 7, new QTableWidgetItem(QString::number(copy.info_count)));
            batch_table_->setItem(row, 8,
                                  new QTableWidgetItem(QString::number(copy.elapsed_ms, 'f', 0)));
            batch_table_->setItem(row, 9,
                                  new QTableWidgetItem(QString::fromStdString(copy.output_path)));
            batch_results_.push_back(copy);
        });
    };

    const auto analyze = qc::QcRunner::MakeAnalyzeFunction(profile, qc::OptionsForDepth(profile.depth));
    // 每个文件分析完立刻按选中格式落盘（keep_reports=false，报告本体没留内存）。
    // 关键修复（P0）：导出与 UI 共用同一组"保留相对子目录"的预定路径（ApplyExportPaths），
    // 避免不同子目录同名文件互相覆盖、且表格路径 = 真实落盘路径；导出失败时也能区分
    // "分析成功 / 导出失败"。
    const qc::QcAnalyzeFn analyze_with_export =
        [analyze, out_dir, directory, formats, profile](const qc::QcAnalyzeRequest& req) {
            qc::QcRunResult result = analyze(req);
            if (!out_dir.empty() && result.ok) {
                ApplyExportPaths(result, out_dir, directory, req.path, formats, profile);
            }
            return result;
        };

    qc::BatchQcRunner runner;
    const auto run = runner.Run(items, options, analyze_with_export, callbacks);

    // 终态：用户取消 -> Canceled；有条目失败或超时 -> Failed；整批跑完且无失败/超时 -> Succeeded。
    // 超时也算"没干净跑完"：它的后台线程被放弃、结果不完整，不能算成功。
    const task::TaskState terminal =
        task->cancel.IsCanceled()
            ? task::TaskState::Canceled
            : ((run.summary.failed == 0 && run.summary.timed_out == 0)
                   ? task::TaskState::Succeeded
                   : task::TaskState::Failed);
    PostToUi(task, [this, task, run, total, terminal]() {
        batch_progress_->setRange(0, std::max(1, total));
        batch_progress_->setValue(batch_table_->rowCount());
        batch_summary_label_->setText(
            tr("共 %1 个 ｜ 完成 %2 ｜ 失败 %3 ｜ 超时 %4 ｜ 取消 %5 ｜ 跳过 %6 ｜ 致命 %7 错误 %8 警告 %9 提示 %10 ｜ 耗时 %11 ms")
                .arg(total)
                .arg(run.summary.succeeded)
                .arg(run.summary.failed)
                .arg(run.summary.timed_out)
                .arg(run.summary.cancelled)
                .arg(run.summary.skipped)
                .arg(run.summary.critical_count)
                .arg(run.summary.error_count)
                .arg(run.summary.warning_count)
                .arg(run.summary.info_count)
                .arg(static_cast<int>(run.summary.elapsed_ms)));
        SetBusy(false);
        // 同 RunSingle：整批正常结束后归还终态并清掉 batch_task_ 句柄。
        FinishTask(task, terminal);
    });
}

void ReportingPanel::UpdateVerdictLabel(const qc::QcRunResult& result) {
    if (!result.ok) {
        verdict_label_->setText(tr("分析未完成"));
        verdict_label_->setStyleSheet("color: #b3261e;");
        issue_count_label_->setText(QString::fromStdString(result.error));
        return;
    }
    verdict_label_->setText(QString::number(result.report.score, 'f', 1) + " / 100  " +
                            QString::fromStdString(result.report.verdict));
    const QString color = (result.report.score >= 90.0) ? "#1b7f3b"
                          : (result.report.score >= 70.0) ? "#b26a00"
                                                         : "#b3261e";
    verdict_label_->setStyleSheet("color: " + color + ";");
    issue_count_label_->setText(
        tr("致命 %1 ｜ 错误 %2 ｜ 警告 %3 ｜ 提示 %4")
            .arg(result.report.CountBySeverity(model::IssueSeverity::Critical))
            .arg(result.report.CountBySeverity(model::IssueSeverity::Error))
            .arg(result.report.CountBySeverity(model::IssueSeverity::Warning))
            .arg(result.report.CountBySeverity(model::IssueSeverity::Info)));
}

void ReportingPanel::SetBusy(bool busy) {
    busy_ = busy;
    start_button_->setEnabled(!busy);
    analyze_button_->setEnabled(!busy);
    export_button_->setEnabled(!busy);
    cancel_button_->setEnabled(busy);
    profile_combo_->setEnabled(!busy);
}

qc::QcProfile ReportingPanel::CurrentProfile() const { return profile_; }

bool ReportingPanel::IsTaskRunning() const { return tasks_.IsRunning(kTaskSlot); }

std::vector<qc::QcReportFormat> ReportingPanel::SelectedFormats() const {
    std::vector<qc::QcReportFormat> formats;
    if (format_json_->isChecked()) formats.push_back(qc::QcReportFormat::Json);
    if (format_csv_->isChecked()) formats.push_back(qc::QcReportFormat::Csv);
    if (format_html_->isChecked()) formats.push_back(qc::QcReportFormat::Html);
    if (format_txt_->isChecked()) formats.push_back(qc::QcReportFormat::Text);
    if (format_pdf_->isChecked()) formats.push_back(qc::QcReportFormat::Pdf);
    if (formats.empty()) formats.push_back(qc::QcReportFormat::Json);  // 一个都不勾退回 JSON
    return formats;
}

void ReportingPanel::AppendSummary(const QString& text) {
    log_view_->append(text);
}

}  // namespace ui
}  // namespace videoeye
