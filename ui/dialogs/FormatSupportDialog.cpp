#include "ui/dialogs/FormatSupportDialog.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {
namespace {

// CSV 单元格转义: 含逗号/引号/换行才加引号，内部引号翻倍。
// 各页导出的 CSV 都靠 Excel 打开，不做这步时"mov,mp4,m4a"这种别名列会直接串列。
QString CsvCell(const QString& text) {
    const bool need_quote = text.contains(',') || text.contains('"') ||
                            text.contains('\n') || text.contains('\r');
    if (!need_quote) return text;
    return '"' + QString(text).replace('"', "\"\"") + '"';
}

QString Mark(bool yes) { return yes ? QStringLiteral("✓") : QStringLiteral(""); }

} // namespace

FormatSupportDialog::FormatSupportDialog(QWidget* parent)
    : QDialog(parent)
    , snapshot_(CollectFfmpegSupport()) {
    setWindowTitle(tr("支持的格式与编解码器"));
    resize(960, 640);
    SetupUi();
}

void FormatSupportDialog::SetupUi() {
    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(tr("以下清单由 VideoEye 当前链接的 FFmpeg 库现场枚举得到，"
                               "不是外部 ffmpeg 命令行；换了 FFmpeg 构建，这里的内容就会变。"),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8B949E; font-size: 11px;"));
    layout->addWidget(hint);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("support_tabs"));
    tabs_->addTab(BuildOverviewTab(), tr("概览"));
    containers_table_ = MakeTable(QStringLiteral("containers_table"),
                                  {tr("格式名"), tr("说明"), tr("扩展名"), tr("MIME"), tr("读取"), tr("写入")});
    tabs_->addTab(containers_table_, tr("封装格式"));
    codecs_table_ = MakeTable(QStringLiteral("codecs_table"),
                              {tr("编解码器"), tr("说明"), tr("类型"), tr("解码"), tr("编码"),
                               tr("实现"), tr("特性")});
    tabs_->addTab(codecs_table_, tr("编解码器"));
    protocols_table_ = MakeTable(QStringLiteral("protocols_table"),
                                 {tr("协议"), tr("读"), tr("写")});
    tabs_->addTab(protocols_table_, tr("协议"));
    hwaccel_table_ = MakeTable(QStringLiteral("hwaccel_table"),
                               {tr("类型"), tr("说明"), tr("可加速解码器数")});
    tabs_->addTab(hwaccel_table_, tr("硬件加速"));
    layout->addWidget(tabs_, 1);

    FillContainers();
    FillCodecs();
    FillProtocols();
    FillHwAccels();

    // 排序要在填完表之后开: 开着排序往里塞行会让 QTableWidget 边插边排，
    // 行号与数据行不再一一对应（填表代码里所有 setItem(row,...) 都会写错行）。
    containers_table_->setSortingEnabled(true);
    codecs_table_->setSortingEnabled(true);
    protocols_table_->setSortingEnabled(true);
    hwaccel_table_->setSortingEnabled(true);

    // 搜索 / 过滤 / 导出
    auto* tools = new QHBoxLayout();
    tools->addWidget(new QLabel(tr("搜索:"), this));
    search_edit_ = new QLineEdit(this);
    search_edit_->setObjectName(QStringLiteral("support_search"));
    search_edit_->setPlaceholderText(tr("按名称 / 说明 / 扩展名过滤"));
    search_edit_->setClearButtonEnabled(true);
    tools->addWidget(search_edit_, 1);
    filter_combo_ = new QComboBox(this);
    filter_combo_->setObjectName(QStringLiteral("support_filter"));
    tools->addWidget(filter_combo_);
    auto* export_btn = new QPushButton(tr("导出 CSV"), this);
    tools->addWidget(export_btn);
    layout->addLayout(tools);

    summary_label_ = new QLabel(this);
    summary_label_->setStyleSheet(QStringLiteral("color: #8B949E; font-size: 11px;"));
    layout->addWidget(summary_label_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(tabs_, &QTabWidget::currentChanged, this, &FormatSupportDialog::OnTabChanged);
    connect(search_edit_, &QLineEdit::textChanged, this, &FormatSupportDialog::OnSearchChanged);
    connect(filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FormatSupportDialog::OnFilterChanged);
    connect(export_btn, &QPushButton::clicked, this, &FormatSupportDialog::OnExportCsv);

    RefreshFilterOptions(tabs_->currentIndex());
    UpdateSummary();
}

QWidget* FormatSupportDialog::BuildOverviewTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto add_row = [&](const QString& title, const QString& value) {
        auto* label = new QLabel(value.isEmpty() ? tr("未提供") : value, page);
        // 版本号这类内容用户多半是想复制走的，默认不可选会逼着他手打
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        form->addRow(title, label);
    };

    add_row(tr("FFmpeg 版本"), snapshot_.build.version);
    for (const auto& lib : snapshot_.build.libraries) {
        add_row(lib.first, lib.second);
    }
    add_row(tr("许可证"), snapshot_.build.license);
    add_row(tr("能力统计"),
            tr("解复用器 %1 · 复用器 %2 · 解码器 %3 · 编码器 %4 · 协议 %5 · 硬件加速 %6")
                .arg(snapshot_.DemuxerCount())
                .arg(snapshot_.MuxerCount())
                .arg(snapshot_.DecoderCount())
                .arg(snapshot_.EncoderCount())
                .arg(static_cast<int>(snapshot_.protocols.size()))
                .arg(static_cast<int>(snapshot_.hwaccels.size())));
    layout->addLayout(form);

    layout->addWidget(new QLabel(tr("构建配置 (configure):"), page));
    build_text_ = new QTextEdit(page);
    build_text_->setObjectName(QStringLiteral("support_build_config"));
    build_text_->setReadOnly(true);
    build_text_->setLineWrapMode(QTextEdit::NoWrap);
    build_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    build_text_->setPlainText(snapshot_.build.configuration.isEmpty()
                                  ? tr("本 FFmpeg 构建未提供 configure 行。")
                                  : snapshot_.build.configuration);
    layout->addWidget(build_text_, 1);

    auto* copy_btn = new QPushButton(tr("复制构建信息"), page);
    connect(copy_btn, &QPushButton::clicked, this, &FormatSupportDialog::OnCopyBuildInfo);
    layout->addWidget(copy_btn, 0, Qt::AlignLeft);
    return page;
}

QTableWidget* FormatSupportDialog::MakeTable(const QString& object_name,
                                             const QStringList& headers) {
    auto* table = new QTableWidget(0, headers.size(), this);
    table->setObjectName(object_name);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    return table;
}

void FormatSupportDialog::FillContainers() {
    TableBatch batch(containers_table_);
    batch.SetRowCount(static_cast<int>(snapshot_.containers.size()));
    int row = 0;
    for (const auto& c : snapshot_.containers) {
        batch.SetText(row, 0, c.name);
        batch.SetText(row, 1, c.long_name);
        batch.SetText(row, 2, c.extensions);
        batch.SetText(row, 3, c.mime_type);
        batch.SetText(row, 4, Mark(c.can_demux));
        batch.SetText(row, 5, Mark(c.can_mux));
        int caps = kCapNone;
        if (c.can_demux) caps |= kCapRead;
        if (c.can_mux) caps |= kCapWrite;
        if (auto* item = batch.Item(row, 0)) item->setData(Qt::UserRole, caps);
        ++row;
    }
}

void FormatSupportDialog::FillCodecs() {
    TableBatch batch(codecs_table_);
    batch.SetRowCount(static_cast<int>(snapshot_.codecs.size()));
    int row = 0;
    for (const auto& c : snapshot_.codecs) {
        QStringList features;
        if (c.intra_only) features << tr("仅帧内");
        if (c.lossless) features << tr("支持无损");
        if (c.experimental) features << tr("实验性");
        batch.SetText(row, 0, c.name);
        batch.SetText(row, 1, c.long_name);
        batch.SetText(row, 2, SupportMediaTypeName(c.type));
        batch.SetText(row, 3, Mark(c.can_decode));
        batch.SetText(row, 4, Mark(c.can_encode));
        batch.SetText(row, 5, c.wrapper.isEmpty() ? tr("内置") : c.wrapper);
        batch.SetText(row, 6, features.join(QStringLiteral(" / ")));
        int caps = kCapNone;
        if (c.can_decode) caps |= kCapRead;
        if (c.can_encode) caps |= kCapWrite;
        if (auto* item = batch.Item(row, 0)) item->setData(Qt::UserRole, caps);
        ++row;
    }
}

void FormatSupportDialog::FillProtocols() {
    TableBatch batch(protocols_table_);
    batch.SetRowCount(static_cast<int>(snapshot_.protocols.size()));
    int row = 0;
    for (const auto& p : snapshot_.protocols) {
        batch.SetText(row, 0, p.name);
        batch.SetText(row, 1, Mark(p.can_read));
        batch.SetText(row, 2, Mark(p.can_write));
        int caps = kCapNone;
        if (p.can_read) caps |= kCapRead;
        if (p.can_write) caps |= kCapWrite;
        if (auto* item = batch.Item(row, 0)) item->setData(Qt::UserRole, caps);
        ++row;
    }
}

void FormatSupportDialog::FillHwAccels() {
    TableBatch batch(hwaccel_table_);
    batch.SetRowCount(static_cast<int>(snapshot_.hwaccels.size()));
    int row = 0;
    for (const auto& h : snapshot_.hwaccels) {
        batch.SetText(row, 0, h.name);
        batch.SetText(row, 1, h.description);
        batch.SetText(row, 2, QString::number(h.decoder_count));
        // 硬件加速只有"有没有解码器支持"这一个过滤维度，借用 kCapRead 位
        if (auto* item = batch.Item(row, 0)) {
            item->setData(Qt::UserRole, h.decoder_count > 0 ? kCapRead : kCapNone);
        }
        ++row;
    }
}

QTableWidget* FormatSupportDialog::TableAt(int tab_index) const {
    switch (tab_index) {
        case kContainers: return containers_table_;
        case kCodecs: return codecs_table_;
        case kProtocols: return protocols_table_;
        case kHwAccel: return hwaccel_table_;
        default: return nullptr;
    }
}

QTableWidget* FormatSupportDialog::CurrentTable() const {
    return tabs_ ? TableAt(tabs_->currentIndex()) : nullptr;
}

void FormatSupportDialog::RefreshFilterOptions(int tab_index) {
    const bool has_table = (TableAt(tab_index) != nullptr);
    search_edit_->setEnabled(has_table);
    filter_combo_->setEnabled(has_table);
    if (!has_table) return;

    struct Option {
        QString text;
        int caps;
    };
    QList<Option> options;
    switch (tab_index) {
        case kContainers:
            options = {{tr("全部"), kCapNone}, {tr("仅能读取"), kCapRead}, {tr("仅能写入"), kCapWrite}};
            break;
        case kCodecs:
            options = {{tr("全部"), kCapNone}, {tr("仅能解码"), kCapRead}, {tr("仅能编码"), kCapWrite}};
            break;
        case kProtocols:
            options = {{tr("全部"), kCapNone}, {tr("仅可读"), kCapRead}, {tr("仅可写"), kCapWrite}};
            break;
        case kHwAccel:
            options = {{tr("全部"), kCapNone}, {tr("有解码器支持"), kCapRead}};
            break;
        default:
            options = {{tr("全部"), kCapNone}};
            break;
    }

    // 换页签会重建下拉，必须挡住信号: 否则 clear() 触发的 currentIndexChanged
    // 会在新选项还没加完时就去查 currentData()，把过滤条件读成上一次的残留值。
    const QSignalBlocker blocker(filter_combo_);
    filter_combo_->clear();
    for (const auto& opt : options) {
        filter_combo_->addItem(opt.text, opt.caps);
    }
    filter_combo_->setCurrentIndex(0);
}

void FormatSupportDialog::ApplyFilter() {
    auto* table = CurrentTable();
    if (!table) return;

    const QString keyword = search_edit_->text().trimmed();
    const int wanted_caps = filter_combo_->currentData().toInt();

    for (int row = 0; row < table->rowCount(); ++row) {
        bool match = true;
        if (wanted_caps != kCapNone) {
            auto* item = table->item(row, 0);
            const int caps = item ? item->data(Qt::UserRole).toInt() : kCapNone;
            if (!(caps & wanted_caps)) match = false;
        }
        if (match && !keyword.isEmpty()) {
            bool hit = false;
            for (int col = 0; col < table->columnCount() && !hit; ++col) {
                auto* item = table->item(row, col);
                if (item && item->text().contains(keyword, Qt::CaseInsensitive)) hit = true;
            }
            match = hit;
        }
        table->setRowHidden(row, !match);
    }
    UpdateSummary();
}

void FormatSupportDialog::UpdateSummary() {
    auto* table = CurrentTable();
    if (!table) {
        summary_label_->setText(tr("共 %1 个封装格式 · %2 个编解码器 · %3 个协议")
                                    .arg(static_cast<int>(snapshot_.containers.size()))
                                    .arg(static_cast<int>(snapshot_.codecs.size()))
                                    .arg(static_cast<int>(snapshot_.protocols.size())));
        return;
    }
    int visible = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (!table->isRowHidden(row)) ++visible;
    }
    summary_label_->setText(tr("显示 %1 / 共 %2 条").arg(visible).arg(table->rowCount()));
}

int FormatSupportDialog::VisibleRowCount() const {
    auto* table = CurrentTable();
    if (!table) return 0;
    int visible = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (!table->isRowHidden(row)) ++visible;
    }
    return visible;
}

void FormatSupportDialog::OnTabChanged(int index) {
    RefreshFilterOptions(index);
    ApplyFilter();
}

void FormatSupportDialog::OnFilterChanged() {
    ApplyFilter();
}

void FormatSupportDialog::OnSearchChanged() {
    ApplyFilter();
}

void FormatSupportDialog::OnExportCsv() {
    auto* table = CurrentTable();
    if (!table) {
        QMessageBox::information(this, tr("提示"), tr("「概览」页没有表格，请切换到其它页签再导出。"));
        return;
    }

    const char* kind = "support";
    switch (tabs_->currentIndex()) {
        case kContainers: kind = "containers"; break;
        case kCodecs: kind = "codecs"; break;
        case kProtocols: kind = "protocols"; break;
        case kHwAccel: kind = "hwaccels"; break;
        default: break;
    }
    const QString default_name =
        QString("videoeye_%1_%2.csv")
            .arg(QString::fromUtf8(kind))
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));

    ExportCsvStream(this, tr("导出能力清单 CSV"), default_name,
                    [table](QTextStream& out) {
                        QStringList header_cells;
                        for (int col = 0; col < table->columnCount(); ++col) {
                            header_cells << CsvCell(table->horizontalHeaderItem(col)
                                                        ? table->horizontalHeaderItem(col)->text()
                                                        : QString());
                        }
                        out << header_cells.join(',') << '\n';
                        // 只导**看得见**的行: 用户设了过滤再导出，拿到的东西
                        // 必须和他屏幕上看到的一致。
                        for (int row = 0; row < table->rowCount(); ++row) {
                            if (table->isRowHidden(row)) continue;
                            QStringList cells;
                            for (int col = 0; col < table->columnCount(); ++col) {
                                auto* item = table->item(row, col);
                                cells << CsvCell(item ? item->text() : QString());
                            }
                            out << cells.join(',') << '\n';
                        }
                    },
                    VisibleRowCount());
}

void FormatSupportDialog::OnCopyBuildInfo() {
    QString text = tr("FFmpeg 版本: %1\n").arg(snapshot_.build.version);
    for (const auto& lib : snapshot_.build.libraries) {
        text += QString("%1: %2\n").arg(lib.first, lib.second);
    }
    text += tr("许可证: %1\n").arg(snapshot_.build.license);
    text += tr("构建配置:\n%1\n").arg(snapshot_.build.configuration);
    if (auto* clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
}

} // namespace ui
} // namespace videoeye
