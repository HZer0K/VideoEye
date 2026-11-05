#include "core/repair/RepairVerifier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <sstream>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"

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

int PickVerificationStream(const AVFormatContext* fmt) {
    int video = -1;
    int audio = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVMediaType type = fmt->streams[i]->codecpar->codec_type;
        if (video < 0 && type == AVMEDIA_TYPE_VIDEO)
            video = static_cast<int>(i);
        else if (audio < 0 && type == AVMEDIA_TYPE_AUDIO)
            audio = static_cast<int>(i);
    }
    return video >= 0 ? video : audio;
}

struct DecodePass {
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    bool opened = false;
    bool read_error = false;
    bool canceled = false;
    std::optional<uint64_t> payload_hash;
    std::string detail;
};

// max_packets <= 0 表示全量解码。
DecodePass DecodeStream(AVFormatContext* fmt, int stream_index, int64_t max_packets, bool hash_payload,
                        const std::atomic<bool>* cancel) {
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

    while (true) {
        if (infrastructure::Checkpoint(cancel)) {
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
        if (hash_payload) {
            pass.payload_hash = Fnv1a64Update(pass.payload_hash.value_or(Fnv1a64Begin()), packet->data,
                                              static_cast<std::size_t>(std::max(packet->size, 0)));
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
            ++pass.frames;
            av_frame_unref(frame);
        }
    }

    if (!pass.canceled) {
        avcodec_send_packet(ctx, nullptr);
        while (true) {
            const int ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                pass.errors++;
                break;
            }
            ++pass.frames;
            av_frame_unref(frame);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);

    std::ostringstream detail;
    detail << "解码 " << pass.packets << " 包 / " << pass.frames << " 帧";
    if (pass.errors > 0)
        detail << ", 错误 " << pass.errors;
    if (pass.read_error)
        detail << ", 读取中断";
    pass.detail = detail.str();
    return pass;
}

// 跳转到指定秒并从该处解码少量包。每次跳转必须重置解码器状态，所以走独立的
// DecodeStream（它自己新建上下文 / 缓冲）。
DecodePass SeekAndDecode(AVFormatContext* fmt, int stream_index, double seconds, const std::atomic<bool>* cancel) {
    DecodePass pass;
    if (infrastructure::Checkpoint(cancel)) {
        pass.canceled = true;
        pass.detail = "跳转验证被取消";
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
    return DecodeStream(fmt, stream_index, 5, false, cancel);
}

} // namespace

RepairVerifyOutcome RepairVerifier::Verify(const std::string& output_path, model::RepairVerificationLevel level,
                                           std::optional<uint64_t> expected_video_hash, const std::atomic<bool>* cancel,
                                           const RepairProgress& progress) {
    RepairVerifyOutcome outcome;
    model::RepairVerification& verification = outcome.verification;
    model::RepairFidelity& fidelity = outcome.fidelity;
    verification.level = level;

    Emit(progress, 90, "重新打开产物");
    AVFormatContext* fmt = avformat_alloc_context();
    ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = cancel ? const_cast<std::atomic<bool>*>(cancel) : nullptr;
    if (fmt)
        ffmpeg_io::AttachInterrupt(fmt, interrupt, ffmpeg_io::kOpenTimeoutUs);
    int open_ret = fmt ? avformat_open_input(&fmt, output_path.c_str(), nullptr, nullptr) : AVERROR(ENOMEM);
    if (open_ret < 0) {
        verification.status = model::RepairCheckStatus::Failed;
        AddVerificationCheck(verification, "output.open", model::DamageLayer::Container,
                             model::RepairCheckStatus::Failed, "无法打开产物: " + AvErrorText(open_ret));
        verification.evidence = "产物不可被 FFmpeg 打开";
        return outcome;
    }

    interrupt.deadline_us.store(av_gettime() + ffmpeg_io::kProbeTimeoutUs);
    const int probe_ret = avformat_find_stream_info(fmt, nullptr);
    interrupt.deadline_us.store(0);
    AddVerificationCheck(verification, "output.open", model::DamageLayer::Container, model::RepairCheckStatus::Passed,
                         "产物打开成功, 格式=" +
                             std::string(fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "unknown"));
    if (probe_ret < 0) {
        AddVerificationCheck(verification, "output.probe", model::DamageLayer::Container,
                             model::RepairCheckStatus::Inconclusive, "流信息探测失败: " + AvErrorText(probe_ret));
    } else {
        AddVerificationCheck(
            verification, "output.probe", model::DamageLayer::Container, model::RepairCheckStatus::Passed,
            "流=" + std::to_string(fmt->nb_streams) + ", duration_ms=" + std::to_string(fmt->duration / 1000));
    }

    const int stream_index = PickVerificationStream(fmt);
    const bool is_video = stream_index >= 0 && fmt->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    if (stream_index < 0) {
        AddVerificationCheck(verification, "output.media_stream", model::DamageLayer::Track,
                             model::RepairCheckStatus::Failed, "产物没有视频/音频轨");
        verification.status = model::RepairCheckStatus::Failed;
        verification.evidence = "产物没有媒体轨";
        avformat_close_input(&fmt);
        return outcome;
    }
    AddVerificationCheck(verification, "output.media_stream", model::DamageLayer::Track,
                         model::RepairCheckStatus::Passed,
                         "验证轨 #" + std::to_string(stream_index) + " (" + (is_video ? "video" : "audio") + ")");

    verification.decode_scope = (is_video ? "video#" : "audio#") + std::to_string(stream_index);

    DecodePass decoded;
    if (level != model::RepairVerificationLevel::V0_Container) {
        Emit(progress, 92, "解码验证");
        const bool full = level == model::RepairVerificationLevel::V2_FullDecode;
        {
            // V1 只取首段；V2 全量。V1 / V2 共用同一个哈希开关：
            // 只有 V2 且期望哈希存在时，才把载荷比对当作"已核对"。
            decoded = DecodeStream(fmt, stream_index, full ? 0 : 12, full && expected_video_hash.has_value(), cancel);
        }
        verification.decoded_frames = decoded.frames;
        verification.decode_errors = decoded.errors;
        outcome.canceled = decoded.canceled;

        model::RepairCheckStatus decode_status = model::RepairCheckStatus::Passed;
        if (decoded.canceled)
            decode_status = model::RepairCheckStatus::Inconclusive;
        else if (!decoded.opened || decoded.frames == 0)
            decode_status = model::RepairCheckStatus::Failed;
        else if (decoded.read_error)
            decode_status = model::RepairCheckStatus::Inconclusive;
        AddVerificationCheck(verification, full ? "decode.full" : "decode.samples", model::DamageLayer::Decode,
                             decode_status, decoded.detail);

        if (full && expected_video_hash.has_value() && is_video) {
            if (decoded.payload_hash.has_value() && *decoded.payload_hash == *expected_video_hash) {
                fidelity.video = model::RepairFidelityStatus::VerifiedMatch;
                AddVerificationCheck(verification, "fidelity.video_payload", model::DamageLayer::Codec,
                                     model::RepairCheckStatus::Passed,
                                     "视频包载荷哈希一致: " + std::to_string(*decoded.payload_hash));
            } else {
                fidelity.video = model::RepairFidelityStatus::Failed;
                AddVerificationCheck(verification, "fidelity.video_payload", model::DamageLayer::Codec,
                                     model::RepairCheckStatus::Failed,
                                     "视频包载荷哈希不一致（输入 " + std::to_string(*expected_video_hash) + " / 产物 " +
                                         (decoded.payload_hash.has_value() ? std::to_string(*decoded.payload_hash)
                                                                           : std::string("unavailable")) +
                                         "）");
            }
        } else if (is_video) {
            fidelity.video = model::RepairFidelityStatus::NotVerified;
        }
        if (decoded.errors > 0 && decoded.frames > 0) {
            model::DamageFinding residual;
            residual.code = "decode.errors";
            residual.detector = "repair.verifier";
            residual.layer = model::DamageLayer::Decode;
            residual.severity = model::IssueSeverity::Warning;
            residual.confidence = model::DamageConfidence::Confirmed;
            residual.location.stream_index = stream_index;
            residual.location.precision = model::DamagePrecision::Bounded;
            residual.evidence = "解码过程中记录 " + std::to_string(decoded.errors) + " 个错误（已产出 " +
                                std::to_string(decoded.frames) + " 帧）";
            residual.limitations.push_back("错误计数包含解码器容忍的损坏帧，需逐帧检查确认画质");
            verification.residual_findings.push_back(std::move(residual));
        }

        // ---- 跳转验证 ----
        double duration_seconds = 0.0;
        if (fmt->duration > 0)
            duration_seconds = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        else if (fmt->streams[stream_index]->duration > 0)
            duration_seconds = static_cast<double>(fmt->streams[stream_index]->duration) *
                               av_q2d(fmt->streams[stream_index]->time_base);
        double near_end = duration_seconds > 0 ? std::max(0.0, duration_seconds - 0.5) : 0.0;

        std::vector<double> seek_points;
        if (level == model::RepairVerificationLevel::V1_FrameSample) {
            seek_points = {0.0, duration_seconds * 0.5, near_end};
        } else if (level == model::RepairVerificationLevel::V2_FullDecode) {
            seek_points = {0.0, duration_seconds * 0.25, duration_seconds * 0.5, duration_seconds * 0.75, near_end};
        }
        for (double point : seek_points) {
            if (infrastructure::Checkpoint(cancel)) {
                outcome.canceled = true;
                break;
            }
            ++verification.seek_checks;
            DecodePass seek_pass = SeekAndDecode(fmt, stream_index, point, cancel);
            if (seek_pass.frames > 0) {
                ++verification.seek_passes;
            } else {
                std::ostringstream check_detail;
                check_detail << "跳转到 " << FormatSeconds(point) << " s 后未能解出帧";
                if (!seek_pass.detail.empty())
                    check_detail << "（" << seek_pass.detail << "）";
                AddVerificationCheck(verification, "decode.seek@" + FormatSeconds(point), model::DamageLayer::Decode,
                                     model::RepairCheckStatus::Failed, check_detail.str());
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
    if (outcome.canceled) {
        verification.status = model::RepairCheckStatus::Inconclusive;
    } else if (any_failed) {
        verification.status = model::RepairCheckStatus::Failed;
    } else if (level != model::RepairVerificationLevel::V0_Container && verification.decoded_frames == 0) {
        verification.status = model::RepairCheckStatus::Failed;
    } else if (any_inconclusive) {
        verification.status = model::RepairCheckStatus::Inconclusive;
    } else {
        verification.status = model::RepairCheckStatus::Passed;
    }

    std::ostringstream evidence;
    evidence << ToString(level) << ": " << verification.decoded_frames << " 帧, " << verification.decode_errors
             << " 个解码错误, 跳转 " << verification.seek_passes << "/" << verification.seek_checks
             << " 通过, 状态=" << ToString(verification.status);
    verification.evidence = evidence.str();

    fidelity.audio = model::RepairFidelityStatus::NotVerified;
    fidelity.video_reencoded = false;
    fidelity.audio_reencoded = false;
    fidelity.limitations.push_back("未做音频载荷哈希比对");
    fidelity.limitations.push_back("未做逐帧画质 / 感知保真度评估（需要参考样片）");

    Emit(progress, 96, verification.evidence);
    avformat_close_input(&fmt);
    return outcome;
}

} // namespace repair
} // namespace videoeye
