#include "core/repair/RepairInspector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/media/container/IsobmffParser.h"
#include "core/media/probe/FileProbe.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairCapabilityContracts.h"
#include "core/repair/RepairFragmentForensics.h"
#include "core/repair/RepairInitSegmentBuilder.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairPlainMp4Rebuilder.h"
#include "core/repair/RepairReferenceMoovRebuilder.h"
#include "core/repair/RepairSampleBounds.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"
#include "infrastructure/logging/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

namespace videoeye {
namespace repair {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kInspector = "repair.inspector";

// R10：优先使用引擎入口创建的共享截止时间；独立调用（无 budget）时退回取消标志。
bool CheckStop(const std::atomic<bool>* cancel, const RepairRunBudget* budget) {
    return budget != nullptr ? budget->Checkpoint() : infrastructure::Checkpoint(cancel);
}

int64_t ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string AvErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void Emit(const RepairProgress& progress, int percent, const char* stage, const std::string& detail) {
    if (progress)
        progress(percent, stage, detail);
}

void AddCheck(model::RepairDiagnosis& diagnosis, const std::string& name, model::DamageLayer layer,
              model::RepairCheckStatus status, const std::string& detail, int64_t cost_ms = 0) {
    model::RepairCheck check;
    check.name = name;
    check.layer = layer;
    check.status = status;
    check.detail = detail;
    check.cost_ms = static_cast<uint64_t>(std::max<int64_t>(0, cost_ms));
    diagnosis.checks.push_back(std::move(check));
}

model::RepairCheckStatus WorstStatus(const std::vector<model::RepairCheck>& checks) {
    model::RepairCheckStatus worst = model::RepairCheckStatus::NotRun;
    for (const auto& check : checks) {
        if (check.status == model::RepairCheckStatus::Failed)
            return model::RepairCheckStatus::Failed;
        if (check.status == model::RepairCheckStatus::Inconclusive)
            worst = model::RepairCheckStatus::Inconclusive;
        else if (check.status == model::RepairCheckStatus::Unsupported && worst == model::RepairCheckStatus::NotRun)
            worst = model::RepairCheckStatus::Unsupported;
        else if (check.status == model::RepairCheckStatus::Passed && worst == model::RepairCheckStatus::NotRun)
            worst = model::RepairCheckStatus::Passed;
    }
    return worst;
}

model::RepairInputSnapshot SnapshotFile(const std::string& path, const RepairRunBudget* budget) {
    // size + mtime + 内容摘要一起取；只比大小挡不住同大小原位改写（R08）。
    // 摘要是分块读，把共享预算传进去，取消时不能把「没算完」当成「内容没变」。
    model::RepairInputSnapshot snapshot = RepairOutputWriter::MakeSnapshot(path, budget);
    const FileHeaderInfo header = ProbeFileHeader(path);
    snapshot.signature = header.first_box_type;
    return snapshot;
}

bool IsKnownIsobmffBox(const std::string& type) {
    static const std::set<std::string> kBoxes = {
        "ftyp", "moov", "mdat", "free", "skip", "wide", "styp", "moof", "sidx", "meta",
    };
    return kBoxes.count(type) > 0;
}

// 扩展名只代表"值得尝试解析 ISOBMFF"，不代表文件真的就是 ISOBMFF。
bool LooksLikeIsobmff(const std::string& path, const std::string& first_box) {
    if (!first_box.empty() && IsKnownIsobmffBox(first_box))
        return true;
    const std::string ext = LowerExtension(path);
    return ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" || ext == "3gp" || ext == "3g2";
}

bool HasMediaStream(const AVFormatContext* fmt) {
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVMediaType type = fmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            return true;
    }
    return false;
}

// 容器轨道 id → FFmpeg 流下标。用于把「哪条轨不可抢救」映射到执行层可剔除的流。
int FindInputStreamByTrackId(const AVFormatContext* fmt, uint32_t track_id) {
    if (!fmt)
        return -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i] && fmt->streams[i]->id == static_cast<int64_t>(track_id))
            return static_cast<int>(i);
    }
    return -1;
}

int PickTargetStream(const AVFormatContext* fmt, const std::vector<int>& requested) {
    for (int index : requested) {
        if (index >= 0 && index < static_cast<int>(fmt->nb_streams))
            return index;
    }
    int video = av_find_best_stream(const_cast<AVFormatContext*>(fmt), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video >= 0)
        return video;
    int audio = av_find_best_stream(const_cast<AVFormatContext*>(fmt), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio >= 0)
        return audio;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVMediaType type = fmt->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            return static_cast<int>(i);
    }
    return -1;
}

// ---------------- 缺初始化段的分片序列取证 ----------------
//
// 只在"没有 moov 但确实是媒体分片序列"时调用。它把逐段实测结果翻译成
// 证据与检查项，并把源侧样本数写进 expected_tracks 供执行后做守恒校验。
// 取证不改变业务结论 —— 可修 / 不可修仍由 RepairPlanner 决定。
bool ApplyFragmentForensics(const model::RepairRequest& request, const IsobmffFile& parsed,
                            const std::atomic<bool>* cancel, const RepairProgress& progress,
                            const RepairRunBudget* budget, model::RepairDiagnosis& diagnosis,
                            RepairSourceCompatibilityEvidence* source_evidence,
                            RepairFragmentEvidence* fragment_evidence) {
    const auto start = Clock::now();
    Emit(progress, 52, stage::Inspecting(), "取证媒体分片结构与码流参数集");
    const RepairFragmentEvidence ev = RepairFragmentForensics::Analyze(request.source_path, parsed, cancel, budget);
    if (fragment_evidence != nullptr)
        *fragment_evidence = ev;
    if (!ev.analyzed) {
        AddCheck(diagnosis, "container.fragment_forensics", model::DamageLayer::Container,
                 model::RepairCheckStatus::Failed, ev.error, ElapsedMs(start));
        return false;
    }

    // 源侧样本数：只有从 moof/trun 逐段数出来的才写，估算值不入账。
    uint64_t declared_samples = 0;
    for (const auto& track : ev.tracks) {
        if (track.sample_count == 0)
            continue;
        declared_samples += track.sample_count;
        model::RepairExpectedTrack expected;
        expected.track_id = track.track_id;
        expected.sample_count = track.sample_count;
        expected.codec_hint = track.bitstream;
        expected.provenance = "moof/trun (" + std::to_string(ev.segment_count) + " segments)";
        diagnosis.expected_tracks.push_back(std::move(expected));
    }

    // 覆盖范围是实测值：不能把"顶层盒大小之和"当成"已扫描字节"。
    model::RepairCoverage& coverage = diagnosis.coverage;
    coverage.bytes_scanned = ev.bytes_scanned;
    coverage.packets_scanned = ev.samples_scanned;
    coverage.frames_scanned = 0; // 容器未打开，没有解码帧
    coverage.time_scanned_seconds = ev.estimated_duration_seconds;
    coverage.reached_eof = ev.reached_eof;
    coverage.truncated_by_budget = !ev.reached_eof;
    coverage.skipped_ranges.push_back("NAL 载荷内部字节（只读取长度前缀与 NAL 头）");
    for (const auto& limitation : ev.limitations)
        coverage.limitations.push_back(limitation);

    // 结构证据（先总览，再逐轨给解码参数实测）。
    {
        model::DamageFinding finding;
        finding.code = "container.fragment_sequence";
        finding.detector = kInspector;
        finding.layer = model::DamageLayer::Container;
        finding.severity = model::IssueSeverity::Warning;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Exact;
        finding.location.provenance = "isobmff.top_level + moof/trun + sidx";
        std::ostringstream text;
        text << "媒体分片 " << ev.segment_count << " 段（styp " << ev.styp_count << " / sidx " << ev.sidx_count
             << " / moof " << ev.moof_count << " / mdat " << ev.mdat_count << "），轨道 " << ev.tracks.size()
             << " 条，声明样本 " << declared_samples << " 个，sidx 时间基 " << ev.sidx_timescale << "，估算时长 "
             << FormatSeconds(ev.estimated_duration_seconds) << " s，不完整分片 " << ev.incomplete_segments << " 段";
        finding.evidence = text.str();
        finding.limitations.push_back("结构与分片元数据取证，不能证明载荷可解码");
        finding.limitations.push_back("时长是按 sidx/tfdt 推算的估计值，不是解码后的实际时长");
        diagnosis.findings.push_back(std::move(finding));
    }

    if (ev.incomplete_segments > 0) {
        model::DamageFinding finding;
        finding.code = "container.fragment_incomplete";
        finding.detector = kInspector;
        finding.layer = model::DamageLayer::Container;
        finding.severity = model::IssueSeverity::Error;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Exact;
        finding.location.provenance = "moof/trun sample_size vs mdat payload_size";
        std::ostringstream text;
        text << ev.incomplete_segments << " 个分片的样本表与 mdat 载荷不一致";
        for (const auto& segment : ev.segments)
            text << "；第 " << segment.index << " 段 @" << segment.moof_offset << ": " << segment.issue;
        finding.evidence = text.str();
        finding.limitations.push_back("只能证明结构与载荷字节数不符，不能定位是哪几个样本损坏");
        finding.candidate_actions.push_back(model::RepairAction::S02_TailSalvage);
        diagnosis.findings.push_back(std::move(finding));
    }
    AddCheck(diagnosis, "container.fragment_integrity", model::DamageLayer::Container,
             ev.incomplete_segments > 0 ? model::RepairCheckStatus::Failed : model::RepairCheckStatus::Passed,
             ev.incomplete_segments > 0 ? std::to_string(ev.incomplete_segments) + " 个分片结构不完整"
                                        : "全部分片的 trun 声明字节与 mdat 载荷吻合",
             ElapsedMs(start));

    // 逐轨：实测 NAL 类型分布与 in-band 参数集，而不是假设"分片里没有解码参数"。
    std::ostringstream spec;
    spec << "重建初始化段所需规格：轨道 [";
    for (std::size_t i = 0; i < ev.tracks.size(); ++i) {
        const auto& track = ev.tracks[i];
        if (i > 0)
            spec << ", ";
        spec << track.track_id;
    }
    spec << "]，时间基 " << ev.sidx_timescale << "；";

    std::size_t tracks_missing_config = 0;
    for (const auto& track : ev.tracks) {
        model::DamageFinding finding;
        finding.detector = kInspector;
        finding.layer = model::DamageLayer::Codec;
        // 容器未打开时没有 FFmpeg 流下标，只有容器轨道 id —— 不填猜测值。
        finding.location.track_id = track.track_id;
        finding.location.precision = model::DamagePrecision::Exact;
        finding.location.provenance = "mdat payload / NAL length-prefixed headers";
        finding.confidence = model::DamageConfidence::Confirmed;

        std::ostringstream text;
        if (track.nal_units_scanned > 0) {
            text << "轨道 " << track.track_id << "：扫描 " << track.nal_units_scanned << " 个 NAL（"
                 << track.nal_length_size << " 字节长度前缀，布局 " << track.payload_layout << "），"
                 << "编码判定 " << track.bitstream << "（" << track.bitstream_basis << "）；";
            // 只输出与已判定编码对应的参数集计数：把"按 H.264 解释时的计数"
            // 和 HEVC 结论并列会让人误以为流里真的有 101 个 PPS。
            if (track.bitstream == "hevc") {
                text << "HEVC VPS/SPS/PPS = " << track.hevc_vps_count << "/" << track.hevc_sps_count << "/"
                     << track.hevc_pps_count << "；";
            } else if (track.bitstream == "h264") {
                text << "AVC SPS/PPS = " << track.avc_sps_count << "/" << track.avc_pps_count << "；";
            } else {
                text << "两种解释下的参数集计数均不足以判定编码；";
            }
            text << "同步 NAL " << track.sync_nal_count << " 个；";
            if (track.missing_parameter_sets.empty()) {
                text << "存在 in-band 参数集，可尝试从分片自行重建解码配置";
                finding.code = "codec.in_band_parameter_sets_present";
                finding.severity = model::IssueSeverity::Info;
            } else {
                ++tracks_missing_config;
                text << "缺失 in-band 参数集 [";
                for (std::size_t i = 0; i < track.missing_parameter_sets.size(); ++i) {
                    if (i > 0)
                        text << ", ";
                    text << track.missing_parameter_sets[i];
                }
                text << "]，无法只靠本文件推导出解码配置";
                finding.code = "codec.parameter_sets_absent";
                finding.severity = model::IssueSeverity::Critical;
            }
            finding.limitations.push_back("只统计 NAL 头类型，未校验参数集内容与 Slice 语法是否匹配");
            finding.limitations.push_back("编码判定是结构启发式，不是解码器确认");
            std::string joined;
            for (const auto& name : track.missing_parameter_sets) {
                if (!joined.empty())
                    joined += "+";
                joined += name;
            }
            const std::string config_box = track.bitstream == "hevc"   ? "hvcC(VPS/SPS/PPS)"
                                           : track.bitstream == "h264" ? "avcC(SPS/PPS)"
                                                                       : "avcC/hvcC";
            spec << "轨道 " << track.track_id << " " << track.bitstream << "（需 " << config_box << "，缺 "
                 << (joined.empty() ? "无" : joined) << "）；";
        } else {
            ++tracks_missing_config;
            text << "轨道 " << track.track_id << "：" << track.sample_count << " 个访问单元为 " << track.payload_layout
                 << " 布局（" << track.raw_fragment_count << " 个分片），分片内没有 stsd/esds 之类的解码配置盒；"
                 << "需要同源初始化段提供 AudioSpecificConfig / esds 才能解码";
            finding.code = "codec.decoder_config_absent";
            finding.severity = model::IssueSeverity::Critical;
            finding.limitations.push_back("无法从裸载荷判定音频编码、采样率与声道数");
            spec << "轨道 " << track.track_id << " 非 NAL 载荷（需 esds/AudioSpecificConfig）；";
        }
        finding.evidence = text.str();
        diagnosis.findings.push_back(std::move(finding));
    }
    AddCheck(diagnosis, "codec.in_band_parameter_sets", model::DamageLayer::Codec,
             tracks_missing_config == 0 ? model::RepairCheckStatus::Passed : model::RepairCheckStatus::Failed,
             "逐 NAL 实测：" + std::to_string(tracks_missing_config) + "/" + std::to_string(ev.tracks.size()) +
                 " 条轨道缺少自足解码配置",
             ElapsedMs(start));

    const bool timeline_bad = ev.timeline_gaps > 0 || ev.timeline_repeats > 0 || ev.timeline_regressions > 0;
    if (timeline_bad) {
        model::DamageFinding finding;
        finding.code = "timing.fragment_discontinuity";
        finding.detector = kInspector;
        finding.layer = model::DamageLayer::Timing;
        finding.severity = model::IssueSeverity::Warning;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Exact;
        finding.location.provenance = "sidx.earliest_presentation_time / tfdt sequence";
        finding.evidence = "时间轴断裂 " + std::to_string(ev.timeline_gaps) + " 处、重复 " +
                           std::to_string(ev.timeline_repeats) + " 处、回退 " +
                           std::to_string(ev.timeline_regressions) + " 处";
        finding.limitations.push_back("只能证明分片起点时间不连续，不能证明内容缺失");
        diagnosis.findings.push_back(std::move(finding));
    }
    AddCheck(diagnosis, "timing.fragment_timeline", model::DamageLayer::Timing,
             timeline_bad ? model::RepairCheckStatus::Failed : model::RepairCheckStatus::Passed,
             timeline_bad ? "分片起点时间存在断裂/重复/回退" : "分片起点时间连续（sidx/tfdt 序列）", ElapsedMs(start));

    // 扩展名与真实内容不一致：这类文件常被误当成 MPEG-TS 直接丢弃。
    const std::string ext = LowerExtension(request.source_path);
    const bool isobmff_ext = ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" || ext == "m4s" ||
                             ext == "3gp" || ext == "3g2" || ext == "cmfv" || ext == "cmfa";
    if (!isobmff_ext && !diagnosis.input.signature.empty()) {
        model::DamageFinding finding;
        finding.code = "container.extension_mismatch";
        finding.detector = kInspector;
        finding.layer = model::DamageLayer::File;
        finding.severity = model::IssueSeverity::Warning;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Exact;
        finding.location.provenance = "file extension vs first top-level box";
        finding.evidence = "扩展名 ." + ext + " 与文件内容不符：首个顶层盒是 " + diagnosis.input.signature +
                           "（ISOBMFF 分片序列），按扩展名选择 demuxer 会直接失败";
        finding.limitations.push_back("扩展名不影响取证结果，只解释为什么常规播放器打不开");
        diagnosis.findings.push_back(std::move(finding));
    }

    spec << "参考样片须为同源 ftyp+moov 初始化段（含 mvex/trex 与上述解码配置），或含它的完整分片 MP4；"
         << "普通 MP4 的 moov 没有 mvex，当前版本还不能直接复用";
    diagnosis.blocking_reasons.push_back(spec.str());

    // 把源侧实测证据交回诊断结果：参考初始化段的时基 / 编码参数必须拿它交叉核对，
    // 不能只凭 track_id 判定兼容（R03）。
    if (source_evidence) {
        source_evidence->sidx_timescale = ev.sidx_timescale;
        source_evidence->sample_count = declared_samples;
        source_evidence->estimated_duration_seconds = ev.estimated_duration_seconds;
        for (const auto& track : ev.tracks) {
            source_evidence->track_ids.push_back(track.track_id);
            // 逐轨 trun 时长是独立于参考段的证据：用它除以借来的 mdhd timescale，
            // 才能发现「只改了非首轨时基」造成的播放时长改写（R03）。
            RepairSourceTrackTiming timing;
            timing.track_id = track.track_id;
            timing.declared_duration_ticks = track.declared_duration_ticks;
            source_evidence->track_timings.push_back(timing);
            if (source_evidence->bitstream.empty() && !track.bitstream.empty() && track.bitstream != "unknown")
                source_evidence->bitstream = track.bitstream;
        }
        source_evidence->provenance = "moof/trun + sidx + mdat NAL (" + std::to_string(ev.segment_count) + " segments)";
    }
    return true;
}

// 样本表边界分析已移到 core/repair/RepairSampleBounds（R02：stts 游标 + 声明数校验 + 预算）。

// ---------------- 抽样解码 ----------------
struct DecodeProbe {
    bool ran = false;
    bool decoder_opened = false;
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    std::string detail;
};

DecodeProbe ProbeDecode(AVFormatContext* fmt, int stream_index, double seek_seconds, const std::atomic<bool>* cancel,
                        const RepairRunBudget* budget) {
    DecodeProbe probe;
    if (stream_index < 0 || stream_index >= static_cast<int>(fmt->nb_streams)) {
        probe.detail = "无可用媒体轨";
        return probe;
    }
    AVStream* stream = fmt->streams[stream_index];
    // seek_seconds < 0 表示从头读取；否则先跳到指定位置再抽样，避免只看到文件开头。
    if (seek_seconds >= 0.0) {
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            probe.detail = "跳转失败: 时间基无效";
            return probe;
        }
        const int64_t ts = static_cast<int64_t>(seek_seconds / av_q2d(stream->time_base));
        if (av_seek_frame(fmt, stream_index, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            std::ostringstream detail;
            detail << "跳转到 " << FormatSeconds(seek_seconds) << " s 失败";
            probe.detail = detail.str();
            return probe;
        }
    }
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        probe.detail = "未找到对应解码器: " + std::string(avcodec_get_name(stream->codecpar->codec_id));
        return probe;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(decoder);
    if (!ctx) {
        probe.detail = "无法分配解码上下文";
        return probe;
    }
    probe.ran = true;
    if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 || avcodec_open2(ctx, decoder, nullptr) < 0) {
        probe.detail = "解码器打开失败";
        avcodec_free_context(&ctx);
        return probe;
    }
    probe.decoder_opened = true;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    constexpr int kMaxPackets = 8;
    bool read_error = false;
    if (packet && frame) {
        while (probe.packets < kMaxPackets) {
            if (CheckStop(cancel, budget)) {
                probe.detail = "抽样解码被取消";
                break;
            }
            const int ret = av_read_frame(fmt, packet);
            if (ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                read_error = true;
                break;
            }
            if (packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            ++probe.packets;
            const int send_ret = avcodec_send_packet(ctx, packet);
            av_packet_unref(packet);
            if (send_ret < 0) {
                ++probe.errors;
                continue;
            }
            while (true) {
                const int receive_ret = avcodec_receive_frame(ctx, frame);
                if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF)
                    break;
                if (receive_ret < 0) {
                    ++probe.errors;
                    break;
                }
                ++probe.frames;
                av_frame_unref(frame);
            }
        }
        // flush
        avcodec_send_packet(ctx, nullptr);
        while (true) {
            const int ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                ++probe.errors;
                break;
            }
            ++probe.frames;
            av_frame_unref(frame);
        }
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);

    std::ostringstream detail;
    detail << "抽样解码: " << probe.packets << " 包 / " << probe.frames << " 帧, 错误 " << probe.errors;
    if (read_error)
        detail << ", 读取在中途失败";
    probe.detail = detail.str();
    return probe;
}

} // namespace

RepairInspectOutcome RepairInspector::Inspect(const model::RepairRequest& request, const std::atomic<bool>* cancel,
                                              const RepairProgress& progress, const RepairRunBudget* budget) {
    RepairInspectOutcome outcome;
    model::RepairDiagnosis& diagnosis = outcome.diagnosis;
    diagnosis.request_id = request.request_id;
    diagnosis.source_path = request.source_path;
    diagnosis.diagnosis_id = "diag-" + FormatLocalRunId();

    Emit(progress, 2, stage::Inspecting(), "读取文件身份");

    std::error_code ec;
    // request.source_path 是 UTF-8。exists(utf8 std::string) 在 Windows 上返回 false
    // **且 ec 为空**，中文路径的源文件会被判成"不存在"——修复入口直接拒绝。
    const std::filesystem::path source_native = ToNativePath(request.source_path);
    if (!std::filesystem::exists(source_native, ec) || !std::filesystem::is_regular_file(source_native, ec)) {
        diagnosis.input = SnapshotFile(request.source_path, budget);
        diagnosis.summary = "输入文件不存在或不是普通文件";
        diagnosis.blocking_reasons.push_back("输入文件不存在或不是普通文件");
        AddCheck(diagnosis, "file.identity", model::DamageLayer::File, model::RepairCheckStatus::Failed,
                 diagnosis.summary);
        return outcome;
    }

    diagnosis.input = SnapshotFile(request.source_path, budget);
    AddCheck(diagnosis, "file.identity", model::DamageLayer::File, model::RepairCheckStatus::Passed,
             "存在且可读, size=" + std::to_string(diagnosis.input.size_bytes) +
                 ", signature=" + (diagnosis.input.signature.empty() ? "unknown" : diagnosis.input.signature));
    if (diagnosis.input.size_bytes == 0) {
        diagnosis.blocking_reasons.push_back("文件为空（0 字节）");
        diagnosis.summary = "文件为空，没有可恢复内容";
        AddCheck(diagnosis, "file.content", model::DamageLayer::File, model::RepairCheckStatus::Failed, "0 字节");
        return outcome;
    }

    if (CheckStop(cancel, budget)) {
        outcome.canceled = true;
        diagnosis.summary = "诊断被取消";
        return outcome;
    }

    const bool is_isobmff = LooksLikeIsobmff(request.source_path, diagnosis.input.signature);
    // container_known 表示"有结构证据确认这就是 ISOBMFF"，不是"扩展名像 MP4"。
    // 证据在盒结构解析之后才能确定，先保持 false。
    diagnosis.container_known = false;

    // ---- 容器打开 / 流信息探测（FFmpeg）----
    Emit(progress, 12, stage::Inspecting(), "FFmpeg 打开与探测");
    const auto open_start = Clock::now();
    AVFormatContext* fmt = avformat_alloc_context();
    ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = cancel ? const_cast<std::atomic<bool>*>(cancel) : nullptr;
    if (fmt)
        ffmpeg_io::AttachInterrupt(fmt, interrupt, ffmpeg_io::kOpenTimeoutUs);

    std::string open_error;
    std::string probe_error;
    bool open_ok = false;
    int open_ret = fmt ? avformat_open_input(&fmt, request.source_path.c_str(), nullptr, nullptr) : AVERROR(ENOMEM);
    if (open_ret < 0) {
        open_error = AvErrorText(open_ret);
    } else {
        open_ok = true;
        interrupt.deadline_us.store(av_gettime() + ffmpeg_io::kProbeTimeoutUs);
        const int probe_ret = avformat_find_stream_info(fmt, nullptr);
        interrupt.deadline_us.store(0);
        if (probe_ret < 0)
            probe_error = "流信息探测失败: " + AvErrorText(probe_ret);
    }

    diagnosis.openable = open_ok;
    if (!open_ok) {
        AddCheck(diagnosis, "container.open", model::DamageLayer::Container, model::RepairCheckStatus::Failed,
                 open_error, ElapsedMs(open_start));
    } else {
        AddCheck(diagnosis, "container.open", model::DamageLayer::Container,
                 probe_error.empty() ? model::RepairCheckStatus::Passed : model::RepairCheckStatus::Inconclusive,
                 "FFmpeg 打开成功, 格式=" +
                     std::string(fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "unknown") +
                     ", 流=" + std::to_string(fmt->nb_streams) + (probe_error.empty() ? "" : "；" + probe_error),
                 ElapsedMs(open_start));
        if (!HasMediaStream(fmt)) {
            diagnosis.blocking_reasons.push_back("容器内没有视频/音频轨");
            diagnosis.summary = "没有可恢复的视频/音频轨";
            AddCheck(diagnosis, "track.presence", model::DamageLayer::Track, model::RepairCheckStatus::Failed,
                     "nb_streams=" + std::to_string(fmt->nb_streams));
        }
        outcome.target_stream_index = PickTargetStream(fmt, request.target_streams);
    }

    if (CheckStop(cancel, budget)) {
        outcome.canceled = true;
        diagnosis.summary = "诊断被取消";
        if (fmt)
            avformat_close_input(&fmt);
        return outcome;
    }

    // ---- ISOBMFF 盒结构 + 样本表边界 ----
    model::RepairCoverage& coverage = diagnosis.coverage;
    bool init_segment_missing = false;
    bool fragment_forensics_applied = false;
    RepairSourceCompatibilityEvidence source_evidence;
    RepairFragmentEvidence fragment_evidence;
    if (is_isobmff) {
        Emit(progress, 35, stage::Inspecting(), "解析 MP4/MOV 盒结构与样本表");
        const auto parse_start = Clock::now();
        IsobmffFile parsed;
        IsobmffOptions options;
        options.cancel = cancel;
        const bool parse_ok = IsobmffParser::Parse(request.source_path, parsed, options);
        diagnosis.moov_present = parse_ok && parsed.found_moov;
        diagnosis.fragmented = parsed.fragmented;

        // 结构证据：至少有一个可识别的顶层盒。只有这时才能把"没有 moov"归因为
        // "缺索引"，否则对象根本不是 ISOBMFF（例如改名为 .mp4 的垃圾文件）。
        bool structural_evidence = false;
        for (const auto& box : parsed.top_level) {
            if (IsKnownIsobmffBox(box.type)) {
                structural_evidence = true;
                break;
            }
        }
        diagnosis.container_known = parse_ok && structural_evidence;

        uint64_t structural_bytes = 0;
        for (const auto& box : parsed.top_level) {
            const uint64_t end = box.size > std::numeric_limits<uint64_t>::max() - box.offset
                                     ? std::numeric_limits<uint64_t>::max()
                                     : box.offset + box.size;
            structural_bytes +=
                std::min(end, diagnosis.input.size_bytes) - std::min(box.offset, diagnosis.input.size_bytes);
            if (end > diagnosis.input.size_bytes) {
                model::DamageFinding finding;
                finding.code = "container.box_out_of_bounds";
                finding.detector = kInspector;
                finding.layer = model::DamageLayer::Container;
                finding.severity = model::IssueSeverity::Error;
                finding.confidence = model::DamageConfidence::Confirmed;
                finding.location.bytes = model::RepairByteExtent{std::min(box.offset, diagnosis.input.size_bytes),
                                                                 diagnosis.input.size_bytes, true};
                finding.location.structure_path = box.type;
                finding.location.precision = model::DamagePrecision::Exact;
                finding.location.provenance = "isobmff.top_level_box vs file_size";
                finding.evidence = "顶层盒 " + box.type + " 声明大小 " + std::to_string(box.size) + "，实际可用 " +
                                   std::to_string(diagnosis.input.size_bytes - box.offset) + " 字节";
                finding.limitations.push_back("只能证明该盒越过文件尾，不能证明载荷是否可解码");
                finding.candidate_actions.push_back(model::RepairAction::S02_TailSalvage);
                diagnosis.findings.push_back(std::move(finding));
            }
        }
        coverage.bytes_scanned = structural_bytes;

        if (!parse_ok) {
            AddCheck(diagnosis, "container.structure", model::DamageLayer::Container, model::RepairCheckStatus::Failed,
                     "ISOBMFF 盒结构解析失败: " + parsed.error_message, ElapsedMs(parse_start));
            model::DamageFinding finding;
            finding.code = "container.structure_unparsable";
            finding.detector = kInspector;
            finding.layer = model::DamageLayer::Container;
            finding.severity = model::IssueSeverity::Critical;
            finding.confidence = model::DamageConfidence::Confirmed;
            finding.location.precision = model::DamagePrecision::Unknown;
            finding.evidence = parsed.error_message;
            finding.limitations.push_back("解析器在损坏处停止，不能据此判断后续内容是否存在");
            diagnosis.findings.push_back(std::move(finding));
        } else if (!structural_evidence) {
            AddCheck(diagnosis, "container.structure", model::DamageLayer::Container, model::RepairCheckStatus::Failed,
                     "未识别到任何 ISOBMFF 顶层盒（ftyp/moov/mdat...）", ElapsedMs(parse_start));
            model::DamageFinding finding;
            finding.code = "container.structure_unparsable";
            finding.detector = kInspector;
            finding.layer = model::DamageLayer::Container;
            finding.severity = model::IssueSeverity::Critical;
            finding.confidence = model::DamageConfidence::Confirmed;
            finding.location.precision = model::DamagePrecision::Unknown;
            finding.location.provenance = "isobmff.top_level_order";
            finding.evidence = "顶层盒序列: " + [&]() {
                std::string order;
                for (const auto& type : parsed.top_level_order) {
                    if (!order.empty())
                        order += " -> ";
                    order += type;
                }
                return order.empty() ? "未识别到任何盒" : order;
            }();
            finding.limitations.push_back("只能证明没有可识别的 ISOBMFF 结构，不能证明文件内不含媒体数据");
            diagnosis.findings.push_back(std::move(finding));
        } else if (!parsed.found_moov) {
            // 没有 moov 但出现了 styp / moof：这是"媒体分片序列 + 初始化段分离"的形态
            // （DASH/CMAF/Smooth Streaming 下载器常见），初始化段里有 ftyp+moov 与
            // hvcC/avcC/esds；分片自身不含解码参数，只能靠同源初始化段补全。
            const bool media_segment_sequence = parsed.styp_count > 0 || !parsed.fragments.empty();
            std::set<uint32_t> source_track_ids;
            for (const auto& fragment : parsed.fragments) {
                if (fragment.track_id != 0)
                    source_track_ids.insert(fragment.track_id);
            }
            init_segment_missing = media_segment_sequence;

            // 先取证：源侧 sidx 时基 / 逐 NAL 码流是核对参考段的独立证据（R03）。
            // 旧顺序把取证放在参考段之后，Reader 只能看到 track_id。
            fragment_forensics_applied = ApplyFragmentForensics(request, parsed, cancel, progress, budget, diagnosis,
                                                                &source_evidence, &fragment_evidence);
            outcome.source_evidence = source_evidence;

            AddCheck(diagnosis, "container.structure", model::DamageLayer::Container, model::RepairCheckStatus::Failed,
                     media_segment_sequence ? "缺少 ftyp+moov 初始化段（媒体分片序列）" : "缺少 moov（索引/元数据）",
                     ElapsedMs(parse_start));
            model::DamageFinding finding;
            finding.code = media_segment_sequence ? "container.init_segment_missing" : "container.moov_missing";
            finding.detector = kInspector;
            finding.layer = model::DamageLayer::Container;
            finding.severity = model::IssueSeverity::Critical;
            finding.confidence = model::DamageConfidence::Confirmed;
            finding.location.precision = model::DamagePrecision::Unknown;
            finding.location.provenance = "isobmff.top_level_order";
            finding.evidence = "顶层盒序列: " + [&]() {
                std::string order;
                for (const auto& type : parsed.top_level_order) {
                    if (!order.empty())
                        order += " -> ";
                    order += type;
                }
                return order.empty() ? "未识别到任何盒" : order;
            }();
            if (media_segment_sequence) {
                std::ostringstream evidence;
                const uint64_t moof_segments =
                    static_cast<uint64_t>(std::count_if(parsed.top_level.begin(), parsed.top_level.end(),
                                                        [](const IsobmffBox& box) { return box.type == "moof"; }));
                evidence << "媒体分片 " << moof_segments << " 段（" << parsed.fragments.size() << " 个 traf / styp "
                         << parsed.styp_count << " 个），源轨道 [";
                bool first = true;
                for (uint32_t track_id : source_track_ids) {
                    if (!first)
                        evidence << ", ";
                    evidence << track_id;
                    first = false;
                }
                evidence << "]，缺少 ftyp+moov 初始化段（解码参数在初始化段里）";
                finding.evidence = evidence.str();
            }
            finding.candidate_actions.push_back(model::RepairAction::S01_Remux);
            finding.limitations.push_back("分片本身不含 hvcC/avcC/esds 等解码参数，无法从本文件推导");
            diagnosis.findings.push_back(std::move(finding));

            if (!media_segment_sequence) {
                // P2-12：普通 MP4 缺 moov，但 mdat 载荷可能是可直接扫描重建的
                // 纯 H.264（AVCC）或纯 ADTS AAC。必须实测能完整解释整段载荷，
                // 才允许进入重建流程；否则仍按"需要参考初始化段"阻止执行。
                const auto probe_start = Clock::now();
                Emit(progress, 55, stage::Inspecting(), "尝试无参考扫描 mdat 载荷");
                // 切片 F：显式帧率会改变"能不能重建"的结论（无 VUI timing 的
                // H.264 只有在给了帧率时才可重建），所以探测必须和重建用同一份参数。
                RepairTimelineAssumption assumption;
                if (request.HasAssumedFrameRate()) {
                    assumption.num = request.assumed_frame_rate_num;
                    assumption.den = request.assumed_frame_rate_den;
                }
                const RepairPlainMp4Probe probe = RepairPlainMp4Rebuilder::Probe(
                    request.source_path, cancel, budget, 0, assumption.Valid() ? &assumption : nullptr);
                if (probe.attemptable) {
                    model::DamageFinding plain;
                    plain.code = "container.mdat_unindexed";
                    plain.detector = kInspector;
                    plain.layer = model::DamageLayer::Container;
                    plain.severity = model::IssueSeverity::Warning;
                    plain.confidence = model::DamageConfidence::Confirmed;
                    plain.location.precision = model::DamagePrecision::Bounded;
                    plain.location.provenance = "mdat payload scan (NAL length-prefix / ADTS frames)";
                    plain.evidence = "缺少 moov；mdat 载荷可无参考解析为 " + probe.codec_summary;
                    plain.limitations = probe.limitations;
                    plain.candidate_actions.push_back(model::RepairAction::S01_Remux);
                    diagnosis.findings.push_back(std::move(plain));
                    AddCheck(diagnosis, "container.mdat_unindexed", model::DamageLayer::Container,
                             model::RepairCheckStatus::Passed, "可无参考重建样本表：" + probe.codec_summary,
                             ElapsedMs(probe_start));
                } else if (!request.reference_path.empty() && !SamePath(request.reference_path, request.source_path)) {
                    // 切片 C：无参考扫描失败（参数集只在 moov 里 / 音视频交织）时，
                    // 用同源完整样片只借解码配置与布局线索，在损坏文件里重扫样本表。
                    const auto reference_probe = RepairReferenceMoovRebuilder::ProbeWithReference(
                        request.source_path, request.reference_path, cancel, budget);
                    if (reference_probe.attemptable) {
                        model::DamageFinding plain;
                        plain.code = "container.mdat_unindexed";
                        plain.detector = kInspector;
                        plain.layer = model::DamageLayer::Container;
                        plain.severity = model::IssueSeverity::Warning;
                        plain.confidence = model::DamageConfidence::Confirmed;
                        plain.location.precision = model::DamagePrecision::Bounded;
                        plain.location.provenance = "reference-assisted mdat scan (layout hints + NAL boundaries)";
                        plain.evidence = "缺少 moov；参考样片辅助可重建双轨样本表：" + reference_probe.codec_summary;
                        plain.limitations = reference_probe.limitations;
                        plain.candidate_actions.push_back(model::RepairAction::S01_Remux);
                        diagnosis.findings.push_back(std::move(plain));

                        model::DamageFinding assisted;
                        assisted.code = "container.mdat_reference_assisted";
                        assisted.detector = kInspector;
                        assisted.layer = model::DamageLayer::Container;
                        assisted.severity = model::IssueSeverity::Info;
                        assisted.confidence = model::DamageConfidence::Probable;
                        assisted.location.precision = model::DamagePrecision::Bounded;
                        assisted.location.provenance = "reference sample layout hints verified in damaged mdat";
                        assisted.evidence = reference_probe.codec_summary;
                        assisted.limitations = reference_probe.limitations;
                        assisted.candidate_actions.push_back(model::RepairAction::S01_Remux);
                        diagnosis.findings.push_back(std::move(assisted));

                        AddCheck(diagnosis, "container.mdat_unindexed", model::DamageLayer::Container,
                                 model::RepairCheckStatus::Passed,
                                 "参考样片辅助重建可行：" + reference_probe.codec_summary, ElapsedMs(probe_start));
                        // R08：执行时读到的参考文件必须与诊断时是同一份。
                        outcome.reference_snapshot = RepairOutputWriter::MakeSnapshot(request.reference_path, budget);
                        outcome.reference_snapshot_valid = true;
                    } else {
                        AddCheck(diagnosis, "container.mdat_unindexed", model::DamageLayer::Container,
                                 model::RepairCheckStatus::Unsupported,
                                 "无参考重建不可行，参考样片辅助亦不可用: " + reference_probe.reason,
                                 ElapsedMs(probe_start));
                        diagnosis.blocking_reasons.push_back("缺少 moov（索引/元数据），自动无参考重建不可行（" +
                                                             probe.reason + "），参考样片辅助重建也不可用：" +
                                                             reference_probe.reason);
                    }
                } else {
                    AddCheck(diagnosis, "container.mdat_unindexed", model::DamageLayer::Container,
                             model::RepairCheckStatus::Unsupported, "无参考重建不可行: " + probe.reason,
                             ElapsedMs(probe_start));
                    diagnosis.blocking_reasons.push_back("缺少 moov（索引/元数据），且自动无参考重建不可行：" +
                                                         probe.reason + "；请提供同源参考样片或初始化段");
                }
            } else {
                // 参考样片：只在缺初始化段的场景下有意义。
                const std::vector<uint32_t> required_track_ids(source_track_ids.begin(), source_track_ids.end());
                if (request.reference_path.empty()) {
                    // 没有参考样片时先尝试从 in-band 参数集自建。Builder 内部对时基、
                    // 参数集完整性和轨道类型都有硬门槛；任何一条不满足都会拒绝并写明原因。
                    const auto synth_start = Clock::now();
                    Emit(progress, 55, stage::Inspecting(), "尝试从 in-band 参数集自建初始化段");
                    const RepairSynthesizedInit synthesized =
                        RepairInitSegmentBuilder::Build(request.source_path, parsed, fragment_evidence, cancel, budget);
                    if (synthesized.ok) {
                        RepairInitSegment init;
                        init.ok = true;
                        init.synthesized = true;
                        init.reference_path = request.source_path;
                        init.codec_summary = synthesized.codec_summary;
                        init.track_ids = synthesized.track_ids;
                        init.init_size = synthesized.bytes.size();
                        init.bytes = synthesized.bytes;
                        init.compatibility = "synthesized";
                        init.compatibility_reason = "由 in-band 参数集自建，未经同源参考段交叉验证";
                        init.reference_timescale = synthesized.movie_timescale;
                        init.reference_tracks = synthesized.tracks;
                        init.timescale_check = synthesized.timescale_basis;
                        AddCheck(diagnosis, "reference.init_segment", model::DamageLayer::Container,
                                 model::RepairCheckStatus::Inconclusive,
                                 "未提供参考样片；已自建初始化段 " + std::to_string(init.init_size) + " 字节（" +
                                     init.codec_summary + "，时基 " + init.timescale_check + "）",
                                 ElapsedMs(synth_start));
                        AddCheck(diagnosis, "reference.init_segment_synthesized", model::DamageLayer::Timing,
                                 model::RepairCheckStatus::Inconclusive,
                                 synthesized.limitations.empty() ? "自建初始化段未经交叉验证"
                                                                 : synthesized.limitations.front());
                        outcome.reference_init = std::move(init);
                    } else {
                        AddCheck(diagnosis, "reference.init_segment", model::DamageLayer::Container,
                                 model::RepairCheckStatus::NotRun,
                                 "未提供参考样片/初始化段；自动自建未成功: " + synthesized.error,
                                 ElapsedMs(synth_start));
                        diagnosis.blocking_reasons.push_back(
                            "源文件是 fMP4 媒体分片序列但缺少初始化段（ftyp+moov）；自动自建未成功：" +
                            synthesized.error +
                            "。请在「参考样片」中提供同源初始化段（含 mvex/trex），或含该初始化段的完整分片 MP4");
                    }
                } else if (SamePath(request.reference_path, request.source_path)) {
                    AddCheck(diagnosis, "reference.init_segment", model::DamageLayer::Container,
                             model::RepairCheckStatus::Failed, "参考文件与源文件相同，无法提供缺失的初始化段");
                    diagnosis.blocking_reasons.push_back("参考文件与源文件相同；请选择同源的初始化段或完整文件");
                } else {
                    const auto reference_start = Clock::now();
                    Emit(progress, 55, stage::Inspecting(), "校验参考样片/初始化段");
                    RepairInitSegment init = RepairInitSegmentReader::Read(request.reference_path, required_track_ids,
                                                                           source_evidence, cancel, budget);
                    if (init.ok) {
                        AddCheck(diagnosis, "reference.init_segment", model::DamageLayer::Container,
                                 init.compatibility == "verified" ? model::RepairCheckStatus::Passed
                                                                  : model::RepairCheckStatus::Inconclusive,
                                 "初始化段 " + std::to_string(init.init_size) + " 字节，含 " + init.codec_summary +
                                     "；兼容性=" + init.compatibility + "（" + init.compatibility_reason + "）",
                                 ElapsedMs(reference_start));
                        // R08：执行时拼进去的必须是诊断时读到的同一份参考文件。
                        outcome.reference_snapshot = RepairOutputWriter::MakeSnapshot(request.reference_path, budget);
                        outcome.reference_snapshot_valid = true;
                        outcome.reference_init = std::move(init);
                    } else {
                        AddCheck(diagnosis, "reference.init_segment", model::DamageLayer::Container,
                                 model::RepairCheckStatus::Failed, "参考样片不可用: " + init.error,
                                 ElapsedMs(reference_start));
                        diagnosis.blocking_reasons.push_back("参考样片不可用: " + init.error);
                    }
                }
            }

        } else {
            AddCheck(diagnosis, "container.structure", model::DamageLayer::Container, model::RepairCheckStatus::Passed,
                     "moov 位于 " + std::to_string(parsed.moov_offset) + ", 大小 " + std::to_string(parsed.moov_size) +
                         (parsed.fragmented ? ", 分片 MP4" : ""),
                     ElapsedMs(parse_start));

            // ---- 样本表越界 ----
            Emit(progress, 50, stage::Inspecting(), "校验样本表与文件边界");
            const auto sample_start = Clock::now();
            bool any_checked = false;
            bool any_bad = false;
            double earliest_salvage = std::numeric_limits<double>::max();
            std::ostringstream bad_detail;
            for (const auto& track : parsed.tracks) {
                if (track.handler != "vide" && track.handler != "soun")
                    continue;
                RepairTrackBounds bounds = AnalyzeTrackBounds(track, diagnosis.input.size_bytes, budget);
                if (!bounds.checked)
                    continue;
                any_checked = true;
                if (!bounds.note.empty())
                    coverage.limitations.push_back("轨道 " + std::to_string(track.track_id) +
                                                   " 样本边界检查: " + bounds.note);
                if (bounds.declaration_implausible || bounds.truncated_by_budget) {
                    any_bad = true; // 声明不可信 / 没查完，不能说成"全部样本位于文件范围内"
                    bad_detail << " track=" << track.track_id << " incomplete_check=1";
                }
                // 逐轨可抢救证据：只有"全部样本都越界"才允许后续规划剔除该轨；
                // 检查不完整时一律按可用处理，不能靠猜测放弃一条轨。
                model::RepairTrackSalvageInfo salvage;
                salvage.track_id = track.track_id;
                salvage.input_stream_index = FindInputStreamByTrackId(fmt, track.track_id);
                salvage.kind = track.handler == "vide" ? "video" : "audio";
                if (bounds.declaration_implausible || bounds.truncated_by_budget) {
                    salvage.salvageable = true;
                    salvage.evidence = "样本边界检查不完整（声明不可信或被预算截断），不能判定该轨不可用";
                } else if (bounds.sample_count > 0 && bounds.bad_sample_count >= bounds.sample_count) {
                    salvage.salvageable = false;
                    salvage.evidence = "全部 " + std::to_string(bounds.sample_count) + " 个样本越过文件尾";
                } else if (bounds.bad_sample_count > 0) {
                    salvage.salvageable = true;
                    salvage.evidence = std::to_string(bounds.bad_sample_count) + "/" +
                                       std::to_string(bounds.sample_count) + " 个样本越界，仍有可用样本";
                } else {
                    salvage.salvageable = true;
                    salvage.evidence = "全部 " + std::to_string(bounds.sample_count) + " 个样本位于文件范围内";
                }
                diagnosis.track_salvage.push_back(std::move(salvage));

                if (bounds.bad_sample_count == 0)
                    continue;
                any_bad = true;

                model::DamageFinding finding;
                finding.code = "track.sample_out_of_bounds";
                finding.detector = kInspector;
                finding.layer = model::DamageLayer::Track;
                finding.severity = model::IssueSeverity::Error;
                finding.confidence = model::DamageConfidence::Confirmed;
                finding.location.bytes = model::RepairByteExtent{
                    std::min(bounds.first_bad_offset, diagnosis.input.size_bytes), diagnosis.input.size_bytes, true};
                finding.location.track_id = track.track_id;
                finding.location.sample_ordinal = bounds.first_bad_sample;
                finding.location.item_count = bounds.bad_sample_count;
                finding.location.structure_path = "moov/trak/mdia/minf/stbl";
                finding.location.field_name = "stsz/stsc/stco";
                finding.location.precision = model::DamagePrecision::Exact;
                finding.location.provenance = "isobmff.stsz+stsc+stco vs file_size";
                std::ostringstream evidence;
                evidence << "轨道 " << track.track_id << " (" << track.TypeName() << ") 共 " << bounds.sample_count
                         << " 个样本，其中 " << bounds.bad_sample_count << " 个越过文件尾；首个越界样本 #"
                         << bounds.first_bad_sample << " @ byte " << bounds.first_bad_offset;
                finding.evidence = evidence.str();
                if (bounds.salvage_seconds.has_value()) {
                    finding.location.estimated_seconds = bounds.salvage_seconds;
                    // 只有"越界样本之后不再有可用样本"才是真正的尾部截断；
                    // 中间损坏的后续样本仍可读取，不能在此提前设置截断边界，
                    // 否则执行层会误判为"边界之后没有内容"而放弃多段恢复。
                    if (!bounds.has_valid_after_bad)
                        earliest_salvage = std::min(earliest_salvage, *bounds.salvage_seconds);
                }
                finding.candidate_actions.push_back(model::RepairAction::S02_TailSalvage);
                finding.limitations.push_back("样本表的边界只说明字节缺失，不代表缺失区之前的样本一定可解码");
                if (bounds.has_valid_after_bad) {
                    finding.candidate_actions.push_back(model::RepairAction::S04_SegmentSalvage);
                    finding.limitations.push_back("越界样本之后仍有可用样本，按中间损坏处理，不视为尾部截断");
                }
                diagnosis.findings.push_back(std::move(finding));

                bad_detail << " track=" << track.track_id << " bad=" << bounds.bad_sample_count;
            }

            // ---- 样本表自洽性（索引与 mdat 布局 / 表计数）----
            // 与"越过文件尾"是两种损伤：越界说明数据已缺失，只能跳段或截断；
            // 这里检测的是"数据仍可能完整、只是索引对不上"的形态 —— 例如文件头
            // 被插入/删除字节后 chunk 偏移整体偏移，或 stts/ctts/stss 条目数与
            // stsz 样本数不符。这类问题可以在执行前重建索引，而不必丢弃内容。
            if (!parsed.fragmented) {
                const auto table_start = Clock::now();
                const IsobmffBox* mdat_box = nullptr;
                std::uint32_t mdat_count = 0;
                for (const auto& box : parsed.top_level) {
                    if (box.type == "mdat") {
                        mdat_box = &box;
                        ++mdat_count;
                    }
                }
                if (mdat_box != nullptr && mdat_count == 1 && mdat_box->size >= mdat_box->header_size) {
                    const uint64_t payload_start = mdat_box->offset + mdat_box->header_size;
                    const uint64_t payload_end = mdat_box->offset + mdat_box->size;
                    bool inconsistent = false;
                    std::ostringstream table_detail;
                    for (const auto& track : parsed.tracks) {
                        if (track.handler != "vide" && track.handler != "soun")
                            continue;
                        if (track.tables_truncated)
                            continue;
                        std::ostringstream track_issues;
                        if ((track.has_stco || track.has_co64) && !track.chunk_offsets.empty()) {
                            for (uint64_t offset : track.chunk_offsets) {
                                if (offset < payload_start || offset >= payload_end) {
                                    track_issues << "chunk 偏移 " << offset << " 不在 mdat 载荷范围 [" << payload_start
                                                 << ", " << payload_end << ")；";
                                    break;
                                }
                            }
                        }
                        if (track.has_stsz && track.has_stts && track.SttsSampleCount() != track.stsz.sample_count)
                            track_issues << "stts 计数 " << track.SttsSampleCount() << " != stsz 样本数 "
                                         << track.stsz.sample_count << "；";
                        if (track.has_stsz && track.has_ctts && track.CttsSampleCount() != track.stsz.sample_count)
                            track_issues << "ctts 计数 " << track.CttsSampleCount() << " != stsz 样本数 "
                                         << track.stsz.sample_count << "；";
                        if (track.has_stsz) {
                            for (uint32_t index : track.stss) {
                                if (index == 0 || index > track.stsz.sample_count) {
                                    track_issues << "stss 同步样本序号 " << index << " 越界；";
                                    break;
                                }
                            }
                        }
                        if (track_issues.str().empty())
                            continue;
                        inconsistent = true;
                        table_detail << " track=" << track.track_id << " " << track_issues.str();

                        model::DamageFinding finding;
                        finding.code = "track.sample_table_inconsistent";
                        finding.detector = kInspector;
                        finding.layer = model::DamageLayer::Track;
                        finding.severity = model::IssueSeverity::Warning;
                        finding.confidence = model::DamageConfidence::Confirmed;
                        finding.location.track_id = track.track_id;
                        finding.location.structure_path = "moov/trak/mdia/minf/stbl";
                        finding.location.field_name = "stco/co64/stts/ctts/stss";
                        finding.location.precision = model::DamagePrecision::Exact;
                        finding.location.provenance = "isobmff.stco/co64 vs mdat payload + table counts";
                        finding.evidence = "轨道 " + std::to_string(track.track_id) + " (" + track.TypeName() +
                                           "): " + track_issues.str();
                        finding.candidate_actions.push_back(model::RepairAction::S01_Remux);
                        finding.limitations.push_back("索引可重建的前提是 mdat 载荷仍完整；"
                                                      "重建后需重新封装并解码验证");
                        diagnosis.findings.push_back(std::move(finding));
                    }
                    if (inconsistent) {
                        AddCheck(diagnosis, "track.sample_table", model::DamageLayer::Track,
                                 model::RepairCheckStatus::Failed, "样本表与 mdat 布局不一致:" + table_detail.str(),
                                 ElapsedMs(table_start));
                    } else {
                        AddCheck(diagnosis, "track.sample_table", model::DamageLayer::Track,
                                 model::RepairCheckStatus::Passed,
                                 "chunk 偏移均位于唯一 mdat 载荷范围内，表计数与样本数一致", ElapsedMs(table_start));
                    }
                } else {
                    AddCheck(diagnosis, "track.sample_table", model::DamageLayer::Track,
                             model::RepairCheckStatus::Inconclusive,
                             "缺少 mdat 或存在多个 mdat，无法核对 chunk 偏移与 mdat 布局", ElapsedMs(table_start));
                }
            }
            if (!any_checked) {
                AddCheck(diagnosis, "track.sample_bounds", model::DamageLayer::Track,
                         model::RepairCheckStatus::Inconclusive,
                         "没有可交叉校验的完整样本表（stsz/stsc/stco 缺失或被截断）", ElapsedMs(sample_start));
            } else if (any_bad) {
                AddCheck(diagnosis, "track.sample_bounds", model::DamageLayer::Track, model::RepairCheckStatus::Failed,
                         "存在越界样本:" + bad_detail.str(), ElapsedMs(sample_start));
            } else {
                AddCheck(diagnosis, "track.sample_bounds", model::DamageLayer::Track, model::RepairCheckStatus::Passed,
                         "全部样本位于文件范围内", ElapsedMs(sample_start));
            }
            if (any_bad && earliest_salvage < std::numeric_limits<double>::max())
                outcome.salvage_end_seconds = earliest_salvage;
        }
        // 分片取证已经按盒序列实扫到文件尾，reached_eof 是实测值；
        // 不能用 "parse_ok && found_moov" 这个派生式把它覆盖掉（缺 moov 时恒 false）。
        if (!fragment_forensics_applied)
            coverage.reached_eof = parse_ok && parsed.found_moov;
    } else {
        // 非 ISOBMFF 不再阻断：结构级检查（盒/样本表交叉校验）确实不可用，但只要
        // FFmpeg 能打开并探测到媒体轨，执行层仍可做通用重封装 + 运行时跳段兜底。
        // 边界必须写进覆盖范围，不能让报告看起来像"做过结构级检查"。
        AddCheck(diagnosis, "container.structure", model::DamageLayer::Container, model::RepairCheckStatus::Unsupported,
                 "非 ISOBMFF 容器：跳过盒/样本表级检查，仅按 FFmpeg 探测到的流做通用重封装");
        coverage.unsupported_items.push_back("非 ISOBMFF 容器的盒/样本表结构级检查");
        coverage.limitations.push_back("通用重封装依赖 FFmpeg 解复用，无法给出样本表级损伤定位");
        if (open_ok && fmt && HasMediaStream(fmt)) {
            diagnosis.candidate_actions.push_back(model::RepairAction::S01_Remux);
            if (request.allow_segment_salvage)
                diagnosis.candidate_actions.push_back(model::RepairAction::S04_SegmentSalvage);
            if (request.repair_timeline)
                diagnosis.candidate_actions.push_back(model::RepairAction::S03_TimelineRepair);
            AddCheck(diagnosis, "container.remux_fallback", model::DamageLayer::Container,
                     model::RepairCheckStatus::Inconclusive,
                     "通用重封装兜底：结构级证据缺失，损伤只能在读取阶段被运行时错误暴露");
        } else {
            diagnosis.blocking_reasons.push_back("非 ISOBMFF 容器无法通过 FFmpeg 探测到可读媒体轨");
        }
    }

    if (CheckStop(cancel, budget)) {
        outcome.canceled = true;
        diagnosis.summary = "诊断被取消";
        if (fmt)
            avformat_close_input(&fmt);
        return outcome;
    }

    // ---- 编码参数集 ----
    if (fmt && outcome.target_stream_index >= 0) {
        const AVStream* stream = fmt->streams[outcome.target_stream_index];
        const std::string codec = avcodec_get_name(stream->codecpar->codec_id);
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (stream->codecpar->extradata_size > 0) {
                AddCheck(diagnosis, "codec.parameter_sets", model::DamageLayer::Codec, model::RepairCheckStatus::Passed,
                         "视频轨 extradata=" + std::to_string(stream->codecpar->extradata_size) +
                             " bytes, codec=" + codec);
            } else {
                AddCheck(diagnosis, "codec.parameter_sets", model::DamageLayer::Codec,
                         model::RepairCheckStatus::Inconclusive,
                         "视频轨没有容器级 extradata（参数集可能在码流内，需解码确认）, codec=" + codec);
            }
        } else {
            AddCheck(diagnosis, "codec.parameter_sets", model::DamageLayer::Codec, model::RepairCheckStatus::Passed,
                     "音频轨 codec=" + codec + "（无参数集要求）");
        }
    }

    // ---- 抽样解码（首 / 中 / 尾三点）----
    // 只看开头会把「索引完好、中段载荷损坏」的文件误判成可完整修复；
    // 在规划前补中段与尾段抽样，才能提前暴露局部坏区。
    Emit(progress, 65, stage::Inspecting(), "抽样解码首/中/尾");
    if (fmt && outcome.target_stream_index >= 0) {
        struct RegionProbe {
            std::string label;
            DecodeProbe probe;
        };
        double duration_seconds = fmt->duration > 0 ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
        const AVStream* target_stream = fmt->streams[outcome.target_stream_index];
        if (duration_seconds <= 0 && target_stream->duration > 0)
            duration_seconds = static_cast<double>(target_stream->duration) * av_q2d(target_stream->time_base);

        std::vector<RegionProbe> regions;
        regions.push_back({"首段", ProbeDecode(fmt, outcome.target_stream_index, -1.0, cancel, budget)});
        if (duration_seconds > 1.0) {
            regions.push_back(
                {"中段", ProbeDecode(fmt, outcome.target_stream_index, duration_seconds * 0.5, cancel, budget)});
            regions.push_back({"尾段", ProbeDecode(fmt, outcome.target_stream_index,
                                                   std::max(0.0, duration_seconds - 0.5), cancel, budget)});
        }

        int ran_regions = 0;
        int passed_regions = 0;
        int failed_regions = 0;
        bool region_inconclusive = false;
        bool region_unavailable = false;
        std::ostringstream detail;
        for (const auto& region : regions) {
            if (!detail.str().empty())
                detail << "; ";
            detail << region.label << ": " << region.probe.detail;
            if (!region.probe.ran) {
                region_unavailable = true;
                continue;
            }
            ++ran_regions;
            if (!region.probe.decoder_opened || region.probe.frames == 0) {
                ++failed_regions;
            } else if (region.probe.errors > 0) {
                region_inconclusive = true;
            } else {
                ++passed_regions;
            }
        }

        model::RepairCheckStatus status = model::RepairCheckStatus::NotRun;
        if (ran_regions > 0) {
            if (passed_regions == 0 && failed_regions == ran_regions)
                status = model::RepairCheckStatus::Failed;
            else if (failed_regions > 0 || region_inconclusive || region_unavailable)
                status = model::RepairCheckStatus::Inconclusive;
            else
                status = model::RepairCheckStatus::Passed;
        }
        AddCheck(diagnosis, "decode.sample", model::DamageLayer::Decode, status, detail.str());
        if (status == model::RepairCheckStatus::Failed) {
            model::DamageFinding finding;
            finding.code = "decode.sample_failed";
            finding.detector = kInspector;
            finding.layer = model::DamageLayer::Decode;
            finding.severity = model::IssueSeverity::Error;
            finding.confidence = model::DamageConfidence::Confirmed;
            finding.location.stream_index = outcome.target_stream_index;
            finding.location.precision = model::DamagePrecision::Bounded;
            finding.evidence = detail.str();
            finding.limitations.push_back(
                "首/中/尾三个抽样点均未能解出帧，不能代表整片全部坏掉，但足以阻断按码流直接修复");
            diagnosis.findings.push_back(std::move(finding));
        } else if (failed_regions > 0 || region_unavailable) {
            model::DamageFinding finding;
            finding.code = "decode.sample_region_failed";
            finding.detector = kInspector;
            finding.layer = model::DamageLayer::Decode;
            finding.severity = model::IssueSeverity::Warning;
            finding.confidence = model::DamageConfidence::Confirmed;
            finding.location.stream_index = outcome.target_stream_index;
            finding.location.precision = model::DamagePrecision::Bounded;
            finding.evidence = detail.str();
            finding.limitations.push_back("局部抽样失败不等于整片不可解码，但需要跳段恢复或更保守的完整性结论");
            diagnosis.findings.push_back(std::move(finding));
        }
    } else if (fmt) {
        AddCheck(diagnosis, "decode.sample", model::DamageLayer::Decode, model::RepairCheckStatus::NotRun,
                 "没有可用于解码的媒体轨");
    }

    // ---- 时间轴 ----
    {
        std::ostringstream detail;
        bool valid = true;
        if (fmt) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                const AVStream* stream = fmt->streams[i];
                if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                    stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;
                const bool tb_ok = stream->time_base.num > 0 && stream->time_base.den > 0;
                const bool duration_ok = stream->duration > 0;
                if (!tb_ok)
                    valid = false;
                detail << " stream#" << i << " tb=" << stream->time_base.num << "/" << stream->time_base.den
                       << " duration=" << stream->duration << ";";
                (void)duration_ok;
            }
        } else {
            valid = false;
            detail << "容器未打开，无法读取时间基";
        }
        AddCheck(diagnosis, "timing.stream_timebase", model::DamageLayer::Timing,
                 valid ? model::RepairCheckStatus::Passed : model::RepairCheckStatus::Inconclusive, detail.str());
    }

    // ---- 覆盖范围说明 ----
    // 分片取证已按 NAL 链实扫 mdat 载荷，不能再写"未逐字节扫描"，
    // 也不能把"顶层盒大小之和"当成 bytes_scanned。
    if (!fragment_forensics_applied) {
        if (is_isobmff) {
            coverage.limitations.push_back("样本表检查基于 stsz/stsc/stco 交叉计算，未逐字节读取 mdat");
            coverage.skipped_ranges.push_back("mdat 载荷（未逐字节扫描）");
        } else {
            // 非 ISOBMFF 没有样本表可查，不能沿用上面的措辞冒充做过结构级检查。
            coverage.limitations.push_back("非 ISOBMFF 容器未做样本表级检查；仅按 FFmpeg 解复用结果统计");
        }
        coverage.limitations.push_back("解码检查只覆盖首/中/尾抽样点，未做全片解码");
        coverage.time_scanned_seconds = 0.0;
        coverage.truncated_by_budget = false;
    } else {
        coverage.limitations.push_back("容器未打开，未执行任何解码检查");
    }

    if (fmt)
        avformat_close_input(&fmt);

    // ---- 候选策略 / 摘要 ----
    if ((diagnosis.openable && is_isobmff && diagnosis.moov_present) || outcome.reference_init.has_value()) {
        diagnosis.candidate_actions.push_back(model::RepairAction::S01_Remux);
        if (request.allow_segment_salvage)
            diagnosis.candidate_actions.push_back(model::RepairAction::S04_SegmentSalvage);
        if (request.repair_timeline)
            diagnosis.candidate_actions.push_back(model::RepairAction::S03_TimelineRepair);
    }
    if (outcome.salvage_end_seconds.has_value())
        diagnosis.candidate_actions.push_back(model::RepairAction::S02_TailSalvage);
    if (request.allow_single_track) {
        std::size_t unsalvageable = 0;
        std::size_t salvageable = 0;
        for (const auto& track : diagnosis.track_salvage) {
            if (track.salvageable)
                ++salvageable;
            else
                ++unsalvageable;
        }
        if (unsalvageable > 0 && salvageable > 0)
            diagnosis.candidate_actions.push_back(model::RepairAction::S09_SingleTrack);
    }

    // 只在本次诊断确实落下对应证据时挂载能力边界；无事实不得输出空壳契约。
    diagnosis.capability_boundaries = BuildCapabilityBoundaries(diagnosis);

    std::ostringstream summary;
    summary << (is_isobmff ? "ISOBMFF" : "未知容器") << "，" << (diagnosis.openable ? "可打开" : "无法打开") << "；";
    if (init_segment_missing) {
        summary << (outcome.reference_init.has_value() ? "缺少初始化段（已由参考样片补全）；" : "缺少初始化段；");
        if (fragment_forensics_applied) {
            uint64_t samples = 0;
            for (const auto& expected : diagnosis.expected_tracks)
                samples += expected.sample_count;
            summary << "实测 " << diagnosis.expected_tracks.size() << " 条轨道 / " << samples << " 个样本 / "
                    << diagnosis.coverage.bytes_scanned << " 字节载荷；";
        }
    } else if (!diagnosis.moov_present && is_isobmff)
        summary << "缺少 moov；";
    if (!diagnosis.blocking_reasons.empty())
        summary << "存在 " << diagnosis.blocking_reasons.size() << " 条能力阻断；";
    if (!diagnosis.capability_boundaries.empty())
        summary << "命中 " << diagnosis.capability_boundaries.size() << " 条未实现能力边界；";
    std::size_t unsalvageable_tracks = 0;
    for (const auto& track : diagnosis.track_salvage) {
        if (!track.salvageable)
            ++unsalvageable_tracks;
    }
    if (unsalvageable_tracks > 0)
        summary << "有 " << unsalvageable_tracks << " 条轨道无可用样本（可单轨抢救）；";
    summary << "证据 " << diagnosis.findings.size() << " 条";
    diagnosis.summary = summary.str();

    Emit(progress, 75, stage::Inspecting(), diagnosis.summary);
    if (CheckStop(cancel, budget))
        outcome.canceled = true;
    return outcome;
}

} // namespace repair
} // namespace videoeye
