#include "ui/bitstream_panel/BitstreamPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>

#include "ui/bitstream_panel/BitstreamComparisonView.h"
#include "ui/bitstream_panel/BitstreamInconsistencyView.h"
#include "ui/bitstream_panel/BitstreamStructureView.h"
#include "ui/bitstream_panel/BitstreamSummaryView.h"

namespace videoeye {
namespace ui {

BitstreamPanel::BitstreamPanel(QWidget* parent) : QWidget(parent) {
    BuildUi();
}

BitstreamPanel::~BitstreamPanel() = default;

void BitstreamPanel::BuildUi() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(8, 8, 8, 8);
    main_layout->setSpacing(6);

    // ---- 顶部：摘要 + 工具栏 ----
    summary_view_ = new BitstreamSummaryView(this);
    main_layout->addWidget(summary_view_);

    // ---- 中部：结构树 + 对比表 ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    structure_view_ = new BitstreamStructureView(splitter);
    comparison_view_ = new BitstreamComparisonView(splitter);
    splitter->addWidget(structure_view_);
    splitter->addWidget(comparison_view_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setMinimumHeight(320);
    main_layout->addWidget(splitter, 1);

    // ---- 底部：不一致项 ----
    inconsistency_view_ = new BitstreamInconsistencyView(this);
    main_layout->addWidget(inconsistency_view_);

    connect(summary_view_, &BitstreamSummaryView::RefreshRequested, this, &BitstreamPanel::OnRefreshClicked);
    connect(summary_view_, &BitstreamSummaryView::CopyJsonRequested, this, &BitstreamPanel::OnCopyJsonClicked);
    connect(summary_view_, &BitstreamSummaryView::ExportJsonRequested, this, &BitstreamPanel::OnExportJsonClicked);
}

// --------------------------------------------------------------------------
// 数据入口
// --------------------------------------------------------------------------
void BitstreamPanel::SetResult(const model::BitstreamAnalysisResult& result) {
    current_result_ = result;
    has_result_ = result.analyzed;

    summary_view_->SetResult(result);
    structure_view_->SetResult(result);
    comparison_view_->SetResult(result);
    inconsistency_view_->SetResult(result);
}

void BitstreamPanel::Clear() {
    current_result_ = model::BitstreamAnalysisResult();
    has_result_ = false;
    summary_view_->Clear();
    structure_view_->Clear();
    comparison_view_->Clear();
    inconsistency_view_->Clear();
}

int BitstreamPanel::IssueCount() const {
    return inconsistency_view_ ? inconsistency_view_->RowCount() : 0;
}

void BitstreamPanel::SetScanActive(bool active) {
    scan_active_ = active;
    // 参数集只读 extradata，跟着全文件扫描一起跑；扫描中禁用按钮避免重复触发。
    summary_view_->SetRefreshEnabled(!active);
}

QString BitstreamPanel::ExportToJson() const {
    return QString::fromStdString(current_result_.ToJson());
}

void BitstreamPanel::CopyJsonToClipboard() const {
    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(ExportToJson());
    }
}

void BitstreamPanel::OnRefreshClicked() {
    emit RefreshRequested();
}

void BitstreamPanel::OnCopyJsonClicked() {
    if (!has_result_) {
        QMessageBox::information(this, tr("码流解析"), tr("还没有可导出的解析结果。"));
        return;
    }
    CopyJsonToClipboard();
}

void BitstreamPanel::OnExportJsonClicked() {
    if (!has_result_) {
        QMessageBox::information(this, tr("码流解析"), tr("还没有可导出的解析结果。"));
        return;
    }
    const QString filename =
        QFileDialog::getSaveFileName(this, tr("导出码流解析结果"), "bitstream.json", tr("JSON 文件 (*.json)"));
    if (filename.isEmpty())
        return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }
    file.write(ExportToJson().toUtf8());
}

} // namespace ui
} // namespace videoeye
