#include "ui/bitstream_panel/BitstreamPanel.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"
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

    // ---- 页头：标题 + 开始分析 + 统一「导出」菜单 ----
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("编码参数集"), row);
        title->setObjectName(QStringLiteral("PageTitle"));
        rl->addWidget(title);
        rl->addStretch();

        refresh_button_ = new QPushButton(tr("开始分析"), row);
        refresh_button_->setToolTip(tr("读取视频流的参数集（只读 extradata，不解码）"));
        connect(refresh_button_, &QPushButton::clicked, this, &BitstreamPanel::OnRefreshClicked);
        rl->addWidget(refresh_button_);

        // 不再提供「复制 JSON」：同一份序列化结果已在「导出」菜单里落盘，
        // 多一个按钮只是把页头挤宽、且与其它页的「分析入口 + 导出」两件套不一致。
        rl->addWidget(MakeExportMenuButton(row, {{tr("码流解析结果 JSON…"), [this]() { OnExportJsonClicked(); },
                                                  tr("把当前参数集解析结果导出为 JSON 文件")}}));
        main_layout->addWidget(row);
    }

    // ---- 顶部：摘要 ----
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
    if (refresh_button_)
        refresh_button_->setEnabled(!active);
}

QString BitstreamPanel::ExportToJson() const {
    return QString::fromStdString(current_result_.ToJson());
}

void BitstreamPanel::OnRefreshClicked() {
    emit RefreshRequested();
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
