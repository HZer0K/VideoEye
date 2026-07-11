#pragma once

#include <QDialog>
#include "core/exporter/MediaExporter.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QDoubleSpinBox;

namespace videoeye {
namespace ui {

// 音视频导出对话框: 选择格式 / 输出路径 / 是否重编码 / 片段区间
class MediaExportDialog : public QDialog {
    Q_OBJECT

public:
    MediaExportDialog(QWidget* parent,
                      exporter::ExportKind kind,
                      const QString& source_path,
                      qint64 duration_ms);

    exporter::ExportOptions GetOptions() const;

private slots:
    void onFormatChanged();
    void onBrowse();
    void onReencodeToggled(bool on);

private:
    void initFormats();
    QString currentExtension() const;

    exporter::ExportKind kind_;
    QString source_path_;
    qint64 duration_ms_;

    QComboBox* format_combo_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
    QCheckBox* reencode_check_ = nullptr;
    QCheckBox* no_audio_check_ = nullptr; // 仅视频导出: 不包含音频
    QComboBox* quality_combo_ = nullptr; // 视频: 高/中/低; 音频: 码率
    QCheckBox* range_check_ = nullptr;
    QDoubleSpinBox* start_spin_ = nullptr;
    QDoubleSpinBox* end_spin_ = nullptr;
};

} // namespace ui
} // namespace videoeye
