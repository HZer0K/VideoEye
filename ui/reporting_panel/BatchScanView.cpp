#include "ui/reporting_panel/BatchScanView.h"

#include <algorithm>

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/reporting_panel/batch_filter.h"
#include "ui/reporting_panel/batch_result_model.h"

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

BatchScanView::BatchScanView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void BatchScanView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    QGroupBox* batch_box = new QGroupBox(tr("批量扫描"), this);
    QVBoxLayout* batch_layout = new QVBoxLayout(batch_box);

    QHBoxLayout* dir_row = new QHBoxLayout();
    dir_row->addWidget(new QLabel(tr("目录:"), batch_box));
    batch_dir_edit_ = new QLineEdit(batch_box);
    batch_dir_edit_->setPlaceholderText(tr("选择要扫描的目录"));
    dir_row->addWidget(batch_dir_edit_, 1);
    QPushButton* pick_dir = new QPushButton(tr("浏览…"), batch_box);
    connect(pick_dir, &QPushButton::clicked, this, &BatchScanView::OnPickDirectory);
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
    connect(pick_out, &QPushButton::clicked, this, &BatchScanView::OnPickOutputDirectory);
    out_row->addWidget(pick_out);
    batch_layout->addLayout(out_row);

    QHBoxLayout* action_row = new QHBoxLayout();
    start_button_ = new QPushButton(tr("开始扫描"), batch_box);
    connect(start_button_, &QPushButton::clicked, this, &BatchScanView::StartRequested);
    action_row->addWidget(start_button_);

    cancel_button_ = new QPushButton(tr("停止"), batch_box);
    cancel_button_->setEnabled(false);
    connect(cancel_button_, &QPushButton::clicked, this, &BatchScanView::CancelRequested);
    action_row->addWidget(cancel_button_);

    batch_progress_ = new QProgressBar(batch_box);
    batch_progress_->setRange(0, 100);
    action_row->addWidget(batch_progress_, 1);

    summary_button_ = new QPushButton(tr("导出汇总…"), batch_box);
    connect(summary_button_, &QPushButton::clicked, this, &BatchScanView::ExportSummaryRequested);
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
    connect(open_report_button_, &QPushButton::clicked, this, &BatchScanView::OnOpenSelectedReport);
    detail_actions->addWidget(open_report_button_);
    batch_layout->addLayout(detail_actions);

    layout->addWidget(batch_box);

    connect(batch_status_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &BatchScanView::OnFilterChanged);
    connect(batch_severity_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &BatchScanView::OnFilterChanged);
    connect(batch_table_, &QTableWidget::itemSelectionChanged, this, &BatchScanView::OnSelectionChanged);
}

QString BatchScanView::DirectoryText() const {
    return batch_dir_edit_ ? batch_dir_edit_->text() : QString();
}

BatchScanView::Settings BatchScanView::CurrentSettings() const {
    Settings settings;
    settings.out_dir = batch_out_edit_ ? batch_out_edit_->text().trimmed().toStdString() : std::string();
    settings.extensions = ParseExtensionList(batch_ext_edit_ ? batch_ext_edit_->text().trimmed() : QString());
    settings.jobs = jobs_spin_ ? jobs_spin_->value() : 4;
    settings.recursive = recursive_check_ ? recursive_check_->isChecked() : true;
    return settings;
}

void BatchScanView::BeginScan() {
    batch_results_.clear();
    if (batch_table_)
        batch_table_->setRowCount(0);
    if (batch_progress_)
        batch_progress_->setValue(0);
    SetSummaryText(tr("正在扫描目录…"));
    ResetDetail();
}

void BatchScanView::AppendResult(const qc::BatchQcItemResult& item) {
    if (!batch_table_)
        return;
    const int row = batch_table_->rowCount();
    batch_table_->insertRow(row);
    const int result_index = static_cast<int>(batch_results_.size());
    batch_results_.push_back(item);
    ApplyBatchRow(batch_table_, row, MakeBatchRowCells(item), result_index);
    // 扫描中也要遵守当前筛选 —— 否则"只看出问题的"会漏进干净行
    batch_table_->setRowHidden(row, !RowVisible(item));
}

int BatchScanView::RowCount() const {
    return batch_table_ ? batch_table_->rowCount() : 0;
}

void BatchScanView::SetProgress(int finished, int total) {
    if (!batch_progress_)
        return;
    batch_progress_->setRange(0, std::max(1, total));
    batch_progress_->setValue(finished);
}

void BatchScanView::SetSummaryText(const QString& text) {
    if (batch_summary_label_)
        batch_summary_label_->setText(text);
}

void BatchScanView::ResetDetail() {
    if (batch_detail_)
        batch_detail_->setText(tr("选中表格中的一行查看详情"));
}

void BatchScanView::SetBusy(bool busy) {
    if (start_button_)
        start_button_->setEnabled(!busy);
    if (cancel_button_)
        cancel_button_->setEnabled(busy);
    // 排序随扫描状态切换：扫描中关掉（每插一行整表重排一次，几百个文件能卡死界面），
    // 结束后打开（打开时会按当前排序指示立即排一次，用户也可再点表头改排法）。
    if (batch_table_)
        batch_table_->setSortingEnabled(!busy);
}

void BatchScanView::OnPickDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择批量扫描目录"));
    if (!path.isEmpty())
        batch_dir_edit_->setText(path);
}

void BatchScanView::OnPickOutputDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择报告输出目录"));
    if (!path.isEmpty())
        batch_out_edit_->setText(path);
}

bool BatchScanView::RowVisible(const qc::BatchQcItemResult& item) const {
    if (!batch_status_filter_ || !batch_severity_filter_)
        return true;
    return BatchMatchesStatusFilter(batch_status_filter_->currentData().toInt(), item.status) &&
           BatchMatchesSeverityFilter(batch_severity_filter_->currentData().toInt(), item);
}

void BatchScanView::OnFilterChanged() {
    ReapplyFilter();
}

void BatchScanView::ReapplyFilter() {
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
        batch_table_->setRowHidden(row, !RowVisible(batch_results_[index]));
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

void BatchScanView::OnSelectionChanged() {
    if (!batch_table_ || !batch_detail_)
        return;
    // 先回到"未选中"态：下面只有真的定位到一条结果时才会把详情和按钮填回来。
    if (open_report_button_) {
        open_report_button_->setEnabled(false);
        open_report_button_->setProperty("reportPath", QString());
    }
    const QList<QTableWidgetItem*> selected = batch_table_->selectedItems();
    if (selected.isEmpty()) {
        ResetDetail();
        return;
    }
    QTableWidgetItem* first = batch_table_->item(selected.first()->row(), 0);
    if (!first) {
        ResetDetail();
        return;
    }
    const int index = first->data(Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(batch_results_.size())) {
        ResetDetail();
        return;
    }
    ShowDetail(index);
}

void BatchScanView::ShowDetail(int result_index) {
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

void BatchScanView::OnOpenSelectedReport() {
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

} // namespace ui
} // namespace videoeye
