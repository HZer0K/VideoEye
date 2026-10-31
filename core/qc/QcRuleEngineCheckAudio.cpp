#include "core/qc/QcRuleEngineInternal.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace videoeye {
namespace qc {
namespace detail {

namespace {

// 一条参与音频 QC 的轨道视图。
//
// 为什么需要它：result.audio_qc 恒等于主轨（兼容既有消费方），多轨结果在
// audio_qc_streams。规则层以前只读主轨，于是次轨的削波/静音/相位问题在 QC
// 结论里完全看不见 —— 这里把"实际参与 QC 的轨道"统一成一个视图，规则只遍历它。
struct AudioQcTrackView {
    int stream_index = -1;
    const model::AudioQcResult* qc = nullptr;
    bool primary = false;
};

std::vector<AudioQcTrackView> AudioQcTrackViews(const model::AnalysisResult& result) {
    const model::StreamDigest* first_audio = result.FirstAudioStream();
    const int primary_index = (result.primary_audio_stream_index >= 0)
                                  ? result.primary_audio_stream_index
                                  : (first_audio != nullptr ? first_audio->index : -1);

    std::vector<AudioQcTrackView> views;
    if (!result.audio_qc_streams.empty()) {
        for (size_t i = 0; i < result.audio_qc_streams.size(); ++i) {
            AudioQcTrackView view;
            view.qc = &result.audio_qc_streams[i];
            view.stream_index =
                (i < result.audio_qc_stream_indices.size()) ? result.audio_qc_stream_indices[i] : primary_index;
            view.primary = (view.stream_index == primary_index);
            views.push_back(view);
        }
        return views;
    }

    // 没有逐轨数据（旧结果 / 单测）时回退主轨兼容字段。
    AudioQcTrackView view;
    view.qc = &result.audio_qc;
    view.stream_index = primary_index;
    view.primary = true;
    views.push_back(view);
    return views;
}

std::string TrackPrefix(const AudioQcTrackView& view, bool multiple) {
    // 单轨结果保持原文案；多轨/次轨必须标明是哪一条，否则报告里两条同名问题无法区分。
    if (!multiple && view.primary)
        return "";
    return "流 #" + std::to_string(view.stream_index) + "：";
}

bool TrackQcIncomplete(const model::AudioQcResult& qc) {
    return !qc.analyzed || qc.completeness == model::AudioQcResult::Completeness::Partial ||
           qc.completeness == model::AudioQcResult::Completeness::Failed ||
           qc.completeness == model::AudioQcResult::Completeness::Unsupported;
}

// 音频 QC 到底跑过没有。NoFalsePositiveWithoutAudioAnalysis 那种"结果里什么都没有"
// 必须继续不判定；而"全部解码失败"（analyzed=false 但维度 executed/Failed）必须判定。
bool AudioQcRan(const model::AnalysisResult& result, const model::DimensionStatus* dimension) {
    if (result.audio_qc.analyzed || result.audio_qc.completeness != model::AudioQcResult::Completeness::NotRun)
        return true;
    if (!result.audio_qc_streams.empty() || result.audio_qc_tracks_total > 0)
        return true;
    return dimension != nullptr && dimension->executed;
}

// 每条位流流自己的实测响度：没有逐轨结果时回退主轨；有逐轨结果但配不上就不给结论。
const model::AudioQcResult* MeasuredQcForStream(const model::AnalysisResult& result, int stream_index) {
    if (result.audio_qc_streams.empty())
        return result.audio_qc.analyzed ? &result.audio_qc : nullptr;
    for (size_t i = 0; i < result.audio_qc_stream_indices.size() && i < result.audio_qc_streams.size(); ++i) {
        if (result.audio_qc_stream_indices[i] == stream_index)
            return &result.audio_qc_streams[i];
    }
    return nullptr;
}

} // namespace

// 音频规则（audio.*）。调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckAudioRules(const model::QcRule& rule, const model::AnalysisResult& result,
                                                    const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- 音频 ----------
    if (rule.id == "audio.sample_rate_low") {
        int min_rate = 0;
        int stream_index = -1;
        for (const auto& stream : result.streams) {
            if (!stream.IsAudio())
                continue;
            if (min_rate == 0 || stream.sample_rate < min_rate) {
                min_rate = stream.sample_rate;
                stream_index = stream.index;
            }
        }
        if (min_rate > 0 && Triggered(rule, static_cast<double>(min_rate))) {
            issues.push_back(
                make_issue(static_cast<double>(min_rate),
                           "最低音频采样率为 " + std::to_string(min_rate) + " Hz（阈值 " + ThresholdText(rule) + "）。",
                           model::TimeRange::Global(), stream_index));
        }
        return issues;
    }
    // ---------- 音频位流参数（core/analysis/quality/AudioBitstreamAnalyzer.h）----------
    // 与下面的音频 QC 是**两条独立链路**：这组只读帧头，不需要解码成功。
    if (rule.id == "audio.bitstream.container_mismatch" || rule.id == "audio.bitstream.header_invalid" ||
        rule.id == "audio.bitstream.param_change" || rule.id == "audio.dialnorm.missing" ||
        rule.id == "audio.dialnorm.mismatch") {
        if (!result.DimensionExecuted(model::AnalysisDimension::AudioBitstream))
            return issues;
        const auto& bitstream = result.audio_bitstream;

        auto stream_label = [](const model::AudioBitstreamStream& stream) {
            return "流 #" + std::to_string(stream.stream_index) + "（" + model::ToString(stream.codec) + "）";
        };

        if (rule.id == "audio.bitstream.container_mismatch") {
            double fields = 0.0;
            std::string detail;
            int stream_index = -1;
            for (const auto& stream : bitstream.streams) {
                if (!stream.container_mismatch)
                    continue;
                fields += static_cast<double>(stream.container_mismatch_fields);
                if (detail.empty()) {
                    stream_index = stream.stream_index;
                    detail =
                        stream_label(stream) + "：" +
                        (stream.inconsistencies.empty() ? std::string("声明不一致") : stream.inconsistencies.front());
                }
            }
            if (Triggered(rule, fields)) {
                issues.push_back(make_issue(fields, detail + "（共 " + FormatValue(fields, 0) + " 项参数对不上）。",
                                            model::TimeRange::Global(), stream_index, static_cast<int>(fields)));
            }
            return issues;
        }

        if (rule.id == "audio.bitstream.header_invalid") {
            int64_t parsed = 0;
            int64_t invalid = 0;
            int64_t unsupported = 0;
            for (const auto& stream : bitstream.streams) {
                parsed += stream.parsed_packets;
                invalid += stream.invalid_packets;
                unsupported += stream.unsupported_packets;
            }
            // 分母只能是"真正参与过帧头判定"的包。尚未接入逐帧解析的编码（PCM 等）
            // 既不算成功也不算失败，否则它们会被算成 100% 失败率。
            // 注：MP3 / FLAC 已有解析路径，不要沿用旧注释把它们列进来。
            const int64_t total = parsed + invalid;
            if (total <= 0)
                return issues;
            const double percent = static_cast<double>(invalid) * 100.0 / static_cast<double>(total);
            if (Triggered(rule, percent)) {
                std::string detail = std::to_string(invalid) + " / " + std::to_string(total) +
                                     " 个音频包帧头解析失败（" + FormatValue(percent, 1) + "%），阈值 " +
                                     ThresholdText(rule) + "。";
                if (unsupported > 0) {
                    detail += "另有 " + std::to_string(unsupported) + " 个包属于尚未实现逐帧解析的编码，未计入失败率。";
                }
                issues.push_back(make_issue(percent, detail, model::TimeRange::Global(), -1));
            }
            return issues;
        }

        if (rule.id == "audio.bitstream.param_change") {
            double changed = 0.0;
            std::string detail;
            int stream_index = -1;
            for (const auto& stream : bitstream.streams) {
                if (!stream.params_changed_midstream)
                    continue;
                changed += 1.0;
                if (detail.empty()) {
                    stream_index = stream.stream_index;
                    detail = stream_label(stream) + " 的参数在扫描中发生变化。";
                }
            }
            if (Triggered(rule, changed)) {
                issues.push_back(make_issue(changed, detail + "共 " + FormatValue(changed, 0) + " 条音频流受影响。",
                                            model::TimeRange::Global(), stream_index, static_cast<int>(changed)));
            }
            return issues;
        }

        if (rule.id == "audio.dialnorm.missing") {
            // 只对 AC-3 / E-AC-3 判定：AAC/Opus 本身没有 dialnorm 字段，
            // 把它们算成"缺失"是纯噪声。
            double missing = 0.0;
            std::string detail;
            int stream_index = -1;
            for (const auto& stream : bitstream.streams) {
                const bool ac3_family = (stream.codec == model::AudioBitstreamCodec::Ac3 ||
                                         stream.codec == model::AudioBitstreamCodec::Eac3);
                if (!ac3_family || stream.has_dialnorm)
                    continue;
                if (stream.parsed_packets == 0)
                    continue;
                missing += 1.0;
                if (detail.empty()) {
                    stream_index = stream.stream_index;
                    detail = stream_label(stream) + " 的头里没有 dialnorm。";
                }
            }
            if (Triggered(rule, missing)) {
                issues.push_back(
                    make_issue(missing, detail, model::TimeRange::Global(), stream_index, static_cast<int>(missing)));
            }
            return issues;
        }

        // audio.dialnorm.mismatch：需要"声明 + 实测"两方数据，没有实测响度就不判定。
        // 多音轨必须按 stream_index 配对**各自那条轨**的实测响度：拿 A 轨实测减
        // B 轨声明算出来的差值不是"声明过期"，而是把两条流混在一起。
        double worst = 0.0;
        std::string detail;
        int stream_index = -1;
        for (const auto& stream : bitstream.streams) {
            if (!stream.dialnorm_delta_available)
                continue;
            const model::AudioQcResult* measured = MeasuredQcForStream(result, stream.stream_index);
            if (measured == nullptr || !measured->analyzed)
                continue;
            if (measured->integrated_lufs <= model::kSilenceLufs + 1.0)
                continue;
            const double delta = measured->integrated_lufs - static_cast<double>(stream.dialnorm_dbfs);
            const double magnitude = std::abs(delta);
            if (magnitude <= worst)
                continue;
            worst = magnitude;
            stream_index = stream.stream_index;
            detail = stream_label(stream) + " 声明 dialnorm " + std::to_string(stream.dialnorm_dbfs) +
                     " dBFS，实测积分响度 " + FormatValue(measured->integrated_lufs, 1) + " LUFS（差 " +
                     FormatValue(delta, 1) + " LU）。";
        }
        if (Triggered(rule, worst)) {
            issues.push_back(make_issue(worst, detail, model::TimeRange::Global(), stream_index));
        }
        return issues;
    }

    // ---------- 音频 QC（core/analysis/quality/AudioQcAnalyzer.h，需解码音频）----------
    // 多音轨（2026-10-31）：以前这里只读 result.audio_qc（主轨兼容字段），于是次轨的
    // 削波/静音/相位问题在 QC 结论里完全看不见。现在按实际参与 QC 的轨道逐条评估，
    // 每条问题都带自己的 stream_index；没有逐轨数据时回退主轨，兼容单轨结果与单测。
    if (rule.id == "audio.loudness.target_high" || rule.id == "audio.loudness.target_low" ||
        rule.id == "audio.loudness.range" || rule.id == "audio.true_peak" || rule.id == "audio.clipping" ||
        rule.id == "audio.silence.longest" || rule.id == "audio.silence.ratio" || rule.id == "audio.dc_offset" ||
        rule.id == "audio.phase_correlation" || rule.id == "audio.metadata.layout" ||
        rule.id == "audio.metadata.duration_mismatch" || rule.id == "audio.qc.incomplete") {
        const model::DimensionStatus* dimension = result.FindDimension(model::AnalysisDimension::AudioQc);

        // 覆盖/失败规则要**先于** analyzed 守卫：全部解码失败时 analyzed=false，
        // 但"一条 PCM 都没拿到"本身就是最该报的不完整。以前它和响度规则共用
        // if (!qc.analyzed) return，失败被"没有足够数据判响度"顺带屏蔽了。
        if (rule.id == "audio.qc.incomplete") {
            if (!AudioQcRan(result, dimension))
                return issues;

            const auto views = AudioQcTrackViews(result);
            const bool multiple = views.size() > 1;
            double weight = 0.0;
            std::string detail;
            int first_stream = -1;
            for (const auto& view : views) {
                const auto& qc = *view.qc;
                if (!TrackQcIncomplete(qc))
                    continue;
                double part = static_cast<double>(qc.decode_error_packets + qc.decode_error_frames + qc.dropped_frames);
                if (part <= 0.0)
                    part = 1.0; // 只有覆盖率不足时也要能触发 NonZero
                weight += part;
                if (first_stream < 0)
                    first_stream = view.stream_index;
                if (!detail.empty())
                    detail += "；";
                detail += TrackPrefix(view, multiple) + "覆盖 " + FormatValue(qc.coverage_ratio * 100.0, 1) + "%（" +
                          FormatValue(qc.covered_seconds, 1) + "s），解码失败 " +
                          std::to_string(qc.decode_error_packets) + " 包 / " + std::to_string(qc.decode_error_frames) +
                          " 帧，丢弃 " + std::to_string(qc.dropped_frames) + " 帧";
            }

            const bool all_decode_failed = result.audio_qc_tracks_total > 0 && result.audio_qc_tracks_analyzed == 0;
            const bool dimension_not_complete = dimension != nullptr && dimension->executed &&
                                                (dimension->capability == model::AnalysisCapability::Failed ||
                                                 dimension->capability == model::AnalysisCapability::Partial);
            if (weight <= 0.0 && (all_decode_failed || dimension_not_complete)) {
                weight = 1.0;
                if (detail.empty()) {
                    detail = all_decode_failed ? "所有音频轨都未能解出 PCM"
                                               : (dimension->failure_reason.empty() ? "音频 QC 未完整执行"
                                                                                    : dimension->failure_reason);
                }
            }
            if (result.audio_qc_tracks_analyzed < result.audio_qc_tracks_total && weight <= 0.0) {
                weight = static_cast<double>(result.audio_qc_tracks_total - result.audio_qc_tracks_analyzed);
                detail = "只分析了 " + std::to_string(result.audio_qc_tracks_analyzed) + "/" +
                         std::to_string(result.audio_qc_tracks_total) + " 条音轨";
            }
            if (Triggered(rule, weight)) {
                issues.push_back(make_issue(weight, detail + "。峰值/响度只代表已覆盖部分。",
                                            model::TimeRange::Global(), first_stream));
            }
            return issues;
        }

        const auto views = AudioQcTrackViews(result);
        const bool multiple = views.size() > 1;
        for (const auto& view : views) {
            const auto& qc = *view.qc;
            if (!qc.analyzed)
                continue;
            const int audio_stream = view.stream_index;
            const std::string prefix = TrackPrefix(view, multiple);
            const bool has_loudness = qc.integrated_lufs > model::kSilenceLufs + 1.0;

            // 在响度曲线里定位指标最值出现的时刻（供"跳转到问题位置"）
            auto extreme_time = [&qc](auto getter, bool maximum) {
                double best = maximum ? -1e30 : 1e30;
                double ts = 0.0;
                for (const auto& p : qc.loudness_points) {
                    const double v = getter(p);
                    if ((maximum && v > best) || (!maximum && v < best)) {
                        best = v;
                        ts = p.timestamp_seconds;
                    }
                }
                return ts;
            };

            if (rule.id == "audio.loudness.target_high") {
                if (has_loudness && Triggered(rule, qc.integrated_lufs)) {
                    issues.push_back(make_issue(qc.integrated_lufs,
                                                prefix + "积分响度为 " + FormatValue(qc.integrated_lufs, 2) +
                                                    " LUFS，高于上限 " + ThresholdText(rule) + "（短期最大 " +
                                                    FormatValue(qc.short_term_max_lufs, 2) + " LUFS，瞬时最大 " +
                                                    FormatValue(qc.momentary_max_lufs, 2) + " LUFS）。",
                                                model::TimeRange::Global(), audio_stream));
                }
            } else if (rule.id == "audio.loudness.target_low") {
                if (has_loudness && Triggered(rule, qc.integrated_lufs)) {
                    issues.push_back(make_issue(qc.integrated_lufs,
                                                prefix + "积分响度为 " + FormatValue(qc.integrated_lufs, 2) +
                                                    " LUFS，低于下限 " + ThresholdText(rule) + "（短期最大 " +
                                                    FormatValue(qc.short_term_max_lufs, 2) + " LUFS）。",
                                                model::TimeRange::Global(), audio_stream));
                }
            } else if (rule.id == "audio.loudness.range") {
                if (qc.loudness_range_lu > 0.0 && Triggered(rule, qc.loudness_range_lu)) {
                    issues.push_back(make_issue(qc.loudness_range_lu,
                                                prefix + "响度动态范围 LRA 为 " + FormatValue(qc.loudness_range_lu, 1) +
                                                    " LU（P10 " + FormatValue(qc.loudness_range_low_lufs, 1) +
                                                    " LUFS，P95 " + FormatValue(qc.loudness_range_high_lufs, 1) +
                                                    " LUFS，阈值 " + ThresholdText(rule) + "）。",
                                                model::TimeRange::Global(), audio_stream));
                }
            } else if (rule.id == "audio.true_peak") {
                if (Triggered(rule, qc.true_peak_dbtp)) {
                    const double ts =
                        extreme_time([](const model::LoudnessPoint& p) { return p.true_peak_dbtp; }, true);
                    issues.push_back(make_issue(qc.true_peak_dbtp,
                                                prefix + "4× 过采样真峰值为 " + FormatValue(qc.true_peak_dbtp, 2) +
                                                    " dBTP（采样峰值 " + FormatValue(qc.sample_peak_dbfs, 2) +
                                                    " dBFS，阈值 " + ThresholdText(rule) + "）。",
                                                model::TimeRange::At(ts), audio_stream));
                }
            } else if (rule.id == "audio.clipping") {
                if (qc.clipping_sample_count > 0) {
                    const double ts = qc.clipping_events.empty() ? 0.0 : qc.clipping_events.front().start_seconds;
                    issues.push_back(
                        make_issue(static_cast<double>(qc.clipping_sample_count),
                                   prefix + std::to_string(qc.clipping_sample_count) + " 个样本达到满刻度（" +
                                       std::to_string(qc.clipping_event_count) + " 段，阈值 " + ThresholdText(rule) +
                                       "），首次出现于 " + FormatValue(ts, 2) + " 秒。",
                                   model::TimeRange::At(ts), audio_stream, static_cast<int>(qc.clipping_event_count)));
                }
            } else if (rule.id == "audio.silence.longest") {
                double longest = 0.0;
                model::TimeRange longest_range = model::TimeRange::Global();
                for (const auto& range : qc.silence_ranges) {
                    if (range.duration_seconds > longest) {
                        longest = range.duration_seconds;
                        longest_range = model::TimeRange::Between(range.start_seconds, range.end_seconds);
                    }
                }
                if (longest > 0.0 && Triggered(rule, longest)) {
                    issues.push_back(make_issue(
                        longest,
                        prefix + "最长静音段 " + FormatValue(longest, 2) + " 秒（共 " +
                            std::to_string(qc.silence_ranges.size()) + " 段，阈值 " + ThresholdText(rule) + "）。",
                        longest_range, audio_stream, static_cast<int>(qc.silence_ranges.size())));
                }
            } else if (rule.id == "audio.silence.ratio") {
                const double percent = qc.silence_ratio * 100.0;
                if (Triggered(rule, percent)) {
                    issues.push_back(make_issue(percent,
                                                prefix + "静音时长占比 " + FormatValue(percent, 1) + "%（" +
                                                    std::to_string(qc.silence_ranges.size()) + " 段，阈值 " +
                                                    ThresholdText(rule) + "）。",
                                                model::TimeRange::Global(), audio_stream));
                }
            } else if (rule.id == "audio.dc_offset") {
                if (Triggered(rule, qc.max_dc_offset)) {
                    issues.push_back(make_issue(qc.max_dc_offset,
                                                prefix + "最大直流偏移为 " + FormatValue(qc.max_dc_offset, 5) + "（" +
                                                    FormatValue(qc.max_dc_offset_dbfs, 1) + " dBFS，阈值 " +
                                                    ThresholdText(rule) + "）。",
                                                model::TimeRange::Global(), audio_stream));
                }
            } else if (rule.id == "audio.phase_correlation") {
                if (qc.correlation_available && Triggered(rule, qc.correlation_min)) {
                    const double ts = extreme_time([](const model::LoudnessPoint& p) { return p.correlation; }, false);
                    issues.push_back(make_issue(qc.correlation_min,
                                                prefix + "最差声道相关性为 " + FormatValue(qc.correlation_min, 3) +
                                                    "（反相块占比 " + FormatValue(qc.out_of_phase_ratio * 100.0, 1) +
                                                    "%，阈值 " + ThresholdText(rule) + "）。",
                                                model::TimeRange::At(ts), audio_stream));
                }
            } else if (rule.id == "audio.metadata.layout") {
                if (!qc.metadata.layout_confirmed && qc.metadata.channels > 2) {
                    issues.push_back(make_issue(static_cast<double>(qc.metadata.channels),
                                                prefix + "声道布局为 " + qc.metadata.channel_layout +
                                                    "，但容器未明确标注声道位置，响度加权按通用顺序估算。",
                                                model::TimeRange::Global(), audio_stream));
                }
            } else if (rule.id == "audio.metadata.duration_mismatch") {
                const bool container_mismatch =
                    qc.metadata.container_duration_seconds > 0.0 && std::abs(qc.metadata.container_delta_seconds) > 0.5;
                const bool video_mismatch = qc.metadata.has_video && qc.metadata.video_duration_seconds > 0.0 &&
                                            std::abs(qc.metadata.video_delta_seconds) > 0.5;
                if (container_mismatch || video_mismatch) {
                    std::string detail;
                    for (const auto& text : qc.metadata.inconsistencies) {
                        if (text.find("时长") == std::string::npos)
                            continue;
                        if (!detail.empty())
                            detail += "；";
                        detail += text;
                    }
                    if (detail.empty())
                        detail = "音频流与容器/视频时长不一致。";
                    issues.push_back(make_issue(std::max(std::abs(qc.metadata.container_delta_seconds),
                                                         std::abs(qc.metadata.video_delta_seconds)),
                                                prefix + detail + "音频 " +
                                                    FormatValue(qc.metadata.stream_duration_seconds, 2) + " 秒。",
                                                model::TimeRange::Global(), audio_stream));
                }
            }
        }
        return issues;
    }

    // audio.channel_missing 在原文件里排在 SCTE-35 段之后，但 id 属 audio.*，
    // 按前缀路由归入本 TU；逻辑与原处逐字一致。
    if (rule.id == "audio.channel_missing") {
        int missing = 0;
        int stream_index = -1;
        for (const auto& stream : result.streams) {
            if (stream.IsAudio() && stream.channels <= 0) {
                ++missing;
                if (stream_index < 0)
                    stream_index = stream.index;
            }
        }
        if (Triggered(rule, static_cast<double>(missing))) {
            issues.push_back(make_issue(static_cast<double>(missing),
                                        std::to_string(missing) + " 条音频流未标注声道数。", model::TimeRange::Global(),
                                        stream_index, missing));
        }
        return issues;
    }

    return issues;
}

} // namespace detail
} // namespace qc
} // namespace videoeye