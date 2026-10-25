#include "core/analysis/orchestration/detail/AnalysisResultAssembler.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "infrastructure/logging/Logger.h"

namespace videoeye {

AnalysisResultAssembler::AnalysisResultAssembler(model::AnalysisResult& result, const AnalysisOptions& options)
    : result_(result), options_(options) {
}

double AnalysisResultAssembler::FinalizeBuckets(const ScanBuckets& buckets, double interval_seconds) {
    if (result_.total_packets > 0) {
        result_.avg_packet_bytes =
            static_cast<double>(result_.total_bytes) / static_cast<double>(result_.total_packets);
    }
    const double measured_duration =
        (result_.duration_seconds > 0.0)
            ? result_.duration_seconds
            : (buckets.empty() ? 0.0 : (static_cast<double>(buckets.rbegin()->first) + 1.0) * interval_seconds);
    if (result_.overall_bitrate_bps <= 0 && measured_duration > 0.0) {
        result_.overall_bitrate_bps = static_cast<int64_t>(result_.total_bytes * 8 / measured_duration);
    }
    for (auto& digest : result_.streams) {
        if (digest.bitrate_bps <= 0 && digest.duration_seconds > 0.0) {
            digest.bitrate_bps = static_cast<int64_t>(digest.byte_count * 8 / digest.duration_seconds);
        }
        if (digest.duration_seconds <= 0.0)
            digest.duration_seconds = measured_duration;
    }

    const double kbps_per_byte_per_sec = 8.0 / 1000.0 / interval_seconds;
    result_.total_bitrate_kbps.name = "total_bitrate";
    result_.total_bitrate_kbps.unit = "kbps";
    result_.video_bitrate_kbps.name = "video_bitrate";
    result_.video_bitrate_kbps.unit = "kbps";
    result_.video_fps.name = "video_fps";
    result_.video_fps.unit = "fps";
    result_.total_bitrate_kbps.Reserve(buckets.size());
    result_.video_bitrate_kbps.Reserve(buckets.size());
    result_.video_fps.Reserve(buckets.size());
    for (const auto& [index, bucket] : buckets) {
        const double t = static_cast<double>(index) * interval_seconds;
        result_.total_bitrate_kbps.Add(t, static_cast<double>(bucket.total_bytes) * kbps_per_byte_per_sec);
        result_.video_bitrate_kbps.Add(t, static_cast<double>(bucket.video_bytes) * kbps_per_byte_per_sec);
        result_.video_fps.Add(t, static_cast<double>(bucket.video_frames) / interval_seconds);
    }
    // 覆盖率的公共分母：容器没给时长时这就是唯一可信的实测时长
    measured_duration_ = measured_duration;
    return measured_duration;
}

void AnalysisResultAssembler::FinalizeTimeline(TimelineAnalyzer& timeline_analyzer) {
    timeline_analyzer.Finish();
    result_.timeline = timeline_analyzer.result();
}

void AnalysisResultAssembler::FinalizeGopFrames() {
    if (result_.max_gop_frames == 0 && !result_.gop_frame_sizes.empty()) {
        result_.max_gop_frames = *std::max_element(result_.gop_frame_sizes.begin(), result_.gop_frame_sizes.end());
    }
}

void AnalysisResultAssembler::FinalizeBitrateGop(BitrateGopAnalyzer& bitrate_gop, int video_stream_index) {
    if (!options_.analyze_bitrate_gop || video_stream_index < 0)
        return;
    result_.bitrate_gop = bitrate_gop.Finish();
    LOG_INFO("码率与 GOP 分析: frames=" + std::to_string(result_.bitrate_gop.total_frames) +
             " gops=" + std::to_string(result_.bitrate_gop.gops.size()) +
             " anomalies=" + std::to_string(result_.bitrate_gop.anomalies.size()));
}

void AnalysisResultAssembler::FinalizeAudioQc(const std::vector<AudioQcTrackInput>& tracks,
                                              int primary_audio_stream_index, int audio_tracks_total) {
    if (!options_.analyze_audio_qc) {
        result_.audio_qc = model::AudioQcResult{};
        result_.audio_qc.notes.push_back("音频 QC 未启用（分析选项关闭）");
        return;
    }
    if (tracks.empty()) {
        result_.audio_qc = model::AudioQcResult{};
        if (primary_audio_stream_index < 0)
            result_.audio_qc.notes.push_back("没有音频流，跳过音频 QC");
        else
            result_.audio_qc.notes.push_back("音频解码器打开失败，未执行音频 QC");
        return;
    }

    const model::StreamDigest* video = result_.FirstVideoStream();
    result_.audio_qc_streams.clear();
    result_.audio_qc_stream_indices.clear();
    result_.audio_qc_tracks_total = audio_tracks_total;
    result_.audio_qc_tracks_analyzed = 0;

    for (const AudioQcTrackInput& track : tracks) {
        if (track.analyzer == nullptr)
            continue;
        const model::StreamDigest* audio =
            (track.stream_index >= 0 && track.stream_index < static_cast<int>(result_.streams.size()))
                ? &result_.streams[static_cast<size_t>(track.stream_index)]
                : nullptr;
        // 每条轨用自己的时长做"音频 vs 视频"偏差判定：多音轨时各轨时长本来就可以不同
        // （评论轨常常比正片短），拿主轨时长去比会造出一堆假不一致。
        track.analyzer->SetDurations(audio ? audio->duration_seconds : 0.0, result_.duration_seconds,
                                     video ? video->duration_seconds : 0.0, video != nullptr);
        model::AudioQcResult one = track.analyzer->Finish();
        if (!one.analyzed) {
            one.notes.push_back(track.decoder_ready ? "音频解码未产出 PCM，未执行音频 QC"
                                                    : "音频解码器打开失败，未执行音频 QC");
        }
        if (track.rate_changed)
            one.notes.push_back("音频采样率/声道数中途改变，部分样本未参与统计");
        if (one.analyzed)
            ++result_.audio_qc_tracks_analyzed;
        result_.audio_qc_streams.push_back(one);
        result_.audio_qc_stream_indices.push_back(track.stream_index);
    }

    // 主轨那一份单独拎出来（顺序上它就是第一个 analyzed 的；若主轨解码失败，
    // 退而从后面几条里挑第一条 —— 但 stream_index 必须与 primary 一致才认，
    // 否则宁可用第一条已分析的并标注，免得把别人的数据当主轨结论）。
    bool assigned = false;
    for (size_t i = 0; i < result_.audio_qc_streams.size(); ++i) {
        if (result_.audio_qc_stream_indices[i] == primary_audio_stream_index) {
            result_.audio_qc = result_.audio_qc_streams[i];
            assigned = true;
            break;
        }
    }
    if (!assigned && !result_.audio_qc_streams.empty()) {
        result_.audio_qc = result_.audio_qc_streams.front();
    }
    if (result_.audio_qc_tracks_total > result_.audio_qc_tracks_analyzed && result_.audio_qc_tracks_analyzed > 0) {
        result_.audio_qc.notes.push_back("只分析了 " + std::to_string(result_.audio_qc_tracks_analyzed) + "/" +
                                         std::to_string(result_.audio_qc_tracks_total) + " 条音轨");
    }
    LOG_INFO("音频 QC: " + result_.audio_qc.ToString());
    for (size_t i = 0; i < result_.audio_qc_streams.size(); ++i) {
        if (result_.audio_qc_stream_indices[i] == primary_audio_stream_index)
            continue;
        LOG_INFO("音频 QC(流#" + std::to_string(result_.audio_qc_stream_indices[i]) +
                 "): " + result_.audio_qc_streams[i].ToString());
    }
}

void AnalysisResultAssembler::FinalizeParameterTimelines(const std::vector<model::ParameterTimeline>& timelines) {
    result_.parameter_timelines = timelines;
    for (const model::ParameterTimeline& timeline : result_.parameter_timelines) {
        LOG_INFO(timeline.Summary());
    }
}

void AnalysisResultAssembler::FinalizeFrameOrder(const std::vector<model::FrameOrderTimeline>& timelines) {
    result_.frame_order_timelines = timelines;
    for (const model::FrameOrderTimeline& timeline : result_.frame_order_timelines) {
        LOG_INFO(timeline.Summary());
    }
}

void AnalysisResultAssembler::FinalizeAudioBitstream(AudioBitstreamAnalyzer& audio_bitstream, int audio_stream_index) {
    if (!options_.analyze_audio_bitstream)
        return;
    result_.audio_bitstream = audio_bitstream.Finish();
    // 没有音频流时 analyzed 已经是 false，这里只在"有流且真的跑过"时才置位 ——
    // QC 规则靠这个标志区分"没跑"和"跑了但没解析出东西"。
    result_.audio_bitstream_analyzed = result_.audio_bitstream.analyzed && !result_.audio_bitstream.streams.empty();
    if (!result_.audio_bitstream_analyzed)
        return;

    // dialnorm 与实测响度的差值必须在这里算：只有收尾时两者都在手上。
    //
    // 多音轨：每条流各自找**自己那条轨**的实测积分响度。以前固定拿主轨的响度去减
    // 任意一条 AC-3 轨的 dialnorm，第二条轨要么算不出来（被 continue 掉），
    // 要么算出来也是错的（拿 A 轨实测减 B 轨声明）。这里按 stream_index 配对，
    // 配不上就不给结论。
    for (model::AudioBitstreamStream& stream : result_.audio_bitstream.streams) {
        const model::AudioQcResult* measured = nullptr;
        for (size_t i = 0; i < result_.audio_qc_stream_indices.size() && i < result_.audio_qc_streams.size(); ++i) {
            if (result_.audio_qc_stream_indices[i] == stream.stream_index) {
                measured = &result_.audio_qc_streams[i];
                break;
            }
        }
        if (!stream.has_dialnorm || measured == nullptr || !measured->analyzed) {
            stream.dialnorm_delta_lu = 0.0;
            stream.dialnorm_delta_available = false;
            continue;
        }
        if (measured->integrated_lufs <= model::kSilenceLufs + 1.0) {
            stream.dialnorm_delta_available = false;
            continue;
        }
        // dialnorm 是"节目平均对白电平相对满刻度的偏移"，与积分响度直接相减即为
        // 交付里真正要看的偏差（dialnorm 声明 -27 而实测 -20 → +7，说明声明过期）。
        stream.dialnorm_delta_lu = measured->integrated_lufs - static_cast<double>(stream.dialnorm_dbfs);
        stream.dialnorm_delta_available = true;
        if (std::abs(stream.dialnorm_delta_lu) >= 3.0) {
            const std::string text = "dialnorm 声明 " + std::to_string(stream.dialnorm_dbfs) + " dBFS，实测积分响度 " +
                                     std::to_string(static_cast<int>(std::lround(measured->integrated_lufs))) +
                                     " LUFS，差 " +
                                     std::to_string(static_cast<int>(std::lround(stream.dialnorm_delta_lu))) + " LU";
            bool exists = false;
            for (const std::string& existing : stream.inconsistencies) {
                if (existing == text)
                    exists = true;
            }
            if (!exists)
                stream.inconsistencies.push_back(text);
        }
    }
    LOG_INFO(result_.audio_bitstream.ToString());
}

void AnalysisResultAssembler::FinalizeColorHdr(ColorHdrAnalyzer& color_hdr, int color_video_stream_index) {
    if (!options_.analyze_color_hdr || color_video_stream_index < 0)
        return;
    result_.color_hdr = color_hdr.Finish();
    LOG_INFO("色彩与 HDR: " + result_.color_hdr.ToString());
}

void AnalysisResultAssembler::FinalizeSubtitle(SubtitleAnalyzer& subtitle_analyzer, double measured_duration) {
    if (!options_.analyze_subtitle)
        return;
    // 用实测时长兜底：容器没给时长时用桶估算值，否则"超出媒体时长"会全漏
    const double cue_limit = (result_.duration_seconds > 0.0) ? result_.duration_seconds : measured_duration;
    subtitle_analyzer.Finish(cue_limit);
    result_.subtitle = subtitle_analyzer.result();
    result_.subtitle_analyzed = result_.subtitle.analyzed && !result_.subtitle.streams.empty();
    LOG_INFO("字幕分析: streams=" + std::to_string(result_.subtitle.streams.size()) +
             " cues=" + std::to_string(result_.subtitle.cues.size()) +
             " issues=" + std::to_string(result_.subtitle.issues.size()));
}

void AnalysisResultAssembler::FinalizeTimecode(TimecodeAnalyzer& timecode_analyzer) {
    if (!options_.analyze_timecode)
        return;
    timecode_analyzer.Finish();
    result_.timecode = timecode_analyzer.result();
    result_.timecode_analyzed = result_.timecode.analyzed;
    LOG_INFO("时码与章节: tracks=" + std::to_string(result_.timecode.tracks.size()) +
             " 首帧时码=" + (result_.timecode.has_primary ? result_.timecode.primary.ToString() : std::string("无")) +
             " chapters=" + std::to_string(result_.timecode.chapters.size()));
}

void AnalysisResultAssembler::FinalizeAuxData(AuxDataAnalyzer& aux_analyzer) {
    if (!options_.analyze_aux_data)
        return;
    aux_analyzer.Finish();
    result_.aux_data = aux_analyzer.result();
    result_.aux_data_analyzed = result_.aux_data.analyzed;
    LOG_INFO("辅助数据轨: streams=" + std::to_string(result_.aux_data.streams.size()) +
             " scte35=" + std::to_string(result_.aux_data.scte35_cue_count) +
             " metadata=" + std::to_string(result_.aux_data.metadata.size()));
}

void AnalysisResultAssembler::FinalizeDimensionStatus(int audio_tracks_analyzed, int audio_tracks_total) {
    result_.dimensions.clear();
    using model::AnalysisCapability;
    using model::AnalysisDimension;
    using model::DimensionStatus;

    const auto add = [this](DimensionStatus status) {
        status.MarkConclusive();
        result_.SetDimension(status);
    };

    // ---- 文件与流表：只要有结果就算跑过（失败已在更上层拦截）----
    {
        DimensionStatus status;
        status.dimension = AnalysisDimension::FileProbe;
        status.executed = true;
        status.capability = result_.streams.empty() ? AnalysisCapability::Failed : AnalysisCapability::Supported;
        status.scope_unit = "流";
        status.units_scanned = static_cast<int64_t>(result_.streams.size());
        status.units_total = static_cast<int64_t>(result_.streams.size());
        status.has_coverage = true;
        status.coverage_ratio = status.units_total > 0 ? 1.0 : 0.0;
        if (result_.streams.empty())
            status.failure_reason = "没有解析出任何流";
        add(status);
    }

    // ---- 容器结构：全文件扫描不产出这一项（结构页单独触发）----
    // 这里必须明说"未在本流程执行"，否则报告读者会把"没这一节"当成"结构没问题"。
    add(model::MakeSkippedDimension(AnalysisDimension::ContainerStructure,
                                    "容器结构在「容器结构」页单独分析，全文件扫描不产出该维度"));

    // ---- 视频码流参数（extradata / 带内）----
    if (!options_.analyze_bitstream) {
        add(model::MakeSkippedDimension(AnalysisDimension::VideoBitstream, "码流解析选项已关闭"));
    } else if (!result_.bitstream_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::VideoBitstream,
                                        "没有视频轨，或所有视频轨都没拿到参数集（extradata 为空且带内也没找到）"));
    } else {
        const model::BitstreamAnalysisResult& bs = result_.bitstream_analysis;
        DimensionStatus status;
        status.dimension = AnalysisDimension::VideoBitstream;
        status.executed = true;
        status.capability = bs.capability;
        status.failure_reason = bs.partial ? bs.capability_note : std::string();
        status.notes.push_back(bs.parameter_set_source.empty() ? "参数集来源未知"
                                                               : ("参数集来源：" + bs.parameter_set_source));
        status.stream_indices.push_back(bs.stream_index);
        // 多条视频轨时把"解析了几条"也报出来
        status.scope_unit = "轨";
        status.units_scanned = static_cast<int64_t>(result_.bitstream_streams.size());
        const int video_tracks = result_.VideoStreamCount();
        status.units_total = video_tracks > 0 ? video_tracks : status.units_scanned;
        status.has_coverage = status.units_total > 0;
        status.coverage_ratio =
            status.has_coverage ? static_cast<double>(status.units_scanned) / static_cast<double>(status.units_total)
                                : 0.0;
        if (bs.parse_outcome == model::BitstreamParseOutcome::ParseFailed)
            status.failures = 1;
        add(status);
    }

    // ---- 帧级结构（slice / SEI 巡检）----
    {
        const model::SliceSeiSummary& slice = result_.bitstream_analysis.slice_sei;
        if (!options_.analyze_bitstream) {
            add(model::MakeSkippedDimension(AnalysisDimension::FrameStructure, "码流解析选项已关闭"));
        } else if (!slice.analyzed) {
            add(model::MakeSkippedDimension(AnalysisDimension::FrameStructure,
                                            "没有视频轨，或巡检器未启用（编码不在支持范围内）"));
        } else {
            DimensionStatus status;
            status.dimension = AnalysisDimension::FrameStructure;
            status.executed = true;
            status.capability = slice.truncated ? AnalysisCapability::Partial : AnalysisCapability::Supported;
            status.scope_unit = "包";
            status.units_scanned = slice.packets_scanned;
            if (slice.truncated) {
                status.failure_reason = "已到扫描窗口上限，只覆盖前 " + std::to_string(slice.packets_scanned) +
                                        " 个视频包（这一点不影响「参数集」结论，但帧级统计只代表该窗口）";
            }
            status.notes.push_back(slice.Summary());
            add(status);
        }
    }

    // ---- 帧级显示顺序（POC，仅 H.264）----
    //
    // 与"参数变化时间线"是同一个包扫描循环里产出的两类结论，但覆盖判据不同：
    // 那里按包计覆盖，这里按**帧**计 —— 帧数能从 POC 时间线直接数出来，
    // 总量拿不到（容器不给"总帧数"这个事实）时就不给覆盖率，不用 0 冒充全覆盖。
    if (!options_.analyze_frame_order) {
        add(model::MakeSkippedDimension(AnalysisDimension::FrameOrder, "帧级顺序追踪选项已关闭"));
    } else if (result_.frame_order_timelines.empty()) {
        add(model::MakeSkippedDimension(AnalysisDimension::FrameOrder,
                                        "没有 H.264 视频轨（HEVC/AV1/VP9/VVC 的 POC 尚未实现）"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::FrameOrder;
        status.executed = true;
        status.scope_unit = "帧";
        int64_t frames = 0;
        int64_t unavailable = 0;
        int64_t duplicates = 0;
        int64_t gaps = 0;
        int64_t reorders = 0;
        bool truncated = false;
        for (const model::FrameOrderTimeline& timeline : result_.frame_order_timelines) {
            status.stream_indices.push_back(timeline.stream_index);
            if (!timeline.available) {
                ++unavailable;
                if (!timeline.unavailable_reason.empty() && status.failure_reason.empty())
                    status.failure_reason = timeline.unavailable_reason;
                continue;
            }
            frames += timeline.frames_observed;
            duplicates += timeline.duplicate_poc_count;
            gaps += timeline.poc_gap_slots;
            reorders += timeline.reorder_events;
            truncated = truncated || timeline.truncated;
            status.notes.push_back("流 #" + std::to_string(timeline.stream_index) + "：" + timeline.Summary());
        }
        status.units_scanned = frames;
        status.capability =
            (unavailable > 0 || truncated) ? AnalysisCapability::Partial : AnalysisCapability::Supported;
        // POC 重复与缺帧都是"内容层面的事实异常"，计进 failures 让结论不能当通过。
        // 重排（B 帧）不算：那是正常编码结构。
        status.failures = duplicates + gaps;
        if (duplicates > 0)
            status.notes.push_back("有 " + std::to_string(duplicates) + " 处 POC 重复：同一幅图像被写入多次");
        if (gaps > 0)
            status.notes.push_back("按 POC 步长推断共缺 " + std::to_string(gaps) + " 帧");
        if (reorders > 0)
            status.notes.push_back("存在显示顺序重排（B 帧）" + std::to_string(reorders) +
                                   " 处 —— 这是正常结构，不是缺陷");
        if (truncated && status.failure_reason.empty())
            status.failure_reason = "已达逐帧明细上限，统计量仍覆盖全程，但明细只留了前面一段";
        add(status);
    }

    // ---- 参数变化时间线 ----
    if (!options_.analyze_parameter_timeline) {
        add(model::MakeSkippedDimension(AnalysisDimension::ParameterTimeline, "参数变化追踪选项已关闭"));
    } else if (result_.parameter_timelines.empty()) {
        add(model::MakeSkippedDimension(AnalysisDimension::ParameterTimeline,
                                        "没有可追踪的视频轨（需要 H.264 / HEVC / AV1）"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::ParameterTimeline;
        status.executed = true;
        status.capability = AnalysisCapability::Supported;
        status.scope_unit = "包";
        int64_t scanned = 0;
        int64_t changes = 0;
        int64_t missing = 0;
        bool truncated = false;
        for (const model::ParameterTimeline& timeline : result_.parameter_timelines) {
            scanned += timeline.packets_scanned;
            changes += static_cast<int64_t>(timeline.changes.size());
            missing += timeline.missing_reference_count;
            truncated = truncated || timeline.truncated;
            status.stream_indices.push_back(timeline.stream_index);
            for (const std::string& note : timeline.notes)
                status.notes.push_back(note);
        }
        status.units_scanned = scanned;
        status.failures = missing;
        if (changes > 0)
            status.notes.push_back("检测到 " + std::to_string(changes) + " 次参数集配置变化");
        if (missing > 0)
            status.failure_reason = "有帧引用了从未传输的参数集（解码必然失败）";
        if (truncated) {
            status.capability = AnalysisCapability::Partial;
            status.failure_reason = status.failure_reason.empty() ? "已到追踪上限，只覆盖前面一段"
                                                                  : status.failure_reason + "；且已到追踪上限";
        }
        add(status);
    }

    // ---- 码率与 GOP ----
    if (!options_.analyze_bitrate_gop || result_.VideoStreamCount() <= 0) {
        add(model::MakeSkippedDimension(AnalysisDimension::BitrateGop, "没有视频轨或选项已关闭"));
    } else {
        const model::BitrateGopAnalysis& gop = result_.bitrate_gop;
        DimensionStatus status;
        status.dimension = AnalysisDimension::BitrateGop;
        status.executed = true;
        status.capability =
            (gop.frame_types_known && !gop.downsampled) ? AnalysisCapability::Supported : AnalysisCapability::Partial;
        status.scope_unit = "帧";
        status.units_scanned = gop.total_frames;
        // 预期帧数：实测时长 × 实测帧率。拿不到帧率就不给覆盖率（不要用 0 冒充全覆盖）。
        const model::StreamDigest* video = result_.FirstVideoStream();
        if (video != nullptr && video->avg_fps > 0.0 && measured_duration_ > 0.0) {
            status.units_total = static_cast<int64_t>(video->avg_fps * measured_duration_ + 0.5);
        }
        status.has_coverage = status.units_total > 0;
        if (status.has_coverage && status.units_total > 0) {
            status.coverage_ratio =
                std::min(1.0, static_cast<double>(status.units_scanned) / static_cast<double>(status.units_total));
        }
        if (!gop.frame_types_known)
            status.failure_reason = "帧类型未知（只有 I 帧可信），开启「解码帧类型」可提高准确度";
        else if (gop.downsampled)
            status.failure_reason = "样本数超过上限，做过有界降采样";
        else if (gop.unassociated_bytes > 0)
            status.notes.push_back("有 " + std::to_string(gop.unassociated_bytes) + " 字节未能关联到帧");
        add(status);
    }

    // ---- 色彩与 HDR ----
    if (!options_.analyze_color_hdr) {
        add(model::MakeSkippedDimension(AnalysisDimension::ColorHdr, "色彩与 HDR 选项已关闭"));
    } else if (!result_.color_hdr.analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::ColorHdr, "没有视频轨，或容器未提供色彩字段"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::ColorHdr;
        status.executed = true;
        status.capability = result_.color_hdr.hdr.analyzed || result_.color_hdr.color.analyzed
                                ? AnalysisCapability::Supported
                                : AnalysisCapability::Partial;
        status.stream_indices.push_back(result_.color_hdr.stream_index);
        for (const std::string& note : result_.color_hdr.notes)
            status.notes.push_back(note);
        if (!result_.color_hdr.StaticMetadataComplete()) {
            status.failure_reason = "HDR 静态元数据不完整（可能只有色彩字段，没有 MasteringDisplay/MaxCLL）";
        }
        add(status);
    }

    // ---- 音频 QC（多音轨：覆盖 M/N 条）----
    if (!options_.analyze_audio_qc) {
        add(model::MakeSkippedDimension(AnalysisDimension::AudioQc, "音频 QC 选项已关闭"));
    } else if (audio_tracks_total <= 0) {
        add(model::MakeSkippedDimension(AnalysisDimension::AudioQc, "没有音频流"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::AudioQc;
        status.executed = true;
        status.scope_unit = "轨";
        status.units_scanned = audio_tracks_analyzed;
        status.units_total = audio_tracks_total;
        status.has_coverage = true;
        status.coverage_ratio = static_cast<double>(audio_tracks_analyzed) / static_cast<double>(audio_tracks_total);
        // 能力评级看两件事：是否所有轨都分析到了，以及主轨的 PCM 覆盖率
        const bool all_tracks = (audio_tracks_analyzed == audio_tracks_total);
        const bool full_pcm = result_.audio_qc.analyzed && result_.audio_qc.coverage_ratio >= 0.99;
        if (audio_tracks_analyzed == 0) {
            status.capability = AnalysisCapability::Failed;
            status.failures = 1;
            status.failure_reason = "所有音轨都未能解出 PCM";
        } else if (all_tracks && full_pcm) {
            status.capability = AnalysisCapability::Supported;
        } else {
            status.capability = AnalysisCapability::Partial;
            status.failure_reason = !all_tracks ? ("只分析了 " + std::to_string(audio_tracks_analyzed) + "/" +
                                                   std::to_string(audio_tracks_total) + " 条音轨")
                                                : "部分音频样本未参与统计（见 coverage）";
        }
        status.failures += result_.audio_qc.decode_error_packets + result_.audio_qc.decode_error_frames +
                           result_.audio_qc.dropped_frames;
        if (status.failures > 0)
            status.notes.push_back("解码失败/丢帧合计 " + std::to_string(status.failures));
        for (size_t i = 0; i < result_.audio_qc_stream_indices.size(); ++i)
            status.stream_indices.push_back(result_.audio_qc_stream_indices[i]);
        add(status);
    }

    // ---- 音频位流参数 ----
    if (!options_.analyze_audio_bitstream) {
        add(model::MakeSkippedDimension(AnalysisDimension::AudioBitstream, "音频位流分析选项已关闭"));
    } else if (!result_.audio_bitstream_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::AudioBitstream, "没有音频流"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::AudioBitstream;
        status.executed = true;
        status.scope_unit = "轨";
        status.units_scanned = static_cast<int64_t>(result_.audio_bitstream.streams.size());
        status.units_total = status.units_scanned;
        status.has_coverage = true;
        status.coverage_ratio = 1.0;
        int64_t invalid = 0;
        int64_t unsupported = 0;
        int64_t mismatched = 0;
        for (const model::AudioBitstreamStream& stream : result_.audio_bitstream.streams) {
            invalid += stream.invalid_packets;
            unsupported += stream.unsupported_packets;
            if (stream.container_mismatch)
                ++mismatched;
            status.stream_indices.push_back(stream.stream_index);
        }
        status.failures = invalid;
        // 全部轨道都是"未实现逐帧解析"的编码时只能算 Partial：容器参数有了，
        // 但"码流自己声明什么"这个核心问题没答。
        if (result_.audio_bitstream.parsed_stream_count == 0 && unsupported > 0) {
            status.capability = AnalysisCapability::Partial;
            status.failure_reason = "该编码尚未实现逐帧位流头解析，只做了容器参数登记";
        } else if (invalid > 0) {
            status.capability = AnalysisCapability::Partial;
            status.failure_reason = std::to_string(invalid) + " 个包帧头校验失败";
        } else {
            status.capability = AnalysisCapability::Supported;
        }
        if (mismatched > 0)
            status.notes.push_back(std::to_string(mismatched) + " 条音轨的位流声明与容器声明不一致");
        for (const std::string& note : result_.audio_bitstream.notes)
            status.notes.push_back(note);
        add(status);
    }

    // ---- 字幕 / 时码 / 辅助数据 ----
    if (!options_.analyze_subtitle) {
        add(model::MakeSkippedDimension(AnalysisDimension::Subtitle, "字幕分析选项已关闭"));
    } else if (!result_.subtitle_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::Subtitle, "没有字幕流"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::Subtitle;
        status.executed = true;
        status.capability =
            result_.subtitle.error_message.empty() ? AnalysisCapability::Supported : AnalysisCapability::Partial;
        status.scope_unit = "轨";
        status.units_scanned = static_cast<int64_t>(result_.subtitle.streams.size());
        status.units_total = status.units_scanned;
        status.has_coverage = true;
        status.coverage_ratio = 1.0;
        status.failure_reason = result_.subtitle.error_message;
        add(status);
    }
    if (!options_.analyze_timecode) {
        add(model::MakeSkippedDimension(AnalysisDimension::Timecode, "时码分析选项已关闭"));
    } else if (!result_.timecode_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::Timecode, "没有时码轨或章节"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::Timecode;
        status.executed = true;
        status.capability =
            result_.timecode.error_message.empty() ? AnalysisCapability::Supported : AnalysisCapability::Partial;
        status.failure_reason = result_.timecode.error_message;
        add(status);
    }
    if (!options_.analyze_aux_data) {
        add(model::MakeSkippedDimension(AnalysisDimension::AuxiliaryData, "辅助数据分析选项已关闭"));
    } else if (!result_.aux_data_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::AuxiliaryData, "没有 data/metadata 轨"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::AuxiliaryData;
        status.executed = true;
        status.capability =
            result_.aux_data.parse_error_count > 0 ? AnalysisCapability::Partial : AnalysisCapability::Supported;
        status.failures = result_.aux_data.parse_error_count;
        status.failure_reason = result_.aux_data.error_message;
        add(status);
    }

    // ---- MP4 样本表 ----
    if (!options_.analyze_mp4_sample_table) {
        add(model::MakeSkippedDimension(AnalysisDimension::Mp4SampleTable, "MP4 样本表校验选项已关闭"));
    } else if (!result_.mp4_samples_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::Mp4SampleTable, "不是 MP4 家族容器"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::Mp4SampleTable;
        status.executed = true;
        status.capability = result_.mp4_samples.valid ? AnalysisCapability::Supported : AnalysisCapability::Failed;
        status.scope_unit = "轨";
        status.units_scanned = static_cast<int64_t>(result_.mp4_samples.tracks.size());
        status.units_total = status.units_scanned;
        status.has_coverage = true;
        status.coverage_ratio = 1.0;
        if (!result_.mp4_samples.valid) {
            status.failure_reason = result_.mp4_samples.error_message;
            status.failures = 1;
        }
        for (const model::Mp4TrackSampleTable& track : result_.mp4_samples.tracks) {
            if (track.samples_truncated)
                status.notes.push_back("轨 " + std::to_string(track.track_id) + " 的样本表被上限截断");
        }
        add(status);
    }

    // ---- MPEG-TS 传输层 ----
    if (!options_.analyze_ts_transport) {
        add(model::MakeSkippedDimension(AnalysisDimension::TsTransport, "TS 传输层体检选项已关闭"));
    } else if (!result_.ts_transport.applicable) {
        add(model::MakeSkippedDimension(AnalysisDimension::TsTransport, "不是 MPEG-TS 容器"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::TsTransport;
        status.executed = result_.ts_transport.analyzed;
        status.capability =
            !result_.ts_transport.analyzed
                ? AnalysisCapability::Failed
                : (result_.ts_transport.scan_complete ? AnalysisCapability::Supported : AnalysisCapability::Partial);
        status.scope_unit = "包";
        status.units_scanned = static_cast<int64_t>(result_.ts_transport.scanned_packets);
        status.units_total = static_cast<int64_t>(result_.ts_transport.estimated_packets);
        status.has_coverage = status.units_total > 0;
        if (status.has_coverage && status.units_total > 0)
            status.coverage_ratio =
                std::min(1.0, static_cast<double>(status.units_scanned) / static_cast<double>(status.units_total));
        status.failures = static_cast<int64_t>(result_.ts_transport.sync_loss + result_.ts_transport.continuity_errors);
        status.failure_reason = result_.ts_transport.error_message;
        add(status);
    }

    // ---- 流媒体包 ----
    if (!options_.analyze_streaming_package) {
        add(model::MakeSkippedDimension(AnalysisDimension::Streaming, "流媒体包分析选项已关闭"));
    } else if (!result_.streaming_analyzed) {
        add(model::MakeSkippedDimension(AnalysisDimension::Streaming, "输入不是 HLS/DASH 清单"));
    } else {
        DimensionStatus status;
        status.dimension = AnalysisDimension::Streaming;
        status.executed = true;
        status.capability = !result_.streaming_package.valid      ? AnalysisCapability::Failed
                            : result_.streaming_package.truncated ? AnalysisCapability::Partial
                                                                  : AnalysisCapability::Supported;
        status.scope_unit = "码率层";
        status.units_scanned = static_cast<int64_t>(result_.streaming_package.ladder.size());
        status.units_total = status.units_scanned;
        status.has_coverage = true;
        status.coverage_ratio = 1.0;
        status.failure_reason = result_.streaming_package.error_message;
        if (result_.streaming_package.truncated)
            status.notes.push_back("清单读取被截断（事件/分片列表不完整）");
        add(status);
    }

    // ---- 老式容器体检 ----
    {
        const model::ContainerDiagnosticsInfo& diag = result_.container_diagnostics;
        const bool any =
            diag.avi.analyzed || diag.flv.analyzed || diag.asf.analyzed || diag.ogg.analyzed || diag.mkv.analyzed;
        if (!any) {
            add(model::MakeSkippedDimension(AnalysisDimension::LegacyContainer,
                                            "不是 AVI/FLV/ASF/OGG/MKV 容器，或该格式无需额外体检"));
        } else {
            DimensionStatus status;
            status.dimension = AnalysisDimension::LegacyContainer;
            status.executed = true;
            status.capability = AnalysisCapability::Supported;
            status.scope_unit = "文件";
            status.units_scanned = 1;
            status.units_total = 1;
            status.has_coverage = true;
            status.coverage_ratio = 1.0;
            const bool complete =
                (!diag.avi.analyzed || diag.avi.scan_complete) && (!diag.flv.analyzed || diag.flv.scan_complete) &&
                (!diag.asf.analyzed || diag.asf.scan_complete) && (!diag.ogg.analyzed || diag.ogg.scan_complete) &&
                (!diag.mkv.analyzed || diag.mkv.scan_complete);
            if (!complete) {
                status.capability = AnalysisCapability::Partial;
                status.failure_reason = "结构扫描未跑完（文件过大或中途遇到畸形数据）";
            }
            add(status);
        }
    }

    // 所有维度都写完之后再回填：executed 是权威口径，历史布尔跟着它走。
    SyncLegacyDimensionFlags();
}

void AnalysisResultAssembler::SyncLegacyDimensionFlags() {
    using model::AnalysisDimension;
    result_.bitstream_analyzed = result_.DimensionExecuted(AnalysisDimension::VideoBitstream);
    result_.audio_bitstream_analyzed = result_.DimensionExecuted(AnalysisDimension::AudioBitstream);
    result_.subtitle_analyzed = result_.DimensionExecuted(AnalysisDimension::Subtitle);
    result_.timecode_analyzed = result_.DimensionExecuted(AnalysisDimension::Timecode);
    result_.aux_data_analyzed = result_.DimensionExecuted(AnalysisDimension::AuxiliaryData);
    result_.mp4_samples_analyzed = result_.DimensionExecuted(AnalysisDimension::Mp4SampleTable);
    result_.streaming_analyzed = result_.DimensionExecuted(AnalysisDimension::Streaming);
}

void AnalysisResultAssembler::Finish(const AnalysisCallbacks& callbacks) {
    result_.scanned_packets = result_.total_packets;
    const bool completed = result_.scan_status != model::AnalysisStatus::Cancelled;
    LOG_INFO("全文件分析完成: packets=" + std::to_string(result_.total_packets) + " duration=" +
             std::to_string(result_.duration_seconds) + " status=" + std::string(ToString(result_.scan_status)));
    // 取消路径也会走到这里（break 之后各分析器照常收尾），进度文案得跟着状态走。
    NotifyProgress(callbacks, 100.0, completed ? "分析完成" : "已取消");
    // 第二个参数沿用旧语义（true = 到达终态而非被取消），Failed 不会走到这里。
    NotifyFinished(callbacks, completed, result_);
}

} // namespace videoeye
