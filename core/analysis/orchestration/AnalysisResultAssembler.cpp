#include "core/analysis/orchestration/AnalysisResultAssembler.h"

#include <algorithm>
#include <string>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace analyzer {

AnalysisResultAssembler::AnalysisResultAssembler(model::AnalysisResult& result,
                                                 const AnalysisOptions& options)
    : result_(result), options_(options) {}

double AnalysisResultAssembler::FinalizeBuckets(const ScanBuckets& buckets, double interval_seconds) {
    if (result_.total_packets > 0) {
        result_.avg_packet_bytes = static_cast<double>(result_.total_bytes) /
                                   static_cast<double>(result_.total_packets);
    }
    const double measured_duration =
        (result_.duration_seconds > 0.0)
            ? result_.duration_seconds
            : (buckets.empty() ? 0.0
                               : (static_cast<double>(buckets.rbegin()->first) + 1.0) * interval_seconds);
    if (result_.overall_bitrate_bps <= 0 && measured_duration > 0.0) {
        result_.overall_bitrate_bps = static_cast<int64_t>(result_.total_bytes * 8 / measured_duration);
    }
    for (auto& digest : result_.streams) {
        if (digest.bitrate_bps <= 0 && digest.duration_seconds > 0.0) {
            digest.bitrate_bps = static_cast<int64_t>(digest.byte_count * 8 / digest.duration_seconds);
        }
        if (digest.duration_seconds <= 0.0) digest.duration_seconds = measured_duration;
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
    return measured_duration;
}

void AnalysisResultAssembler::FinalizeTimeline(TimelineAnalyzer& timeline_analyzer) {
    timeline_analyzer.Finish();
    result_.timeline = timeline_analyzer.result();
}

void AnalysisResultAssembler::FinalizeGopFrames() {
    if (result_.max_gop_frames == 0 && !result_.gop_frame_sizes.empty()) {
        result_.max_gop_frames = *std::max_element(result_.gop_frame_sizes.begin(),
                                                   result_.gop_frame_sizes.end());
    }
}

void AnalysisResultAssembler::FinalizeBitrateGop(BitrateGopAnalyzer& bitrate_gop,
                                                 int video_stream_index) {
    if (!options_.analyze_bitrate_gop || video_stream_index < 0) return;
    result_.bitrate_gop = bitrate_gop.Finish();
    LOG_INFO("码率与 GOP 分析: frames=" + std::to_string(result_.bitrate_gop.total_frames) +
             " gops=" + std::to_string(result_.bitrate_gop.gops.size()) +
             " anomalies=" + std::to_string(result_.bitrate_gop.anomalies.size()));
}

void AnalysisResultAssembler::FinalizeAudioQc(AudioQcAnalyzer& audio_qc, int audio_stream_index,
                                              bool audio_decoder_ready, bool audio_rate_changed) {
    if (!options_.analyze_audio_qc || audio_stream_index < 0) return;
    const model::StreamDigest* audio =
        (audio_stream_index < static_cast<int>(result_.streams.size()))
            ? &result_.streams[static_cast<size_t>(audio_stream_index)]
            : nullptr;
    const model::StreamDigest* video = result_.FirstVideoStream();
    audio_qc.SetDurations(audio ? audio->duration_seconds : 0.0, result_.duration_seconds,
                          video ? video->duration_seconds : 0.0, video != nullptr);
    result_.audio_qc = audio_qc.Finish();
    if (!result_.audio_qc.analyzed) {
        result_.audio_qc.notes.push_back(audio_decoder_ready
                                             ? "音频解码未产出 PCM，未执行音频 QC"
                                             : "音频解码器打开失败，未执行音频 QC");
    }
    if (audio_rate_changed) {
        result_.audio_qc.notes.push_back("音频采样率/声道数中途改变，部分样本未参与统计");
    }
    LOG_INFO("音频 QC: " + result_.audio_qc.ToString());
}

void AnalysisResultAssembler::FinalizeColorHdr(ColorHdrAnalyzer& color_hdr,
                                               int color_video_stream_index) {
    if (!options_.analyze_color_hdr || color_video_stream_index < 0) return;
    result_.color_hdr = color_hdr.Finish();
    LOG_INFO("色彩与 HDR: " + result_.color_hdr.ToString());
}

void AnalysisResultAssembler::FinalizeSubtitle(SubtitleAnalyzer& subtitle_analyzer,
                                               double measured_duration) {
    if (!options_.analyze_subtitle) return;
    // 用实测时长兜底：容器没给时长时用桶估算值，否则"超出媒体时长"会全漏
    const double cue_limit = (result_.duration_seconds > 0.0) ? result_.duration_seconds
                                                              : measured_duration;
    subtitle_analyzer.Finish(cue_limit);
    result_.subtitle = subtitle_analyzer.result();
    result_.subtitle_analyzed = result_.subtitle.analyzed && !result_.subtitle.streams.empty();
    LOG_INFO("字幕分析: streams=" + std::to_string(result_.subtitle.streams.size()) +
             " cues=" + std::to_string(result_.subtitle.cues.size()) +
             " issues=" + std::to_string(result_.subtitle.issues.size()));
}

void AnalysisResultAssembler::FinalizeTimecode(TimecodeAnalyzer& timecode_analyzer) {
    if (!options_.analyze_timecode) return;
    timecode_analyzer.Finish();
    result_.timecode = timecode_analyzer.result();
    result_.timecode_analyzed = result_.timecode.analyzed;
    LOG_INFO("时码与章节: tracks=" + std::to_string(result_.timecode.tracks.size()) +
             " 首帧时码=" + (result_.timecode.has_primary ? result_.timecode.primary.ToString()
                                                          : std::string("无")) +
             " chapters=" + std::to_string(result_.timecode.chapters.size()));
}

void AnalysisResultAssembler::FinalizeAuxData(AuxDataAnalyzer& aux_analyzer) {
    if (!options_.analyze_aux_data) return;
    aux_analyzer.Finish();
    result_.aux_data = aux_analyzer.result();
    result_.aux_data_analyzed = result_.aux_data.analyzed;
    LOG_INFO("辅助数据轨: streams=" + std::to_string(result_.aux_data.streams.size()) +
             " scte35=" + std::to_string(result_.aux_data.scte35_cue_count) +
             " metadata=" + std::to_string(result_.aux_data.metadata.size()));
}

void AnalysisResultAssembler::Finish(const AnalysisCallbacks& callbacks) {
    result_.scanned_packets = result_.total_packets;
    const bool completed = result_.scan_status != model::AnalysisStatus::Cancelled;
    LOG_INFO("全文件分析完成: packets=" + std::to_string(result_.total_packets) +
             " duration=" + std::to_string(result_.duration_seconds) +
             " status=" + std::string(ToString(result_.scan_status)));
    // 取消路径也会走到这里（break 之后各分析器照常收尾），进度文案得跟着状态走。
    NotifyProgress(callbacks, 100.0, completed ? "分析完成" : "已取消");
    // 第二个参数沿用旧语义（true = 到达终态而非被取消），Failed 不会走到这里。
    NotifyFinished(callbacks, completed, result_);
}

}  // namespace analyzer
}  // namespace videoeye
