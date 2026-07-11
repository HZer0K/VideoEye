#include "MediaExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace videoeye {
namespace ui {

namespace {
struct Fmt {
    QString label;
    QString ext;
};
const QList<Fmt> kVideoFormats = {
    {QStringLiteral("MP4 (H.264 + AAC)"), "mp4"},
    {QStringLiteral("MKV (H.264 + AAC)"), "mkv"},
    {QStringLiteral("MOV (H.264 + AAC)"), "mov"},
    {QStringLiteral("AVI (H.264 + MP3)"),  "avi"},
    {QStringLiteral("WebM (VP9 + Opus)"),  "webm"},
    {QStringLiteral("TS (H.264 + AAC)"),   "ts"},
};
const QList<Fmt> kAudioFormats = {
    {QStringLiteral("MP3 (libmp3lame)"), "mp3"},
    {QStringLiteral("WAV (PCM)"),        "wav"},
    {QStringLiteral("M4A (AAC)"),        "m4a"},
    {QStringLiteral("FLAC"),             "flac"},
    {QStringLiteral("OGG (Opus)"),       "ogg"},
};
} // namespace

MediaExportDialog::MediaExportDialog(QWidget* parent,
                                     exporter::ExportKind kind,
                                     const QString& source_path,
                                     qint64 duration_ms)
    : QDialog(parent)
    , kind_(kind)
    , source_path_(source_path)
    , duration_ms_(duration_ms) {
    setWindowTitle(kind == exporter::ExportKind::Video ? tr("导出视频") : tr("导出音频"));
    setMinimumWidth(460);

    auto* vlay = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // 格式
    format_combo_ = new QComboBox(this);
    initFormats();
    connect(format_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MediaExportDialog::onFormatChanged);
    form->addRow(tr("导出格式"), format_combo_);

    // 输出路径
    auto* out_row = new QHBoxLayout();
    output_edit_ = new QLineEdit(this);
    browse_btn_ = new QPushButton(tr("浏览..."), this);
    connect(browse_btn_, &QPushButton::clicked, this, &MediaExportDialog::onBrowse);
    out_row->addWidget(output_edit_);
    out_row->addWidget(browse_btn_);
    form->addRow(tr("输出文件"), out_row);

    // 重新编码
    reencode_check_ = new QCheckBox(tr("重新编码 (否则尽量无损复制)"), this);
    reencode_check_->setChecked(false);
    connect(reencode_check_, &QCheckBox::toggled, this, &MediaExportDialog::onReencodeToggled);
    form->addRow(tr("编码"), reencode_check_);

    // 视频: 是否排除音轨
    if (kind == exporter::ExportKind::Video) {
        no_audio_check_ = new QCheckBox(tr("不包含音频 (仅导出视频画面)"), this);
        no_audio_check_->setChecked(false);
        form->addRow(tr("音频"), no_audio_check_);
    }

    // 画质 / 码率
    quality_combo_ = new QComboBox(this);
    if (kind == exporter::ExportKind::Video) {
        quality_combo_->addItems({tr("高"), tr("中"), tr("低")});
        quality_combo_->setCurrentIndex(1);
    } else {
        quality_combo_->addItems({tr("128 kbps"), tr("192 kbps"), tr("256 kbps"), tr("320 kbps")});
        quality_combo_->setCurrentIndex(1);
    }
    quality_combo_->setEnabled(false);
    form->addRow(tr("质量/码率"), quality_combo_);

    // 片段区间
    range_check_ = new QCheckBox(tr("仅导出指定片段"), this);
    range_check_->setChecked(false);
    connect(range_check_, &QCheckBox::toggled, [this](bool on) {
        start_spin_->setEnabled(on);
        end_spin_->setEnabled(on);
    });
    form->addRow(tr("区间"), range_check_);

    auto* range_row = new QHBoxLayout();
    start_spin_ = new QDoubleSpinBox(this);
    end_spin_ = new QDoubleSpinBox(this);
    const double dur_sec = duration_ms_ > 0 ? duration_ms_ / 1000.0 : 0.0;
    start_spin_->setRange(0.0, dur_sec);
    start_spin_->setValue(0.0);
    end_spin_->setRange(0.0, dur_sec);
    end_spin_->setValue(dur_sec);
    start_spin_->setSuffix(tr(" s"));
    end_spin_->setSuffix(tr(" s"));
    start_spin_->setEnabled(false);
    end_spin_->setEnabled(false);
    range_row->addWidget(new QLabel(tr("起:")));
    range_row->addWidget(start_spin_);
    range_row->addWidget(new QLabel(tr("止:")));
    range_row->addWidget(end_spin_);
    form->addRow(QString(), range_row);

    vlay->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("导出"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    vlay->addWidget(buttons);

    onFormatChanged(); // 初始化默认输出文件名
}

void MediaExportDialog::initFormats() {
    const auto& fmts = (kind_ == exporter::ExportKind::Video) ? kVideoFormats : kAudioFormats;
    for (const auto& f : fmts) {
        format_combo_->addItem(f.label, f.ext);
    }
}

QString MediaExportDialog::currentExtension() const {
    const int idx = format_combo_->currentIndex();
    const QVariant v = format_combo_->itemData(idx);
    return v.toString();
}

void MediaExportDialog::onFormatChanged() {
    const QString ext = currentExtension();
    if (ext.isEmpty()) return;
    QFileInfo fi(source_path_);
    const QString base = fi.completeBaseName().isEmpty() ? "media" : fi.completeBaseName();
    const QString dir = fi.absolutePath();
    // 仅当用户尚未手动修改输出路径时才自动更新
    const QString suggested = dir + "/" + base + "_export." + ext;
    output_edit_->setText(suggested);
}

void MediaExportDialog::onBrowse() {
    const QString ext = currentExtension();
    const QString filter = QString("*.%1").arg(ext);
    QString path = QFileDialog::getSaveFileName(this, tr("选择输出文件"),
                                                output_edit_->text(), filter);
    if (!path.isEmpty()) output_edit_->setText(path);
}

void MediaExportDialog::onReencodeToggled(bool on) {
    quality_combo_->setEnabled(on);
}

exporter::ExportOptions MediaExportDialog::GetOptions() const {
    exporter::ExportOptions opt;
    opt.input_path = source_path_;
    opt.kind = kind_;

    QString out = output_edit_->text().trimmed();
    const QString ext = currentExtension();
    // 补全扩展名
    if (!ext.isEmpty()) {
        QFileInfo fi(out);
        if (fi.suffix().toLower() != ext) {
            if (!out.isEmpty() && !out.endsWith('.')) out += ".";
            out += ext;
        }
    }
    opt.output_path = out;
    opt.format = ext;
    opt.reencode = reencode_check_->isChecked();
    opt.no_audio = no_audio_check_ ? no_audio_check_->isChecked() : false;

    if (kind_ == exporter::ExportKind::Video) {
        opt.videoQuality = quality_combo_->currentIndex(); // 0=高 1=中 2=低
        opt.audioBitrateKbps = 192;
    } else {
        // 码率下拉: 0=128 1=192 2=256 3=320
        const int rates[] = {128, 192, 256, 320};
        const int i = qBound(0, quality_combo_->currentIndex(), 3);
        opt.audioBitrateKbps = rates[i];
        opt.videoQuality = 1;
    }

    if (range_check_->isChecked()) {
        opt.start_ms = static_cast<qint64>(start_spin_->value() * 1000.0);
        opt.end_ms = static_cast<qint64>(end_spin_->value() * 1000.0);
    } else {
        opt.start_ms = -1;
        opt.end_ms = -1;
    }
    return opt;
}

} // namespace ui
} // namespace videoeye
