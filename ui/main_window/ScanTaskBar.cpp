#include "ui/main_window/ScanTaskBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

#include <algorithm>

namespace videoeye {
namespace ui {

namespace {
// 段高固定：它是"常驻条"，高度会变的话会带着下面的内容一直抖。
constexpr int kScanTaskBarHeight = 36;
} // namespace

ScanTaskBar::ScanTaskBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ScanTaskBar"));
    setFixedHeight(kScanTaskBarHeight);

    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 8, 0);
    layout->setSpacing(10);

    QLabel* title = new QLabel(tr("全文件扫描"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    stage_label_ = new QLabel(tr("准备中…"), this);
    stage_label_->setObjectName(QStringLiteral("ScanTaskStage"));
    layout->addWidget(stage_label_);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("ScanTaskProgress"));
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->setMinimumWidth(160);
    layout->addWidget(progress_, 1);

    cancel_button_ = new QPushButton(tr("取消扫描"), this);
    connect(cancel_button_, &QPushButton::clicked, this, &ScanTaskBar::OnCancelClicked);
    layout->addWidget(cancel_button_);

    setVisible(false);
}

void ScanTaskBar::SetScanState(bool running, double percent, const QString& stage) {
    if (!running) {
        cancelling_ = false;
        setVisible(false);
        return;
    }

    setVisible(true);
    if (cancelling_) {
        // 取消中：进度与阶段都在收尾，不再用扫描侧的更新覆盖等待文案，
        // 否则进度条还在往前跑，看起来像"取消没被受理"。
        return;
    }

    stage_label_->setText(stage.isEmpty() ? tr("正在分析…") : stage);
    progress_->setValue(static_cast<int>(std::clamp(percent, 0.0, 100.0)));
    cancel_button_->setEnabled(true);
    cancel_button_->setText(tr("取消扫描"));
}

void ScanTaskBar::OnCancelClicked() {
    if (cancelling_)
        return;
    cancelling_ = true;
    cancel_button_->setEnabled(false);
    stage_label_->setText(tr("取消中…（后台仍在收尾）"));
    emit CancelRequested();
}

} // namespace ui
} // namespace videoeye
