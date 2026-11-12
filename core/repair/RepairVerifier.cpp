#include "core/repair/RepairVerifier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <sstream>
#include <vector>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/repair/RepairPayloadHash.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace videoeye {
namespace repair {
namespace {

using Clock = std::chrono::steady_clock;

std::string AvErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void Emit(const RepairProgress& progress, int percent, const std::string& detail) {
    if (progress)
        progress(percent, stage::Verifying(), detail);
}

void AddVerificationCheck(model::RepairVerification& verification, const std::string& name, model::DamageLayer layer,
                          model::RepairCheckStatus status, const std::string& detail, int64_t cost_ms = 0) {
    model::RepairCheck check;
    check.name = name;
    check.layer = layer;
    check.status = status;
    check.detail = detail;
    check.cost_ms = static_cast<uint64_t>(std::max<int64_t>(0, cost_ms));
    verification.checks.push_back(std::move(check));
}

struct DecodePass {
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    // R05：分开记账，不能揉进一个"解码错误"。
    uint64_t corrupt_packets = 0;
    uint64_t corrupt_frames = 0;
    uint64_t error_recovery_frames = 0;
    uint64_t valid_frames = 0;
    bool opened = false;
    bool read_error = false;
    bool canceled = false;
    bool timed_out = false;
    std::optional<uint64_t> payload_hash;
    std::string detail;
};

bool CheckStop(const std::atomic<bool>* cancel, const RepairRunBudget* budget) {
    return budget != nullptr ? budget->Checkpoint() : infrastructure::Checkpoint(cancel);
}

// R03：时长差必须参与结论，不能只打印在 timeline_checks 的文字里。
// 容器 / 流层时长本身是估算值，所以要求相对和绝对偏差同时超阈值，避免把正常的
// 时基缩放误判成错误；原报告实测的错误时基是 2.000s -> 1.050s（47.5%）。
constexpr double kTimelineRelativeTolerance = 0.05;
constexpr double kTimelineAbsoluteToleranceSeconds = 0.2;

bool DurationMismatch(double expected, double observed) {
    if (expected <= 0.0 || observed <= 0.0)
        return false;
    const double delta = std::fabs(expected - observed);
    const double relative = delta / std::max(expected, observed);
    return delta > kTimelineAbsoluteToleranceSeconds && relative > kTimelineRelativeTolerance;
}

// 单次打开产物。逐轨验证各用一份独立的 AVFormatContext，避免 seek/read 位置
// 在多条轨之间互相污染。
AVFormatContext* OpenOutput(const std::string& output_path, const std::atomic<bool>* cancel,
                            const RepairRunBudget* budget, ffmpeg_io::AvInterruptState* interrupt, int* open_ret) {
    AVFormatContext* fmt = avformat_alloc_context();
    interrupt->cancel = cancel ? const_cast<std::atomic<bool>*>(cancel) : nullptr;
    if (fmt)
        ffmpeg_io::AttachInterrupt(fmt, *interrupt, ffmpeg_io::kOpenTimeoutUs);
    *open_ret = fmt ? avformat_open_input(&fmt, output_path.c_str(), nullptr, nullptr) : AVERROR(ENOMEM);
    if (*open_ret >= 0) {
        int64_t probe_timeout = ffmpeg_io::kProbeTimeoutUs;
        if (budget != nullptr && budget->HasDeadline()) {
            const int64_t left = budget->RemainingUs();
            probe_timeout = left > 0 ? std::min<int64_t>(probe_timeout, left) : 1;
        }
        interrupt->deadline_us.store(av_gettime() + probe_timeout);
        avformat_find_stream_info(fmt, nullptr);
        interrupt->deadline_us.store(0);
    }
    return fmt;
}

// max_packets <= 0 表示全量解码。
DecodePass DecodeStream(AVFormatContext* fmt, int stream_index, int64_t max_packets, bool hash_payload,
                        const std::atomic<bool>* cancel, const RepairRunBudget* budget) {
    DecodePass pass;
    if (stream_index < 0 || stream_index >= static_cast<int>(fmt->nb_streams)) {
        pass.detail = "无可用媒体轨";
        return pass;
    }
    AVStream* stream = fmt->streams[stream_index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        pass.detail = "未找到解码器: " + std::string(avcodec_get_name(stream->codecpar->codec_id));
        return pass;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(decoder);
    if (!ctx) {
        pass.detail = "无法分配解码上下文";
        return pass;
    }
    if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 || avcodec_open2(ctx, decoder, nullptr) < 0) {
        pass.detail = "解码器打开失败";
        avcodec_free_context(&ctx);
        return pass;
    }
    pass.opened = true;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        pass.detail = "无法分配解码缓冲";
        return pass;
    }

    // 帧状态分类：AV_FRAME_FLAG_CORRUPT = 解码器明确标记画面损坏；
    // decode_error_flags != 0 = 解码器做了错误隐藏 / 恢复，画面可能失真。
    auto count_frame = [&](AVFrame* decoded) {
        const bool corrupt = (decoded->flags & AV_FRAME_FLAG_CORRUPT) != 0;
        const bool recovered = decoded->decode_error_flags != 0;
        if (corrupt)
            ++pass.corrupt_frames;
        else if (recovered)
            ++pass.error_recovery_frames;
        else
            ++pass.valid_frames;
        ++pass.frames;
        av_frame_unref(decoded);
    };

    while (true) {
        if (budget != nullptr) {
            if (budget->Checkpoint()) {
                pass.canceled = budget->Canceled();
                pass.timed_out = !pass.canceled;
                break;
            }
        } else if (infrastructure::Checkpoint(cancel)) {
            pass.canceled = true;
            break;
        }
        if (max_packets > 0 && static_cast<int64_t>(pass.packets) >= max_packets)
            break;

        const int ret = av_read_frame(fmt, packet);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            pass.read_error = true;
            pass.errors++;
            break;
        }
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }
        ++pass.packets;
        if (packet->flags & AV_PKT_FLAG_CORRUPT)
            ++pass.corrupt_packets;
        if (hash_payload) {
            const std::size_t payload_size = static_cast<std::size_t>(std::max(packet->size, 0));
            const uint64_t state = pass.payload_hash.value_or(Fnv1a64Begin());
            // 音频与执行器输入侧共用规范化（AAC 剥离 ADTS 头）；视频保持原始包字节。
            pass.payload_hash = stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                                    ? UpdateAudioPayloadHash(state, packet->data, payload_size,
                                                             stream->codecpar->codec_id == AV_CODEC_ID_AAC)
                                    : Fnv1a64Update(state, packet->data, payload_size);
        }

        const int send_ret = avcodec_send_packet(ctx, packet);
        av_packet_unref(packet);
        if (send_ret < 0) {
            pass.errors++;
            continue;
        }
        while (true) {
            const int receive_ret = avcodec_receive_frame(ctx, frame);
            if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF)
                break;
            if (receive_ret < 0) {
                pass.errors++;
                break;
            }
            count_frame(frame);
        }
    }

    if (!pass.canceled && !pass.timed_out) {
        avcodec_send_packet(ctx, nullptr);
        while (true) {
            const int ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                pass.errors++;
                break;
            }
            count_frame(frame);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);

    std::ostringstream detail;
    detail << "解码 " << pass.packets << " 包 / " << pass.frames << " 帧（有效 " << pass.valid_frames << ", 损坏 "
           << pass.corrupt_frames << ", 错误恢复 " << pass.error_recovery_frames << "）";
    if (pass.errors > 0)
        detail << ", API 错误 " << pass.errors;
    if (pass.corrupt_packets > 0)
        detail << ", 损坏包 " << pass.corrupt_packets;
    if (pass.read_error)
        detail << ", 读取中断";
    if (pass.timed_out)
        detail << ", 超出时间预算";
    pass.detail = detail.str();
    return pass;
}

// 跳转到指定秒并从该处解码少量包。每次跳转必须重置解码器状态，所以走独立的
// DecodeStream（它自己新建上下文 / 缓冲）。
DecodePass SeekAndDecode(AVFormatContext* fmt, int stream_index, double seconds, const std::atomic<bool>* cancel,
                         const RepairRunBudget* budget) {
    DecodePass pass;
    if (CheckStop(cancel, budget)) {
        pass.canceled = budget == nullptr || budget->Canceled();
        pass.timed_out = budget != nullptr && !pass.canceled;
        pass.detail = "跳转验证被取消或超时";
        return pass;
    }
    if (stream_index < 0 || stream_index >= static_cast<int>(fmt->nb_streams)) {
        pass.detail = "无可用媒体轨";
        return pass;
    }
    const AVStream* stream = fmt->streams[stream_index];
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        pass.detail = "时间基无效";
        return pass;
    }
    const int64_t ts = static_cast<int64_t>(std::llround(seconds / av_q2d(stream->time_base)));
    if (av_seek_frame(fmt, stream_index, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        pass.detail = "跳转失败";
        return pass;
    }
    return DecodeStream(fmt, stream_index, 5, false, cancel, budget);
}

// R04 覆盖守护：requested 是执行器声明的「保留轨 → 输出流」契约，但契约本身可能
// 漏报产物里真实存在的媒体轨。旧实现直接 return requested —— 少报一条轨，验证器
// 就完全不提它，整体仍能 Passed（实测：双轨产物只报视频轨时得到「逐轨 1 条, passed」）。
// 这里以产物实际枚举为基准补齐，补进来的轨标成覆盖缺口：不擅自替契约背书、不解码、
// 不冒充已验证，只让结论降级为不确定并把缺口写进报告。
std::vector<model::RepairTrackVerificationTarget>
BuildTargets(const AVFormatContext* fmt, const std::vector<model::RepairTrackVerificationTarget>& requested) {
    // 调用方没有给契约（例如独立调用验证器）→ 由验证器自己枚举，这是合法路径，
    // 不能当成覆盖缺口，否则所有轨都会被判成未覆盖。
    if (requested.empty()) {
        std::vector<model::RepairTrackVerificationTarget> result;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* stream = fmt->streams[i];
            const AVMediaType type = stream->codecpar->codec_type;
            if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
                continue;
            model::RepairTrackVerificationTarget target;
            target.track_id = stream->id > 0 ? static_cast<uint32_t>(stream->id) : 0;
            target.output_stream_index = static_cast<int>(i);
            target.kind = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0
                              ? "attached_pic"
                              : (type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
            target.required = target.kind != "attached_pic";
            if (!target.required)
                target.note = "封面图片不是视频内容轨，不参与可播放性结论";
            result.push_back(std::move(target));
        }
        return result;
    }

    std::vector<model::RepairTrackVerificationTarget> result = requested;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* stream = fmt->streams[i];
        const AVMediaType type = stream->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
            continue;
        const bool declared = std::any_of(result.begin(), result.end(), [i](const auto& target) {
            return target.output_stream_index >= 0 && static_cast<unsigned>(target.output_stream_index) == i;
        });
        if (declared)
            continue;
        model::RepairTrackVerificationTarget target;
        target.track_id = stream->id > 0 ? static_cast<uint32_t>(stream->id) : 0;
        target.output_stream_index = static_cast<int>(i);
        target.kind = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0
                          ? "attached_pic"
                          : (type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
        target.required = target.kind != "attached_pic";
        target.coverage_gap = true;
        target.note = "产物里存在该媒体轨，但执行器未上报保留映射；本轨未纳入本次验证范围";
        result.push_back(std::move(target));
    }
    return result;
}

// 音画同步探测：扫描一遍产物的包时间戳，比较首条视频轨 / 音频轨的展示时间范围。
// 不做解码，只读 PTS；对 V2 全量验证而言，这次全包扫描的成本相对逐轨解码可以忽略。
struct AvSyncProbe {
    int video_index = -1;
    int audio_index = -1;
    bool video_seen = false;
    bool audio_seen = false;
    double video_min = 0.0;
    double video_max = 0.0;
    double audio_min = 0.0;
    double audio_max = 0.0;
    bool completed = false;
    bool read_error = false;
    bool canceled = false;
    bool timed_out = false;
};

AvSyncProbe ProbeAvSync(AVFormatContext* fmt, int video_index, int audio_index, const std::atomic<bool>* cancel,
                        const RepairRunBudget* budget) {
    AvSyncProbe probe;
    probe.video_index = video_index;
    probe.audio_index = audio_index;
    if (fmt == nullptr || video_index < 0 || audio_index < 0)
        return probe;

    AVPacket* packet = av_packet_alloc();
    if (!packet)
        return probe;

    // find_stream_info 可能留下探测读取位置；尽可能回到文件头，保证 first PTS 是真正的起点。
    av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);

    while (true) {
        if (CheckStop(cancel, budget)) {
            probe.canceled = budget == nullptr || budget->Canceled();
            probe.timed_out = budget != nullptr && !probe.canceled;
            break;
        }
        const int ret = av_read_frame(fmt, packet);
        if (ret == AVERROR_EOF) {
            probe.completed = true;
            break;
        }
        if (ret < 0) {
            probe.read_error = true;
            break;
        }
        const int index = packet->stream_index;
        if (index != video_index && index != audio_index) {
            av_packet_unref(packet);
            continue;
        }
        if (packet->pts == AV_NOPTS_VALUE) {
            av_packet_unref(packet);
            continue;
        }
        const AVStream* stream = fmt->streams[index];
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            av_packet_unref(packet);
            continue;
        }
        const double seconds = static_cast<double>(packet->pts) * av_q2d(stream->time_base);
        if (index == video_index) {
            if (!probe.video_seen) {
                probe.video_seen = true;
                probe.video_min = probe.video_max = seconds;
            } else {
                probe.video_min = std::min(probe.video_min, seconds);
                probe.video_max = std::max(probe.video_max, seconds);
            }
        } else {
            if (!probe.audio_seen) {
                probe.audio_seen = true;
                probe.audio_min = probe.audio_max = seconds;
            } else {
                probe.audio_min = std::min(probe.audio_min, seconds);
                probe.audio_max = std::max(probe.audio_max, seconds);
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return probe;
}

} // namespace

RepairVerifyOutcome RepairVerifier::Verify(const std::string& output_path, model::RepairVerificationLevel level,
                                           std::optional<uint64_t> expected_video_hash, const std::atomic<bool>* cancel,
                                           const RepairProgress& progress,
                                           const std::vector<model::RepairTrackVerificationTarget>& targets,
                                           const RepairRunBudget* budget, double expected_duration_seconds,
                                           std::optional<uint64_t> expected_audio_hash) {
    RepairVerifyOutcome outcome;
    model::RepairVerification& verification = outcome.verification;
    model::RepairFidelity& fidelity = outcome.fidelity;
    verification.level = level;

    Emit(progress, 90, "重新打开产物");
    ffmpeg_io::AvInterruptState interrupt;
    int open_ret = 0;
    AVFormatContext* fmt = OpenOutput(output_path, cancel, budget, &interrupt, &open_ret);
    if (open_ret < 0) {
        if (fmt)
            avformat_close_input(&fmt);
        verification.status = model::RepairCheckStatus::Failed;
        AddVerificationCheck(verification, "output.open", model::DamageLayer::Container,
                             model::RepairCheckStatus::Failed, "无法打开产物: " + AvErrorText(open_ret));
        verification.evidence = "产物不可被 FFmpeg 打开";
        return outcome;
    }
    AddVerificationCheck(verification, "output.open", model::DamageLayer::Container, model::RepairCheckStatus::Passed,
                         "产物打开成功, 格式=" +
                             std::string(fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "unknown"));
    AddVerificationCheck(verification, "output.probe", model::DamageLayer::Container, model::RepairCheckStatus::Passed,
                         "流=" + std::to_string(fmt->nb_streams) +
                             ", duration_ms=" + std::to_string(fmt->duration / 1000));
    const double container_duration = fmt->duration > 0 ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;

    const std::vector<model::RepairTrackVerificationTarget> effective = BuildTargets(fmt, targets);
    if (effective.empty()) {
        AddVerificationCheck(verification, "output.media_stream", model::DamageLayer::Track,
                             model::RepairCheckStatus::Failed, "产物没有视频/音频轨");
        verification.status = model::RepairCheckStatus::Failed;
        verification.evidence = "产物没有媒体轨";
        avformat_close_input(&fmt);
        return outcome;
    }

    // ---- R09：V0 只做结构层检查，明确写"尚未验证播放" ----
    if (level == model::RepairVerificationLevel::V0_Container) {
        verification.structure_only = true;
        std::ostringstream scope;
        for (const auto& target : effective) {
            model::RepairTrackVerification record;
            record.track_id = target.track_id;
            record.output_stream_index = target.output_stream_index;
            record.kind = target.kind;
            record.status = model::RepairCheckStatus::NotRun;
            record.verified = false;
            record.detail = "V0 只验证容器结构，未解码本轨";
            verification.per_track.push_back(std::move(record));
            if (!scope.str().empty())
                scope << ", ";
            scope << target.kind << "#" << target.output_stream_index;
        }
        verification.decode_scope = scope.str() + " (structure only, no decode)";
        AddVerificationCheck(verification, "decode.playback", model::DamageLayer::Decode,
                             model::RepairCheckStatus::NotRun, "V0 未执行任何解码，尚未验证播放");
        verification.status = model::RepairCheckStatus::Inconclusive;
        verification.evidence = "V0: 容器结构有效，但尚未验证播放；不能据此断言可播放或音画完整";
        fidelity.video = model::RepairFidelityStatus::NotVerified;
        fidelity.audio = model::RepairFidelityStatus::NotVerified;
        fidelity.limitations.push_back("V0 未解码任何帧，仅结构验证");
        verification.frame_marker_evidence = "not_run";
        verification.frame_marker_limitations.push_back("V0 未解码，不存在帧级损坏标记证据");
        Emit(progress, 96, verification.evidence);
        avformat_close_input(&fmt);
        return outcome;
    }

    const bool full = level == model::RepairVerificationLevel::V2_FullDecode;
    bool first_video_checked = false;
    bool first_audio_checked = false;
    int seek_stream_index = -1;
    int uncovered_tracks = 0;
    bool timeline_mismatch = false;
    double first_comparable_observed = 0.0;
    std::ostringstream scope;

    for (const auto& target : effective) {
        model::RepairTrackVerification record;
        record.track_id = target.track_id;
        record.output_stream_index = target.output_stream_index;
        record.kind = target.kind;
        record.scope = full ? "all packets" : "first 12 packets";

        // 覆盖缺口只留痕、不解码：执行器没声明这轨是保留轨，验证器不能替它背书。
        if (target.coverage_gap) {
            record.status = model::RepairCheckStatus::Inconclusive;
            record.verified = false;
            record.scope = "not verified";
            record.detail = target.note;
            verification.per_track.push_back(std::move(record));
            ++uncovered_tracks;
            AddVerificationCheck(verification, "decode.coverage_gap#" + std::to_string(target.output_stream_index),
                                 model::DamageLayer::Track, model::RepairCheckStatus::Inconclusive, target.note);
            if (!scope.str().empty())
                scope << ", ";
            scope << target.kind << "#" << target.output_stream_index << "(uncovered)";
            continue;
        }
        if (target.output_stream_index < 0 || target.output_stream_index >= static_cast<int>(fmt->nb_streams)) {
            record.status = model::RepairCheckStatus::Failed;
            record.verified = false;
            record.detail = "输出流下标超出产物范围，无法验证";
            verification.per_track.push_back(record);
            // 失败分支也必须进 checks：整体状态只从 checks 汇总，只写 per_track 会被
            // 其它轨的 Passed 盖住（实测：一条越界轨 + 两条正常轨仍得到整体 passed）。
            AddVerificationCheck(verification,
                                 "decode.target_out_of_range#" + std::to_string(target.output_stream_index),
                                 model::DamageLayer::Track, model::RepairCheckStatus::Failed, record.detail);
            continue;
        }
        if (target.kind == "attached_pic" || !target.required) {
            record.status = model::RepairCheckStatus::NotRun;
            record.verified = false;
            record.detail = target.note.empty() ? "封面图片/未要求验证的轨不参与可播放性结论" : target.note;
            verification.per_track.push_back(record);
            AddVerificationCheck(verification, "decode.not_required#" + std::to_string(target.output_stream_index),
                                 model::DamageLayer::Track, model::RepairCheckStatus::NotRun, record.detail);
            continue;
        }
        if (!scope.str().empty())
            scope << ", ";
        scope << target.kind << "#" << target.output_stream_index;

        // 每条轨独立打开：上一轨的 av_read_frame / seek 不会把读位置带过来。
        ffmpeg_io::AvInterruptState track_interrupt;
        int track_open_ret = 0;
        AVFormatContext* track_fmt = OpenOutput(output_path, cancel, budget, &track_interrupt, &track_open_ret);
        if (track_open_ret < 0 || !track_fmt) {
            if (track_fmt)
                avformat_close_input(&track_fmt);
            record.status = model::RepairCheckStatus::Failed;
            record.verified = false;
            record.detail = "重新打开产物失败: " + AvErrorText(track_open_ret);
            verification.per_track.push_back(record);
            AddVerificationCheck(verification, "decode.reopen#" + std::to_string(target.output_stream_index),
                                 model::DamageLayer::Track, model::RepairCheckStatus::Failed, record.detail);
            continue;
        }
        const AVMediaType media_type = track_fmt->streams[target.output_stream_index]->codecpar->codec_type;
        const bool is_video = media_type == AVMEDIA_TYPE_VIDEO;
        const bool is_audio = media_type == AVMEDIA_TYPE_AUDIO;
        const bool hash_video_this = full && is_video && expected_video_hash.has_value() && !first_video_checked;
        const bool hash_audio_this = full && is_audio && expected_audio_hash.has_value() && !first_audio_checked;
        const bool hash_this = hash_video_this || hash_audio_this;
        DecodePass decoded =
            DecodeStream(track_fmt, target.output_stream_index, full ? 0 : 12, hash_this, cancel, budget);
        if (is_video && seek_stream_index < 0)
            seek_stream_index = target.output_stream_index;

        // verified 的契约是「这一轨真的被解码验证过」。解码器没打开、一帧没解出、
        // 被取消 / 超预算都只能算尝试过，不能自称已验证（RepairTypes.h 的字段约定）。
        record.verified = decoded.opened && decoded.frames > 0 && !decoded.canceled && !decoded.timed_out;
        record.packets = decoded.packets;
        record.frames = decoded.frames;
        record.errors = decoded.errors;
        record.corrupt_packets = decoded.corrupt_packets;
        record.corrupt_frames = decoded.corrupt_frames;
        record.error_recovery_frames = decoded.error_recovery_frames;
        record.valid_frames = decoded.valid_frames;
        record.detail = decoded.detail;
        const AVStream* track_stream = track_fmt->streams[target.output_stream_index];
        record.duration_seconds = track_stream->duration > 0
                                      ? static_cast<double>(track_stream->duration) * av_q2d(track_stream->time_base)
                                      : container_duration;
        {
            const bool comparable_here = expected_duration_seconds > 0.0 && record.duration_seconds > 0.0;
            if (comparable_here && first_comparable_observed <= 0.0)
                first_comparable_observed = record.duration_seconds;
            const bool mismatch_here =
                comparable_here && DurationMismatch(expected_duration_seconds, record.duration_seconds);
            if (mismatch_here)
                timeline_mismatch = true;

            std::ostringstream timeline;
            timeline << target.kind << "#" << target.output_stream_index
                     << ": time_base=" << track_stream->time_base.num << "/" << track_stream->time_base.den
                     << ", duration=" << FormatSeconds(record.duration_seconds) << "s, frames=" << decoded.frames;
            if (expected_duration_seconds > 0.0) {
                timeline << ", 源侧 sidx/tfdt 估算=" << FormatSeconds(expected_duration_seconds)
                         << "s, 差值=" << FormatSeconds(std::fabs(record.duration_seconds - expected_duration_seconds))
                         << "s";
                if (mismatch_here)
                    timeline << " [超出容差，疑似时基改写]";
                else if (!comparable_here)
                    timeline << " [产物无可用时长，本轨无法比对]";
            }
            verification.timeline_checks.push_back(timeline.str());
        }
        avformat_close_input(&track_fmt);

        verification.decoded_frames += decoded.frames;
        verification.decode_errors += decoded.errors;
        verification.corrupt_packets += decoded.corrupt_packets;
        verification.corrupt_frames += decoded.corrupt_frames;
        verification.error_recovery_frames += decoded.error_recovery_frames;
        verification.valid_frames += decoded.valid_frames;
        outcome.canceled = outcome.canceled || decoded.canceled;
        outcome.timed_out = outcome.timed_out || decoded.timed_out;

        model::RepairCheckStatus status = model::RepairCheckStatus::Passed;
        if (decoded.canceled || decoded.timed_out)
            status = model::RepairCheckStatus::Inconclusive;
        else if (!decoded.opened || decoded.frames == 0)
            status = model::RepairCheckStatus::Failed;
        else if (decoded.read_error || decoded.errors > 0 || decoded.corrupt_frames > 0 ||
                 decoded.error_recovery_frames > 0 || decoded.corrupt_packets > 0)
            status = model::RepairCheckStatus::Inconclusive;
        record.status = status;
        verification.per_track.push_back(record);
        AddVerificationCheck(verification,
                             (full ? "decode.full." : "decode.samples.") + target.kind + "#" +
                                 std::to_string(target.output_stream_index),
                             model::DamageLayer::Decode, status, decoded.detail);

        if (hash_video_this) {
            first_video_checked = true;
            const DecodePass& hash_pass = decoded;
            if (hash_pass.payload_hash.has_value() && *hash_pass.payload_hash == *expected_video_hash) {
                fidelity.video = model::RepairFidelityStatus::VerifiedMatch;
                AddVerificationCheck(verification, "fidelity.video_payload", model::DamageLayer::Codec,
                                     model::RepairCheckStatus::Passed,
                                     "视频包载荷哈希一致: " + std::to_string(*hash_pass.payload_hash));
            } else {
                fidelity.video = model::RepairFidelityStatus::Failed;
                AddVerificationCheck(verification, "fidelity.video_payload", model::DamageLayer::Codec,
                                     model::RepairCheckStatus::Failed,
                                     "视频包载荷哈希不一致（输入 " + std::to_string(*expected_video_hash) + " / 产物 " +
                                         (hash_pass.payload_hash.has_value() ? std::to_string(*hash_pass.payload_hash)
                                                                             : std::string("unavailable")) +
                                         "）");
            }
        }
        if (hash_audio_this) {
            first_audio_checked = true;
            const DecodePass& hash_pass = decoded;
            const AVCodecID audio_codec = fmt->streams[target.output_stream_index]->codecpar->codec_id;
            const std::string scope = AudioHashScope(audio_codec == AV_CODEC_ID_AAC);
            fidelity.limitations.push_back(
                "音频载荷哈希口径=" + scope +
                "；AAC 跨 MP4/TS 时剥离 ADTS 头后按访问单元累计，不代表原始包字节逐字节一致");
            if (hash_pass.payload_hash.has_value() && *hash_pass.payload_hash == *expected_audio_hash) {
                fidelity.audio = model::RepairFidelityStatus::VerifiedMatch;
                AddVerificationCheck(verification, "fidelity.audio_payload", model::DamageLayer::Codec,
                                     model::RepairCheckStatus::Passed,
                                     "音频访问单元载荷哈希一致: " + std::to_string(*hash_pass.payload_hash));
            } else {
                // 没有规范化能力时不能把「容器表示变了」硬判成内容被改：明确标为
                // 未验证，改由逐轨解码 + 样本守恒支撑完整性结论。
                const bool reformatted = fmt->iformat != nullptr && fmt->iformat->name != nullptr &&
                                         std::string(fmt->iformat->name).find("mpegts") != std::string::npos;
                const bool unverifiable = reformatted && audio_codec != AV_CODEC_ID_AAC;
                if (unverifiable) {
                    fidelity.audio = model::RepairFidelityStatus::NotVerified;
                    const std::string detail = "音频编码 " + std::string(avcodec_get_name(audio_codec)) +
                                               " 在容器切换后缺少访问单元规范化能力，原始载荷未验证";
                    AddVerificationCheck(verification, "fidelity.audio_payload", model::DamageLayer::Codec,
                                         model::RepairCheckStatus::Inconclusive, detail);
                    fidelity.limitations.push_back(detail + "；完整性证据由逐轨解码与包数守恒补足");
                } else {
                    fidelity.audio = model::RepairFidelityStatus::Failed;
                    AddVerificationCheck(
                        verification, "fidelity.audio_payload", model::DamageLayer::Codec,
                        model::RepairCheckStatus::Failed,
                        "音频载荷哈希不一致（输入 " + std::to_string(*expected_audio_hash) + " / 产物 " +
                            (hash_pass.payload_hash.has_value() ? std::to_string(*hash_pass.payload_hash)
                                                                : std::string("unavailable")) +
                            "，口径 " + scope + "）");
                }
            }
        }
    }
    if (!first_video_checked)
        fidelity.video = model::RepairFidelityStatus::NotVerified;
    if (!first_audio_checked)
        fidelity.audio = model::RepairFidelityStatus::NotVerified;
    verification.decode_scope = scope.str() + (full ? ", all packets" : ", sampled packets");

    // ---- 音画同步（V2 全包扫描）----
    // 单轨可解码不代表音画关系还在：跳段恢复 / 单轨抢救 / 时间轴重写都可能让
    // 音频与视频的展示区间相对错开。这里用首尾 PTS 差值的变化量给出结构化结论。
    if (full) {
        int sync_video = -1;
        int sync_audio = -1;
        for (const auto& target : effective) {
            if (target.coverage_gap || !target.required)
                continue;
            if (target.kind == "video" && sync_video < 0)
                sync_video = target.output_stream_index;
            if (target.kind == "audio" && sync_audio < 0)
                sync_audio = target.output_stream_index;
        }
        if (sync_video >= 0 && sync_audio >= 0) {
            AvSyncProbe sync = ProbeAvSync(fmt, sync_video, sync_audio, cancel, budget);
            outcome.canceled = outcome.canceled || sync.canceled;
            outcome.timed_out = outcome.timed_out || sync.timed_out;
            if (sync.completed && sync.video_seen && sync.audio_seen) {
                verification.av_sync_checked = true;
                verification.av_sync_start_delta_seconds = std::fabs(sync.video_min - sync.audio_min);
                verification.av_sync_end_delta_seconds = std::fabs(sync.video_max - sync.audio_max);
                verification.av_sync_drift_seconds =
                    std::fabs(verification.av_sync_end_delta_seconds - verification.av_sync_start_delta_seconds);
                constexpr double kDriftToleranceSeconds = 0.5;
                const bool drift = verification.av_sync_drift_seconds > kDriftToleranceSeconds;
                verification.av_sync_status = drift ? "drift" : "within_tolerance";
                std::ostringstream sync_detail;
                sync_detail << "视频展示区间 [" << FormatSeconds(sync.video_min) << ", "
                            << FormatSeconds(sync.video_max) << "] s，音频 [" << FormatSeconds(sync.audio_min) << ", "
                            << FormatSeconds(sync.audio_max) << "] s，首端偏移 "
                            << FormatSeconds(verification.av_sync_start_delta_seconds) << " s，尾端偏移 "
                            << FormatSeconds(verification.av_sync_end_delta_seconds) << " s，漂移 "
                            << FormatSeconds(verification.av_sync_drift_seconds) << " s（容差 "
                            << FormatSeconds(kDriftToleranceSeconds) << " s）";
                verification.timeline_checks.push_back("A/V sync: " + sync_detail.str());
                AddVerificationCheck(verification, "timing.av_sync", model::DamageLayer::Timing,
                                     drift ? model::RepairCheckStatus::Inconclusive : model::RepairCheckStatus::Passed,
                                     sync_detail.str());
                if (drift) {
                    model::DamageFinding residual;
                    residual.code = "timing.av_sync_drift";
                    residual.detector = "repair.verifier";
                    residual.layer = model::DamageLayer::Timing;
                    residual.severity = model::IssueSeverity::Warning;
                    residual.confidence = model::DamageConfidence::Possible;
                    residual.location.precision = model::DamagePrecision::Bounded;
                    residual.location.provenance = "output video/audio first-last PTS";
                    residual.evidence = sync_detail.str();
                    residual.limitations.push_back(
                        "只比较展示时间范围，不证明逐帧音画同步；音频编码器 priming 会引入小额固定偏移");
                    verification.residual_findings.push_back(std::move(residual));
                }
            } else if (sync.canceled || sync.timed_out) {
                verification.av_sync_status = "not_run";
                AddVerificationCheck(verification, "timing.av_sync", model::DamageLayer::Timing,
                                     model::RepairCheckStatus::Inconclusive, "音画同步扫描被取消或超出预算");
            } else {
                verification.av_sync_status = "not_applicable";
                std::ostringstream detail;
                detail << "音画同步无法判定（读取" << (sync.read_error ? "中断" : "未覆盖到流尾") << "）";
                AddVerificationCheck(verification, "timing.av_sync", model::DamageLayer::Timing,
                                     model::RepairCheckStatus::NotRun, detail.str());
            }
        } else {
            verification.av_sync_status = "not_applicable";
            AddVerificationCheck(verification, "timing.av_sync", model::DamageLayer::Timing,
                                 model::RepairCheckStatus::NotRun, "产物缺少可比较的视频轨或音频轨");
        }
    }

    // ---- R03：把时长证据从「可读文字」升级为「参与结论的结构化结果」----
    verification.expected_duration_seconds = expected_duration_seconds;
    verification.observed_duration_seconds = first_comparable_observed;
    verification.timeline_comparable = first_comparable_observed > 0.0;
    verification.duration_delta_seconds =
        verification.timeline_comparable ? std::fabs(first_comparable_observed - expected_duration_seconds) : 0.0;
    verification.timeline_mismatch = timeline_mismatch;
    if (timeline_mismatch) {
        std::ostringstream detail;
        detail << "源侧 sidx/tfdt 估算时长 " << FormatSeconds(expected_duration_seconds) << "s，产物实测 "
               << FormatSeconds(first_comparable_observed) << "s，差值 "
               << FormatSeconds(verification.duration_delta_seconds)
               << "s；帧可以全部解出但播放时长 / 速度可能已被借用的参考时基改变";
        AddVerificationCheck(verification, "timeline.duration_mismatch", model::DamageLayer::Timing,
                             model::RepairCheckStatus::Inconclusive, detail.str());
        model::DamageFinding residual;
        residual.code = "timeline.duration_mismatch";
        residual.detector = "repair.verifier";
        residual.layer = model::DamageLayer::Timing;
        residual.severity = model::IssueSeverity::Warning;
        residual.confidence = model::DamageConfidence::Possible;
        residual.location.precision = model::DamagePrecision::Bounded;
        residual.location.provenance = "source sidx/tfdt duration vs output stream duration/time_base";
        residual.evidence = detail.str();
        residual.limitations.push_back("源侧时长是结构估算值，不是原始完整视频的解码时长");
        residual.limitations.push_back("没有原始完整视频，不能给出恢复百分比或断言无失真");
        verification.residual_findings.push_back(std::move(residual));
    }

    // R05：损坏帧 / 错误恢复帧单独作为残留证据，且不能报"通过"。
    if (verification.decode_errors > 0) {
        model::DamageFinding residual;
        residual.code = "decode.api_errors";
        residual.detector = "repair.verifier";
        residual.layer = model::DamageLayer::Decode;
        residual.severity = model::IssueSeverity::Warning;
        residual.location.precision = model::DamagePrecision::Bounded;
        residual.evidence = "解码 API 返回 " + std::to_string(verification.decode_errors) + " 个错误";
        verification.residual_findings.push_back(std::move(residual));
    }
    if (verification.corrupt_frames > 0 || verification.corrupt_packets > 0) {
        model::DamageFinding residual;
        residual.code = "decode.corrupt_frames";
        residual.detector = "repair.verifier";
        residual.layer = model::DamageLayer::Decode;
        residual.severity = model::IssueSeverity::Error;
        residual.location.precision = model::DamagePrecision::Bounded;
        residual.evidence = "发现 " + std::to_string(verification.corrupt_frames) + " 个带 CORRUPT 标记的帧 / " +
                            std::to_string(verification.corrupt_packets) + " 个带 CORRUPT 标记的包";
        residual.limitations.push_back("没有原始完整视频，不能判断这些帧的实际失真程度");
        verification.residual_findings.push_back(std::move(residual));
    }
    if (verification.error_recovery_frames > 0) {
        model::DamageFinding residual;
        residual.code = "decode.error_concealment";
        residual.detector = "repair.verifier";
        residual.layer = model::DamageLayer::Decode;
        residual.severity = model::IssueSeverity::Warning;
        residual.location.precision = model::DamagePrecision::Bounded;
        residual.evidence = "发现 " + std::to_string(verification.error_recovery_frames) +
                            " 个带 decode_error_flags 的帧（解码器做了错误隐藏 / 恢复）";
        residual.limitations.push_back("错误隐藏不必然意味着可见失真，但也无法证明画面干净");
        verification.residual_findings.push_back(std::move(residual));
    }

    // ---- 跳转验证（只在第一条视频轨上做，逐轨全量解码已覆盖其余轨）----
    // MPEG-TS 是线性流式容器：没有样本索引，BACKWARD seek 落在任意包上（多半不是
    // 关键帧），跳转后短窗口解不出帧并不代表产物损坏。因此对 TS 不执行严格跳转判据，
    // 只如实标注"未执行"，避免把合法的容器转换误判成验证失败。
    // 注意：验证拿到的是临时 .part 路径，不能靠扩展名判断容器，必须看实际 demuxer。
    if (seek_stream_index >= 0) {
        ffmpeg_io::AvInterruptState seek_interrupt;
        int seek_open_ret = 0;
        AVFormatContext* seek_fmt = OpenOutput(output_path, cancel, budget, &seek_interrupt, &seek_open_ret);
        if (seek_open_ret >= 0 && seek_fmt) {
            const std::string demuxer_name =
                seek_fmt->iformat != nullptr && seek_fmt->iformat->name != nullptr ? seek_fmt->iformat->name : "";
            if (demuxer_name.find("mpegts") != std::string::npos) {
                AddVerificationCheck(verification, "decode.seek_unavailable", model::DamageLayer::Decode,
                                     model::RepairCheckStatus::Inconclusive,
                                     "MPEG-TS 为线性流式容器，缺少样本索引；本次未执行随机跳转验证（不据此判定失败）");
                avformat_close_input(&seek_fmt);
            } else {
                double duration_seconds = container_duration;
                if (seek_fmt->streams[seek_stream_index]->duration > 0)
                    duration_seconds = static_cast<double>(seek_fmt->streams[seek_stream_index]->duration) *
                                       av_q2d(seek_fmt->streams[seek_stream_index]->time_base);
                const double near_end = duration_seconds > 0 ? std::max(0.0, duration_seconds - 0.5) : 0.0;
                std::vector<double> seek_points;
                if (level == model::RepairVerificationLevel::V1_FrameSample)
                    seek_points = {0.0, duration_seconds * 0.5, near_end};
                else if (full)
                    seek_points = {0.0, duration_seconds * 0.25, duration_seconds * 0.5, duration_seconds * 0.75,
                                   near_end};
                for (double point : seek_points) {
                    if (CheckStop(cancel, budget)) {
                        outcome.canceled = budget == nullptr || budget->Canceled();
                        outcome.timed_out = budget != nullptr && !outcome.canceled;
                        break;
                    }
                    ++verification.seek_checks;
                    DecodePass seek_pass = SeekAndDecode(seek_fmt, seek_stream_index, point, cancel, budget);
                    if (seek_pass.frames > 0) {
                        ++verification.seek_passes;
                    } else {
                        std::ostringstream check_detail;
                        check_detail << "跳转到 " << FormatSeconds(point) << " s 后未能解出帧";
                        if (!seek_pass.detail.empty())
                            check_detail << "（" << seek_pass.detail << "）";
                        AddVerificationCheck(verification, "decode.seek@" + FormatSeconds(point),
                                             model::DamageLayer::Decode, model::RepairCheckStatus::Failed,
                                             check_detail.str());
                    }
                }
                if (!seek_points.empty()) {
                    AddVerificationCheck(verification, "decode.seek_summary", model::DamageLayer::Decode,
                                         verification.seek_passes == verification.seek_checks
                                             ? model::RepairCheckStatus::Passed
                                             : model::RepairCheckStatus::Inconclusive,
                                         std::to_string(verification.seek_passes) + "/" +
                                             std::to_string(verification.seek_checks) + " 个跳转点可解码");
                }
                avformat_close_input(&seek_fmt);
            }
        } else {
            if (seek_fmt)
                avformat_close_input(&seek_fmt);
            // 旧实现在这里什么都不写：跳转验证整个被静默跳过，整体结论照样能 Passed。
            AddVerificationCheck(verification, "decode.seek_unavailable", model::DamageLayer::Decode,
                                 model::RepairCheckStatus::Inconclusive,
                                 "跳转验证未能重新打开产物，未执行任何跳转检查");
        }
    }

    // ---- R05：帧级标记证据的三态 ----
    //
    // 帧标记是「解码器能力」而不是「画面干净度」的全集。项目锁定 FFmpeg 8.1 的
    // 可用解码器对字节损坏只返回 API 错误 / decode_error_flags，AV_FRAME_FLAG_CORRUPT
    // 实测恒 0；因此 corrupt_frames=0 必须被标注成 not_observed，不能读成「无损坏」。
    verification.marked_frames = verification.corrupt_frames + verification.error_recovery_frames;
    verification.frame_marker_evidence =
        verification.decoded_frames == 0 ? "not_run" : (verification.marked_frames > 0 ? "observed" : "not_observed");
    verification.frame_marker_limitations.push_back(
        "AV_FRAME_FLAG_CORRUPT 在项目锁定的 FFmpeg 8.1 可用解码器上实测恒 0（H.264/HEVC/AAC/"
        "MPEG-1/2/4/VP8/VP9/AV1/MJPEG/FLV1 的字节损坏样本）；corrupt_frames=0 不能推出画面干净");
    verification.frame_marker_limitations.push_back("decode_error_flags 只覆盖解码器主动做了错误隐藏、仍吐出帧的情况；"
                                                    "返回 API 错误后被丢弃的帧不会带任何帧标记");
    if (verification.frame_marker_evidence == "not_observed" && verification.decode_errors > 0) {
        AddVerificationCheck(verification, "decode.frame_marker_unavailable", model::DamageLayer::Decode,
                             model::RepairCheckStatus::Inconclusive,
                             "解出 " + std::to_string(verification.decoded_frames) + " 帧且 API 报 " +
                                 std::to_string(verification.decode_errors) +
                                 " 个错误，但没有任何帧级损坏标记；帧级分类在本解码器上不可达，"
                                 "不能据此断言画面干净");
    }

    // ---- 汇总 ----
    bool any_failed = false;
    bool any_inconclusive = false;
    for (const auto& check : verification.checks) {
        if (check.status == model::RepairCheckStatus::Failed)
            any_failed = true;
        if (check.status == model::RepairCheckStatus::Inconclusive)
            any_inconclusive = true;
    }
    if (outcome.canceled || outcome.timed_out) {
        verification.status = model::RepairCheckStatus::Inconclusive;
    } else if (any_failed) {
        verification.status = model::RepairCheckStatus::Failed;
    } else if (verification.decoded_frames == 0) {
        verification.status = model::RepairCheckStatus::Failed;
    } else if (any_inconclusive || uncovered_tracks > 0 || verification.timeline_mismatch ||
               verification.corrupt_frames > 0 || verification.error_recovery_frames > 0 ||
               verification.corrupt_packets > 0) {
        verification.status = model::RepairCheckStatus::Inconclusive;
    } else {
        verification.status = model::RepairCheckStatus::Passed;
    }

    std::ostringstream evidence;
    evidence << ToString(level) << ": " << verification.decoded_frames << " 帧（有效 " << verification.valid_frames
             << " / 损坏 " << verification.corrupt_frames << " / 错误恢复 " << verification.error_recovery_frames
             << "）, API 错误 " << verification.decode_errors << ", 跳转 " << verification.seek_passes << "/"
             << verification.seek_checks << " 通过, 逐轨 " << verification.per_track.size() << " 条, 未覆盖轨 "
             << uncovered_tracks << " 条, 时间轴="
             << (verification.timeline_mismatch ? "mismatch"
                                                : (verification.timeline_comparable ? "comparable" : "no_baseline"))
             << ", 帧标记=" << verification.frame_marker_evidence << " (" << verification.marked_frames << ")"
             << ", 状态=" << ToString(verification.status);
    verification.evidence = evidence.str();

    fidelity.video_reencoded = false;
    fidelity.audio_reencoded = false;
    if (!first_audio_checked)
        fidelity.limitations.push_back("未做音频载荷哈希比对");
    if (!full)
        fidelity.limitations.push_back("未做 V2 全包音画同步扫描");
    // 切片 M：本层不知道调用方是否提供了参考样片，不能写「需要参考样片」——
    // 参考样片辅助重建那条路明明有参考，这句会和结论层自相矛盾。
    fidelity.limitations.push_back("未做逐帧画质 / 感知保真度评估（本次未启用画质比对）");
    fidelity.limitations.push_back("解码器未设置 CORRUPT / decode_error_flags 的失真无法被本检查发现");

    Emit(progress, 96, verification.evidence);
    avformat_close_input(&fmt);
    return outcome;
}

} // namespace repair
} // namespace videoeye
