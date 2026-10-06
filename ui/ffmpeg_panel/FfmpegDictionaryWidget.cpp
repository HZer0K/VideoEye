#include "ui/ffmpeg_panel/FfmpegDictionaryWidget.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLatin1String>
#include <QVBoxLayout>

#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {

FfmpegDictionaryWidget::FfmpegDictionaryWidget(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
    // 词条是静态表，构造时就能列出全部，不必等页面注入任何东西。
    RefreshDictionary();
}

void FfmpegDictionaryWidget::SetupUi() {
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    QGroupBox* box = new QGroupBox(tr("指令字典"), this);
    QVBoxLayout* layout = new QVBoxLayout(box);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);
    outer->addWidget(box);

    search_edit_ = new QLineEdit(box);
    search_edit_->setPlaceholderText(tr("搜索参数或滤镜，如 crf / scale / 码率"));
    connect(search_edit_, &QLineEdit::textChanged,
            this, &FfmpegDictionaryWidget::OnSearchTextChanged);
    layout->addWidget(search_edit_);

    category_combo_ = new QComboBox(box);
    category_combo_->addItem(tr("全部分类"));
    for (const QString& name : ffmpeg::FfmpegCommandCatalog::CategoryNames()) {
        category_combo_->addItem(name);
    }
    connect(category_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FfmpegDictionaryWidget::OnCategoryChanged);
    layout->addWidget(category_combo_);

    dict_list_ = new QListWidget(box);
    dict_list_->setAlternatingRowColors(true);
    connect(dict_list_, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem*, QListWidgetItem*) { OnDictionaryCurrentChanged(); });
    layout->addWidget(dict_list_, 1);

    dict_insert_button_ = new QPushButton(tr("插入到命令"), box);
    connect(dict_insert_button_, &QPushButton::clicked,
            this, &FfmpegDictionaryWidget::OnInsertEntry);
    layout->addWidget(dict_insert_button_);

    dict_detail_ = new QTextBrowser(box);
    dict_detail_->setOpenExternalLinks(true);
    dict_detail_->setMinimumHeight(180);
    layout->addWidget(dict_detail_, 1);
}

void FfmpegDictionaryWidget::RefreshDictionary() {
    QString category = category_combo_->currentText();
    if (category == tr("全部分类")) {
        category.clear();
    }
    dict_results_ = ffmpeg::FfmpegCommandCatalog::Search(search_edit_->text(), category);

    const int keep = dict_list_->currentRow();
    dict_list_->blockSignals(true);
    dict_list_->clear();
    for (const auto* entry : dict_results_) {
        const QString label = entry->kind == ffmpeg::FfmpegEntryKind::Filter
                                  ? QStringLiteral("%1（滤镜）").arg(entry->name)
                                  : entry->name;
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("%1  %2").arg(label, entry->title));
        item->setToolTip(entry->summary);
        dict_list_->addItem(item);
    }
    dict_list_->blockSignals(false);

    if (!dict_results_.empty()) {
        const int row = (keep >= 0 && keep < dict_list_->count()) ? keep : 0;
        dict_list_->setCurrentRow(row);
    } else {
        ShowEntryDetail(nullptr);
    }
}

void FfmpegDictionaryWidget::OnSearchTextChanged() {
    RefreshDictionary();
}

void FfmpegDictionaryWidget::OnCategoryChanged() {
    RefreshDictionary();
}

void FfmpegDictionaryWidget::OnDictionaryCurrentChanged() {
    const int row = dict_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(dict_results_.size())) {
        ShowEntryDetail(nullptr);
        return;
    }
    ShowEntryDetail(dict_results_.at(row));
}

void FfmpegDictionaryWidget::ShowEntryDetail(const ffmpeg::FfmpegCatalogEntry* entry) {
    if (entry == nullptr) {
        dict_detail_->setHtml(
            QStringLiteral("<p style='color:%1'>没有匹配的词条。</p>")
                .arg(QLatin1String(theme::color::kTextSecondary)));
        dict_insert_button_->setEnabled(false);
        return;
    }
    dict_insert_button_->setEnabled(true);

    QString html = QStringLiteral("<h3 style='margin:2px'>%1</h3>").arg(entry->name.toHtmlEscaped());
    html += QStringLiteral("<p style='color:%2'>%1</p>")
                .arg(entry->title.toHtmlEscaped(), QLatin1String(theme::color::kAccent));
    html += QStringLiteral("<p>%1</p>").arg(entry->summary.toHtmlEscaped());
    html += QStringLiteral("<p>%1</p>").arg(entry->detail.toHtmlEscaped());
    html += QStringLiteral("<p><b>适用位置:</b> %1</p>").arg(entry->position.toHtmlEscaped());
    html += QStringLiteral("<p><b>示例:</b> <code>%1</code></p>").arg(entry->example.toHtmlEscaped());
    if (!entry->typical_values.isEmpty()) {
        html += QStringLiteral("<p><b>常见取值:</b> <code>%1</code></p>")
                    .arg(entry->typical_values.join(QStringLiteral(" / ")).toHtmlEscaped());
    }
    if (!entry->related.isEmpty()) {
        html += QStringLiteral("<p><b>相关:</b> %1</p>")
                    .arg(entry->related.join(QStringLiteral("、")).toHtmlEscaped());
    }
    if (entry->kind == ffmpeg::FfmpegEntryKind::Filter) {
        html += QStringLiteral("<p style='color:%1'>滤镜只能出现在 -vf / -af / -filter_complex 里，"
                               "「插入到命令」会自动补上对应的 -vf / -af。</p>")
                    .arg(QLatin1String(theme::color::kTextSecondary));
    }
    dict_detail_->setHtml(html);
}

void FfmpegDictionaryWidget::OnInsertEntry() {
    const int row = dict_list_->currentRow();
    if (row < 0 || row >= static_cast<int>(dict_results_.size())) {
        return;
    }
    const ffmpeg::FfmpegCatalogEntry* entry = dict_results_.at(row);
    QString snippet = entry->insert_text;
    if (entry->kind == ffmpeg::FfmpegEntryKind::Filter) {
        // 滤镜自己不能独立出现，按它所属的分类补上 -vf / -af
        const QString prefix = (entry->category == ffmpeg::FfmpegEntryCategory::Audio)
                                   ? QStringLiteral("-af ")
                                   : QStringLiteral("-vf ");
        snippet = prefix + snippet;
    }
    if (!snippet.endsWith(QLatin1Char(' '))) {
        snippet += QLatin1Char(' ');
    }
    // 命令输入框属于页面，这里只发信号，由页面决定插到哪儿。
    emit InsertRequested(snippet);
}

}  // namespace ui
}  // namespace videoeye
