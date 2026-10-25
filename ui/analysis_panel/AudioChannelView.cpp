#include "ui/analysis_panel/AudioChannelView.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHeaderView>
#include <QStringList>
#include <QVBoxLayout>

#include "core/domain/model/AudioQcResult.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

AudioChannelView::AudioChannelView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void AudioChannelView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    channel_chart_ = new MetricChartWidget(this);
    channel_chart_->SetTitle(tr("声道能量（RMS / 峰值 dBFS）"));
    channel_series_ = channel_chart_->AddBarSeries(tr("RMS dBFS"), QColor("#42a5f5"));
    channel_peak_series_ = channel_chart_->AddBarSeries(tr("峰值 dBFS"), QColor("#66bb6a"));
    channel_axis_x_ = channel_chart_->AxisX();
    channel_axis_y_ = channel_chart_->AxisY();
    channel_axis_y_->SetTitleText(tr("dBFS"));
    channel_chart_->setMinimumHeight(200);
    layout->addWidget(channel_chart_);

    corr_chart_ = new MetricChartWidget(this);
    corr_chart_->SetTitle(tr("声道相关性（最差声道对，1=同相 / -1=反相）"));
    corr_series_ = corr_chart_->AddLineSeries(tr("相关性"), QColor("#42a5f5"));
    corr_axis_x_ = corr_chart_->AxisX();
    corr_axis_y_ = corr_chart_->AxisY();
    corr_axis_x_->SetTitleText(tr("时间 (s)"));
    corr_axis_y_->SetRange(-1.05, 1.05);
    corr_chart_->setMinimumHeight(200);
    layout->addWidget(corr_chart_);

    metadata_table_ = new QTableWidget(0, 2, this);
    metadata_table_->setHorizontalHeaderLabels({tr("项目"), tr("值")});
    metadata_table_->verticalHeader()->setVisible(false);
    metadata_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    metadata_table_->horizontalHeader()->setStretchLastSection(true);
    metadata_table_->setMinimumHeight(150);
    layout->addWidget(metadata_table_);
}

void AudioChannelView::SetResult(const model::AnalysisResult* result) {
    result_ = result;
    UpdateCharts();
    RebuildMetadataTable();
}

void AudioChannelView::UpdateCharts() {
    channel_series_->Clear();
    channel_peak_series_->Clear();
    corr_series_->Clear();
    if (result_ == nullptr || !result_->audio_qc.analyzed)
        return;

    const auto& qc = result_->audio_qc;

    // 声道能量柱状图（每个声道两根柱: RMS / 峰值）
    if (!qc.channels.empty()) {
        QStringList categories;
        // 多轨素材的声道数可以到几十（5.1 / 7.1 / 多语言音轨各一组），
        // 逐点 Append 就是"每根柱子投一次重绘"，这里攒完一次性提交。
        SeriesBatch channel(channel_series_);
        SeriesBatch channel_peak(channel_peak_series_);
        channel.Reserve(static_cast<int>(qc.channels.size()));
        channel_peak.Reserve(static_cast<int>(qc.channels.size()));
        for (const auto& ch : qc.channels) {
            const QString name = ch.name.empty() ? QString("Ch%1").arg(ch.index + 1) : QString::fromStdString(ch.name);
            categories << name;
            const double rms = (ch.rms_dbfs <= model::kSilenceLevelDb + 1.0) ? -120.0 : ch.rms_dbfs;
            const double peak = (ch.peak_dbfs <= model::kSilenceLevelDb + 1.0) ? -120.0 : ch.peak_dbfs;
            const double x = static_cast<double>(categories.size() - 1);
            channel.Add(x, rms);
            channel_peak.Add(x, peak);
        }
        channel_chart_->SetCategories(categories);
        channel_axis_y_->SetRange(-90.0, 6.0);
    }

    // 相关性曲线
    const auto& points = qc.loudness_points;
    if (!points.empty()) {
        // 不可用哨兵（-2.0 附近）统一落到 -2.0，避免与真实的 -1 反相混在一起
        auto get = [](const model::LoudnessPoint& p) { return (p.correlation <= -1.0) ? -2.0 : p.correlation; };
        SubmitDecimated(corr_series_, DecimateFrom(
                                          points, [](const model::LoudnessPoint& p) { return p.timestamp_seconds; },
                                          get, kMaxChartPoints));
        const double span = std::max(1.0, qc.duration_seconds);
        corr_axis_x_->SetRange(0.0, span);
    }
}

void AudioChannelView::RebuildMetadataTable() {
    if (!metadata_table_)
        return;
    metadata_table_->setRowCount(0);
    if (result_ == nullptr)
        return;
    const auto& result = *result_;
    const auto& meta = result.audio_qc.metadata;

    auto add_row = [this](const QString& key, const QString& value) {
        const int row = metadata_table_->rowCount();
        metadata_table_->insertRow(row);
        SetTableItemText(metadata_table_, row, 0, key);
        SetTableItemText(metadata_table_, row, 1, value);
    };

    add_row(tr("声道布局"), QString::fromStdString(meta.channel_layout));
    add_row(tr("声道数"), QString::number(meta.channels));
    add_row(tr("采样率"), QString::number(meta.sample_rate) + tr(" Hz"));
    add_row(tr("采样格式"), meta.sample_format.empty() ? tr("未知") : QString::fromStdString(meta.sample_format));
    add_row(tr("位深"), meta.bits_per_sample > 0 ? QString::number(meta.bits_per_sample) + tr(" bit") : tr("未知"));
    add_row(tr("音频时长"), QString::number(meta.stream_duration_seconds, 'f', 3) + tr(" s"));
    add_row(tr("容器时长"), QString::number(meta.container_duration_seconds, 'f', 3) + tr(" s"));
    add_row(tr("视频时长"),
            meta.has_video ? QString::number(meta.video_duration_seconds, 'f', 3) + tr(" s") : tr("无视频流"));
    add_row(tr("与容器时差"), QString::number(meta.container_delta_seconds, 'f', 3) + tr(" s"));
    if (meta.has_video) {
        add_row(tr("与视频时差"), QString::number(meta.video_delta_seconds, 'f', 3) + tr(" s"));
    }
    add_row(tr("一致性问题"),
            meta.inconsistencies.empty() ? tr("无") : QString::fromStdString(meta.inconsistencies.front()));

    // ---- 音频位流参数（D1）----
    // 这些行来自**帧头**而不是 PCM，所以即使响度那一段因解码失败没有数据，
    // 这里仍然有结论 —— 审核时"容器说 48k、码流其实是 44.1k"正是靠这几行看出来。
    if (!result.audio_bitstream_analyzed)
        return;
    for (const auto& stream : result.audio_bitstream.streams) {
        const QString prefix =
            result.audio_bitstream.streams.size() > 1 ? tr("位流 #%1 ").arg(stream.stream_index) : tr("位流 ");
        QString codec = QString::fromStdString(model::ToString(stream.codec));
        if (!stream.profile.empty()) {
            codec += QStringLiteral(" / ") + QString::fromStdString(stream.profile);
        }
        add_row(prefix + tr("编码"), codec);

        if (stream.sample_rate > 0) {
            QString rate = QString::number(stream.sample_rate) + tr(" Hz");
            if (stream.container_sample_rate > 0 && stream.container_sample_rate != stream.sample_rate) {
                rate += tr("（容器声明 %1 Hz）").arg(stream.container_sample_rate);
            }
            add_row(prefix + tr("采样率"), rate);
        }
        if (stream.channels > 0) {
            QString channels = QString::number(stream.channels);
            if (stream.acmod >= 0) {
                channels +=
                    QStringLiteral(" — ") + QString::fromStdString(model::DescribeAcmod(stream.acmod, stream.lfeon));
            } else if (!stream.container_channel_layout.empty()) {
                channels += QStringLiteral(" — ") + QString::fromStdString(stream.container_channel_layout);
            }
            if (stream.container_channels > 0 && stream.container_channels != stream.channels) {
                channels += tr("（容器声明 %1）").arg(stream.container_channels);
            }
            add_row(prefix + tr("声道"), channels);
        }
        if (stream.bit_rate_kbps > 0) {
            add_row(prefix + tr("码率"), QString::number(stream.bit_rate_kbps) + tr(" kbps") +
                                             (stream.vbr ? tr("（帧长变化，疑似 VBR）") : QString()));
        }
        if (stream.has_dialnorm) {
            QString dialnorm = QString::number(stream.dialnorm_dbfs) + tr(" dBFS");
            if (stream.dialnorm_delta_available) {
                dialnorm += tr("（与实测响度差 %1 LU）").arg(stream.dialnorm_delta_lu, 0, 'f', 1);
            }
            add_row(prefix + tr("dialnorm"), dialnorm);
        } else if (stream.codec == model::AudioBitstreamCodec::Ac3 ||
                   stream.codec == model::AudioBitstreamCodec::Eac3) {
            add_row(prefix + tr("dialnorm"), tr("未声明"));
        }
        if (stream.eac3) {
            QString substreams = stream.dependent_substream_count > 0 ? tr("%1 个从属子流（+%2 声道）")
                                                                            .arg(stream.dependent_substream_count)
                                                                            .arg(stream.dependent_channels)
                                                                      : tr("无");
            add_row(prefix + tr("E-AC-3 子流"), substreams);
        }
        if (stream.atmos_joc) {
            // 启发式命中必须写成"疑似"，不能给用户一个确定结论
            QString atmos = stream.atmos_heuristic ? tr("疑似检出（启发式线索）") : tr("检出");
            if (!stream.atmos_source.empty()) {
                atmos += QStringLiteral("（") + QString::fromStdString(stream.atmos_source) + QStringLiteral("）");
            }
            add_row(prefix + tr("Atmos / JOC"), atmos);
        }
        if (stream.codec == model::AudioBitstreamCodec::Opus && stream.opus_head) {
            QString opus = tr("pre-skip %1 样本 / 增益 %2 dB / 原始采样率 %3 Hz")
                               .arg(stream.pre_skip)
                               .arg(stream.OutputGainDb(), 0, 'f', 2)
                               .arg(stream.input_sample_rate);
            if (stream.opus_mode >= 0) {
                opus += tr(" / %1 %2")
                            .arg(QString::fromStdString(model::OpusModeName(stream.opus_mode)))
                            .arg(QString::fromStdString(model::OpusBandwidthName(stream.opus_bandwidth)));
            }
            add_row(prefix + tr("Opus"), opus);
        }
        if (stream.unsupported_packets > 0) {
            add_row(prefix + tr("帧头解析"), tr("该编码尚未实现逐帧解析，%1 个包未参与判定")
                                                 .arg(static_cast<qlonglong>(stream.unsupported_packets)));
        } else {
            add_row(prefix + tr("帧头解析"), tr("%1 个包 / %2 帧，失败 %3 个")
                                                 .arg(static_cast<qlonglong>(stream.parsed_packets))
                                                 .arg(static_cast<qlonglong>(stream.frame_count))
                                                 .arg(static_cast<qlonglong>(stream.invalid_packets)));
        }
        if (!stream.inconsistencies.empty()) {
            add_row(prefix + tr("不一致"), QString::fromStdString(stream.inconsistencies.front()));
        }
        if (!stream.note.empty()) {
            add_row(prefix + tr("说明"), QString::fromStdString(stream.note));
        }
    }
}

} // namespace ui
} // namespace videoeye
