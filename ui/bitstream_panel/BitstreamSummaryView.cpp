#include "ui/bitstream_panel/BitstreamSummaryView.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

#include "ui/bitstream_panel/bitstream_labels.h"

namespace videoeye {
namespace ui {
namespace {

QString LevelName(const model::BitstreamAnalysisResult& result) {
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
        return QObject::tr("未知");
    }
    return QObject::tr("未知");
}

QString DescribeCodec(const model::BitstreamAnalysisResult& result) {
    if (!result.analyzed)
        return QObject::tr("未支持/未解析");

    QString profile = QObject::tr("未知");
    QString chroma = QObject::tr("未知");
    if (result.has_h264 && result.h264_sps.present) {
        profile = QString::fromStdString(result.h264_sps.ProfileName());
        chroma = BitstreamChromaName(result.h264_sps.chroma_format_idc);
    } else if (result.has_hevc && result.hevc_sps.present) {
        profile = QString::fromStdString(result.hevc_sps.ProfileName());
        chroma = BitstreamChromaName(result.hevc_sps.chroma_format_idc);
    } else if (result.has_av1 && result.av1_seq_header.present) {
        profile = QString("Profile %1").arg(result.av1_seq_header.profile);
        chroma = QString::fromStdString(result.av1_seq_header.ChromaFormatString());
    } else if (result.has_av1 && result.has_av1_config) {
        profile = QString("Profile %1 (av1C)").arg(result.av1_config.profile);
    } else if (result.has_vvc && result.vvc_sps.present) {
        profile = QString::fromStdString(result.vvc_sps.ProfileName());
        chroma = BitstreamChromaName(result.vvc_sps.sps_chroma_format_idc);
    } else if (result.has_vvc && result.has_vvc_config) {
        profile = QString::fromStdString(result.vvc_config.ProfileName()) + QObject::tr(" (vvcC)");
        chroma = BitstreamChromaName(result.vvc_config.chroma_format_idc);
    }

    const QString bit_depth =
        result.bit_depth > 0 ? QObject::tr("%1bit").arg(result.bit_depth) : QObject::tr("位深未知");
    return QObject::tr("%1 · %2 · Level %3 · %4 · %5")
        .arg(QString::fromStdString(result.codec_name))
        .arg(profile)
        .arg(LevelName(result))
        .arg(chroma)
        .arg(bit_depth);
}

} // namespace

BitstreamSummaryView::BitstreamSummaryView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void BitstreamSummaryView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    summary_label_ = new QLabel(tr("暂无码流解析结果。"), this);
    summary_label_->setWordWrap(true);
    summary_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summary_label_);

    QHBoxLayout* toolbar = new QHBoxLayout();
    refresh_button_ = new QPushButton(tr("开始分析"), this);
    copy_json_button_ = new QPushButton(tr("复制 JSON"), this);
    export_json_button_ = new QPushButton(tr("导出 JSON"), this);
    toolbar->addWidget(refresh_button_);
    toolbar->addWidget(copy_json_button_);
    toolbar->addWidget(export_json_button_);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    connect(refresh_button_, &QPushButton::clicked, this, &BitstreamSummaryView::RefreshRequested);
    connect(copy_json_button_, &QPushButton::clicked, this, &BitstreamSummaryView::CopyJsonRequested);
    connect(export_json_button_, &QPushButton::clicked, this, &BitstreamSummaryView::ExportJsonRequested);
}

void BitstreamSummaryView::SetResult(const model::BitstreamAnalysisResult& result) {
    summary_label_->setToolTip(QString());
    if (!result.analyzed) {
        // 按 parse_outcome 区分"没有 extradata"与"编码不支持"，旧文案把两种
        // 情况混成一句，用户无法判断换封装重解是否有用。
        if (result.parse_outcome == model::BitstreamParseOutcome::NoExtradata) {
            summary_label_->setText(tr("没有可用的参数集：该视频流没有 extradata（常见于裸流 / TS），"
                                       "无法进行码流级解析。"));
        } else {
            const QString codec = result.codec_name.empty() ? QString::fromStdString(result.codec_id_str)
                                                            : QString::fromStdString(result.codec_name);
            summary_label_->setText(tr("编码格式 %1 暂不支持码流级解析（当前支持 H.264 / HEVC / AV1；"
                                       "VVC 为实验性部分支持）。")
                                        .arg(codec.isEmpty() ? tr("未知") : codec));
        }
        if (!result.capability_note.empty()) {
            summary_label_->setToolTip(QString::fromStdString(result.capability_note));
        }
        return;
    }

    const int warning_count = [&result] {
        int n = 0;
        for (const auto& item : result.inconsistencies) {
            if (item.severity != "info")
                ++n;
        }
        return n;
    }();

    QString resolution = tr("未知");
    if (result.width > 0 && result.height > 0) {
        resolution = QString("%1x%2").arg(result.width).arg(result.height);
        if (result.capability != model::AnalysisCapability::Supported) {
            resolution += tr("（参考）");
        }
    }

    // 能力状态徽标：Supported 不打扰；Partial / Failed 显式标出，
    // 鼠标悬停可看 capability_note；Partial 字段只作诊断参考，不代表正式结论。
    QString capability_badge;
    switch (result.capability) {
    case model::AnalysisCapability::Supported:
        break;
    case model::AnalysisCapability::Partial:
        capability_badge = tr("　<b>[部分支持/结果仅供参考]</b>");
        break;
    case model::AnalysisCapability::Failed:
        capability_badge = tr("　<b>[解析失败]</b>");
        break;
    case model::AnalysisCapability::Unavailable:
        capability_badge = tr("　<b>[不可用]</b>");
        break;
    }
    if (!capability_badge.isEmpty() && !result.capability_note.empty()) {
        summary_label_->setToolTip(QString::fromStdString(result.capability_note));
    }

    summary_label_->setText(tr("<b>%1</b>　流 #%2　%3　参数集: NAL %4 / OBU %5　不一致: %6%7")
                                .arg(DescribeCodec(result))
                                .arg(result.stream_index)
                                .arg(resolution)
                                .arg(result.nal_units.size())
                                .arg(result.obu_units.size())
                                .arg(warning_count)
                                .arg(capability_badge));
}

void BitstreamSummaryView::Clear() {
    summary_label_->setText(tr("暂无码流解析结果。"));
}

void BitstreamSummaryView::SetRefreshEnabled(bool enabled) {
    if (refresh_button_)
        refresh_button_->setEnabled(enabled);
}

} // namespace ui
} // namespace videoeye
