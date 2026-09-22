#include "ui/bitstream_panel/BitstreamPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSplitter>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <string>

namespace videoeye {
namespace ui {
namespace {

// 数值 -> 文本；0 一律显示成「未标注」，避免把「没写」误读成「值为 0」
QString Num(int value) {
    return value > 0 ? QString::number(value) : QObject::tr("未标注");
}

QString FlagText(bool value) {
    return value ? QObject::tr("是") : QObject::tr("否");
}

QTreeWidgetItem* MakeNode(QTreeWidgetItem* parent, const QString& name, const QString& value) {
    QTreeWidgetItem* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    item->setText(1, value);
    return item;
}

}  // namespace

BitstreamPanel::BitstreamPanel(QWidget* parent)
    : QWidget(parent) {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(8, 8, 8, 8);
    main_layout->setSpacing(6);

    // ---- 顶部：摘要 + 工具栏 ----
    summary_label_ = new QLabel(tr("暂无码流解析结果。"), this);
    summary_label_->setWordWrap(true);
    summary_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    main_layout->addWidget(summary_label_);

    auto* toolbar = new QHBoxLayout();
    refresh_button_ = new QPushButton(tr("重新扫描"), this);
    copy_json_button_ = new QPushButton(tr("复制 JSON"), this);
    export_json_button_ = new QPushButton(tr("导出 JSON"), this);
    toolbar->addWidget(refresh_button_);
    toolbar->addWidget(copy_json_button_);
    toolbar->addWidget(export_json_button_);
    toolbar->addStretch(1);
    main_layout->addLayout(toolbar);

    // ---- 中部：结构树 + 对比表 ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    structure_tree_ = new QTreeWidget(splitter);
    structure_tree_->setHeaderLabels({tr("参数集"), tr("值")});
    structure_tree_->setRootIsDecorated(true);
    structure_tree_->setAlternatingRowColors(true);

    comparison_table_ = new QTableWidget(splitter);
    comparison_table_->setColumnCount(4);
    comparison_table_->setHorizontalHeaderLabels(
        {tr("字段"), tr("容器值"), tr("码流值"), tr("判定")});
    comparison_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    comparison_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    comparison_table_->setAlternatingRowColors(true);
    if (comparison_table_->horizontalHeader()) {
        comparison_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }

    splitter->addWidget(structure_tree_);
    splitter->addWidget(comparison_table_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setMinimumHeight(320);
    main_layout->addWidget(splitter, 1);

    // ---- 底部：不一致项 ----
    main_layout->addWidget(new QLabel(tr("容器与码流不一致项:"), this));
    inconsistency_table_ = new QTableWidget(this);
    inconsistency_table_->setColumnCount(5);
    inconsistency_table_->setHorizontalHeaderLabels(
        {tr("级别"), tr("字段"), tr("容器值"), tr("码流值"), tr("说明")});
    inconsistency_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    inconsistency_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    inconsistency_table_->setAlternatingRowColors(true);
    inconsistency_table_->setMinimumHeight(120);
    if (inconsistency_table_->horizontalHeader()) {
        inconsistency_table_->horizontalHeader()->setSectionResizeMode(
            4, QHeaderView::Stretch);
    }
    main_layout->addWidget(inconsistency_table_);

    connect(refresh_button_, &QPushButton::clicked, this, &BitstreamPanel::OnRefreshClicked);
    connect(copy_json_button_, &QPushButton::clicked, this, &BitstreamPanel::OnCopyJsonClicked);
    connect(export_json_button_, &QPushButton::clicked, this, &BitstreamPanel::OnExportJsonClicked);
}

BitstreamPanel::~BitstreamPanel() = default;

// --------------------------------------------------------------------------
// 数据入口
// --------------------------------------------------------------------------
void BitstreamPanel::SetResult(const model::BitstreamAnalysisResult& result) {
    current_result_ = result;
    has_result_ = result.analyzed;

    UpdateSummary(result);
    BuildStructureTree(result);
    BuildComparisonTable(result);
    BuildInconsistencyTable(result);
}

void BitstreamPanel::Clear() {
    current_result_ = model::BitstreamAnalysisResult();
    has_result_ = false;
    summary_label_->setText(tr("暂无码流解析结果。"));
    structure_tree_->clear();
    comparison_table_->setRowCount(0);
    inconsistency_table_->setRowCount(0);
}

QString BitstreamPanel::ExportToJson() const {
    return QString::fromStdString(current_result_.ToJson());
}

void BitstreamPanel::CopyJsonToClipboard() const {
    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(ExportToJson());
    }
}

void BitstreamPanel::OnRefreshClicked() { emit RefreshRequested(); }

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
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出码流解析结果"), "bitstream.json", tr("JSON 文件 (*.json)"));
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入文件:\n%1").arg(filename));
        return;
    }
    file.write(ExportToJson().toUtf8());
}

// --------------------------------------------------------------------------
// 摘要
// --------------------------------------------------------------------------
QString BitstreamPanel::LevelName(const model::BitstreamAnalysisResult& result) {
    if (result.has_h264 && result.h264_sps.present) {
        return QString::fromStdString(result.h264_sps.LevelVersion());
    }
    if (result.has_hevc && result.hevc_sps.present) {
        // HEVC: general_level_idc = 30 * major + 3 * minor
        const int idc = static_cast<int>(result.hevc_sps.general_level_idc);
        return QString("%1.%2").arg(idc / 30).arg((idc % 30) / 3);
    }
    if (result.has_av1 && result.av1_seq_header.present) {
        return QString::fromStdString(result.av1_seq_header.LevelString());
    }
    if (result.has_vvc) {
        // VVC：优先 SPS 自带的 PTL；SPS 没带 PTL 时退到 vvcC 配置记录。
        // general_level_idc = major*16 + minor*3（H.266 A.4），换算在 LevelString() 里。
        if (result.vvc_sps.present && result.vvc_sps.general_level_idc > 0) {
            return QString::fromStdString(result.vvc_sps.LevelString());
        }
        if (result.vvc_vps.present && result.vvc_vps.general_level_idc > 0) {
            return QString::fromStdString(result.vvc_vps.LevelString());
        }
        if (result.vvc_config.present && result.vvc_config.general_level_idc > 0) {
            return QString::fromStdString(result.vvc_config.LevelString());
        }
        return tr("未知");
    }
    return tr("未知");
}

QString BitstreamPanel::DescribeCodec(const model::BitstreamAnalysisResult& result) {
    if (!result.analyzed) return tr("未支持/未解析");

    QString profile = tr("未知");
    QString chroma = tr("未知");
    if (result.has_h264 && result.h264_sps.present) {
        profile = QString::fromStdString(result.h264_sps.ProfileName());
        chroma = ChromaName(result.h264_sps.chroma_format_idc);
    } else if (result.has_hevc && result.hevc_sps.present) {
        profile = QString::fromStdString(result.hevc_sps.ProfileName());
        chroma = ChromaName(result.hevc_sps.chroma_format_idc);
    } else if (result.has_av1 && result.av1_seq_header.present) {
        profile = QString("Profile %1").arg(result.av1_seq_header.profile);
        chroma = QString::fromStdString(result.av1_seq_header.ChromaFormatString());
    } else if (result.has_av1 && result.has_av1_config) {
        profile = QString("Profile %1 (av1C)").arg(result.av1_config.profile);
    } else if (result.has_vvc && result.vvc_sps.present) {
        profile = QString::fromStdString(result.vvc_sps.ProfileName());
        chroma = ChromaName(result.vvc_sps.sps_chroma_format_idc);
    } else if (result.has_vvc && result.has_vvc_config) {
        profile = QString::fromStdString(result.vvc_config.ProfileName()) + tr(" (vvcC)");
        chroma = ChromaName(result.vvc_config.chroma_format_idc);
    }

    return tr("%1 · %2 · Level %3 · %4 · %5bit")
        .arg(QString::fromStdString(result.codec_name), profile, LevelName(result), chroma)
        .arg(result.bit_depth);
}

void BitstreamPanel::UpdateSummary(const model::BitstreamAnalysisResult& result) {
    if (!result.analyzed) {
        summary_label_->setText(
            tr("没有可用的参数集：该视频流没有 extradata，或编码格式暂不支持"
               "（当前支持 H.264 / HEVC / AV1 / VVC）。"));
        return;
    }

    const int warning_count = [&result] {
        int n = 0;
        for (const auto& item : result.inconsistencies) {
            if (item.severity != "info") ++n;
        }
        return n;
    }();

    QString resolution = tr("未知");
    if (result.width > 0 && result.height > 0) {
        resolution = QString("%1x%2").arg(result.width).arg(result.height);
    }

    summary_label_->setText(
        tr("<b>%1</b>　流 #%2　%3　参数集: NAL %4 / OBU %5　不一致: %6")
            .arg(DescribeCodec(result))
            .arg(result.stream_index)
            .arg(resolution)
            .arg(result.nal_units.size())
            .arg(result.obu_units.size())
            .arg(warning_count));
}

// --------------------------------------------------------------------------
// 结构树
// --------------------------------------------------------------------------
QString BitstreamPanel::ChromaName(int chroma_format_idc) {
    switch (chroma_format_idc) {
        case 0: return "4:0:0";
        case 1: return "4:2:0";
        case 2: return "4:2:2";
        case 3: return "4:4:4";
        default: return QObject::tr("未知");
    }
}

QString BitstreamPanel::PrimariesName(int value) {
    switch (value) {
        case 1: return "BT.709";
        case 4: return "BT.470M";
        case 5: return "BT.470BG";
        case 6: return "BT.601";
        case 7: return "SMPTE 240M";
        case 8: return "Generic film";
        case 9: return "BT.2020";
        case 10: return "SMPTE 428";
        case 11: return "SMPTE 431 (DCI-P3)";
        case 12: return "SMPTE 432 (Display P3)";
        case 22: return "EBU 3213";
        default: return QObject::tr("未标注");
    }
}

QString BitstreamPanel::TransferName(int value) {
    switch (value) {
        case 1: return "BT.709";
        case 4: return "Gamma 2.2";
        case 5: return "Gamma 2.8";
        case 6: return "BT.601";
        case 7: return "SMPTE 240M";
        case 8: return "Linear";
        case 9: return "Log";
        case 10: return "Log Sqrt";
        case 11: return "IEC 61966-2-4";
        case 12: return "BT.1361";
        case 13: return "sRGB/sYCC";
        case 14: return "BT.2020 (10bit)";
        case 15: return "BT.2020 (12bit)";
        case 16: return "SMPTE 2084 (PQ)";
        case 17: return "SMPTE 428";
        case 18: return "HLG (ARIB B67)";
        default: return QObject::tr("未标注");
    }
}

QString BitstreamPanel::MatrixName(int value) {
    switch (value) {
        case 0: return "GBR";
        case 1: return "BT.709";
        case 4: return "FCC";
        case 5: return "BT.470BG";
        case 6: return "BT.601";
        case 7: return "SMPTE 240M";
        case 8: return "YCgCo";
        case 9: return "BT.2020 NCL";
        case 10: return "BT.2020 CL";
        case 11: return "SMPTE 2085";
        case 12: return "Chroma-derived NCL";
        case 13: return "Chroma-derived CL";
        case 14: return "ICtCp";
        default: return QObject::tr("未标注");
    }
}

void BitstreamPanel::BuildStructureTree(const model::BitstreamAnalysisResult& result) {
    structure_tree_->clear();
    if (!result.analyzed) return;

    QTreeWidgetItem* root = new QTreeWidgetItem(structure_tree_);
    root->setText(0, tr("参数集 (%1)").arg(QString::fromStdString(result.codec_name)));
    root->setText(1, QString::fromStdString(result.codec_id_str));

    if (!result.nal_units.empty()) {
        QTreeWidgetItem* nal_root =
            MakeNode(root, tr("NAL 单元 (%1)").arg(result.nal_units.size()), QString());
        for (size_t i = 0; i < result.nal_units.size(); ++i) {
            const auto& nal = result.nal_units[i];
            MakeNode(nal_root, tr("NAL #%1").arg(static_cast<qulonglong>(i)),
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
            MakeNode(obu_root, tr("OBU #%1").arg(static_cast<qulonglong>(i)),
                     tr("type=%1 size=%2%3")
                         .arg(obu.type)
                         .arg(obu.size)
                         .arg(obu.is_sequence_header ? tr(" [SeqHeader]") : QString()));
        }
    }

    // ---- H.264 ----
    if (result.has_h264) {
        if (result.h264_sps.present) {
            const model::H264SpsInfo& sps = result.h264_sps;
            QTreeWidgetItem* node = MakeNode(root, tr("H.264 SPS"), QString());
            MakeNode(node, tr("Profile"), QString::fromStdString(sps.ProfileName()));
            MakeNode(node, tr("Level"), QString::fromStdString(sps.LevelVersion()));
            MakeNode(node, tr("SPS ID"), QString::number(sps.seq_parameter_set_id));
            MakeNode(node, tr("分辨率"),
                     tr("%1x%2").arg(sps.width()).arg(sps.height()));
            MakeNode(node, tr("编码尺寸 (MB)"),
                     tr("%1x%2 MB")
                         .arg(sps.pic_width_in_mbs_minus1 + 1)
                         .arg(sps.pic_height_in_mbs_minus1 + 1));
            MakeNode(node, tr("Chroma Format"), ChromaName(sps.chroma_format_idc));
            MakeNode(node, tr("位深 亮度/色度"),
                     tr("%1 / %2 bit").arg(sps.BitDepthLuma()).arg(sps.BitDepthChroma()));
            MakeNode(node, tr("最大参考帧"), QString::number(sps.max_num_ref_frames));
            MakeNode(node, tr("POC 类型"), QString::number(sps.pic_order_cnt_type));
            MakeNode(node, tr("log2_max_frame_num"),
                     QString::number(sps.log2_max_frame_num_minus4 + 4));
            MakeNode(node, tr("frame_mbs_only"), FlagText(sps.frame_mbs_only_flag));
            if (sps.frame_cropping_flag) {
                MakeNode(node, tr("裁剪 (L/R/T/B)"),
                         tr("%1 / %2 / %3 / %4")
                             .arg(sps.frame_crop_left_offset)
                             .arg(sps.frame_crop_right_offset)
                             .arg(sps.frame_crop_top_offset)
                             .arg(sps.frame_crop_bottom_offset));
            }
            if (sps.vui.present) {
                const model::H264VuiInfo& vui = sps.vui;
                QTreeWidgetItem* vui_node = MakeNode(node, tr("VUI"), QString());
                if (vui.timing_info_present) {
                    MakeNode(vui_node, tr("帧率 (time_scale/tick)"),
                             tr("%1 / %2").arg(vui.time_scale).arg(vui.num_units_in_tick));
                }
                if (vui.video_signal_type_present) {
                    MakeNode(vui_node, tr("full_range"), FlagText(vui.video_full_range_flag));
                }
                if (vui.colour_description_present) {
                    MakeNode(vui_node, tr("Color Primaries"), PrimariesName(vui.color_primaries));
                    MakeNode(vui_node, tr("Transfer"), TransferName(vui.transfer_characteristics));
                    MakeNode(vui_node, tr("Matrix"), MatrixName(vui.matrix_coefficients));
                }
            }
        } else {
            MakeNode(root, tr("H.264 SPS"), tr("未找到"));
        }
        if (result.h264_pps.present) {
            const model::H264PpsInfo& pps = result.h264_pps;
            QTreeWidgetItem* node = MakeNode(root, tr("H.264 PPS"), QString());
            MakeNode(node, tr("PPS ID"), QString::number(pps.pic_parameter_set_id));
            MakeNode(node, tr("初始 QP"), QString::number(pps.pic_init_qp_minus26 + 26));
            MakeNode(node, tr("加权预测"), FlagText(pps.weighted_pred_flag));
            MakeNode(node, tr("8x8 变换"), FlagText(pps.transform_8x8_mode_flag));
            MakeNode(node, tr("去块滤波控制存在"),
                     FlagText(pps.deblocking_filter_control_present_flag));
        }
    }

    // ---- HEVC ----
    if (result.has_hevc) {
        if (result.hevc_vps.present) {
            const model::HevcVpsInfo& vps = result.hevc_vps;
            QTreeWidgetItem* node = MakeNode(root, tr("HEVC VPS"), QString());
            MakeNode(node, tr("Profile IDC"), QString::number(vps.general_profile_idc));
            MakeNode(node, tr("Tier"),
                     vps.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"),
                     QString("%1.%2")
                         .arg(vps.general_level_idc / 30)
                         .arg((vps.general_level_idc % 30) / 3));
            MakeNode(node, tr("层 / 子层"),
                     tr("%1 / %2").arg(vps.vps_max_layers).arg(vps.vps_max_sub_layers));
        }
        if (result.hevc_sps.present) {
            const model::HevcSpsInfo& sps = result.hevc_sps;
            QTreeWidgetItem* node = MakeNode(root, tr("HEVC SPS"), QString());
            MakeNode(node, tr("Profile"), QString::fromStdString(sps.ProfileName()));
            MakeNode(node, tr("Tier"), sps.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"),
                     QString("%1.%2")
                         .arg(sps.general_level_idc / 30)
                         .arg((sps.general_level_idc % 30) / 3));
            MakeNode(node, tr("分辨率"), tr("%1x%2").arg(sps.width()).arg(sps.height()));
            MakeNode(node, tr("编码尺寸"),
                     tr("%1x%2")
                         .arg(sps.pic_width_in_luma_samples)
                         .arg(sps.pic_height_in_luma_samples));
            MakeNode(node, tr("Chroma Format"), ChromaName(sps.chroma_format_idc));
            MakeNode(node, tr("位深 亮度/色度"),
                     tr("%1 / %2 bit").arg(sps.BitDepthLuma()).arg(sps.BitDepthChroma()));
            MakeNode(node, tr("CTU 尺寸"),
                     QString::number(1 << (sps.log2_min_luma_coding_block_size_minus3 + 3 +
                                           sps.log2_diff_max_min_luma_coding_block_size)));
            MakeNode(node, tr("SAO"), FlagText(sps.sample_adaptive_offset_enabled_flag != 0));
            MakeNode(node, tr("短时参考帧集数"),
                     QString::number(sps.num_short_term_ref_pic_sets));
            if (sps.conformance_window_flag) {
                MakeNode(node, tr("Conformance 窗口 (L/R/T/B)"),
                         tr("%1 / %2 / %3 / %4")
                             .arg(sps.conf_win_left_offset)
                             .arg(sps.conf_win_right_offset)
                             .arg(sps.conf_win_top_offset)
                             .arg(sps.conf_win_bottom_offset));
            }
        } else {
            MakeNode(root, tr("HEVC SPS"), tr("未找到"));
        }
        if (result.hevc_pps.present) {
            const model::HevcPpsInfo& pps = result.hevc_pps;
            QTreeWidgetItem* node = MakeNode(root, tr("HEVC PPS"), QString());
            MakeNode(node, tr("PPS ID"), QString::number(pps.pic_parameter_set_id));
            MakeNode(node, tr("初始 QP"), QString::number(pps.init_qp_minus26 + 26));
            MakeNode(node, tr("tiles"),
                     pps.single_tile_in_pic_flag ? tr("单 tile")
                                                 : tr("%1 x %2")
                                                       .arg(pps.num_tile_columns_minus1 + 1)
                                                       .arg(pps.num_tile_rows_minus1 + 1));
        }
    }

    // ---- AV1 ----
    if (result.has_av1) {
        if (result.av1_seq_header.present) {
            const model::Av1SequenceHeaderInfo& sh = result.av1_seq_header;
            QTreeWidgetItem* node = MakeNode(root, tr("AV1 Sequence Header"), QString());
            MakeNode(node, tr("Profile"), QString::number(sh.profile));
            MakeNode(node, tr("Level"), QString::fromStdString(sh.LevelString()));
            MakeNode(node, tr("Tier"), sh.tier ? tr("High") : tr("Main"));
            MakeNode(node, tr("分辨率"),
                     tr("%1x%2").arg(sh.FrameWidth()).arg(sh.FrameHeight()));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(sh.BitDepth()));
            MakeNode(node, tr("Chroma Format"),
                     QString::fromStdString(sh.ChromaFormatString()));
            MakeNode(node, tr("Still picture"), FlagText(sh.still_picture));
            MakeNode(node, tr("128x128 超级块"), FlagText(sh.use_128x128_superblock));
            MakeNode(node, tr("CDEF"), FlagText(sh.enable_cdef));
            MakeNode(node, tr("Restoration"), FlagText(sh.enable_restoration));
            MakeNode(node, tr("Film grain"), FlagText(sh.film_grain_params_present));
            if (sh.color_config.present) {
                const model::Av1ColorConfigInfo& cc = sh.color_config;
                QTreeWidgetItem* cc_node = MakeNode(node, tr("color_config"), QString());
                MakeNode(cc_node, tr("high_bitdepth"), FlagText(cc.high_bitdepth));
                MakeNode(cc_node, tr("twelve_bit"), FlagText(cc.twelve_bit));
                MakeNode(cc_node, tr("monochrome"), FlagText(cc.monochrome));
                MakeNode(cc_node, tr("subsampling x/y"),
                         tr("%1 / %2").arg(cc.subsampling_x).arg(cc.subsampling_y));
                if (cc.color_description_present_flag) {
                    MakeNode(cc_node, tr("Color Primaries"), PrimariesName(cc.color_primaries));
                    MakeNode(cc_node, tr("Transfer"), TransferName(cc.transfer_characteristics));
                    MakeNode(cc_node, tr("Matrix"), MatrixName(cc.matrix_coefficients));
                    MakeNode(cc_node, tr("full_range"), FlagText(cc.full_range_flag != 0));
                } else {
                    MakeNode(cc_node, tr("color_description"), tr("未出现（色彩标注缺失）"));
                }
            }
        } else {
            MakeNode(root, tr("AV1 Sequence Header"),
                     tr("未找到（MP4/WebM 的 av1C 里通常不含序列头 OBU）"));
        }
        if (result.has_av1_config && result.av1_config.present) {
            const model::Av1CodecConfigInfo& cfg = result.av1_config;
            QTreeWidgetItem* node = MakeNode(root, tr("AV1 av1C 配置记录"), QString());
            MakeNode(node, tr("Profile"), QString::number(cfg.profile));
            MakeNode(node, tr("Level"), QString::number(cfg.level));
            MakeNode(node, tr("Tier"), cfg.tier ? tr("High") : tr("Main"));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(cfg.bit_depth));
            MakeNode(node, tr("monochrome"), FlagText(cfg.monochrome));
            MakeNode(node, tr("subsampling x/y"),
                     tr("%1 / %2").arg(cfg.subsampling_x).arg(cfg.subsampling_y));
        }
    }

    // ---- VVC ----
    if (result.has_vvc) {
        if (result.vvc_vps.present) {
            const model::VvcVpsInfo& vps = result.vvc_vps;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC VPS"), QString());
            MakeNode(node, tr("VPS ID"), QString::number(vps.vps_video_parameter_set_id));
            MakeNode(node, tr("Profile"), QString::fromStdString(vps.ProfileName()));
            MakeNode(node, tr("Tier"), vps.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"), QString::fromStdString(vps.LevelString()));
            MakeNode(node, tr("最大层数"), QString::number(vps.MaxLayers()));
            MakeNode(node, tr("最大子层数"), QString::number(vps.MaxSubLayers()));
            MakeNode(node, tr("各层独立"), FlagText(vps.vps_each_layer_is_an_ols_flag));
        }

        if (result.vvc_sps.present) {
            const model::VvcSpsInfo& sps = result.vvc_sps;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC SPS"), QString());
            MakeNode(node, tr("SPS ID"), QString::number(sps.sps_seq_parameter_set_id));
            MakeNode(node, tr("引用的 VPS ID"),
                     QString::number(sps.sps_video_parameter_set_id));
            MakeNode(node, tr("Profile"), QString::fromStdString(sps.ProfileName()));
            if (sps.general_level_idc > 0) {
                MakeNode(node, tr("Level"), QString::fromStdString(sps.LevelString()));
            } else {
                // 单层码流通常把 sps_ptl_dpb_hrd_params_present_flag 置 0，PTL 只在 VPS 里
                MakeNode(node, tr("Level"), tr("SPS 未携带 PTL（见 VPS / vvcC）"));
            }
            MakeNode(node, tr("分辨率"), tr("%1x%2").arg(sps.width()).arg(sps.height()));
            MakeNode(node, tr("编码尺寸 (luma)"),
                     tr("%1x%2")
                         .arg(sps.sps_pic_width_max_in_luma_samples)
                         .arg(sps.sps_pic_height_max_in_luma_samples));
            if (sps.sps_conformance_window_flag) {
                MakeNode(node, tr("裁剪 (L/R/T/B)"),
                         tr("%1 / %2 / %3 / %4")
                             .arg(sps.sps_conf_win_left_offset)
                             .arg(sps.sps_conf_win_right_offset)
                             .arg(sps.sps_conf_win_top_offset)
                             .arg(sps.sps_conf_win_bottom_offset));
            }
            MakeNode(node, tr("Chroma Format"), ChromaName(sps.sps_chroma_format_idc));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(sps.BitDepthLuma()));
            MakeNode(node, tr("CTU 尺寸"), tr("%1x%2").arg(sps.CtuSize()).arg(sps.CtuSize()));
            MakeNode(node, tr("最小 CB 尺寸"),
                     tr("%1x%2").arg(sps.MinCbSizeY()).arg(sps.MinCbSizeY()));
            MakeNode(node, tr("MaxPicOrderCntLsb"),
                     QString::number(sps.MaxPicOrderCntLsb()));
            MakeNode(node, tr("最大子层数"),
                     QString::number(sps.sps_max_sublayers_minus1 + 1));
            MakeNode(node, tr("GDR"), FlagText(sps.sps_gdr_enabled_flag));
            MakeNode(node, tr("参考图像重采样"),
                     FlagText(sps.sps_ref_pic_resampling_enabled_flag));
            MakeNode(node, tr("CLVS 内分辨率可变"),
                     FlagText(sps.sps_res_change_in_clvs_allowed_flag));

            QTreeWidgetItem* tools = MakeNode(node, tr("编码工具"), QString());
            MakeNode(tools, tr("SAO"), FlagText(sps.sps_sao_enabled_flag));
            MakeNode(tools, tr("ALF"), FlagText(sps.sps_alf_enabled_flag));
            MakeNode(tools, tr("CC-ALF"), FlagText(sps.sps_ccalf_enabled_flag));
            MakeNode(tools, tr("LMCS"), FlagText(sps.sps_lmcs_enabled_flag));
            MakeNode(tools, tr("MTS"), FlagText(sps.sps_mts_enabled_flag));
            MakeNode(tools, tr("LFNST"), FlagText(sps.sps_lfnst_enabled_flag));
            MakeNode(tools, tr("Transform Skip"),
                     FlagText(sps.sps_transform_skip_enabled_flag));
            MakeNode(tools, tr("Joint CbCr"), FlagText(sps.sps_joint_cbcr_enabled_flag));
            MakeNode(tools, tr("加权预测 P/B"),
                     tr("%1 / %2")
                         .arg(FlagText(sps.sps_weighted_pred_flag))
                         .arg(FlagText(sps.sps_weighted_bipred_flag)));
            MakeNode(tools, tr("Temporal MVP"), FlagText(sps.sps_temporal_mvp_enabled_flag));
            MakeNode(tools, tr("Affine"), FlagText(sps.sps_affine_enabled_flag));
            MakeNode(tools, tr("Palette"), FlagText(sps.sps_palette_enabled_flag));
            MakeNode(tools, tr("IBC"), FlagText(sps.sps_ibc_enabled_flag));
            MakeNode(tools, tr("Dependent QP"), FlagText(sps.sps_dep_quant_enabled_flag));
            MakeNode(tools, tr("参考列表数量 (list0/list1)"),
                     tr("%1 / %2").arg(sps.sps_num_ref_pic_lists[0])
                                  .arg(sps.sps_num_ref_pic_lists[1]));

            if (sps.vui.present) {
                const model::VvcVuiInfo& vui = sps.vui;
                QTreeWidgetItem* vui_node = MakeNode(node, tr("VUI"), QString());
                MakeNode(vui_node, tr("progressive_source"),
                         FlagText(vui.progressive_source_flag));
                MakeNode(vui_node, tr("interlaced_source"),
                         FlagText(vui.interlaced_source_flag));
                if (vui.aspect_ratio_info_present_flag) {
                    MakeNode(vui_node, tr("SAR"),
                             vui.aspect_ratio_idc == 0xFF
                                 ? tr("%1:%2 (Extended_SAR)")
                                       .arg(vui.sar_width).arg(vui.sar_height)
                                 : tr("idc=%1").arg(vui.aspect_ratio_idc));
                }
                if (vui.colour_description_present_flag) {
                    MakeNode(vui_node, tr("Color Primaries"),
                             PrimariesName(vui.colour_primaries));
                    MakeNode(vui_node, tr("Transfer"),
                             TransferName(vui.transfer_characteristics));
                    MakeNode(vui_node, tr("Matrix"), MatrixName(vui.matrix_coeffs));
                    MakeNode(vui_node, tr("full_range"), FlagText(vui.full_range_flag));
                } else {
                    MakeNode(vui_node, tr("color_description"),
                             tr("未出现（色彩标注缺失）"));
                }
            }
        } else {
            MakeNode(root, tr("VVC SPS"),
                     tr("未找到（vvcC 里通常只带 VPS/SPS/PPS 的一部分）"));
        }

        if (result.vvc_pps.present) {
            const model::VvcPpsInfo& pps = result.vvc_pps;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC PPS"), QString());
            MakeNode(node, tr("PPS ID"), QString::number(pps.pps_pic_parameter_set_id));
            MakeNode(node, tr("引用的 SPS ID"),
                     QString::number(pps.pps_seq_parameter_set_id));
            if (pps.pps_pic_width_in_luma_samples > 0) {
                MakeNode(node, tr("PPS 分辨率"),
                         tr("%1x%2").arg(pps.width()).arg(pps.height()));
            }
            MakeNode(node, tr("混合 NAL 类型"),
                     FlagText(pps.pps_mixed_nalu_types_in_pic_flag));
            MakeNode(node, tr("不分片 (no_pic_partition)"),
                     FlagText(pps.pps_no_pic_partition_flag));
            MakeNode(node, tr("子图数量"),
                     QString::number(pps.pps_num_subpics_minus1 + 1));
        }

        if (result.has_vvc_config && result.vvc_config.present) {
            const model::VvcCodecConfigInfo& cfg = result.vvc_config;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC vvcC 配置记录"), QString());
            MakeNode(node, tr("Profile"), QString::fromStdString(cfg.ProfileName()));
            MakeNode(node, tr("Tier"), cfg.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"), QString::fromStdString(cfg.LevelString()));
            MakeNode(node, tr("Chroma Format"), ChromaName(cfg.chroma_format_idc));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(cfg.BitDepth()));
            MakeNode(node, tr("子层数"), QString::number(cfg.MaxSubLayers()));
            MakeNode(node, tr("最大图像尺寸"),
                     tr("%1x%2").arg(cfg.max_picture_width).arg(cfg.max_picture_height));
        }
    }

    root->setExpanded(true);
    structure_tree_->expandAll();
    structure_tree_->resizeColumnToContents(0);
}

// --------------------------------------------------------------------------
// 容器 vs 码流 对比表
// --------------------------------------------------------------------------
void BitstreamPanel::BuildComparisonTable(const model::BitstreamAnalysisResult& result) {
    comparison_table_->setRowCount(0);
    if (!result.analyzed) return;

    auto AddRow = [this](const QString& field, const QString& container,
                         const QString& bitstream, bool comparable, bool equal) {
        const int row = comparison_table_->rowCount();
        comparison_table_->insertRow(row);
        comparison_table_->setItem(row, 0, new QTableWidgetItem(field));
        comparison_table_->setItem(row, 1, new QTableWidgetItem(container));
        comparison_table_->setItem(row, 2, new QTableWidgetItem(bitstream));

        QTableWidgetItem* verdict = new QTableWidgetItem(
            comparable ? (equal ? tr("一致") : tr("不一致")) : tr("—"));
        if (comparable && !equal) {
            verdict->setForeground(QColor("#d32f2f"));   // 红：不一致
        } else if (comparable) {
            verdict->setForeground(QColor("#2e7d32"));   // 绿：一致
        }
        comparison_table_->setItem(row, 3, verdict);
    };

    AddRow(tr("Codec"), QString::fromStdString(result.codec_id_str),
           QString::fromStdString(result.codec_name), false, false);

    const bool has_container = result.has_container;
    auto Cmp = [&](const QString& field, int container_value, int bitstream_value) {
        const bool comparable =
            has_container && container_value > 0 && bitstream_value > 0;
        AddRow(field,
               has_container ? QString::number(container_value) : tr("—"),
               bitstream_value > 0 ? QString::number(bitstream_value) : tr("未解析"),
               comparable, container_value == bitstream_value);
    };

    Cmp(tr("Width"), result.container_width, result.width);
    Cmp(tr("Height"), result.container_height, result.height);
    Cmp(tr("Bit Depth"), result.container_bit_depth, result.bit_depth);

    // 色彩三要素用名称展示，同时在判定列给出是否一致
    auto CmpNamed = [&](const QString& field, int container_value, int bitstream_value,
                        const QString& container_name, const QString& bitstream_name) {
        const bool comparable =
            has_container && container_value > 0 && bitstream_value > 0;
        AddRow(field,
               has_container ? container_name : tr("—"),
               bitstream_value > 0 ? bitstream_name : tr("未标注"),
               comparable, container_value == bitstream_value);
    };

    CmpNamed(tr("Color Primaries"), result.container_color_primaries, result.color_primaries,
             PrimariesName(result.container_color_primaries), PrimariesName(result.color_primaries));
    CmpNamed(tr("Transfer"), result.container_transfer_characteristics,
             result.transfer_characteristics,
             TransferName(result.container_transfer_characteristics),
             TransferName(result.transfer_characteristics));
    CmpNamed(tr("Matrix"), result.container_matrix_coefficients, result.matrix_coefficients,
             MatrixName(result.container_matrix_coefficients),
             MatrixName(result.matrix_coefficients));

    comparison_table_->resizeColumnsToContents();
}

// --------------------------------------------------------------------------
// 不一致项
// --------------------------------------------------------------------------
void BitstreamPanel::BuildInconsistencyTable(const model::BitstreamAnalysisResult& result) {
    inconsistency_table_->setRowCount(0);
    if (!result.analyzed || result.inconsistencies.empty()) return;

    for (const auto& item : result.inconsistencies) {
        const int row = inconsistency_table_->rowCount();
        inconsistency_table_->insertRow(row);

        const bool is_info = item.severity == "info";
        QTableWidgetItem* severity = new QTableWidgetItem(
            is_info ? tr("提示") : (item.severity == "error" ? tr("错误") : tr("警告")));
        severity->setForeground(QColor(is_info ? "#616161" : "#d32f2f"));

        inconsistency_table_->setItem(row, 0, severity);
        inconsistency_table_->setItem(
            row, 1, new QTableWidgetItem(QString::fromStdString(item.field)));
        inconsistency_table_->setItem(
            row, 2, new QTableWidgetItem(QString::fromStdString(item.container_value)));
        inconsistency_table_->setItem(
            row, 3, new QTableWidgetItem(QString::fromStdString(item.bitstream_value)));

        QString description = QString::fromStdString(item.description);
        if (!item.suggestion.empty()) {
            description += tr("（建议：%1）").arg(QString::fromStdString(item.suggestion));
        }
        inconsistency_table_->setItem(row, 4, new QTableWidgetItem(description));
    }

    inconsistency_table_->resizeColumnsToContents();
}

}  // namespace ui
}  // namespace videoeye
