#include "ui/reporting_panel/ReportingPanel.h"

#include <algorithm>

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QMetaObject>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/domain/task/ThreadBody.h"
#include "core/qc/QcProfileMapper.h"
#include "core/reporting/QcReportExporter.h"

#include "infrastructure/logging/Logger.h"

#include "ui/reporting_panel/batch_filter.h"
#include "ui/reporting_panel/batch_result_model.h"
#include "ui/reporting_panel/report_path.h"

namespace videoeye {
namespace ui {
namespace {

QStringList BatchTableHeaders() {
    return QStringList() << QObject::tr("文件") << QObject::tr("状态") << QObject::tr("评分") << QObject::tr("结论")
                         << QObject::tr("致命") << QObject::tr("错误") << QObject::tr("警告") << QObject::tr("提示")
                         << QObject::tr("耗时(ms)") << QObject::tr("输出");
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
    if (!current.empty())
        extensions.push_back(current);
    return extensions;
}

// 把一行十个格子铺进表格。数值列原样存进 Qt::DisplayRole（double / int），
// 显示格式由列委托负责 —— 存成字符串会让排序变成字典序（见 batch_result_model.h）。
// 第一列带 batch_results_ 下标：排序（sortItems 会移动 item 对象，数据跟着走）
// 与筛选（只切可见性）之后仍能对回原始结果。
void ApplyBatchRow(QTableWidget* table, int row, const BatchRowCells& cells, int result_index) {
    if (!table)
        return;
    QTableWidgetItem* path_item = new QTableWidgetItem(cells.path);
    path_item->setData(Qt::UserRole, result_index);
    table->setItem(row, 0, path_item);
    table->setItem(row, 1, new QTableWidgetItem(cells.status));

    auto numeric_item = [](auto value) {
        auto* item = new QTableWidgetItem;
        item->setData(Qt::DisplayRole, QVariant::fromValue(value));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    };
    table->setItem(row, 2, numeric_item(cells.score));
    table->setItem(row, 3, new QTableWidgetItem(cells.verdict));
    table->setItem(row, 4, numeric_item(cells.critical));
    table->setItem(row, 5, numeric_item(cells.error_count));
    table->setItem(row, 6, numeric_item(cells.warning));
    table->setItem(row, 7, numeric_item(cells.info));
    table->setItem(row, 8, numeric_item(cells.elapsed_ms));
    table->setItem(row, 9, new QTableWidgetItem(cells.output_path));
}

// 数值列的显示格式委托。
//
// 评分/耗时以 double 存在 Qt::DisplayRole，排序时 QTableWidgetItem::operator<
// 走 QVariant 数值比较（否则 "10" 会排在 "9" 前面）；显示再格式化成固定小数位。
// displayText 拿不到列号，所以一列一个委托实例（评分 1 位、耗时取整）。
class DoubleFormatDelegate : public QStyledItemDelegate {
public:
    DoubleFormatDelegate(int decimals, QObject* parent = nullptr) : QStyledItemDelegate(parent), decimals_(decimals) {
    }

    QString displayText(const QVariant& value, const QLocale& locale) const override {
        if (value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Float) {
            return QString::number(value.toDouble(), 'f', decimals_);
        }
        return QStyledItemDelegate::displayText(value, locale);
    }

private:
    int decimals_;
};

} // namespace

ReportingPanel::ReportingPanel(QWidget* parent) : QWidget(parent) {
    profile_ = qc::BuiltinQcProfiles().front(); // 通用
    BuildUi();
    // 控制器要读 batch_dir_edit_ 等控件，必须在 BuildUi 之后建。
    SetupExportController();
    SetupTaskCoordinator();
}

void ReportingPanel::SetupTaskCoordinator() {
    ReportingTaskCoordinator::Deps deps;
    deps.ui = this;
    deps.show_failure = [this](const QString& text) {
        verdict_label_->setText(text);
        batch_summary_label_->setText(text);
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
    deps.batch_results = [this]() { return batch_results_; };
    deps.batch_root = [this]() { return batch_dir_edit_ ? batch_dir_edit_->text().toStdString() : std::string(); };
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

    // ---- 模板选择 ----
    QGroupBox* profile_box = new QGroupBox(tr("QC 模板"), this);
    QHBoxLayout* profile_row = new QHBoxLayout(profile_box);
    profile_combo_ = new QComboBox(profile_box);
    for (const auto& profile : qc::BuiltinQcProfiles()) {
        profile_combo_->addItem(QString::fromStdString(profile.name), QString::fromStdString(profile.id));
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

    // 批量表格筛选：结果一多（几百上千行）"看完再挑"不现实，先把状态/严重度收窄。
    // 筛选只切可见性（setRowHidden），不动数据 —— 行与 batch_results_ 的绑定
    // （第一列 item 的 UserRole）因此不受排序/筛选影响。
    QHBoxLayout* filter_row = new QHBoxLayout();
    filter_row->addWidget(new QLabel(tr("筛选:"), batch_box));
    batch_status_filter_ = new QComboBox(batch_box);
    batch_status_filter_->addItem(tr("全部状态"), kBatchStatusFilterAll);
    batch_status_filter_->addItem(tr("已完成"), static_cast<int>(qc::BatchItemStatus::Succeeded));
    batch_status_filter_->addItem(tr("导出失败"), static_cast<int>(qc::BatchItemStatus::ExportFailed));
    batch_status_filter_->addItem(tr("失败"), static_cast<int>(qc::BatchItemStatus::Failed));
    batch_status_filter_->addItem(tr("超时"), static_cast<int>(qc::BatchItemStatus::TimedOut));
    batch_status_filter_->addItem(tr("已取消"), static_cast<int>(qc::BatchItemStatus::Cancelled));
    batch_status_filter_->addItem(tr("已跳过"), static_cast<int>(qc::BatchItemStatus::Skipped));
    batch_status_filter_->setToolTip(tr("按条目的终态筛选"));
    filter_row->addWidget(batch_status_filter_);
    batch_severity_filter_ = new QComboBox(batch_box);
    batch_severity_filter_->addItem(tr("全部严重度"), kBatchSeverityFilterAll);
    batch_severity_filter_->addItem(tr("致命及以上"), kBatchSeverityFilterCritical);
    batch_severity_filter_->addItem(tr("错误及以上"), kBatchSeverityFilterError);
    batch_severity_filter_->addItem(tr("警告及以上"), kBatchSeverityFilterWarning);
    batch_severity_filter_->addItem(tr("有任何问题"), kBatchSeverityFilterInfo);
    batch_severity_filter_->addItem(tr("无异常"), kBatchSeverityFilterClean);
    batch_severity_filter_->setToolTip(tr("按最严重的级别筛选：选中的级别及其之上都会留下"));
    filter_row->addWidget(batch_severity_filter_);
    filter_row->addStretch();
    batch_layout->addLayout(filter_row);

    batch_table_ = new QTableWidget(0, BatchTableHeaders().size(), batch_box);
    batch_table_->setHorizontalHeaderLabels(BatchTableHeaders());
    batch_table_->verticalHeader()->setVisible(false);
    batch_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    batch_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    batch_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    batch_table_->setColumnWidth(0, 260);
    batch_table_->setMinimumHeight(220);
    // 排序默认关：扫描中每插入一行都会触发整表重排（几百个文件就是 O(n^2 log n)，
    // 界面直接卡死）。跑完由 SetBusy(false) 打开，届时用户点表头即可按列排序。
    batch_table_->setSortingEnabled(false);
    // setSortingEnabled(true) 会按表头当前的排序指示立即排一次，而 QHeaderView 的
    // 默认指示是降序（实测）—— 不钉的话扫描一结束行序会莫名倒过来。钉在文件名升序，
    // 用户点过别的表头后指示随之更新，之后的扫描按用户上次的选择排。
    batch_table_->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    // 数值列（评分/计数/耗时）按数值排序：数据以 int/double 存 DisplayRole，
    // 显示由这两个委托格式化，兼顾排序正确性与可读性。
    batch_table_->setItemDelegateForColumn(2, new DoubleFormatDelegate(1, batch_table_));
    batch_table_->setItemDelegateForColumn(8, new DoubleFormatDelegate(0, batch_table_));
    batch_layout->addWidget(batch_table_);

    // 选中行详情：十列表格装不下"为什么失败 / 问题清单"，选中一行在这里展开。
    // keep_reports=false（见 RunBatch）时问题清单为空，详情退化为状态+计数+路径。
    batch_detail_ = new QLabel(tr("选中表格中的一行查看详情"), batch_box);
    batch_detail_->setWordWrap(true);
    batch_detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    batch_detail_->setMinimumHeight(80);
    batch_layout->addWidget(batch_detail_);

    // 报告本体默认不驻留内存（keep_reports=false，见 RunBatch），详情标签只能给摘要；
    // 要看完整的问题清单，就打开这一行落盘的报告文件。
    QHBoxLayout* detail_actions = new QHBoxLayout();
    detail_actions->addStretch();
    open_report_button_ = new QPushButton(tr("打开报告文件"), batch_box);
    open_report_button_->setEnabled(false);
    open_report_button_->setToolTip(tr("打开该条目落盘的报告文件 —— 完整问题清单在里面"));
    connect(open_report_button_, &QPushButton::clicked, this, &ReportingPanel::OnOpenSelectedReport);
    detail_actions->addWidget(open_report_button_);
    batch_layout->addLayout(detail_actions);

    root->addWidget(batch_box);

    // ---- 文本摘要 ----
    log_view_ = new QTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMinimumHeight(120);
    log_view_->setPlaceholderText(tr("分析报告的纯文本摘要会显示在这里"));
    root->addWidget(log_view_, 1);

    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ReportingPanel::OnProfileChanged);

    connect(batch_status_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ReportingPanel::OnBatchFilterChanged);
    connect(batch_severity_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ReportingPanel::OnBatchFilterChanged);
    connect(batch_table_, &QTableWidget::itemSelectionChanged, this, &ReportingPanel::OnBatchSelectionChanged);

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
    current_file_label_->setText(path.isEmpty() ? tr("未选择") : path);
    // 换文件后上一次的结果不再对应当前文件 —— 标为过期并禁用导出，避免把旧结果导出去。
    // 同一个路径重复同步（主窗口可能不止一处发）不算切换，保留结果。
    if (changed)
        InvalidateResult();
}

void ReportingPanel::OnProfileChanged(int index) {
    Q_UNUSED(index);
    const QString id = profile_combo_->currentData().toString();
    if (const qc::QcProfile* builtin = qc::FindBuiltinQcProfile(id.toStdString())) {
        // 模板变了：上一次按旧模板算的结论不再作数，但原始 AnalysisResult 在满足
        // 复用条件时可以直接拿来按新模板重算规则（见 RefreshResultForProfile）。
        const bool changed = (profile_.id != builtin->id);
        profile_ = *builtin;
        profile_description_->setText(QString::fromStdString(profile_.description));
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
    profile_description_->setText(
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

void ReportingPanel::OnPickBatchDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择批量扫描目录"));
    if (!path.isEmpty())
        batch_dir_edit_->setText(path);
}

void ReportingPanel::OnPickOutputDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择报告输出目录"));
    if (!path.isEmpty())
        batch_out_edit_->setText(path);
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
    task_runner_->RequestCancel(ReportingTaskCoordinator::Slot::Batch);
    batch_summary_label_->setText(tr("正在停止…（已经完成的文件保留结果）"));
}

void ReportingPanel::OnExportBatchSummary() {
    if (export_controller_)
        export_controller_->ExportBatchSummary();
}

// ===========================================================================
// 分析执行
// ===========================================================================

void ReportingPanel::PostToUi(const std::shared_ptr<AnalysisTask>& task, const std::function<void()>& updater) const {
    if (task_runner_)
        task_runner_->PostToUi(task, updater);
}

void ReportingPanel::StartSingleAnalysis(const std::string& path) {
    SetBusy(true);
    verdict_label_->setText(tr("分析中…"));
    issue_count_label_->setText(QString());

    // 起线程（回收上一次 / 分配句柄 / 异常收口）全在任务协调器里。
    const std::shared_ptr<AnalysisTask> task =
        task_runner_->Start(ReportingTaskCoordinator::Slot::Single,
                            [this, path](const std::shared_ptr<AnalysisTask>& t) { RunSingle(t, path); });
    // 并发满。busy_ 本该拦住，这里兜一次：宁可提示，别静默不跑。
    if (!task) {
        SetBusy(false);
        verdict_label_->setText(tr("任务并发已满，请稍后再试"));
    }
}

void ReportingPanel::RunSingle(std::shared_ptr<AnalysisTask> task, const std::string& path) {
    const qc::QcProfile profile = profile_;
    // 冻结这次分析实际用的参数（模板决定深度，见 OptionsForDepth）。它既是引擎的输入，
    // 也是之后"换模板能否复用这份结果"的判据（见 result_stamp.h 的 AnalysisOptionsCovers）。
    const videoeye::AnalysisOptions analyzed_options = qc::OptionsForDepth(profile.depth);
    qc::QcRunner runner;
    qc::QcRunCallbacks callbacks;
    callbacks.progress = [this, task](double percent, const std::string& stage) {
        PostToUi(task, [this, percent, stage]() {
            batch_progress_->setValue(static_cast<int>(percent));
            emit StatusMessage(tr("分析进度 %1% %2").arg(static_cast<int>(percent)).arg(QString::fromStdString(stage)));
        });
    };
    // 取消只看令牌这一颗 —— 与批量扫描、诊断页、QcRunner 内部轮询的是同一个来源。
    callbacks.should_cancel = [task]() { return task->cancel.IsCanceled(); };

    // 把这次分析的等待预算压到"回收预算"量级：面板关停时 RetireAll 先置取消令牌，
    // QcRunner 在等待循环里把它转成引擎的 cancel_source（FFmpeg 中断回调也能看见），
    // 所以正常情况下任务体会很快自己收尾；这个预算只是兜底 —— 免得引擎卡在一次不响应
    // 中断的调用里时，关界面变成"5s 等任务体 + 30s 等引擎"两段预算相加。超预算时
    // QcRunner 只持 Box 的 worker 会被放弃(detach)，RunSingle 照常返回。
    qc::QcRunResult result = runner.AnalyzeFile(path, profile, analyzed_options, callbacks, kDefaultRecycleBudgetMs);
    // 终态由结果 + 取消令牌共同判定：用户取消 -> Canceled；分析跑完 -> Succeeded；
    // 其余（打开失败 / 超时 / 引擎报错）-> Failed。
    const task::TaskState terminal = task->cancel.IsCanceled()
                                         ? task::TaskState::Canceled
                                         : (result.ok ? task::TaskState::Succeeded : task::TaskState::Failed);
    // 结果的新鲜度要素在发起分析时（主线程已冻结 profile_ 与 path）就固定下来，
    // 完成回调里按同一份身份登记，避免分析期间状态被改导致"结果对不上文件/模板"。
    const std::string analyzed_path = path;
    const std::string analyzed_profile_id = profile.id;
    PostToUi(task, [this, task, result, terminal, analyzed_path, analyzed_profile_id, analyzed_options]() {
        if (result.ok) {
            last_result_ = result;
            result_stamp_ = ResultStamp{};
            result_stamp_.path = analyzed_path;
            result_stamp_.profile_id = analyzed_profile_id;
            result_stamp_.valid = true;
            // 复用判据：这次用的参数、扫描终态、分析时本地文件版本（网络源为 invalid）。
            result_stamp_.options = analyzed_options;
            result_stamp_.scan_status = result.analysis.scan_status;
            result_stamp_.version = QueryFileVersion(analyzed_path);
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
    batch_results_.clear();
    batch_table_->setRowCount(0);
    batch_progress_->setValue(0);
    batch_summary_label_->setText(tr("正在扫描目录…"));
    if (batch_detail_)
        batch_detail_->setText(tr("选中表格中的一行查看详情"));

    // 所有控件读取都在这一帧（主线程）里完成
    BatchRequest request;
    request.directory = directory;
    request.out_dir = batch_out_edit_->text().trimmed().toStdString();
    request.extensions = ParseExtensionList(batch_ext_edit_->text().trimmed());
    request.formats = SelectedFormats();
    request.profile = profile_;
    request.jobs = jobs_spin_->value();
    request.recursive = recursive_check_->isChecked();

    const std::shared_ptr<AnalysisTask> task =
        task_runner_->Start(ReportingTaskCoordinator::Slot::Batch,
                            [this, request](const std::shared_ptr<AnalysisTask>& t) { RunBatch(t, request); });
    // 并发满：busy_ 本该拦住，兜一次别静默不跑
    if (!task) {
        SetBusy(false);
        batch_summary_label_->setText(tr("任务并发已满，请稍后再试"));
    }
}

void ReportingPanel::RunBatch(std::shared_ptr<AnalysisTask> task, const BatchRequest& request) {
    auto items = qc::BatchQcRunner::Discover(request.directory, request.recursive, request.extensions);
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
            const int result_index = static_cast<int>(batch_results_.size());
            batch_results_.push_back(copy);
            ApplyBatchRow(batch_table_, row, MakeBatchRowCells(copy), result_index);
            // 扫描中也要遵守当前筛选 —— 否则"只看出问题的"会漏进干净行
            batch_table_->setRowHidden(row, !BatchRowVisible(copy));
        });
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
    PostToUi(task, [this, task, run, total, terminal]() {
        batch_progress_->setRange(0, std::max(1, total));
        batch_progress_->setValue(batch_table_->rowCount());
        batch_summary_label_->setText(BatchSummaryText(run.summary, total));
        SetBusy(false);
        // 同 RunSingle：整批正常结束后归还终态并清掉该槽位句柄。
        task_runner_->Finish(task, terminal);
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
    const QString color = (result.report.score >= 90.0)   ? "#1b7f3b"
                          : (result.report.score >= 70.0) ? "#b26a00"
                                                          : "#b3261e";
    verdict_label_->setStyleSheet("color: " + color + ";");
    issue_count_label_->setText(tr("致命 %1 ｜ 错误 %2 ｜ 警告 %3 ｜ 提示 %4")
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Critical))
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Error))
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Warning))
                                    .arg(result.report.CountBySeverity(model::IssueSeverity::Info)));
}

void ReportingPanel::ShowResult(const qc::QcRunResult& result) {
    UpdateVerdictLabel(result);
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
    QcRuleEngine engine(qc::BuildRulesForProfile(profile_));
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
    start_button_->setEnabled(!busy);
    analyze_button_->setEnabled(!busy);
    cancel_button_->setEnabled(busy);
    profile_combo_->setEnabled(!busy);
    // 排序随扫描状态切换：扫描中关掉（每插一行整表重排一次，几百个文件能卡死界面），
    // 结束后打开（打开时会按当前排序指示立即排一次，用户也可再点表头改排法）。
    if (batch_table_)
        batch_table_->setSortingEnabled(!busy);
    // 导出使能 = 空闲 && 结果新鲜。不能只看 busy_ —— 否则切换文件/模板后仍可点导出。
    UpdateExportEnabled();
}

void ReportingPanel::InvalidateResult() {
    result_stamp_ = ResultStamp{};
    // 有文件时明确提示"过期需重分析"，没文件时回到初始文案。
    if (current_path_.empty()) {
        verdict_label_->setText(tr("尚无结果"));
    } else {
        verdict_label_->setText(tr("结果已过期，请重新分析"));
    }
    verdict_label_->setStyleSheet(QString());
    issue_count_label_->setText(QString());
    UpdateExportEnabled();
}

void ReportingPanel::UpdateExportEnabled() {
    if (export_button_)
        export_button_->setEnabled(!busy_ && HasFreshResult());
}

bool ReportingPanel::HasFreshResult() const {
    return IsResultFresh(result_stamp_, current_path_, profile_.id);
}

bool ReportingPanel::IsExportEnabled() const {
    return export_button_ && export_button_->isEnabled();
}

bool ReportingPanel::BatchRowVisible(const qc::BatchQcItemResult& item) const {
    if (!batch_status_filter_ || !batch_severity_filter_)
        return true;
    return BatchMatchesStatusFilter(batch_status_filter_->currentData().toInt(), item.status) &&
           BatchMatchesSeverityFilter(batch_severity_filter_->currentData().toInt(), item);
}

void ReportingPanel::OnBatchFilterChanged() {
    ReapplyBatchFilter();
}

void ReportingPanel::ReapplyBatchFilter() {
    if (!batch_table_)
        return;
    // 批量动辄几百上千行，逐行 setRowHidden 每行都会触发一次视口重算/重绘。
    // 整段关掉更新，最后一次重绘即可 —— 用户感知是"筛选立刻生效"而不是逐行闪。
    const bool was_updates_enabled = batch_table_->updatesEnabled();
    batch_table_->setUpdatesEnabled(false);
    for (int row = 0; row < batch_table_->rowCount(); ++row) {
        QTableWidgetItem* first = batch_table_->item(row, 0);
        if (!first)
            continue;
        const int index = first->data(Qt::UserRole).toInt();
        if (index < 0 || index >= static_cast<int>(batch_results_.size()))
            continue;
        batch_table_->setRowHidden(row, !BatchRowVisible(batch_results_[index]));
    }
    batch_table_->setUpdatesEnabled(was_updates_enabled);
    // 当前选中行可能被筛掉：selection 不会自动清，详情留着会让人以为选的还是它。
    // setCurrentItem(nullptr) 只清当前索引、不一定清选择范围，直接走 selectionModel。
    if (QTableWidgetItem* current = batch_table_->currentItem()) {
        if (batch_table_->isRowHidden(current->row())) {
            batch_table_->selectionModel()->clearSelection();
        }
    }
}

void ReportingPanel::OnBatchSelectionChanged() {
    if (!batch_table_ || !batch_detail_)
        return;
    // 先回到"未选中"态：下面只有真的定位到一条结果时才会把详情和按钮填回来。
    if (open_report_button_) {
        open_report_button_->setEnabled(false);
        open_report_button_->setProperty("reportPath", QString());
    }
    const QList<QTableWidgetItem*> selected = batch_table_->selectedItems();
    if (selected.isEmpty()) {
        batch_detail_->setText(tr("选中表格中的一行查看详情"));
        return;
    }
    QTableWidgetItem* first = batch_table_->item(selected.first()->row(), 0);
    if (!first) {
        batch_detail_->setText(tr("选中表格中的一行查看详情"));
        return;
    }
    const int index = first->data(Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(batch_results_.size())) {
        batch_detail_->setText(tr("选中表格中的一行查看详情"));
        return;
    }
    ShowBatchDetail(index);
}

void ReportingPanel::ShowBatchDetail(int result_index) {
    if (!batch_detail_ || result_index < 0 || result_index >= static_cast<int>(batch_results_.size())) {
        return;
    }
    const qc::BatchQcItemResult& item = batch_results_[result_index];
    // 详情文本的口径（含 ExportFailed 的专门说明、问题清单）在 batch_result_model.h。
    batch_detail_->setText(BatchDetailText(item));

    // 报告文件按选中行走：路径存在按钮的 property 上，省一个只为了传参的成员。
    if (open_report_button_) {
        const QString output = QString::fromStdString(item.output_path);
        open_report_button_->setProperty("reportPath", output);
        open_report_button_->setEnabled(!output.isEmpty());
    }
}

void ReportingPanel::OnOpenSelectedReport() {
    const QString path = open_report_button_ ? open_report_button_->property("reportPath").toString() : QString();
    if (path.isEmpty())
        return;
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("报告文件不存在"), tr("该条目的报告文件已不在原处:\n%1").arg(path));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, tr("打不开报告文件"), tr("系统没有为这类文件关联打开方式:\n%1").arg(path));
    }
}

qc::QcProfile ReportingPanel::CurrentProfile() const {
    return profile_;
}

bool ReportingPanel::IsTaskRunning() const {
    return task_runner_ && task_runner_->IsRunning();
}

std::vector<qc::QcReportFormat> ReportingPanel::SelectedFormats() const {
    std::vector<qc::QcReportFormat> formats;
    if (format_json_->isChecked())
        formats.push_back(qc::QcReportFormat::Json);
    if (format_csv_->isChecked())
        formats.push_back(qc::QcReportFormat::Csv);
    if (format_html_->isChecked())
        formats.push_back(qc::QcReportFormat::Html);
    if (format_txt_->isChecked())
        formats.push_back(qc::QcReportFormat::Text);
    if (format_pdf_->isChecked())
        formats.push_back(qc::QcReportFormat::Pdf);
    if (formats.empty())
        formats.push_back(qc::QcReportFormat::Json); // 一个都不勾退回 JSON
    return formats;
}

void ReportingPanel::AppendSummary(const QString& text) {
    log_view_->append(text);
}

} // namespace ui
} // namespace videoeye
