#include "core/analysis/quality/ReferenceQualityRunner.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include "core/analysis/quality/QualityAnalyzer.h"
#include "core/analysis/quality/ReferenceQualityAnalyzer.h"
#include "core/ffmpeg_io/FfmpegInterrupt.h"

namespace videoeye {
namespace {

std::string AvErrorText(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(error_code, buffer, sizeof(buffer)) < 0)
        return "错误码 " + std::to_string(error_code);
    return buffer;
}

// 单路视频解码: 打开 / 探测 / 取视频流 / 逐帧解码。
//
// 中断状态必须是成员: 回调挂在 AVFormatContext 上，会被派生出的 AVIOContext
// 各复制一份指针，栈上一返回就悬垂（见 core/ffmpeg_io/FfmpegInterrupt.h）。
class Decoder {
public:
    std::atomic<bool>* cancel = nullptr;

    ~Decoder() {
        Close();
    }

    bool Open(const std::string& path, std::string& error) {
        if (path.empty()) {
            error = "路径为空";
            return false;
        }
        if (path.find("://") != std::string::npos) {
            error = "参考帧对比只支持本地文件";
            return false;
        }
        fmt_ = avformat_alloc_context();
        if (!fmt_) {
            error = "无法分配 AVFormatContext";
            return false;
        }
        interrupt_.cancel = cancel;
        ffmpeg_io::AttachInterrupt(fmt_, interrupt_, ffmpeg_io::kOpenTimeoutUs);
        int ret = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            error = "打开失败: " + AvErrorText(ret);
            Close();
            return false;
        }
        interrupt_.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
        ret = avformat_find_stream_info(fmt_, nullptr);
        if (ret < 0) {
            error = "探测流信息失败: " + AvErrorText(ret);
            Close();
            return false;
        }
        const int index = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) {
            error = "没有可用的视频流";
            Close();
            return false;
        }
        stream_index_ = index;
        AVStream* stream = fmt_->streams[index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            error = "找不到对应的视频解码器";
            Close();
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) {
            error = "无法分配解码器上下文";
            Close();
            return false;
        }
        ret = avcodec_parameters_to_context(ctx_, stream->codecpar);
        if (ret < 0) {
            error = "复制解码参数失败: " + AvErrorText(ret);
            Close();
            return false;
        }
        ctx_->pkt_timebase = stream->time_base;
        ret = avcodec_open2(ctx_, codec, nullptr);
        if (ret < 0) {
            error = "打开解码器失败: " + AvErrorText(ret);
            Close();
            return false;
        }
        time_base_ = stream->time_base;
        // pts 缺失时的推进步长: 优先平均帧率，再退到 25fps
        const AVRational rate = (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
                                    ? stream->avg_frame_rate
                                    : stream->r_frame_rate;
        fallback_interval_ = (rate.num > 0 && rate.den > 0) ? av_q2d(av_inv_q(rate)) : 0.04;
        if (!(fallback_interval_ > 0.0))
            fallback_interval_ = 0.04;

        packet_ = av_packet_alloc();
        if (!packet_) {
            error = "无法分配 AVPacket";
            Close();
            return false;
        }
        // 逐包扫描阶段只响应取消: 本地文件的正常读取不能被打开阶段的超时误杀
        interrupt_.deadline_us = 0;
        return true;
    }

    void Close() {
        if (ctx_) {
            avcodec_free_context(&ctx_);
            ctx_ = nullptr;
        }
        if (fmt_) {
            avformat_close_input(&fmt_);
            fmt_ = nullptr;
        }
        if (packet_) {
            av_packet_free(&packet_);
            packet_ = nullptr;
        }
        stream_index_ = -1;
    }

    // 返回 1 = 有一帧；0 = EOF；<0 = 错误（AVERROR_EXIT 表示取消）。
    // out 在写入前会被 av_frame_unref。
    int Next(AVFrame* out) {
        if (!ctx_ || !fmt_ || !out || !packet_)
            return AVERROR(EINVAL);
        av_frame_unref(out);
        for (;;) {
            if (cancel && cancel->load(std::memory_order_relaxed))
                return AVERROR_EXIT;
            int ret = avcodec_receive_frame(ctx_, out);
            if (ret == 0) {
                ++frame_count_;
                return 1;
            }
            if (ret == AVERROR_EOF)
                return 0;
            if (ret != AVERROR(EAGAIN))
                return ret;

            // 需要更多输入: 读到下一个属于本视频流的包
            for (;;) {
                if (cancel && cancel->load(std::memory_order_relaxed))
                    return AVERROR_EXIT;
                ret = av_read_frame(fmt_, packet_);
                if (ret == AVERROR_EOF) {
                    avcodec_send_packet(ctx_, nullptr); // 冲刷解码器里缓存的帧
                    break;
                }
                if (ret < 0)
                    return ret;
                if (packet_->stream_index != stream_index_) {
                    av_packet_unref(packet_);
                    continue;
                }
                ret = avcodec_send_packet(ctx_, packet_);
                av_packet_unref(packet_);
                if (ret < 0)
                    return ret;
                break;
            }
        }
    }

    // 最近一次 Next() 返回的帧的显示时间（秒）。pts 缺失时按帧率累加兜底，
    // 保证时间单调，采样栅格不会因为个别坏帧停住。
    double CurrentSeconds(const AVFrame* frame) {
        int64_t pts = frame->pts;
        if (pts == AV_NOPTS_VALUE)
            pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) {
            last_seconds_ += fallback_interval_;
            return last_seconds_;
        }
        last_seconds_ = pts * av_q2d(time_base_);
        return last_seconds_;
    }

    int frame_count() const {
        return frame_count_;
    }

private:
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* packet_ = nullptr;
    int stream_index_ = -1;
    AVRational time_base_{1, 25};
    double fallback_interval_ = 0.04;
    double last_seconds_ = 0.0;
    int frame_count_ = 0;
    ffmpeg_io::AvInterruptState interrupt_;
};

struct DecodedSide {
    Decoder decoder;
    AVFrame* frame = nullptr;  // 当前帧
    AVFrame* peeked = nullptr; // 预读的下一帧（待测侧挑"更近"的帧时才用）
    bool has_frame = false;
    bool has_peek = false;
    bool eof = false;
    int frame_count = 0;
    double first_seconds = 0.0;
    double normalized = 0.0; // 当前帧首帧归一后的时间
    double last_normalized = 0.0;
    int current_index = -1;
    int peek_index = -1;
    double peek_normalized = 0.0;

    bool Init() {
        frame = av_frame_alloc();
        peeked = av_frame_alloc();
        return frame != nullptr && peeked != nullptr;
    }
    ~DecodedSide() {
        if (frame)
            av_frame_free(&frame);
        if (peeked)
            av_frame_free(&peeked);
    }
};

// 解码一帧到 target。返回 1 = 有帧；0 = EOF；-1 = 失败（error 非空）或被取消（error 为空）。
int ReadFrame(DecodedSide& side, AVFrame* target, double& normalized, int& index, std::string& error) {
    const int ret = side.decoder.Next(target);
    if (ret == 0)
        return 0;
    if (ret < 0) {
        if (ret == AVERROR_EXIT) {
            error.clear();
            return -1;
        }
        error = AvErrorText(ret);
        return -1;
    }
    ++side.frame_count;
    const double seconds = side.decoder.CurrentSeconds(target);
    if (side.frame_count == 1)
        side.first_seconds = seconds;
    normalized = seconds - side.first_seconds;
    index = side.frame_count - 1;
    side.last_normalized = normalized;
    return 1;
}

// 把下一帧读进 current。返回 false: error 为空 = 取消，非空 = 解码失败。
bool Pull(DecodedSide& side, std::string& error) {
    if (side.eof) {
        side.has_frame = false;
        return true;
    }
    const int ret = ReadFrame(side, side.frame, side.normalized, side.current_index, error);
    if (ret == 1) {
        side.has_frame = true;
        return true;
    }
    side.has_frame = false;
    if (ret == 0) {
        side.eof = true;
        return true;
    }
    return false;
}

// 预读下一帧到 peeked（不影响 current）
bool EnsurePeek(DecodedSide& side, std::string& error) {
    if (side.has_peek || side.eof || !side.has_frame)
        return true;
    const int ret = ReadFrame(side, side.peeked, side.peek_normalized, side.peek_index, error);
    if (ret == 1) {
        side.has_peek = true;
        return true;
    }
    if (ret == 0) {
        side.eof = true;
        return true;
    }
    return false;
}

// 消费当前帧: 已预读的下一帧顶上来，否则再解码一帧
bool Consume(DecodedSide& side, std::string& error) {
    if (side.has_peek) {
        std::swap(side.frame, side.peeked);
        side.normalized = side.peek_normalized;
        side.current_index = side.peek_index;
        side.has_frame = true;
        side.has_peek = false;
        return true;
    }
    return Pull(side, error);
}

} // namespace

model::ReferenceQualityResult ReferenceQualityRunner::Run(const std::string& reference_path,
                                                          const std::string& distorted_path,
                                                          const ReferenceQualityRunOptions& options,
                                                          std::atomic<bool>* cancel, const ProgressCallback& progress) {
    ReferenceQualityAccumulator accumulator(options.compare);

    auto failed = [&accumulator](const std::string& message) {
        auto result = accumulator.Finish();
        result.valid = false;
        result.error_message = message;
        return result;
    };

    if (reference_path.empty() || distorted_path.empty())
        return failed("参考与待测文件路径不能为空");
    if (reference_path.find("://") != std::string::npos || distorted_path.find("://") != std::string::npos)
        return failed("参考帧对比只支持本地文件，不接受网络地址");

    DecodedSide reference;
    DecodedSide distorted;
    if (!reference.Init() || !distorted.Init())
        return failed("无法分配帧缓冲");
    reference.decoder.cancel = cancel;
    distorted.decoder.cancel = cancel;

    std::string error;
    if (!reference.decoder.Open(reference_path, error))
        return failed("参考视频打开失败: " + error);
    if (!distorted.decoder.Open(distorted_path, error))
        return failed("待测视频打开失败: " + error);

    const double fps = options.target_fps > 0.0 ? std::min(options.target_fps, 1000.0) : 1.0;
    const double interval = 1.0 / fps;
    const double tolerance =
        options.compare.alignment_tolerance_seconds >= 0.0 ? options.compare.alignment_tolerance_seconds : 0.0;
    const double offset = options.compare.offset_seconds;
    const double end_seconds = options.compare.end_seconds;
    const int sample_width = options.sample_width > 16 ? options.sample_width : 16;

    if (!Pull(reference, error))
        return failed(error.empty() ? "对比已取消" : "参考视频解码失败: " + error);
    if (!Pull(distorted, error))
        return failed(error.empty() ? "对比已取消" : "待测视频解码失败: " + error);
    if (!reference.has_frame)
        return failed("参考视频没有可解码的视频帧");
    if (!distorted.has_frame)
        return failed("待测视频没有可解码的视频帧");

    int target_count = 0;
    int compared = 0;
    int skipped = 0;
    double next_target = options.compare.start_seconds;
    bool cancelled = false;
    bool decode_failed = false;
    model::FrameSample reference_sample;
    model::FrameSample distorted_sample;

    while (reference.has_frame) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            cancelled = true;
            break;
        }

        // 推进到第一个不早于采样栅格的参考帧
        while (reference.has_frame && reference.normalized < next_target - 1e-9) {
            if (!Pull(reference, error)) {
                cancelled = error.empty();
                decode_failed = !cancelled;
                break;
            }
        }
        if (cancelled || decode_failed || !reference.has_frame)
            break;
        if (end_seconds >= 0.0 && reference.normalized > end_seconds + 1e-9)
            break;

        const double target_time = reference.normalized + offset;

        // 待测侧: 丢掉早于容差窗口的帧
        while (distorted.has_frame && distorted.normalized < target_time - tolerance) {
            if (!Consume(distorted, error)) {
                cancelled = error.empty();
                decode_failed = !cancelled;
                break;
            }
        }
        if (cancelled || decode_failed)
            break;

        ++target_count;
        bool matched = false;
        if (distorted.has_frame && std::fabs(distorted.normalized - target_time) <= tolerance) {
            // 再看一帧: 25fps 源按 2fps 采样时，窗口里往往有两帧，
            // 只取"第一帧落进窗口"会整体错开一格（参考侧取的是目标时间之后的第一帧）
            if (!EnsurePeek(distorted, error)) {
                cancelled = error.empty();
                decode_failed = !cancelled;
                break;
            }
            if (distorted.has_peek &&
                std::fabs(distorted.peek_normalized - target_time) < std::fabs(distorted.normalized - target_time)) {
                if (!Consume(distorted, error)) {
                    cancelled = error.empty();
                    decode_failed = !cancelled;
                    break;
                }
            }
            matched = true;
        }
        if (matched) {
            std::string build_error;
            bool built = QualityAnalyzer::BuildSample(reference.frame, sample_width, sample_width, false,
                                                      reference_sample, build_error, false);
            if (!built) {
                accumulator.AddFailure(reference.normalized, "参考帧采样失败: " + build_error);
            } else if (!QualityAnalyzer::BuildSample(distorted.frame, sample_width, sample_width, false,
                                                     distorted_sample, build_error, false)) {
                accumulator.AddFailure(reference.normalized, "待测帧采样失败: " + build_error);
            } else {
                reference_sample.timestamp_seconds = reference.normalized;
                distorted_sample.timestamp_seconds = distorted.normalized;
                reference_sample.frame_index = reference.current_index;
                distorted_sample.frame_index = distorted.current_index;
                const bool accepted =
                    accumulator.AddPair(reference_sample, distorted_sample, reference.normalized, distorted.normalized);
                if (!accepted) {
                    // 命中 max_frames: 已尝试的采样点都记进汇总后结束
                    break;
                }
                ++compared;
                if (progress && compared % 25 == 0)
                    progress(
                        ReferenceQualityRunProgress{reference.normalized, distorted.normalized, compared, skipped});
            }
            // 命中的待测帧只消费一次
            if (!Consume(distorted, error)) {
                cancelled = error.empty();
                decode_failed = !cancelled;
                break;
            }
        } else {
            accumulator.AddSkipped();
            ++skipped;
        }

        next_target = reference.normalized + interval;
        if (!Pull(reference, error)) {
            cancelled = error.empty();
            decode_failed = !cancelled;
            break;
        }
    }

    accumulator.SetSampleCounts(reference.frame_count, distorted.frame_count);
    accumulator.SetDurations(reference.last_normalized, distorted.last_normalized);
    accumulator.SetTargetCount(target_count);
    auto result = accumulator.Finish();
    result.cancelled = cancelled;
    if (decode_failed)
        result.error_message = "解码中断: " + error;
    else if (cancelled)
        result.error_message = "对比已取消";
    return result;
}

} // namespace videoeye
