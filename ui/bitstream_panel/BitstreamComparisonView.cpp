#include "ui/bitstream_panel/BitstreamComparisonView.h"

#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>

#include "ui/bitstream_panel/bitstream_labels.h"

namespace videoeye {
namespace ui {

BitstreamComparisonView::BitstreamComparisonView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void BitstreamComparisonView::SetupUi() {
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    comparison_table_ = new QTableWidget(this);
    comparison_table_->setColumnCount(4);
    comparison_table_->setHorizontalHeaderLabels({tr("字段"), tr("容器值"), tr("码流值"), tr("判定")});
    comparison_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    comparison_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    comparison_table_->setAlternatingRowColors(true);
    if (comparison_table_->horizontalHeader()) {
        comparison_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }
    layout->addWidget(comparison_table_);
}

void BitstreamComparisonView::Clear() {
    comparison_table_->setRowCount(0);
}

void BitstreamComparisonView::SetResult(const model::BitstreamAnalysisResult& result) {
    comparison_table_->setRowCount(0);
    if (!result.analyzed)
        return;
    BuildTable(result);
}

void BitstreamComparisonView::BuildTable(const model::BitstreamAnalysisResult& result) {
    auto AddRow = [this](const QString& field, const QString& container, const QString& bitstream, bool comparable,
                         bool equal) {
        const int row = comparison_table_->rowCount();
        comparison_table_->insertRow(row);
        comparison_table_->setItem(row, 0, new QTableWidgetItem(field));
        comparison_table_->setItem(row, 1, new QTableWidgetItem(container));
        comparison_table_->setItem(row, 2, new QTableWidgetItem(bitstream));

        QTableWidgetItem* verdict = new QTableWidgetItem(comparable ? (equal ? tr("一致") : tr("不一致")) : tr("—"));
        if (comparable && !equal) {
            verdict->setForeground(QColor("#d32f2f")); // 红：不一致
        } else if (comparable) {
            verdict->setForeground(QColor("#2e7d32")); // 绿：一致
        }
        comparison_table_->setItem(row, 3, verdict);
    };

    AddRow(tr("Codec"), QString::fromStdString(result.codec_id_str), QString::fromStdString(result.codec_name), false,
           false);

    const bool has_container = result.has_container;
    const bool reliable_for_comparison = result.capability == model::AnalysisCapability::Supported;
    auto Cmp = [&](const QString& field, int container_value, int bitstream_value) {
        const bool comparable = reliable_for_comparison && has_container && container_value > 0 && bitstream_value > 0;
        AddRow(field,
               // 0 = 容器没说（如 bits_per_raw_sample 缺失），显示"—"而不是 0：
               // 否则 10bit 文件会显示"容器 0bit / 码流 10bit"这种看不懂的行
               (has_container && container_value > 0) ? QString::number(container_value) : tr("—"),
               bitstream_value > 0 ? QString::number(bitstream_value) : tr("未解析"), comparable,
               container_value == bitstream_value);
    };

    Cmp(tr("Width"), result.container_width, result.width);
    Cmp(tr("Height"), result.container_height, result.height);
    Cmp(tr("Bit Depth"), result.container_bit_depth, result.bit_depth);

    // 色彩三要素用名称展示，同时在判定列给出是否一致
    auto CmpNamed = [&](const QString& field, int container_value, int bitstream_value, const QString& container_name,
                        const QString& bitstream_name) {
        const bool comparable = reliable_for_comparison && has_container && container_value > 0 && bitstream_value > 0;
        AddRow(field, has_container ? container_name : tr("—"), bitstream_value > 0 ? bitstream_name : tr("未标注"),
               comparable, container_value == bitstream_value);
    };

    CmpNamed(tr("Color Primaries"), result.container_color_primaries, result.color_primaries,
             BitstreamPrimariesName(result.container_color_primaries), BitstreamPrimariesName(result.color_primaries));
    CmpNamed(tr("Transfer"), result.container_transfer_characteristics, result.transfer_characteristics,
             BitstreamTransferName(result.container_transfer_characteristics),
             BitstreamTransferName(result.transfer_characteristics));
    CmpNamed(tr("Matrix"), result.container_matrix_coefficients, result.matrix_coefficients,
             BitstreamMatrixName(result.container_matrix_coefficients),
             BitstreamMatrixName(result.matrix_coefficients));

    comparison_table_->resizeColumnsToContents();
}

} // namespace ui
} // namespace videoeye
