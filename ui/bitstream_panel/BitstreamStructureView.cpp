#include "ui/bitstream_panel/BitstreamStructureView.h"

#include <QVBoxLayout>

#include <QTreeWidgetItem>

#include "ui/bitstream_panel/bitstream_labels.h"

namespace videoeye {
namespace ui {
namespace {

QTreeWidgetItem* MakeNode(QTreeWidgetItem* parent, const QString& name, const QString& value) {
    QTreeWidgetItem* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    item->setText(1, value);
    return item;
}

} // namespace

BitstreamStructureView::BitstreamStructureView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void BitstreamStructureView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    structure_tree_ = new QTreeWidget(this);
    structure_tree_->setHeaderLabels({tr("参数集"), tr("值")});
    structure_tree_->setRootIsDecorated(true);
    structure_tree_->setAlternatingRowColors(true);
    layout->addWidget(structure_tree_);
}

void BitstreamStructureView::Clear() {
    structure_tree_->clear();
}

void BitstreamStructureView::SetResult(const model::BitstreamAnalysisResult& result) {
    structure_tree_->clear();
    if (!result.analyzed)
        return;
    BuildTree(result);
}

void BitstreamStructureView::BuildTree(const model::BitstreamAnalysisResult& result) {
    QTreeWidgetItem* root = new QTreeWidgetItem(structure_tree_);
    root->setText(0, tr("参数集 (%1)").arg(QString::fromStdString(result.codec_name)));
    root->setText(1, QString::fromStdString(result.codec_id_str));

    if (!result.nal_units.empty()) {
        QTreeWidgetItem* nal_root = MakeNode(root, tr("NAL 单元 (%1)").arg(result.nal_units.size()), QString());
        for (size_t i = 0; i < result.nal_units.size(); ++i) {
            const auto& nal = result.nal_units[i];
            MakeNode(nal_root, tr("NAL #%1").arg(static_cast<qulonglong>(i)),
                     tr("type=%1 size=%2%3").arg(nal.type).arg(nal.size).arg(nal.is_idr ? tr(" [IDR]") : QString()));
        }
    }
    if (!result.obu_units.empty()) {
        QTreeWidgetItem* obu_root = MakeNode(root, tr("OBU 单元 (%1)").arg(result.obu_units.size()), QString());
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
            MakeNode(node, tr("分辨率"), tr("%1x%2").arg(sps.width()).arg(sps.height()));
            MakeNode(node, tr("编码尺寸 (MB)"),
                     tr("%1x%2 MB").arg(sps.pic_width_in_mbs_minus1 + 1).arg(sps.pic_height_in_mbs_minus1 + 1));
            MakeNode(node, tr("Chroma Format"), BitstreamChromaName(sps.chroma_format_idc));
            MakeNode(node, tr("位深 亮度/色度"), tr("%1 / %2 bit").arg(sps.BitDepthLuma()).arg(sps.BitDepthChroma()));
            MakeNode(node, tr("最大参考帧"), QString::number(sps.max_num_ref_frames));
            MakeNode(node, tr("POC 类型"), QString::number(sps.pic_order_cnt_type));
            MakeNode(node, tr("log2_max_frame_num"), QString::number(sps.log2_max_frame_num_minus4 + 4));
            MakeNode(node, tr("frame_mbs_only"), BitstreamFlagText(sps.frame_mbs_only_flag));
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
                    MakeNode(vui_node, tr("full_range"), BitstreamFlagText(vui.video_full_range_flag));
                }
                if (vui.colour_description_present) {
                    MakeNode(vui_node, tr("Color Primaries"), BitstreamPrimariesName(vui.color_primaries));
                    MakeNode(vui_node, tr("Transfer"), BitstreamTransferName(vui.transfer_characteristics));
                    MakeNode(vui_node, tr("Matrix"), BitstreamMatrixName(vui.matrix_coefficients));
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
            MakeNode(node, tr("加权预测"), BitstreamFlagText(pps.weighted_pred_flag));
            MakeNode(node, tr("8x8 变换"), BitstreamFlagText(pps.transform_8x8_mode_flag));
            MakeNode(node, tr("去块滤波控制存在"), BitstreamFlagText(pps.deblocking_filter_control_present_flag));
        }
    }

    // ---- HEVC ----
    if (result.has_hevc) {
        if (result.hevc_vps.present) {
            const model::HevcVpsInfo& vps = result.hevc_vps;
            QTreeWidgetItem* node = MakeNode(root, tr("HEVC VPS"), QString());
            MakeNode(node, tr("Profile IDC"), QString::number(vps.general_profile_idc));
            MakeNode(node, tr("Tier"), vps.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"),
                     QString("%1.%2").arg(vps.general_level_idc / 30).arg((vps.general_level_idc % 30) / 3));
            MakeNode(node, tr("层 / 子层"), tr("%1 / %2").arg(vps.vps_max_layers).arg(vps.vps_max_sub_layers));
        }
        if (result.hevc_sps.present) {
            const model::HevcSpsInfo& sps = result.hevc_sps;
            QTreeWidgetItem* node = MakeNode(root, tr("HEVC SPS"), QString());
            MakeNode(node, tr("Profile"), QString::fromStdString(sps.ProfileName()));
            MakeNode(node, tr("Tier"), sps.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"),
                     QString("%1.%2").arg(sps.general_level_idc / 30).arg((sps.general_level_idc % 30) / 3));
            MakeNode(node, tr("分辨率"), tr("%1x%2").arg(sps.width()).arg(sps.height()));
            MakeNode(node, tr("编码尺寸"),
                     tr("%1x%2").arg(sps.pic_width_in_luma_samples).arg(sps.pic_height_in_luma_samples));
            MakeNode(node, tr("Chroma Format"), BitstreamChromaName(sps.chroma_format_idc));
            MakeNode(node, tr("位深 亮度/色度"), tr("%1 / %2 bit").arg(sps.BitDepthLuma()).arg(sps.BitDepthChroma()));
            MakeNode(node, tr("CTU 尺寸"),
                     QString::number(1 << (sps.log2_min_luma_coding_block_size_minus3 + 3 +
                                           sps.log2_diff_max_min_luma_coding_block_size)));
            MakeNode(node, tr("SAO"), BitstreamFlagText(sps.sample_adaptive_offset_enabled_flag != 0));
            MakeNode(node, tr("短时参考帧集数"), QString::number(sps.num_short_term_ref_pic_sets));
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
                     pps.single_tile_in_pic_flag
                         ? tr("单 tile")
                         : tr("%1 x %2").arg(pps.num_tile_columns_minus1 + 1).arg(pps.num_tile_rows_minus1 + 1));
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
            MakeNode(node, tr("分辨率"), tr("%1x%2").arg(sh.FrameWidth()).arg(sh.FrameHeight()));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(sh.BitDepth()));
            MakeNode(node, tr("Chroma Format"), QString::fromStdString(sh.ChromaFormatString()));
            MakeNode(node, tr("Still picture"), BitstreamFlagText(sh.still_picture));
            MakeNode(node, tr("128x128 超级块"), BitstreamFlagText(sh.use_128x128_superblock));
            MakeNode(node, tr("CDEF"), BitstreamFlagText(sh.enable_cdef));
            MakeNode(node, tr("Restoration"), BitstreamFlagText(sh.enable_restoration));
            MakeNode(node, tr("Film grain"), BitstreamFlagText(sh.film_grain_params_present));
            if (sh.color_config.present) {
                const model::Av1ColorConfigInfo& cc = sh.color_config;
                QTreeWidgetItem* cc_node = MakeNode(node, tr("color_config"), QString());
                MakeNode(cc_node, tr("high_bitdepth"), BitstreamFlagText(cc.high_bitdepth));
                MakeNode(cc_node, tr("twelve_bit"), BitstreamFlagText(cc.twelve_bit));
                MakeNode(cc_node, tr("monochrome"), BitstreamFlagText(cc.monochrome));
                MakeNode(cc_node, tr("subsampling x/y"), tr("%1 / %2").arg(cc.subsampling_x).arg(cc.subsampling_y));
                // ❗ full_range 与 color_description_present_flag **互相独立**：
                // 色彩描述没出现时 color_range f(1) 照样在码流里。挂在
                // color_description 分支下会让"无色彩标注"的流连色域范围也看不到。
                MakeNode(cc_node, tr("full_range"), BitstreamFlagText(cc.full_range_flag != 0));
                if (cc.color_description_present_flag) {
                    MakeNode(cc_node, tr("Color Primaries"), BitstreamPrimariesName(cc.color_primaries));
                    MakeNode(cc_node, tr("Transfer"), BitstreamTransferName(cc.transfer_characteristics));
                    MakeNode(cc_node, tr("Matrix"), BitstreamMatrixName(cc.matrix_coefficients));
                } else {
                    MakeNode(cc_node, tr("color_description"), tr("未出现（色彩标注缺失）"));
                }
            }
        } else {
            MakeNode(root, tr("AV1 Sequence Header"), tr("未找到（MP4/WebM 的 av1C 里通常不含序列头 OBU）"));
        }
        if (result.has_av1_config && result.av1_config.present) {
            const model::Av1CodecConfigInfo& cfg = result.av1_config;
            QTreeWidgetItem* node = MakeNode(root, tr("AV1 av1C 配置记录"), QString());
            MakeNode(node, tr("Profile"), QString::number(cfg.profile));
            MakeNode(node, tr("Level"), QString::number(cfg.level));
            MakeNode(node, tr("Tier"), cfg.tier ? tr("High") : tr("Main"));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(cfg.bit_depth));
            MakeNode(node, tr("monochrome"), BitstreamFlagText(cfg.monochrome));
            MakeNode(node, tr("subsampling x/y"), tr("%1 / %2").arg(cfg.subsampling_x).arg(cfg.subsampling_y));
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
            MakeNode(node, tr("各层独立"), BitstreamFlagText(vps.vps_each_layer_is_an_ols_flag));
        }

        if (result.vvc_sps.present) {
            const model::VvcSpsInfo& sps = result.vvc_sps;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC SPS"), QString());
            MakeNode(node, tr("SPS ID"), QString::number(sps.sps_seq_parameter_set_id));
            MakeNode(node, tr("引用的 VPS ID"), QString::number(sps.sps_video_parameter_set_id));
            MakeNode(node, tr("Profile"), QString::fromStdString(sps.ProfileName()));
            if (sps.general_level_idc > 0) {
                MakeNode(node, tr("Level"), QString::fromStdString(sps.LevelString()));
            } else {
                // 单层码流通常把 sps_ptl_dpb_hrd_params_present_flag 置 0，PTL 只在 VPS 里
                MakeNode(node, tr("Level"), tr("SPS 未携带 PTL（见 VPS / vvcC）"));
            }
            MakeNode(node, tr("分辨率"), tr("%1x%2").arg(sps.width()).arg(sps.height()));
            MakeNode(
                node, tr("编码尺寸 (luma)"),
                tr("%1x%2").arg(sps.sps_pic_width_max_in_luma_samples).arg(sps.sps_pic_height_max_in_luma_samples));
            if (sps.sps_conformance_window_flag) {
                MakeNode(node, tr("裁剪 (L/R/T/B)"),
                         tr("%1 / %2 / %3 / %4")
                             .arg(sps.sps_conf_win_left_offset)
                             .arg(sps.sps_conf_win_right_offset)
                             .arg(sps.sps_conf_win_top_offset)
                             .arg(sps.sps_conf_win_bottom_offset));
            }
            MakeNode(node, tr("Chroma Format"), BitstreamChromaName(sps.sps_chroma_format_idc));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(sps.BitDepthLuma()));
            MakeNode(node, tr("CTU 尺寸"), tr("%1x%2").arg(sps.CtuSize()).arg(sps.CtuSize()));
            MakeNode(node, tr("最小 CB 尺寸"), tr("%1x%2").arg(sps.MinCbSizeY()).arg(sps.MinCbSizeY()));
            MakeNode(node, tr("MaxPicOrderCntLsb"), QString::number(sps.MaxPicOrderCntLsb()));
            MakeNode(node, tr("最大子层数"), QString::number(sps.sps_max_sublayers_minus1 + 1));
            MakeNode(node, tr("GDR"), BitstreamFlagText(sps.sps_gdr_enabled_flag));
            MakeNode(node, tr("参考图像重采样"), BitstreamFlagText(sps.sps_ref_pic_resampling_enabled_flag));
            MakeNode(node, tr("CLVS 内分辨率可变"), BitstreamFlagText(sps.sps_res_change_in_clvs_allowed_flag));

            QTreeWidgetItem* tools = MakeNode(node, tr("编码工具"), QString());
            MakeNode(tools, tr("SAO"), BitstreamFlagText(sps.sps_sao_enabled_flag));
            MakeNode(tools, tr("ALF"), BitstreamFlagText(sps.sps_alf_enabled_flag));
            MakeNode(tools, tr("CC-ALF"), BitstreamFlagText(sps.sps_ccalf_enabled_flag));
            MakeNode(tools, tr("LMCS"), BitstreamFlagText(sps.sps_lmcs_enabled_flag));
            MakeNode(tools, tr("MTS"), BitstreamFlagText(sps.sps_mts_enabled_flag));
            MakeNode(tools, tr("LFNST"), BitstreamFlagText(sps.sps_lfnst_enabled_flag));
            MakeNode(tools, tr("Transform Skip"), BitstreamFlagText(sps.sps_transform_skip_enabled_flag));
            MakeNode(tools, tr("Joint CbCr"), BitstreamFlagText(sps.sps_joint_cbcr_enabled_flag));
            MakeNode(tools, tr("加权预测 P/B"),
                     tr("%1 / %2")
                         .arg(BitstreamFlagText(sps.sps_weighted_pred_flag))
                         .arg(BitstreamFlagText(sps.sps_weighted_bipred_flag)));
            MakeNode(tools, tr("Temporal MVP"), BitstreamFlagText(sps.sps_temporal_mvp_enabled_flag));
            MakeNode(tools, tr("Affine"), BitstreamFlagText(sps.sps_affine_enabled_flag));
            MakeNode(tools, tr("Palette"), BitstreamFlagText(sps.sps_palette_enabled_flag));
            MakeNode(tools, tr("IBC"), BitstreamFlagText(sps.sps_ibc_enabled_flag));
            MakeNode(tools, tr("Dependent QP"), BitstreamFlagText(sps.sps_dep_quant_enabled_flag));
            MakeNode(tools, tr("参考列表数量 (list0/list1)"),
                     tr("%1 / %2").arg(sps.sps_num_ref_pic_lists[0]).arg(sps.sps_num_ref_pic_lists[1]));

            if (sps.vui.present) {
                const model::VvcVuiInfo& vui = sps.vui;
                QTreeWidgetItem* vui_node = MakeNode(node, tr("VUI"), QString());
                MakeNode(vui_node, tr("progressive_source"), BitstreamFlagText(vui.progressive_source_flag));
                MakeNode(vui_node, tr("interlaced_source"), BitstreamFlagText(vui.interlaced_source_flag));
                if (vui.aspect_ratio_info_present_flag) {
                    MakeNode(vui_node, tr("SAR"),
                             vui.aspect_ratio_idc == 0xFF
                                 ? tr("%1:%2 (Extended_SAR)").arg(vui.sar_width).arg(vui.sar_height)
                                 : tr("idc=%1").arg(vui.aspect_ratio_idc));
                }
                if (vui.colour_description_present_flag) {
                    MakeNode(vui_node, tr("Color Primaries"), BitstreamPrimariesName(vui.colour_primaries));
                    MakeNode(vui_node, tr("Transfer"), BitstreamTransferName(vui.transfer_characteristics));
                    MakeNode(vui_node, tr("Matrix"), BitstreamMatrixName(vui.matrix_coeffs));
                    MakeNode(vui_node, tr("full_range"), BitstreamFlagText(vui.full_range_flag));
                } else {
                    MakeNode(vui_node, tr("color_description"), tr("未出现（色彩标注缺失）"));
                }
            }
        } else {
            MakeNode(root, tr("VVC SPS"), tr("未找到（vvcC 里通常只带 VPS/SPS/PPS 的一部分）"));
        }

        if (result.vvc_pps.present) {
            const model::VvcPpsInfo& pps = result.vvc_pps;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC PPS"), QString());
            MakeNode(node, tr("PPS ID"), QString::number(pps.pps_pic_parameter_set_id));
            MakeNode(node, tr("引用的 SPS ID"), QString::number(pps.pps_seq_parameter_set_id));
            if (pps.pps_pic_width_in_luma_samples > 0) {
                MakeNode(node, tr("PPS 分辨率"), tr("%1x%2").arg(pps.width()).arg(pps.height()));
            }
            MakeNode(node, tr("混合 NAL 类型"), BitstreamFlagText(pps.pps_mixed_nalu_types_in_pic_flag));
            MakeNode(node, tr("不分片 (no_pic_partition)"), BitstreamFlagText(pps.pps_no_pic_partition_flag));
            MakeNode(node, tr("子图数量"), QString::number(pps.pps_num_subpics_minus1 + 1));
        }

        if (result.has_vvc_config && result.vvc_config.present) {
            const model::VvcCodecConfigInfo& cfg = result.vvc_config;
            QTreeWidgetItem* node = MakeNode(root, tr("VVC vvcC 配置记录"), QString());
            MakeNode(node, tr("Profile"), QString::fromStdString(cfg.ProfileName()));
            MakeNode(node, tr("Tier"), cfg.general_tier_flag ? tr("High") : tr("Main"));
            MakeNode(node, tr("Level"), QString::fromStdString(cfg.LevelString()));
            MakeNode(node, tr("Chroma Format"), BitstreamChromaName(cfg.chroma_format_idc));
            MakeNode(node, tr("位深"), tr("%1 bit").arg(cfg.BitDepth()));
            MakeNode(node, tr("子层数"), QString::number(cfg.MaxSubLayers()));
            MakeNode(node, tr("最大图像尺寸"), tr("%1x%2").arg(cfg.max_picture_width).arg(cfg.max_picture_height));
        }
    }

    root->setExpanded(true);
    structure_tree_->expandAll();
    structure_tree_->resizeColumnToContents(0);
}

} // namespace ui
} // namespace videoeye
