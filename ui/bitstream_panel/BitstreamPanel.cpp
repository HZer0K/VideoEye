#include "ui/bitstream_panel/BitstreamPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace videoeye {
namespace ui {

namespace {

QTreeWidgetItem* MakeNode(QTreeWidgetItem* parent, const QString& name, const QString& value) {
    QTreeWidgetItem* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    item->setText(1, value);
    return item;
}

QString ToQString(int value) {
    return QString::number(value);
}

} // namespace

BitstreamPanel::BitstreamPanel(QWidget* parent)
    : QWidget(parent),
      structure_tree_(nullptr),
      comparison_table_(nullptr),
      warning_text_(nullptr),
      refresh_btn_(nullptr),
      copy_json_btn_(nullptr) {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(6, 6, 6, 6);
    main_layout->setSpacing(6);

    // ---- 顶部工具栏 ----
    auto* toolbar = new QHBoxLayout();
    refresh_btn_ = new QPushButton(tr("刷新"), this);
    copy_json_btn_ = new QPushButton(tr("复制 JSON"), this);
    toolbar->addWidget(refresh_btn_);
    toolbar->addWidget(copy_json_btn_);
    toolbar->addStretch(1);
    main_layout->addLayout(toolbar);

    // ---- 中部：结构树 + 对比表 ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    structure_tree_ = new QTreeWidget(splitter);
    structure_tree_->setHeaderLabels({tr("项目"), tr("值")});
    structure_tree_->setRootIsDecorated(true);
    structure_tree_->setAlternatingRowColors(true);

    comparison_table_ = new QTableWidget(splitter);
    comparison_table_->setColumnCount(4);
    comparison_table_->setHorizontalHeaderLabels(
        {tr("字段"), tr("容器值"), tr("码流值"), tr("说明")});
    comparison_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    comparison_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    comparison_table_->setAlternatingRowColors(true);
    if (comparison_table_->horizontalHeader()) {
        comparison_table_->horizontalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
    }

    splitter->addWidget(structure_tree_);
    splitter->addWidget(comparison_table_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    main_layout->addWidget(splitter, 1);

    // ---- 底部：不一致警告 ----
    main_layout->addWidget(new QLabel(tr("不一致项:"), this));
    warning_text_ = new QTextEdit(this);
    warning_text_->setReadOnly(true);
    warning_text_->setFixedHeight(120);
    main_layout->addWidget(warning_text_);

    connect(refresh_btn_, &QPushButton::clicked,
            this, &BitstreamPanel::OnRefreshClicked);
    connect(copy_json_btn_, &QPushButton::clicked,
            this, &BitstreamPanel::OnCopyJsonClicked);
    connect(structure_tree_, &QTreeWidget::itemDoubleClicked,
            this, &BitstreamPanel::OnTreeWidgetItemDoubleClicked);
}

BitstreamPanel::~BitstreamPanel() = default;

void BitstreamPanel::SetAnalysisResult(const videoeye::analyzer::BitstreamAnalyzer& analyzer) {
    current_result_ = analyzer.result();

    // 用码流解析结果回填汇总字段，供对比表使用
    if (current_result_.has_h264 && current_result_.h264_sps.present) {
        current_result_.width = current_result_.h264_sps.width();
        current_result_.height = current_result_.h264_sps.height();
        current_result_.bit_depth = current_result_.h264_sps.BitDepthLuma();
        current_result_.color_primaries = current_result_.h264_sps.vui.color_primaries;
        current_result_.transfer_characteristics =
            current_result_.h264_sps.vui.transfer_characteristics;
        current_result_.matrix_coefficients =
            current_result_.h264_sps.vui.matrix_coefficients;
    } else if (current_result_.has_hevc && current_result_.hevc_sps.present) {
        current_result_.width = current_result_.hevc_sps.width();
        current_result_.height = current_result_.hevc_sps.height();
        current_result_.bit_depth = current_result_.hevc_sps.BitDepthLuma();
        current_result_.color_primaries = current_result_.hevc_sps.colour_primaries;
        current_result_.transfer_characteristics =
            current_result_.hevc_sps.transfer_characteristics;
        current_result_.matrix_coefficients = current_result_.hevc_sps.matrix_coefficients;
    } else if (current_result_.has_av1 && current_result_.av1_seq_header.present) {
        current_result_.width = current_result_.av1_seq_header.FrameWidth();
        current_result_.height = current_result_.av1_seq_header.FrameHeight();
        current_result_.bit_depth = current_result_.av1_seq_header.bit_depth_minus_8 + 8;
        current_result_.color_primaries = current_result_.av1_seq_header.color_config.color_primaries;
        current_result_.transfer_characteristics =
            current_result_.av1_seq_header.color_config.transfer_characteristics;
        current_result_.matrix_coefficients =
            current_result_.av1_seq_header.color_config.matrix_coefficients;
    }

    BuildStructureTreeView(current_result_);
    BuildComparisonTable(current_result_);
    ShowInconsistencies(current_result_.inconsistencies);
}

void BitstreamPanel::Clear() {
    current_result_ = videoeye::model::BitstreamAnalysisResult();
    structure_tree_->clear();
    comparison_table_->setRowCount(0);
    warning_text_->clear();
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
    CopyJsonToClipboard();
}

void BitstreamPanel::OnTreeWidgetItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) {
        return;
    }
    item->setExpanded(!item->isExpanded());
}

void BitstreamPanel::BuildStructureTreeView(
    const videoeye::model::BitstreamAnalysisResult& result) {
    structure_tree_->clear();

    const QString codec = QString::fromStdString(result.codec_name);
    QTreeWidgetItem* root = new QTreeWidgetItem(structure_tree_);
    root->setText(0, tr("码流 (%1)").arg(codec));

    // NAL / OBU 列表
    if (!result.nal_units.empty()) {
        QTreeWidgetItem* nal_root =
            MakeNode(root, tr("NAL 单元 (%1)").arg(result.nal_units.size()), QString());
        for (size_t i = 0; i < result.nal_units.size(); ++i) {
            const auto& nal = result.nal_units[i];
            MakeNode(nal_root,
                     tr("NAL #%1").arg(static_cast<qulonglong>(i)),
                     tr("type=%1 size=%2%3")
                         .arg(nal.type)
                         .arg(nal.size)
                         .arg(nal.is_idr ? tr(" [IDR]") : QString()));
        }
    }
    if (!result.obu_units.empty()) {
        QTreeWidgetItem* obu_root =
            MakeNode(root, tr("OBU 单元 (%1)").arg(result.obu_units.size()), QString());
        for (size_t i = 0; i < result.obu_units.size(); ++i) {
            const auto& obu = result.obu_units[i];
            MakeNode(obu_root,
                     tr("OBU #%1").arg(static_cast<qulonglong>(i)),
                     tr("type=%1 size=%2%3")
                         .arg(obu.type)
                         .arg(obu.size)
                         .arg(obu.is_sequence_header ? tr(" [SeqHeader]") : QString()));
        }
    }

    // H.264
    if (result.has_h264 && result.h264_sps.present) {
        QTreeWidgetItem* sps = MakeNode(root, tr("H.264 SPS"), QString());
        MakeNode(sps, tr("Profile"),
                 QString::fromStdString(result.h264_sps.ProfileName()));
        MakeNode(sps, tr("Level"),
                 QString::fromStdString(result.h264_sps.LevelVersion()));
        MakeNode(sps, tr("分辨率"),
                 tr("%1x%2").arg(result.h264_sps.width()).arg(result.h264_sps.height()));
        MakeNode(sps, tr("Chroma Format"), ToQString(result.h264_sps.chroma_format_idc));
        MakeNode(sps, tr("位深 (亮度)"), ToQString(result.h264_sps.BitDepthLuma()));
        MakeNode(sps, tr("位深 (色度)"), ToQString(result.h264_sps.BitDepthChroma()));
        MakeNode(sps, tr("最大参考帧"), ToQString(result.h264_sps.max_num_ref_frames));
    }

    // H.265
    if (result.has_hevc) {
        if (result.hevc_sps.present) {
            QTreeWidgetItem* sps = MakeNode(root, tr("HEVC SPS"), QString());
            MakeNode(sps, tr("Profile"),
                     QString::fromStdString(result.hevc_sps.ProfileName()));
            MakeNode(sps, tr("分辨率"),
                     tr("%1x%2").arg(result.hevc_sps.width()).arg(result.hevc_sps.height()));
            MakeNode(sps, tr("Chroma Format"), ToQString(result.hevc_sps.chroma_format_idc));
            MakeNode(sps, tr("位深 (亮度)"), ToQString(result.hevc_sps.BitDepthLuma()));
            MakeNode(sps, tr("位深 (色度)"), ToQString(result.hevc_sps.BitDepthChroma()));
        }
        if (result.hevc_vps.present) {
            QTreeWidgetItem* vps = MakeNode(root, tr("HEVC VPS"), QString());
            MakeNode(vps, tr("Profile IDC"), ToQString(result.hevc_vps.general_profile_idc));
            MakeNode(vps, tr("Level IDC"), ToQString(result.hevc_vps.general_level_idc));
        }
    }

    // AV1
    if (result.has_av1 && result.av1_seq_header.present) {
        QTreeWidgetItem* seq = MakeNode(root, tr("AV1 Sequence Header"), QString());
        MakeNode(seq, tr("Profile"), ToQString(result.av1_seq_header.profile));
        MakeNode(seq, tr("分辨率"),
                 tr("%1x%2")
                     .arg(result.av1_seq_header.FrameWidth())
                     .arg(result.av1_seq_header.FrameHeight()));
        MakeNode(seq, tr("位深"), ToQString(result.av1_seq_header.bit_depth_minus_8 + 8));
    }

    // VVC
    if (result.has_vvc && result.vvc_sps.present) {
        QTreeWidgetItem* sps = MakeNode(root, tr("VVC SPS"), QString());
        MakeNode(sps, tr("Profile IDC"), ToQString(result.vvc_sps.general_profile_idc));
        MakeNode(sps, tr("位深 (亮度)"), ToQString(result.vvc_sps.BitDepthLuma()));
        MakeNode(sps, tr("位深 (色度)"), ToQString(result.vvc_sps.BitDepthChroma()));
    }

    root->setExpanded(true);
    structure_tree_->resizeColumnToContents(0);
}

void BitstreamPanel::BuildComparisonTable(
    const videoeye::model::BitstreamAnalysisResult& result) {
    comparison_table_->setRowCount(0);

    auto AddRow = [this](const QString& field, const QString& container,
                         const QString& bitstream, const QString& note) {
        int row = comparison_table_->rowCount();
        comparison_table_->insertRow(row);
        comparison_table_->setItem(row, 0, new QTableWidgetItem(field));
        comparison_table_->setItem(row, 1, new QTableWidgetItem(container));
        comparison_table_->setItem(row, 2, new QTableWidgetItem(bitstream));
        comparison_table_->setItem(row, 3, new QTableWidgetItem(note));
    };

    AddRow(tr("Codec"),
           QString::fromStdString(result.codec_id_str),
           QString::fromStdString(result.codec_name),
           QString());
    AddRow(tr("Width"), QString(), ToQString(result.width), QString());
    AddRow(tr("Height"), QString(), ToQString(result.height), QString());
    AddRow(tr("Bit Depth"), QString(), ToQString(result.bit_depth), QString());
    AddRow(tr("Color Primaries"), QString(), ToQString(result.color_primaries), QString());
    AddRow(tr("Transfer Characteristics"), QString(),
           ToQString(result.transfer_characteristics), QString());
    AddRow(tr("Matrix Coefficients"), QString(),
           ToQString(result.matrix_coefficients), QString());

    comparison_table_->resizeColumnsToContents();
}

void BitstreamPanel::ShowInconsistencies(
    const std::vector<videoeye::model::BitstreamAnalysisResult::Inconsistency>&
        inconsistencies) {
    if (inconsistencies.empty()) {
        warning_text_->setPlainText(tr("未发现容器与码流之间的不一致。"));
        return;
    }

    QString text;
    for (const auto& item : inconsistencies) {
        text += tr("[%1] %2\n")
                    .arg(QString::fromStdString(item.severity),
                         QString::fromStdString(item.field));
        text += tr("  容器值: %1\n").arg(QString::fromStdString(item.container_value));
        text += tr("  码流值: %1\n").arg(QString::fromStdString(item.bitstream_value));
        text += tr("  说明: %1\n").arg(QString::fromStdString(item.description));
        if (!item.suggestion.empty()) {
            text += tr("  建议: %1\n").arg(QString::fromStdString(item.suggestion));
        }
        text += "\n";
    }
    warning_text_->setPlainText(text);
}

QString BitstreamPanel::GetFieldName(const std::string& field_name) const {
    return QString::fromStdString(field_name);
}

QIcon BitstreamPanel::GetCodecIcon(const std::string& /*codec_name*/) const {
    // 尚未提供按 codec 区分的图标资源，返回空图标。
    return QIcon();
}

} // namespace ui
} // namespace videoeye
