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

#include "utils/QcReportExporter.h"

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
    // 取消 + detach：等 join 会让关闭窗口卡住好几秒。
    // 队列里没跑完的回调由 Qt 随 QObject 销毁丢弃，不会用到悬垂的 this。
    // 批量/单文件分析在 worker 内部靠 atomic 标记中断，Run() 返回前会 join 掉所有线程，
    // 不会遗留后台线程（12.6 的要求）。
    single_cancelled_.store(true);
    batch_cancelled_.store(true);
    if (single_worker_.joinable()) single_worker_.detach();
    if (batch_worker_.joinable()) batch_worker_.detach();
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

    utils::QcExportBundle bundle;
    bundle.profile_id = profile_.id;
    bundle.profile_name = profile_.name;
    bundle.run = last_result_;

    bool ok = true;
    const std::string base = BaseNameOf(last_result_.report.file_path.empty()
                                            ? current_path_
                                            : last_result_.report.file_path);
    for (const auto format : SelectedFormats()) {
        const std::string target = utils::QcReportOutputPath(directory.toStdString(), base, format);
        if (format == qc::QcReportFormat::Pdf) {
            const auto pdf = utils::QcReportExporter::ExportPdf(target, bundle);
            ok &= pdf.ok;
            if (pdf.text_loss) {
                QMessageBox::information(
                    this, tr("PDF 提示"),
                    tr("PDF 使用非嵌入字体，报告里的中文已被替换为 '?'。需要完整中文请勾选 HTML。"));
            }
        } else {
            ok &= utils::QcReportExporter::Export(target, bundle, format);
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
    batch_cancelled_.store(true);
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

    const auto input = utils::QcReportExporter::MakeBatchSummary(
        batch_dir_edit_->text().toStdString(), run, profile_);
    emit StatusMessage(utils::QcReportExporter::ExportBatchSummaryAuto(path.toStdString(), input)
                           ? tr("汇总已导出到 %1").arg(path)
                           : tr("汇总导出失败"));
}

// ===========================================================================
// 分析执行
// ===========================================================================

void ReportingPanel::PostToUi(const std::function<void()>& updater) {
    QMetaObject::invokeMethod(this, updater, Qt::QueuedConnection);
}

void ReportingPanel::StartSingleAnalysis(const std::string& path) {
    SetBusy(true);
    verdict_label_->setText(tr("分析中…"));
    issue_count_label_->setText(QString());
    single_cancelled_.store(false);
    single_worker_ = std::thread(&ReportingPanel::RunSingle, this, path);
}

void ReportingPanel::RunSingle(const std::string& path) {
    const qc::QcProfile profile = profile_;
    qc::QcRunner runner;
    qc::QcRunCallbacks callbacks;
    callbacks.progress = [this](double percent, const std::string& stage) {
        PostToUi([this, percent, stage]() {
            batch_progress_->setValue(static_cast<int>(percent));
            emit StatusMessage(tr("分析进度 %1% %2")
                                   .arg(static_cast<int>(percent))
                                   .arg(QString::fromStdString(stage)));
        });
    };
    callbacks.should_cancel = [this]() { return single_cancelled_.load(); };

    qc::QcRunResult result = runner.AnalyzeFile(path, profile,
                                               qc::OptionsForDepth(profile.depth), callbacks);
    PostToUi([this, result]() {
        last_result_ = result;
        UpdateVerdictLabel(result);
        if (result.ok) {
            log_view_->setPlainText(QString::fromStdString(utils::QcReportExporter::BuildText(
                [&result, this]() {
                    utils::QcExportBundle bundle;
                    bundle.profile_id = profile_.id;
                    bundle.profile_name = profile_.name;
                    bundle.run = result;
                    return bundle;
                }())));
        } else {
            log_view_->setPlainText(tr("分析失败: %1").arg(QString::fromStdString(result.error)));
        }
        SetBusy(false);
    });
}

void ReportingPanel::StartBatchScan(const std::string& directory) {
    SetBusy(true);
    batch_results_.clear();
    batch_table_->setRowCount(0);
    batch_progress_->setValue(0);
    batch_summary_label_->setText(tr("正在扫描目录…"));
    batch_cancelled_.store(false);

    // 所有控件读取都在这一帧（主线程）里完成
    BatchRequest request;
    request.directory = directory;
    request.out_dir = batch_out_edit_->text().trimmed().toStdString();
    request.extensions = ParseExtensionList(batch_ext_edit_->text().trimmed());
    request.formats = SelectedFormats();
    request.profile = profile_;
    request.jobs = jobs_spin_->value();
    request.recursive = recursive_check_->isChecked();

    batch_worker_ = std::thread(&ReportingPanel::RunBatch, this, request);
}

void ReportingPanel::RunBatch(const BatchRequest& request) {
    auto items = qc::BatchQcRunner::Discover(request.directory, request.recursive,
                                             request.extensions);
    if (items.empty()) {
        PostToUi([this]() {
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
    if (!out_dir.empty()) {
        options.output_path_factory = [directory, out_dir, formats](const std::string& path) {
            std::error_code error;
            const auto relative = std::filesystem::relative(path, directory, error);
            const auto rel = error ? std::filesystem::path(path).filename() : relative;
            auto target = std::filesystem::path(out_dir) / rel.parent_path() / rel.stem();
            target += qc::QcReportExtension(formats.empty() ? qc::QcReportFormat::Json
                                                            : formats.front());
            return target.string();
        };
    }

    const int total = static_cast<int>(items.size());

    qc::BatchQcCallbacks callbacks;
    callbacks.is_cancelled = [this]() { return batch_cancelled_.load(); };
    callbacks.progress = [this, total](int finished, int) {
        PostToUi([this, finished, total]() {
            batch_progress_->setRange(0, std::max(1, total));
            batch_progress_->setValue(finished);
        });
    };
    callbacks.item_finished = [this](const qc::BatchQcItemResult& item) {
        // 注意：item 是 worker 线程的栈对象，跨线程投递前必须先拷一份
        const qc::BatchQcItemResult copy = item;
        PostToUi([this, copy]() {
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
    // 每个文件分析完立刻按选中格式落盘（keep_reports=false，报告本体没留内存）
    const qc::QcAnalyzeFn analyze_with_export =
        [analyze, out_dir, formats, profile](const qc::QcAnalyzeRequest& req) {
            qc::QcRunResult result = analyze(req);
            if (out_dir.empty() || !result.ok) return result;
            utils::QcExportBundle bundle;
            bundle.profile_id = profile.id;
            bundle.profile_name = profile.name;
            bundle.run = result;
            for (const auto format : formats) {
                if (format == qc::QcReportFormat::Pdf) {
                    // UI 里已经提示过字形限制，这里不弹窗打扰
                    utils::QcReportExporter::ExportPdf(
                        utils::QcReportOutputPath(out_dir, BaseNameOf(req.path), format),
                        bundle);
                } else {
                    utils::QcReportExporter::Export(
                        utils::QcReportOutputPath(out_dir, BaseNameOf(req.path), format),
                        bundle, format);
                }
            }
            return result;
        };

    qc::BatchQcRunner runner;
    const auto run = runner.Run(items, options, analyze_with_export, callbacks);

    PostToUi([this, run, total]() {
        batch_progress_->setRange(0, std::max(1, total));
        batch_progress_->setValue(batch_table_->rowCount());
        batch_summary_label_->setText(
            tr("共 %1 个 ｜ 完成 %2 ｜ 失败 %3 ｜ 取消 %4 ｜ 跳过 %5 ｜ 致命 %6 错误 %7 警告 %8 提示 %9 ｜ 耗时 %10 ms")
                .arg(total)
                .arg(run.summary.succeeded)
                .arg(run.summary.failed)
                .arg(run.summary.cancelled)
                .arg(run.summary.skipped)
                .arg(run.summary.critical_count)
                .arg(run.summary.error_count)
                .arg(run.summary.warning_count)
                .arg(run.summary.info_count)
                .arg(static_cast<int>(run.summary.elapsed_ms)));
        SetBusy(false);
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
