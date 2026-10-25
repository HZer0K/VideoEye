#include "ui/reporting_panel/QcProfileView.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/qc/QcProfile.h"

namespace videoeye {
namespace ui {

QcProfileView::QcProfileView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void QcProfileView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    QGroupBox* profile_box = new QGroupBox(tr("QC 模板"), this);
    QHBoxLayout* profile_row = new QHBoxLayout(profile_box);
    profile_combo_ = new QComboBox(profile_box);
    for (const auto& profile : qc::BuiltinQcProfiles()) {
        profile_combo_->addItem(QString::fromStdString(profile.name), QString::fromStdString(profile.id));
    }
    profile_combo_->setToolTip(tr("内置模板按交付场景调整了阈值与严重度；也可加载自定义 JSON 模板"));
    profile_row->addWidget(profile_combo_);

    QPushButton* load_button = new QPushButton(tr("加载模板文件…"), profile_box);
    connect(load_button, &QPushButton::clicked, this, &QcProfileView::LoadRequested);
    profile_row->addWidget(load_button);

    QPushButton* save_button = new QPushButton(tr("另存为…"), profile_box);
    connect(save_button, &QPushButton::clicked, this, &QcProfileView::SaveRequested);
    profile_row->addWidget(save_button);

    layout->addWidget(profile_box);

    description_ = new QLabel(this);
    description_->setWordWrap(true);
    description_->setStyleSheet("color: #6b7280;");
    layout->addWidget(description_);

    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { emit ProfileChanged(CurrentProfileId()); });
}

QString QcProfileView::CurrentProfileId() const {
    return profile_combo_ ? profile_combo_->currentData().toString() : QString();
}

void QcProfileView::SetDescription(const QString& text) {
    if (description_)
        description_->setText(text);
}

void QcProfileView::SetBusy(bool busy) {
    if (profile_combo_)
        profile_combo_->setEnabled(!busy);
}

} // namespace ui
} // namespace videoeye
