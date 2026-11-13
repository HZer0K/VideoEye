#include "core/repair/RepairTolerantTranscode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <vector>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/repair/RepairSampleScan.h"
#include "core/repair/RepairTranscodeFidelity.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace videoeye {
namespace repair {
namespace {

using Clock = std::chrono::steady_clock;

// 测试专用故障注入，默认全关（见 RepairTolerantTranscode.h）。
std::atomic<int> g_inject_write_failure_nth{0};
std::atomic<bool> g_inject_trailer_failure{false};

bool ShouldInjectWriteFailure(uint64_t attempt) {
    const int nth = g_inject_write_failure_nth.load(std::memory_order_relaxed);
    return nth > 0 && attempt == static_cast<uint64_t>(nth);
}

std::string AvErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void Emit(const RepairProgress& progress, int percent, const std::string& detail) {
    if (progress)
        progress(percent, stage::Executing(), detail);
}

std::string FormatDb(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << value << " dB";
    return out.str();
}

// 预算 / 取消统一出口：所有 CPU 循环（转码逐包、首帧探测、PSNR 抽帧、
// FrameReader 内部读包循环）共用同一份状态，避免"只在外层查一次"的漏网。
struct StopState {
    const std::atomic<bool>* cancel = nullptr;
    const RepairRunBudget* budget = nullptr;
    bool canceled = false;
    bool timed_out = false;

    StopState(const std::atomic<bool>* c, const RepairRunBudget* b) : cancel(c), budget(b) {
    }

    bool Check() {
        if (budget != nullptr) {
            if (!budget->Checkpoint())
                return false;
            canceled = budget->Canceled();
            timed_out = !canceled;
            return true;
        }
        if (infrastructure::Checkpoint(cancel)) {
            canceled = true;
            return true;
        }
        return false;
    }

    const char* Reason() const {
        return canceled ? "被取消" : "超出本次修复预算";
    }
};

// 临时产物名字带 .part 后缀，容器扩展名要从 paths.extension 取。
const char* FormatNameForExtension(const std::string& extension) {
    if (extension == "mov")
        return "mov";
    if (extension == "mkv")
        return "matroska";
    if (extension == "ts")
        return "mpegts";
    return "mp4";
}

bool FormatNeedsExtradata(const char* format_name) {
    return std::string(format_name) == "mp4" || std::string(format_name) == "mov" ||
           std::string(format_name) == "matroska";
}

// 首帧探测期间读到的非视频包暂存表。RAII 保证任何早退路径都不泄漏 AVPacket。
class PendingPackets {
public:
    PendingPackets() = default;
    ~PendingPackets() {
        Clear();
    }
    PendingPackets(const PendingPackets&) = delete;
    PendingPackets& operator=(const PendingPackets&) = delete;

    bool Push(const AVPacket* packet, std::uint64_t* bytes, std::uint64_t max_bytes, std::uint64_t max_count) {
        const std::uint64_t size = static_cast<std::uint64_t>(std::max(packet->size, 0));
        if (packets_.size() >= max_count || *bytes + size > max_bytes)
            return false;
        AVPacket* clone = av_packet_clone(packet);
        if (clone == nullptr)
            return false;
        packets_.push_back(clone);
        *bytes += size;
        return true;
    }
    const std::vector<AVPacket*>& items() const {
        return packets_;
    }
    void Clear() {
        for (AVPacket* packet : packets_)
            av_packet_free(&packet);
        packets_.clear();
    }

private:
    std::vector<AVPacket*> packets_;
};

AVRational SourceFrameRate(const AVStream* stream) {
    AVRational fps = stream->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0)
        fps = stream->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0)
        fps = AVRational{25, 1};
    if (fps.num <= 0 || fps.den <= 0)
        fps = AVRational{25, 1};
    return fps;
}

// 打开一个只读的解码器读取器；用于转码前的输入解码与转码后的 PSNR 抽帧。
class FrameReader {
public:
    ~FrameReader() {
        if (frame_ != nullptr)
            av_frame_free(&frame_);
        if (packet_ != nullptr)
            av_packet_free(&packet_);
        if (decoder_ != nullptr)
            avcodec_free_context(&decoder_);
        if (fmt_ != nullptr)
            avformat_close_input(&fmt_);
    }

    bool Open(const std::string& path, std::string* error, const std::atomic<bool>* cancel,
              const RepairRunBudget* budget) {
        // 打开 / 探测必须在起 IO 之前装好中断回调：网络源或半开的本地管道会让
        // avformat_open_input / find_stream_info 永远不返回，外层的循环检查点够不着。
        fmt_ = avformat_alloc_context();
        if (fmt_ == nullptr) {
            *error = "无法分配输入封装上下文";
            return false;
        }
        interrupt_.cancel = cancel ? const_cast<std::atomic<bool>*>(cancel) : nullptr;
        int64_t open_timeout = ffmpeg_io::kOpenTimeoutUs;
        if (budget != nullptr && budget->HasDeadline()) {
            const int64_t left = budget->RemainingUs();
            open_timeout = left > 0 ? std::min<int64_t>(open_timeout, left) : 1;
        }
        ffmpeg_io::AttachInterrupt(fmt_, interrupt_, open_timeout);
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0 || fmt_ == nullptr) {
            *error = "无法打开源文件: " + path;
            return false;
        }
        int64_t probe_timeout = ffmpeg_io::kProbeTimeoutUs;
        if (budget != nullptr && budget->HasDeadline()) {
            const int64_t left = budget->RemainingUs();
            probe_timeout = left > 0 ? std::min<int64_t>(probe_timeout, left) : 1;
        }
        interrupt_.deadline_us.store(av_gettime() + probe_timeout, std::memory_order_relaxed);
        const int info_ret = avformat_find_stream_info(fmt_, nullptr);
        interrupt_.deadline_us.store(0, std::memory_order_relaxed);
        if (info_ret < 0) {
            *error = "无法读取源文件媒体信息";
            return false;
        }
        stream_index_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index_ < 0) {
            *error = "源文件没有可解码的视频轨";
            return false;
        }
        const AVCodec* codec = avcodec_find_decoder(fmt_->streams[stream_index_]->codecpar->codec_id);
        if (codec == nullptr) {
            *error = "没有可用的视频解码器";
            return false;
        }
        decoder_ = avcodec_alloc_context3(codec);
        if (decoder_ == nullptr ||
            avcodec_parameters_to_context(decoder_, fmt_->streams[stream_index_]->codecpar) < 0) {
            *error = "无法初始化视频解码器";
            return false;
        }
        decoder_->thread_count = 0;
        if (avcodec_open2(decoder_, codec, nullptr) < 0) {
            *error = "无法打开视频解码器";
            return false;
        }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (frame_ == nullptr || packet_ == nullptr) {
            *error = "无法分配解码缓冲";
            return false;
        }
        return true;
    }

    // 返回下一帧（引用内部的 frame_，下一次调用前有效）；EOF / 持续解码失败时返回 nullptr。
    // stop 贯穿内部读包循环：预算/取消不能在"每帧一次"的外层粒度上检查。
    AVFrame* Next(uint64_t* skipped_packets, StopState* stop) {
        if (eof_)
            return nullptr;
        av_frame_unref(frame_);
        for (;;) {
            if (stop != nullptr && stop->Check()) {
                eof_ = true;
                return nullptr;
            }
            const int ret = avcodec_receive_frame(decoder_, frame_);
            if (ret == 0) {
                ++decoded_frames_;
                return frame_;
            }
            if (ret == AVERROR_EOF) {
                eof_ = true;
                return nullptr;
            }
            // 旧实现在冲刷解码器时只 return 第一帧就置 eof_，其余延迟帧全丢，
            // PSNR 配对会静默少算。改成下次调用继续从解码器取，直到真的 EOF。
            if (flushing_) {
                eof_ = true;
                return nullptr;
            }
            if (ret != AVERROR(EAGAIN)) {
                // 帧级解码错误：跳过并继续尝试下一包，不能把整个输入当成 EOF。
                if (!SendNext(skipped_packets, stop))
                    StartFlush();
                continue;
            }
            if (!SendNext(skipped_packets, stop))
                StartFlush();
        }
    }

    int stream_index() const {
        return stream_index_;
    }
    AVCodecContext* decoder() const {
        return decoder_;
    }
    uint64_t decoded_frames() const {
        return decoded_frames_;
    }

private:
    void StartFlush() {
        avcodec_send_packet(decoder_, nullptr);
        flushing_ = true;
    }

    bool SendNext(uint64_t* skipped_packets, StopState* stop) {
        for (;;) {
            if (stop != nullptr && stop->Check())
                return false;
            const int ret = av_read_frame(fmt_, packet_);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    return false;
                if (read_failed_)
                    return false;
                read_failed_ = true;
                continue; // 读错误后再试一次，仍失败则按输入结束处理
            }
            if (packet_->stream_index != stream_index_) {
                av_packet_unref(packet_);
                continue;
            }
            if (avcodec_send_packet(decoder_, packet_) < 0) {
                if (skipped_packets != nullptr)
                    ++*skipped_packets;
                av_packet_unref(packet_);
                return true; // 让上层继续 receive_frame
            }
            av_packet_unref(packet_);
            return true;
        }
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* decoder_ = nullptr;
    ffmpeg_io::AvInterruptState interrupt_;
    int stream_index_ = -1;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    uint64_t decoded_frames_ = 0;
    bool eof_ = false;
    bool flushing_ = false;
    bool read_failed_ = false;
};

// 亮度平面 PSNR（只支持 8-bit 4:2:0 / 4:2:2 / 4:4:4 平面布局的 Y 通道）。
double LumaPsnr(const AVFrame* a, const AVFrame* b) {
    if (a->width != b->width || a->height != b->height)
        return -1.0;
    if (a->format != b->format)
        return -1.0;
    const int width = a->width;
    const int height = a->height;
    const uint8_t* pa = a->data[0];
    const uint8_t* pb = b->data[0];
    if (pa == nullptr || pb == nullptr)
        return -1.0;
    double mse = 0.0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* ra = pa + static_cast<std::ptrdiff_t>(y) * a->linesize[0];
        const uint8_t* rb = pb + static_cast<std::ptrdiff_t>(y) * b->linesize[0];
        for (int x = 0; x < width; ++x) {
            const double d = static_cast<double>(ra[x]) - static_cast<double>(rb[x]);
            mse += d * d;
        }
    }
    mse /= static_cast<double>(width) * static_cast<double>(height);
    if (mse <= 1e-9)
        return 99.0;
    return 10.0 * std::log10(65025.0 / mse);
}

// 抽帧 PSNR：按"第 N 个可解码帧"逐一配对（转码时帧序号就是输出 PTS），
// 前 16 帧逐帧比对，之后每 100 帧取一帧，最多 32 个样本。
struct PsnrResult {
    uint64_t samples = 0;
    double average_db = 0.0;
    std::string note;
    bool stopped = false;
    std::string stop_reason;
};

PsnrResult MeasurePsnr(const std::string& input_path, const std::string& output_path, const std::atomic<bool>* cancel,
                       const RepairRunBudget* budget, StopState* stop) {
    PsnrResult result;
    FrameReader input;
    FrameReader output;
    std::string error;
    if (!input.Open(input_path, &error, cancel, budget) || !output.Open(output_path, &error, cancel, budget)) {
        result.note = error;
        return result;
    }

    uint64_t pair_index = 0;
    uint64_t skipped_input = 0;
    uint64_t skipped_output = 0;
    double sum = 0.0;
    uint64_t measured = 0;
    constexpr uint64_t kMaxPsnrSamples = 32;

    for (;;) {
        // 采样目标达成后立刻收手：旧实现把整段输入 / 产物继续解码到 EOF，
        // 32 个样本之外的时间全部白烧在预算之外。
        if (measured >= kMaxPsnrSamples)
            break;
        if (stop != nullptr && stop->Check()) {
            result.stopped = true;
            result.stop_reason = stop->Reason();
            break;
        }
        AVFrame* in_frame = input.Next(&skipped_input, stop);
        AVFrame* out_frame = output.Next(&skipped_output, stop);
        if (in_frame == nullptr || out_frame == nullptr)
            break;
        const bool sample = (pair_index < 16) || (pair_index % 100 == 0);
        if (sample) {
            const double psnr = LumaPsnr(in_frame, out_frame);
            if (psnr >= 0.0) {
                sum += psnr;
                ++measured;
            }
        }
        ++pair_index;
    }

    result.samples = measured;
    result.average_db = measured > 0 ? sum / static_cast<double>(measured) : 0.0;
    std::ostringstream note;
    note << "抽帧 PSNR 比对 " << pair_index << " 对帧，有效样本 " << measured;
    if (skipped_input > 0)
        note << "；输入侧跳过 " << skipped_input << " 个解码失败包";
    if (result.stopped)
        note << "；画质评估阶段" << result.stop_reason << "，未跑满采样计划";
    note << "（亮度平面，非全量逐帧比对）";
    result.note = note.str();
    return result;
}

} // namespace

void SetTranscodeWriteFailureForTesting(int nth_write) {
    g_inject_write_failure_nth.store(nth_write, std::memory_order_relaxed);
}

void SetTranscodeTrailerFailureForTesting(bool enabled) {
    g_inject_trailer_failure.store(enabled, std::memory_order_relaxed);
}

RepairExecutionOutcome RepairTolerantTranscode::Execute(const model::RepairRequest& request,
                                                        const model::RepairDiagnosis& diagnosis,
                                                        const model::RepairPlan& plan, const RepairOutputPaths& paths,
                                                        const std::atomic<bool>* cancel, const RepairProgress& progress,
                                                        const RepairRunBudget* budget) {
    RepairExecutionOutcome outcome;
    outcome.part_path = paths.part_path;
    (void)diagnosis;
    (void)plan;
    outcome.ffmpeg_version = av_version_info();
    StopState stop(cancel, budget);

    // 打开 / 探测前装 IO 中断：预算到期或用户取消时，阻塞在 avformat_open_input /
    // find_stream_info 里的调用必须能被踢出来，而不是等外层循环下一次检查。
    ffmpeg_io::AvInterruptState input_interrupt;
    AVFormatContext* in = avformat_alloc_context();
    if (in == nullptr) {
        outcome.error = "无法分配输入封装上下文";
        return outcome;
    }
    input_interrupt.cancel = cancel ? const_cast<std::atomic<bool>*>(cancel) : nullptr;
    int64_t open_timeout = ffmpeg_io::kOpenTimeoutUs;
    if (budget != nullptr && budget->HasDeadline()) {
        const int64_t left = budget->RemainingUs();
        open_timeout = left > 0 ? std::min<int64_t>(open_timeout, left) : 1;
    }
    ffmpeg_io::AttachInterrupt(in, input_interrupt, open_timeout);
    if (avformat_open_input(&in, request.source_path.c_str(), nullptr, nullptr) < 0 || in == nullptr) {
        outcome.error = "无法打开源文件（转码前读取失败）";
        return outcome;
    }
    int64_t probe_timeout = ffmpeg_io::kProbeTimeoutUs;
    if (budget != nullptr && budget->HasDeadline()) {
        const int64_t left = budget->RemainingUs();
        probe_timeout = left > 0 ? std::min<int64_t>(probe_timeout, left) : 1;
    }
    input_interrupt.deadline_us.store(av_gettime() + probe_timeout, std::memory_order_relaxed);
    const int info_ret = avformat_find_stream_info(in, nullptr);
    input_interrupt.deadline_us.store(0, std::memory_order_relaxed);
    if (info_ret < 0) {
        avformat_close_input(&in);
        outcome.error = "无法读取源文件媒体信息";
        return outcome;
    }
    if (stop.Check()) {
        avformat_close_input(&in);
        outcome.canceled = stop.canceled;
        outcome.timed_out = stop.timed_out;
        outcome.error = std::string("转码打开/探测阶段") + stop.Reason();
        return outcome;
    }

    const int video_index = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0) {
        avformat_close_input(&in);
        outcome.error = "源文件没有可转码的视频轨";
        return outcome;
    }

    const AVStream* video_in = in->streams[video_index];
    const AVCodec* decoder_codec = avcodec_find_decoder(video_in->codecpar->codec_id);
    if (decoder_codec == nullptr) {
        avformat_close_input(&in);
        outcome.error = "没有可用的视频解码器";
        return outcome;
    }
    AVCodecContext* decoder = avcodec_alloc_context3(decoder_codec);
    if (decoder == nullptr || avcodec_parameters_to_context(decoder, video_in->codecpar) < 0) {
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法初始化视频解码器";
        return outcome;
    }
    decoder->thread_count = 0;
    if (avcodec_open2(decoder, decoder_codec, nullptr) < 0) {
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法打开视频解码器（编码参数可能已损坏）";
        return outcome;
    }

    // ---- 输出上下文 ----
    const std::string output_ext = paths.extension.empty() ? LowerExtension(paths.part_path) : paths.extension;
    const char* format_name = FormatNameForExtension(output_ext);
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, format_name, paths.part_path.c_str()) < 0 || out == nullptr) {
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法创建输出封装上下文 (" + std::string(format_name) + ")";
        return outcome;
    }

    // 切片 O：首选编码器不可用必须留下事实，不能静默换一个继续跑。
    // 本项目锁定的 FFmpeg 是 LGPL 变体（--disable-libx264），这里必然走兜底。
    constexpr const char* kPreferredEncoder = "libx264";
    const AVCodec* encoder_codec = avcodec_find_encoder_by_name(kPreferredEncoder);
    const bool encoder_fallback = encoder_codec == nullptr;
    if (encoder_fallback)
        encoder_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (encoder_codec == nullptr) {
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "当前构建缺少 H.264 编码器，无法执行容忍式转码";
        return outcome;
    }

    // 先解出第一帧，拿到真实分辨率 / 像素格式后再配置编码器。
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* first_frame = nullptr;
    if (packet == nullptr || frame == nullptr) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法分配转码缓冲";
        return outcome;
    }

    uint64_t dropped_decode_packets = 0;
    // 首帧之前出现的音频包必须缓存并在输出音轨建好后回放：B 帧延迟、自动解码
    // 线程、视频起点靠后都会造成"先来一批音频"。旧实现直接 unref，产物静默
    // 丢掉这一段，而且 S08 又不要求验证音频轨，丢了没人看得见。
    PendingPackets pending_nonvideo;
    uint64_t pending_bytes = 0;
    bool pending_overflow = false;
    constexpr uint64_t kMaxPendingPackets = 16384;
    constexpr uint64_t kMaxPendingBytes = 64ull * 1024ull * 1024ull;
    for (;;) {
        if (stop.Check())
            break;
        const int read_ret = av_read_frame(in, packet);
        if (read_ret < 0) {
            // 冲刷解码器取剩余帧。
            avcodec_send_packet(decoder, nullptr);
            if (avcodec_receive_frame(decoder, frame) == 0)
                first_frame = frame;
            break;
        }
        if (packet->stream_index != video_index) {
            if (!pending_nonvideo.Push(packet, &pending_bytes, kMaxPendingBytes, kMaxPendingPackets))
                pending_overflow = true;
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(decoder, packet) < 0) {
            ++dropped_decode_packets;
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);
        if (avcodec_receive_frame(decoder, frame) == 0) {
            first_frame = frame;
            break;
        }
    }

    if (first_frame == nullptr || first_frame->width <= 0 || first_frame->height <= 0 || stop.canceled ||
        stop.timed_out) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        if (stop.canceled || stop.timed_out) {
            outcome.canceled = stop.canceled;
            outcome.timed_out = stop.timed_out;
            outcome.error = std::string("转码首帧探测阶段") + stop.Reason();
        } else {
            outcome.error = "源视频没有可解码帧（编码层已损坏，无法转码）";
        }
        return outcome;
    }

    const int width = first_frame->width;
    const int height = first_frame->height;
    const AVRational fps = SourceFrameRate(video_in);

    AVCodecContext* encoder = avcodec_alloc_context3(encoder_codec);
    if (encoder == nullptr) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法分配视频编码器";
        return outcome;
    }

    AVPixelFormat encoder_format = AV_PIX_FMT_YUV420P;
    if (encoder_codec->pix_fmts != nullptr) {
        bool has_420 = false;
        for (const AVPixelFormat* p = encoder_codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_YUV420P)
                has_420 = true;
        }
        if (!has_420)
            encoder_format = encoder_codec->pix_fmts[0];
    }

    encoder->width = width;
    encoder->height = height;
    encoder->pix_fmt = encoder_format;
    // 阶段 6 / 切片 J：把「不依赖位深」的源侧属性原样交给编码器，并逐项实测
    // 这次重编码到底会丢什么。色彩标记只在位深不变时继承 —— 给 8-bit SDR
    // 画面贴 bt2020/smpte2084，播放器会按 HDR 渲染，比不写标记更糟。
    RepairTranscodeInheritance inherited = ApplyTranscodeInheritance(
        encoder, video_in, first_frame, TranscodeBitDepth(static_cast<AVPixelFormat>(first_frame->format)),
        TranscodeBitDepth(encoder_format));
    // CFR 帧间隔 = av_inv_q(fps)。旧实现用 {1, fps.num} 且每帧 PTS 只 +1，
    // 等于把分母丢掉：30000/1001 的内容被按 30000/1 的速度写，约 1001 秒
    // 的跨度压成 1 秒。PTS 落在编码器时基上逐帧 +1 tick 才是正确帧间隔。
    encoder->time_base = av_inv_q(fps);
    encoder->framerate = fps;
    encoder->gop_size = std::max(1, static_cast<int>(fps.num / std::max(1, fps.den)) * 12);
    encoder->max_b_frames = 0;
    encoder->bit_rate = video_in->codecpar->bit_rate > 0 ? video_in->codecpar->bit_rate : 2000000;
    if (FormatNeedsExtradata(format_name))
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (encoder_codec->id == AV_CODEC_ID_H264) {
        av_opt_set(encoder->priv_data, "preset", "veryfast", 0);
        av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
    }
    if (avcodec_open2(encoder, encoder_codec, nullptr) < 0) {
        avcodec_free_context(&encoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法打开 H.264 编码器（" + std::string(encoder_codec->name) + "）";
        return outcome;
    }
    outcome.transcode_note += "编码器=" + std::string(encoder_codec->name);
    // 切片 O：编码器与 profile 的事实就地固化 —— encoder 后面会被 free，
    // encoder->profile 也只有在 open 之后才由编码器回填。
    // 产物侧拿不到回报就留空，报告如实写「未实测」，不从源侧或 codec id 推断。
    const std::string used_encoder_name = encoder_codec->name;
    const std::string output_profile_name =
        encoder->profile != AV_PROFILE_UNKNOWN
            ? TranscodeColorName(avcodec_profile_name(encoder->codec_id, encoder->profile))
            : std::string();
    const std::string source_profile_name =
        video_in->codecpar->profile != AV_PROFILE_UNKNOWN
            ? TranscodeColorName(avcodec_profile_name(video_in->codecpar->codec_id, video_in->codecpar->profile))
            : std::string();

    AVStream* video_out = avformat_new_stream(out, nullptr);
    if (video_out == nullptr || avcodec_parameters_from_context(video_out->codecpar, encoder) < 0) {
        avcodec_free_context(&encoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法创建输出视频轨";
        return outcome;
    }
    video_out->time_base = encoder->time_base;
    video_out->avg_frame_rate = fps;
    const int video_out_index = static_cast<int>(out->nb_streams) - 1;
    // 逐轨覆盖要按输出流下标交代「恢复的是哪条轨、什么编码」。收尾时 out / in
    // 都已释放，所以流类型与编码名必须在这里就地捕获。
    std::map<int, std::pair<std::string, std::string>> stream_meta;
    stream_meta[video_out_index] = {"video", avcodec_get_name(encoder->codec_id)};

    // 音频：尽量按原码流复制；失败 / 不兼容的轨道逐项记账。
    std::vector<int> audio_map(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* src = in->streams[i];
        if (src->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (src->discard == AVDISCARD_ALL)
            continue;
        AVStream* dst = avformat_new_stream(out, nullptr);
        if (dst == nullptr || avcodec_parameters_copy(dst->codecpar, src->codecpar) < 0) {
            outcome.omitted_items.push_back("音频流 #" + std::to_string(i) + " 无法复制编码参数，产物未保留");
            continue;
        }
        dst->codecpar->codec_tag = 0;
        dst->time_base = src->time_base;
        av_dict_copy(&dst->metadata, src->metadata, 0);
        dst->disposition = src->disposition;
        audio_map[i] = static_cast<int>(out->nb_streams) - 1;
        stream_meta[audio_map[i]] = {"audio", avcodec_get_name(src->codecpar->codec_id)};
    }

    if (avio_open(&out->pb, paths.part_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        avcodec_free_context(&encoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法创建临时产物: " + paths.part_path;
        return outcome;
    }
    if (avformat_write_header(out, nullptr) < 0) {
        avio_closep(&out->pb);
        avcodec_free_context(&encoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "写入封装头失败（音频编码参数可能不被目标容器接受）";
        return outcome;
    }

    AVFrame* converted = av_frame_alloc();
    AVPacket* encoded = av_packet_alloc();
    SwsContext* sws = nullptr;
    if (converted == nullptr || encoded == nullptr) {
        av_packet_free(&encoded);
        av_frame_free(&converted);
        av_write_trailer(out);
        avio_closep(&out->pb);
        avcodec_free_context(&encoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "无法分配转码输出缓冲";
        return outcome;
    }
    converted->format = encoder_format;
    converted->width = width;
    converted->height = height;

    // ---- 切片 P：音画起点用「统一原点」平移，不再每条轨各自归零 ----
    //
    // 旧实现：视频 PTS = 帧序号（永远从 0 开始），音频 PTS = 各自首包 PTS 归零。
    // 两条轨互不知情，源里视频比音频晚 Δ 秒时（首段视频丢失 / 录音被打断 /
    // -itsoffset），产物把 Δ 静默抹成 0，报告一个字都不提 —— 用户看到的是
    // 「修好了但口型对不上」。实测 1.6 s 样本：源 video_start=0.48 / audio_start=0，
    // 产物两者都变 0。
    //
    // 现在：先取两条轨的源侧起点，取较小者作统一原点，两条轨都减同一个原点。
    // 起点差因此被保留，且 Δ 会写进报告（transcode.av_start_offset_seconds）。
    const double fps_value = fps.num > 0 && fps.den > 0 ? static_cast<double>(fps.num) / fps.den : 25.0;
    std::optional<double> source_video_start_seconds;
    std::optional<double> source_audio_start_seconds;
    // 每条轨的「呈现起点」：优先用容器声明的 stream->start_time —— 它已经包含
    // elst 补偿，是播放器实际看到的起点；只有容器没声明时才退回首帧 / 首包时间戳。
    // ❗ 不能直接用首包 PTS：AAC 的首包 PTS 常是负的（编码器 priming，实测
    // -0.0232 s），容器用 elst 把它补到 0。拿它当起点会让每条产物凭空多一帧。
    std::vector<std::optional<double>> stream_start_seconds(in->nb_streams);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* stream = in->streams[i];
        if (stream->start_time != AV_NOPTS_VALUE)
            stream_start_seconds[i] = static_cast<double>(stream->start_time) * av_q2d(stream->time_base);
    }
    if (first_frame->pts != AV_NOPTS_VALUE) {
        const double video_frame_seconds = static_cast<double>(first_frame->pts) * av_q2d(video_in->time_base);
        if (!stream_start_seconds[video_index] || video_frame_seconds < *stream_start_seconds[video_index])
            stream_start_seconds[video_index] = video_frame_seconds;
    }
    for (const AVPacket* cached : pending_nonvideo.items()) {
        if (cached->stream_index < 0 || cached->stream_index >= static_cast<int>(in->nb_streams))
            continue;
        const std::size_t index = static_cast<std::size_t>(cached->stream_index);
        if (audio_map[index] < 0 || stream_start_seconds[index].has_value())
            continue;
        const int64_t pts = cached->pts != AV_NOPTS_VALUE ? cached->pts : cached->dts;
        if (pts != AV_NOPTS_VALUE)
            stream_start_seconds[index] = static_cast<double>(pts) * av_q2d(in->streams[index]->time_base);
    }
    // 统一原点 = 已参与输出的轨里最早的呈现起点。两条轨都减它，相对关系因此保留。
    source_video_start_seconds = stream_start_seconds[video_index];
    std::optional<double> origin_seconds_option;
    if (source_video_start_seconds)
        origin_seconds_option = *source_video_start_seconds;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        if (audio_map[i] < 0 || !stream_start_seconds[i])
            continue;
        if (!origin_seconds_option || *stream_start_seconds[i] < *origin_seconds_option)
            origin_seconds_option = *stream_start_seconds[i];
        if (!source_audio_start_seconds || *stream_start_seconds[i] < *source_audio_start_seconds)
            source_audio_start_seconds = *stream_start_seconds[i];
    }
    // 负的呈现起点只有一个来源：容器已经把编码器 priming 用 elst 补偿掉了
    // （实测 AAC 首包 -0.0232 s，而 stream->start_time 报 NOPTS）。播放器看到的
    // 起点是 0，锚点也必须取 0 —— 否则「没有起点差」的源会被算出 0.023 s 的假差值。
    if (source_video_start_seconds && *source_video_start_seconds < 0.0)
        source_video_start_seconds = 0.0;
    if (source_audio_start_seconds && *source_audio_start_seconds < 0.0)
        source_audio_start_seconds = 0.0;
    if (origin_seconds_option && *origin_seconds_option < 0.0)
        origin_seconds_option = 0.0;
    const double origin_seconds = origin_seconds_option.value_or(0.0);
    // 视频侧平移量落在编码器时基（1/fps）上，按帧取整。
    const int64_t video_pts_offset_frames =
        source_video_start_seconds ? std::llround((*source_video_start_seconds - origin_seconds) * fps_value) : 0;
    std::vector<int64_t> audio_origin_ticks(in->nb_streams, AV_NOPTS_VALUE);

    sws = sws_getContext(width, height, static_cast<AVPixelFormat>(first_frame->format), width, height, encoder_format,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
    // 切片 J：sws 是按「首帧尺寸 + 首帧像素格式」建的。源片中途换尺寸 / 换像素
    // 格式时，继续用旧 context 喂新帧是**越界读**（srcSliceH 与行宽都对不上），
    // 旧实现没有任何保护。状态由 TranscodeScaleState 统一维护，逐帧实测计数。
    TranscodeScaleState scale_state;
    scale_state.output_width = width;
    scale_state.output_height = height;
    scale_state.source_width = width;
    scale_state.source_height = height;
    scale_state.source_format = first_frame->format;
    // 固定尺寸只分配一次缓冲，之后逐帧 av_frame_make_writable。
    // ❗ av_frame_unref() 会连同 format / width / height 一起清零，再调
    // av_frame_get_buffer 必然 EINVAL：旧实现从第二帧起把每一帧都算成
    // "源视频损坏"丢掉，40 帧正常输入只写 1 帧，执行器却仍返回成功。
    const bool buffer_ok = av_frame_get_buffer(converted, 0) >= 0;
    if (!buffer_ok || sws == nullptr) {
        outcome.error = buffer_ok ? "无法初始化像素格式转换器（源像素格式不受支持，不是帧内容损坏）"
                                  : "无法分配转码输出帧缓冲（资源不足，不是源视频损坏）";
        if (sws != nullptr)
            sws_freeContext(sws);
        av_packet_free(&encoded);
        av_frame_free(&converted);
        av_write_trailer(out);
        avio_closep(&out->pb);
        avcodec_free_context(&encoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        return outcome;
    }

    // 到这里编码器已打开、转换帧缓冲已分配，才具备「逐项实测」的前提。
    // 前面任何一步失败都保持 inspected=false —— 空表不许冒充「属性均已保留」。
    inherited.chroma_location_copied = ApplyTranscodeFrameProperties(converted, video_in);
    outcome.transcode_fidelity = InspectTranscodeFidelity(video_in, first_frame, encoder_format, inherited);
    // 切片 O：编码器事实与被探测属性分开落 —— 前者在「没探测到首帧」的路径上
    // 同样成立（编码器已经打开过），不能因为 inspected=false 就一起丢掉。
    outcome.transcode_fidelity.encoder_preferred = kPreferredEncoder;
    outcome.transcode_fidelity.encoder_used = used_encoder_name;
    outcome.transcode_fidelity.encoder_fallback = encoder_fallback;
    outcome.transcode_fidelity.source_profile = source_profile_name;
    outcome.transcode_fidelity.output_profile = output_profile_name;
    // 渲染只做一次，报告层与限制项都读这里。
    outcome.transcode_fidelity.encoder_note = TranscodeEncoderLabel(outcome.transcode_fidelity);
    outcome.transcode_fidelity.profile_note = TranscodeProfileNote(outcome.transcode_fidelity);

    uint64_t encoded_frames = 0;
    // 样本账本：S08 产物是「视频重编码 + 音频码流复制」的混合。源字节偏移不可得，
    // 时间戳按首包归零重建，一律标 Inferred，不与保码流路径的容器原值混淆。
    // 报告里的写入包数 / 逐轨包数全部由它求和，不再另设计数器。
    RepairSampleLedger sample_ledger;
    uint64_t bytes_written = 0;
    bool write_failed = false;
    std::string write_error;
    // 资源错误（缓冲分配失败）要和"源帧损坏"分开：它不是可丢弃的坏帧。
    bool resource_failed = false;
    std::string resource_error;
    bool size_capped = false;
    uint64_t write_attempts = 0;
    const uint64_t max_output_bytes = request.limits.max_output_bytes;
    const double frame_seconds = av_q2d(encoder->time_base);
    // 报告时长取实际写出包的 PTS 跨度，不再用 encoded_frames/fps 自证。
    bool video_pts_seen = false;
    double video_pts_min = 0.0;
    double video_pts_max = 0.0;
    // 切片 P：报告用的源侧音频起点（跨轨取最小）。它只用于结论层，
    // 不参与平移 —— 平移量统一由 origin_seconds 决定。
    std::optional<double> observed_audio_start_seconds = source_audio_start_seconds;

    auto drain_encoder = [&]() -> bool {
        for (;;) {
            const int ret = avcodec_receive_packet(encoder, encoded);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return true;
            if (ret < 0) {
                write_failed = true;
                write_error = "读取编码器输出失败: " + AvErrorText(ret);
                return false;
            }
            encoded->stream_index = video_out_index;
            av_packet_rescale_ts(encoded, encoder->time_base, video_out->time_base);
            if (encoded->pts != AV_NOPTS_VALUE) {
                const double tb = av_q2d(video_out->time_base);
                const double start = static_cast<double>(encoded->pts) * tb;
                const double end =
                    start + (encoded->duration > 0 ? static_cast<double>(encoded->duration) * tb : frame_seconds);
                if (!video_pts_seen) {
                    video_pts_min = start;
                    video_pts_max = end;
                    video_pts_seen = true;
                } else {
                    video_pts_min = std::min(video_pts_min, start);
                    video_pts_max = std::max(video_pts_max, end);
                }
            }
            // 达上限后仍要把编码器里的包排空（释放引用），但不再落盘。
            if (size_capped) {
                av_packet_unref(encoded);
                continue;
            }
            const int written = encoded->size;
            ++write_attempts;
            if (ShouldInjectWriteFailure(write_attempts)) {
                av_packet_unref(encoded);
                write_failed = true;
                write_error = "写入编码包失败（故障注入）";
                return false;
            }
            const int write_ret = av_interleaved_write_frame(out, encoded);
            av_packet_unref(encoded);
            if (write_ret < 0) {
                write_failed = true;
                write_error = "写入编码包失败";
                return false;
            }
            {
                model::RecoveredSample sample;
                sample.size = static_cast<uint64_t>(std::max(written, 0));
                sample.track_id = 0; // 重编码产物以输出流下标归属，轨号未映射
                sample.stream_index = video_out_index;
                sample.config_id = "s08_reencoded_video";
                sample.decode_ordinal = sample_ledger.CountForTrack(video_out_index);
                sample.timestamp_evidence = model::SampleTimestampEvidence::Inferred;
                sample.inference_basis = "S08 视频重编码，时间戳按统一原点平移重建";
                sample.confidence = model::SampleConfidence::Inferred;
                sample_ledger.Add(sample);
            }
            bytes_written += static_cast<uint64_t>(std::max(written, 0));
            if (max_output_bytes > 0 && bytes_written > max_output_bytes)
                size_capped = true;
        }
    };

    auto encode_frame = [&](AVFrame* source) -> bool {
        // 编码器可能仍持有上一帧的引用；make_writable 会在必要时换一块缓冲，
        // 且不会像 av_frame_unref 那样把 format / width / height 一并清掉。
        if (av_frame_make_writable(converted) < 0) {
            resource_failed = true;
            resource_error = "转换帧缓冲不可写（无法分配新的输出帧缓冲）";
            return false;
        }
        const bool needs_rebuild = scale_state.Observe(source);
        if (needs_rebuild) {
            SwsContext* next = sws_getContext(source->width, source->height, static_cast<AVPixelFormat>(source->format),
                                              width, height, encoder_format, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (next == nullptr) {
                // 宁可显式丢这一帧，也不能把错尺寸的帧喂给旧 context（越界读）。
                ++outcome.dropped_video_frames;
                return true;
            }
            sws_freeContext(sws);
            sws = next;
            scale_state.Commit(source);
        }
        sws_scale(sws, (const uint8_t* const*)source->data, source->linesize, 0, scale_state.source_height,
                  converted->data, converted->linesize);
        // encoder->time_base = av_inv_q(fps)，PTS 每帧 +1 tick = 1/fps 秒。
        converted->pts = static_cast<int64_t>(encoded_frames) + video_pts_offset_frames;
        if (avcodec_send_frame(encoder, converted) < 0) {
            ++outcome.dropped_video_frames;
        } else {
            ++encoded_frames;
        }
        return drain_encoder();
    };

    // 视频 / 音频共用一个受预算的写包入口：bytes_written 口径与重封装路径一致
    // （输出包载荷累计字节，不含封装头尾，故实际文件可能略超上限）。
    auto write_audio_packet = [&](AVPacket* pkt) -> bool {
        if (size_capped)
            return true;
        const int in_index = pkt->stream_index;
        const int out_index = in_index >= 0 && in_index < static_cast<int>(audio_map.size())
                                  ? audio_map[static_cast<std::size_t>(in_index)]
                                  : -1;
        if (out_index < 0)
            return true;
        AVStream* src = in->streams[in_index];
        AVStream* dst = out->streams[out_index];
        // 切片 P：所有音频轨减同一个原点（按各自时基换算），不再逐轨归零。
        if (audio_origin_ticks[static_cast<std::size_t>(in_index)] == AV_NOPTS_VALUE) {
            audio_origin_ticks[static_cast<std::size_t>(in_index)] =
                av_rescale_q(std::llround(origin_seconds * AV_TIME_BASE), AV_TIME_BASE_Q, src->time_base);
        }
        const int64_t offset = audio_origin_ticks[static_cast<std::size_t>(in_index)];
        if (pkt->pts != AV_NOPTS_VALUE) {
            // 负的首包是 priming（容器用 elst 补掉），呈现起点不是负数：
            // 这里也要夹到 0，否则「本来对齐」的源会被算出一个 AAC 帧的假差值。
            const double source_seconds = std::max(0.0, static_cast<double>(pkt->pts) * av_q2d(src->time_base));
            if (!observed_audio_start_seconds || source_seconds < *observed_audio_start_seconds)
                observed_audio_start_seconds = source_seconds;
            pkt->pts -= offset;
        }
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts -= offset;
        // 平移后仍为负的包只可能来自编码器 priming（AAC 首包实测 -0.0232 s），
        // 挪到 0 反而会让音频整体晚一个 priming。原样交给封装器：mov 会用 elst
        // 把这段补偿掉，和源片一致。
        av_packet_rescale_ts(pkt, src->time_base, dst->time_base);
        pkt->stream_index = out_index;
        const int written = pkt->size;
        ++write_attempts;
        if (ShouldInjectWriteFailure(write_attempts)) {
            av_packet_unref(pkt);
            write_failed = true;
            write_error = "写入音频包失败（故障注入）";
            return false;
        }
        const int write_ret = av_interleaved_write_frame(out, pkt);
        if (write_ret < 0) {
            write_failed = true;
            write_error = "写入音频包失败";
            return false;
        }
        {
            model::RecoveredSample sample;
            sample.size = static_cast<uint64_t>(std::max(written, 0));
            sample.track_id = 0;
            sample.stream_index = out_index;
            sample.config_id = "s08_copied_audio";
            sample.decode_ordinal = sample_ledger.CountForTrack(out_index);
            sample.timestamp_evidence = model::SampleTimestampEvidence::Inferred;
            sample.inference_basis = "S08 音频码流复制，PTS 按统一原点平移";
            sample.confidence = model::SampleConfidence::Inferred;
            sample_ledger.Add(sample);
        }
        bytes_written += static_cast<uint64_t>(std::max(written, 0));
        if (max_output_bytes > 0 && bytes_written > max_output_bytes)
            size_capped = true;
        return true;
    };

    // 回放首帧探测期间缓存的音频包，之后才继续读 `in`。
    for (AVPacket* cached : pending_nonvideo.items()) {
        if (!write_audio_packet(cached))
            break;
    }
    pending_nonvideo.Clear();

    // ---- 解码 → 转码 → 封装 ----
    if (!resource_failed && encode_frame(first_frame) && !write_failed) {
        for (;;) {
            if (stop.Check() || size_capped)
                break;
            const int read_ret = av_read_frame(in, packet);
            if (read_ret < 0) {
                if (read_ret != AVERROR_EOF)
                    outcome.read_error = true;
                avcodec_send_packet(decoder, nullptr);
                while (!write_failed && !resource_failed && !size_capped &&
                       avcodec_receive_frame(decoder, frame) == 0) {
                    if (!encode_frame(frame)) {
                        av_frame_unref(frame);
                        break;
                    }
                    av_frame_unref(frame);
                }
                break;
            }
            if (packet->stream_index == video_index) {
                if (avcodec_send_packet(decoder, packet) < 0) {
                    ++dropped_decode_packets;
                } else {
                    while (!write_failed && !resource_failed && !size_capped &&
                           avcodec_receive_frame(decoder, frame) == 0) {
                        if (!encode_frame(frame)) {
                            av_frame_unref(frame);
                            break;
                        }
                        av_frame_unref(frame);
                    }
                }
            } else {
                write_audio_packet(packet);
            }
            av_packet_unref(packet);
            if (write_failed || resource_failed || size_capped)
                break;
            if (sample_ledger.Total() % 200 == 0)
                Emit(progress, 80, "已转码 " + std::to_string(encoded_frames) + " 帧");
        }
    }

    if (!write_failed && !resource_failed) {
        avcodec_send_frame(encoder, nullptr);
        if (!drain_encoder())
            write_failed = true;
    }
    // 逐帧实测的动态分辨率帧数并进同一个保真结论，写在所有返回路径之前：
    // 后面失败 / 取消 / 截断的分支也得带上这份实测，不能退回去用空表自证。
    FinalizeTranscodeFidelity(outcome.transcode_fidelity, scale_state.resolution_changed_frames);
    if (outcome.transcode_fidelity.inspected) {
        const model::RepairTranscodeFidelity& fidelity = outcome.transcode_fidelity;
        outcome.transcode_note += "；源侧保真探测=" + fidelity.source_pixel_format + "→" +
                                  fidelity.output_pixel_format + "，丢失属性 " +
                                  std::to_string(fidelity.lost_properties.size()) + " 项，继承 " +
                                  std::to_string(fidelity.preserved_properties.size()) + " 项";
    }

    // 关闭输入上下文（in / video_in）之前先固化 track id：验证目标与守恒记录
    // 之后还要用到这些值，直接访问已释放的 AVFormatContext 会导致崩溃。
    const uint32_t video_track_id = video_in->id > 0 ? static_cast<uint32_t>(video_in->id) : 0;
    const unsigned input_stream_count = in->nb_streams;
    std::vector<uint32_t> audio_track_ids(input_stream_count, 0);
    for (unsigned i = 0; i < input_stream_count; ++i) {
        if (audio_map[i] >= 0 && in->streams[i]->id > 0)
            audio_track_ids[i] = static_cast<uint32_t>(in->streams[i]->id);
    }

    if (sws != nullptr)
        sws_freeContext(sws);
    av_packet_free(&encoded);
    av_frame_free(&converted);
    const int trailer_ret = av_write_trailer(out);
    const bool trailer_injected = g_inject_trailer_failure.load(std::memory_order_relaxed);
    if ((trailer_ret < 0 || trailer_injected) && !write_failed) {
        write_failed = true;
        write_error = trailer_injected ? "写入封装尾失败（故障注入，产物索引可能不完整）"
                                       : "写入封装尾失败（产物索引 / moov 可能不完整）";
    }
    avio_closep(&out->pb);
    avformat_free_context(out);
    avcodec_free_context(&encoder);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&in);

    // 覆盖所有写包过的返回路径（取消 / 超时 / 写失败 / 达上限）：账本里是
    // 实测的部分值，不能事后伪装成完整扫描。
    outcome.sample_ledger = sample_ledger.Summary();
    // 切片 K：S08 同样接了样本账本，逐轨覆盖不能再整块丢掉。旧实现只有
    // sample_ledger 没有 track_coverage，报告兜底还会写「本次执行没有样本账本」
    // —— 有 88 个样本却说没有账本，属于把「没统计」报成「没数据」。
    // 这里只上抛账本里**确实有的事实**：每轨样本数、无时间戳样本数。
    // S08 的样本没有输入侧时间戳（产物时间轴按推定帧率从零重建），所以
    // span_known=false / common_span_known=false 是如实结论，不填假区间。
    outcome.track_coverage = sample_ledger.Coverage();
    outcome.track_coverage.complete =
        !stop.canceled && !stop.timed_out && !outcome.read_error && !write_failed && !resource_failed && !size_capped;
    for (auto& track : outcome.track_coverage.tracks) {
        const auto it = stream_meta.find(track.output_stream_index);
        if (it != stream_meta.end()) {
            track.kind = it->second.first;
            track.codec = it->second.second;
        } else if (track.kind.empty()) {
            track.kind = "other";
        }
    }
    outcome.track_coverage.limitations.push_back(
        "S08 重编码：产物时间轴按推定帧率从零重建，样本未携带输入侧时间戳；逐轨覆盖只能说明每轨恢复"
        "了多少样本，不能给出输入时间轴上的覆盖区间，也不可与损失区间相减");
    if (!outcome.track_coverage.complete)
        outcome.track_coverage.limitations.push_back("本次执行未走到源末尾或中途失败，逐轨覆盖只统计到已写入的前缀");

    outcome.canceled = stop.canceled;
    outcome.timed_out = stop.timed_out;
    outcome.packets_read = encoded_frames + dropped_decode_packets;
    outcome.packets_written = sample_ledger.Total();
    outcome.bytes_written = bytes_written;
    outcome.output_size_capped = size_capped;
    outcome.transcoded_frames = encoded_frames;
    outcome.dropped_video_frames = dropped_decode_packets + outcome.dropped_video_frames;
    // 报告时长以实际写入包的 PTS 跨度为准（含分数帧率），仅在没有任何包
    // 时间证据时才退回帧数 / 帧率的估算值。
    const double measured_span = video_pts_seen ? std::max(0.0, video_pts_max - video_pts_min) : 0.0;
    outcome.output_start_seconds = 0.0;
    outcome.output_end_seconds =
        measured_span > 0.0 ? measured_span : (fps_value > 0.0 ? static_cast<double>(encoded_frames) / fps_value : 0.0);
    // 切片 P：音画起点差的结论层。只有「两条轨都有实测起点」才算量到了差值；
    // 单轨视频没有音画同步可言，一声不响即可；有音频却没量到起点则明说没比对。
    {
        const bool has_audio = std::any_of(audio_map.begin(), audio_map.end(), [](int mapped) { return mapped >= 0; });
        if (has_audio && source_video_start_seconds && observed_audio_start_seconds) {
            const double offset = *source_video_start_seconds - *observed_audio_start_seconds;
            outcome.transcode_fidelity.av_start_offset_seconds = offset;
            outcome.transcode_fidelity.av_start_offset_preserved = true;
            outcome.transcode_fidelity.av_sync_note = "源侧音画起点差 " + FormatSeconds(offset) + "（视频" +
                                                      (offset >= 0.0 ? "较晚" : "较早") +
                                                      "）；产物按统一原点平移，相对关系保留";
        } else if (has_audio) {
            outcome.transcode_fidelity.av_sync_note =
                "音频轨存在但没有取得可比的源侧起点，本次未做音画起点比对（不声称音画同步已保持）";
        }
    }
    if (size_capped) {
        outcome.transcode_note += "；超过输出字节上限 " + std::to_string(max_output_bytes) + " 字节，仅交付前 " +
                                  FormatSeconds(*outcome.output_end_seconds) + " 的可播放前缀，之后的源内容未恢复";
    }
    if (pending_overflow) {
        outcome.omitted_items.push_back("首帧探测期间的音频包超过缓存上限，产物可能缺少音频前缀");
        model::DamageFinding overflow;
        overflow.code = "audio.prefix_cache_overflow";
        overflow.detector = "repair.transcode";
        overflow.layer = model::DamageLayer::Track;
        overflow.severity = model::IssueSeverity::Warning;
        overflow.confidence = model::DamageConfidence::Confirmed;
        overflow.location.precision = model::DamagePrecision::Bounded;
        overflow.location.provenance = "first-frame probe pending packet cache";
        overflow.evidence = "首帧探测阶段读到的非视频包超过 16384 个 / 64 MiB 缓存上限，超出部分未回放";
        overflow.limitations.push_back("只能证明音频前缀不完整，无法确定丢失的具体时间区间");
        outcome.residual_findings.push_back(std::move(overflow));
    }

    // 资源错误 / 写入或收尾失败一律不是「成功转码」：旧实现只在
    // packets_written == 0 时才报错，写过部分包就把磁盘 / 封装错误掩盖成
    // 一次成功，并把 record.succeeded 置 true。
    if (resource_failed || write_failed) {
        outcome.error = resource_failed ? (resource_error.empty() ? "转码输出缓冲资源错误" : resource_error)
                                        : (write_error.empty() ? "转码写入或封装收尾失败" : write_error);
        model::DamageFinding failed;
        failed.code = resource_failed ? "output.transcode_resource_failed" : "output.transcode_write_failed";
        failed.detector = "repair.transcode";
        failed.layer = model::DamageLayer::Container;
        failed.severity = model::IssueSeverity::Error;
        failed.confidence = model::DamageConfidence::Confirmed;
        failed.location.precision = model::DamagePrecision::Bounded;
        failed.location.provenance = "av_frame_make_writable / av_interleaved_write_frame / av_write_trailer";
        failed.evidence =
            outcome.error + "；已写入 " + std::to_string(outcome.packets_written) + " 个包的前缀不作为修复完成发布";
        failed.limitations.push_back("已写出的前缀可能可读，但未经完整验证，不能据此宣称内容已恢复");
        outcome.residual_findings.push_back(std::move(failed));
        return outcome;
    }
    if (stop.canceled || stop.timed_out) {
        outcome.ok = outcome.packets_written > 0;
        outcome.error = std::string("转码阶段") + stop.Reason();
        return outcome;
    }
    if (outcome.packets_written == 0) {
        outcome.error = "转码没有写入任何数据包（没有可解码内容）";
        return outcome;
    }

    outcome.ok = true;
    outcome.video_reencoded = encoded_frames > 0;
    outcome.audio_reencoded = false;

    // ---- 画质评估（独立解码输入与产物，按帧序号配对）----
    Emit(progress, 86, "抽帧 PSNR 画质评估");
    const PsnrResult psnr = MeasurePsnr(request.source_path, paths.part_path, cancel, budget, &stop);
    outcome.psnr_samples = psnr.samples;
    outcome.psnr_average_db = psnr.average_db;
    outcome.transcode_note += "；" + psnr.note;
    if (psnr.stopped) {
        // 预算在 PSNR 阶段到期：产物已写完，但画质结论没跑完，不能冒充完整修复。
        outcome.canceled = stop.canceled;
        outcome.timed_out = stop.timed_out;
        outcome.ok = outcome.packets_written > 0;
        outcome.error = std::string("画质评估阶段") + psnr.stop_reason;
        model::RepairActionExecution aborted;
        aborted.action = model::RepairAction::S08_TolerantTranscode;
        aborted.attempted = true;
        aborted.succeeded = false;
        aborted.error = outcome.error;
        aborted.packets_written = outcome.packets_written;
        aborted.bytes_written = outcome.bytes_written;
        aborted.detail = outcome.transcode_note;
        outcome.executions.push_back(std::move(aborted));
        return outcome;
    }

    model::RepairActionExecution record;
    record.action = model::RepairAction::S08_TolerantTranscode;
    record.attempted = true;
    record.succeeded = true;
    record.packets_read = outcome.packets_read;
    record.packets_written = outcome.packets_written;
    record.bytes_written = outcome.bytes_written;
    record.output_start_seconds = outcome.output_start_seconds;
    record.output_end_seconds = outcome.output_end_seconds;
    std::ostringstream detail;
    detail << "容忍式转码：解码并编码 " << encoded_frames << " 帧（" << std::string(encoder_codec->name) << "），跳过 "
           << outcome.dropped_video_frames << " 个解码失败包/帧；音频包 "
           << (sample_ledger.Total() - sample_ledger.CountForTrack(video_out_index)) << " 个";
    if (size_capped)
        detail << "；超过输出字节上限 " << max_output_bytes << " 字节，仅交付可播放前缀，其余源内容未恢复";
    if (psnr.samples > 0)
        detail << "；抽帧 PSNR 均值 " << FormatDb(psnr.average_db) << "（" << psnr.samples << " 个样本）";
    record.detail = detail.str();
    outcome.executions.push_back(std::move(record));

    outcome.output_video_stream_index = 0;

    model::RepairTrackVerificationTarget video_target;
    video_target.track_id = video_track_id;
    video_target.input_stream_index = video_index;
    video_target.output_stream_index = video_out_index;
    video_target.kind = "video";
    video_target.required = true;
    video_target.note = "S08 转码产物视频轨";
    outcome.verification_targets.push_back(std::move(video_target));
    for (unsigned i = 0; i < input_stream_count; ++i) {
        if (audio_map[i] < 0)
            continue;
        model::RepairTrackVerificationTarget audio_target;
        audio_target.track_id = audio_track_ids[i];
        audio_target.input_stream_index = static_cast<int>(i);
        audio_target.output_stream_index = audio_map[i];
        audio_target.kind = "audio";
        // 与重封装路径一致：默认逐条验证所有保留媒体轨；显式 target_streams
        // 时以用户点名的轨为强制验证对象。旧实现把所有音频轨固定 required=false，
        // 验证器既不解码也不做音画同步，音频丢包 / 错位完全没有证据。
        const bool explicitly_requested = std::find(request.target_streams.begin(), request.target_streams.end(),
                                                    static_cast<int>(i)) != request.target_streams.end();
        audio_target.required = explicitly_requested || request.target_streams.empty();
        audio_target.note = "S08 转码产物保留音频轨（码流复制，PTS 按首包归零）";
        outcome.verification_targets.push_back(std::move(audio_target));
    }

    {
        model::RepairTrackConservation video_record;
        video_record.track_id = video_track_id;
        video_record.input_stream_index = video_index;
        video_record.output_stream_index = video_out_index;
        video_record.kind = "video";
        video_record.packets_written = sample_ledger.CountForTrack(video_out_index);
        video_record.status = "unknown";
        video_record.relation = "reencoded";
        video_record.note = "转码后样本数与源不可直接比对";
        outcome.track_conservation.push_back(std::move(video_record));
    }

    const auto conservation = CheckUpperLayerConservation(sample_ledger, outcome.recovered_segments,
                                                          outcome.track_conservation, outcome.packets_written);
    outcome.sample_conservation_problems = conservation.problems;

    if (outcome.dropped_video_frames > 0) {
        model::DamageFinding finding;
        finding.code = "video.decode_drops_during_transcode";
        finding.detector = "repair.transcode";
        finding.layer = model::DamageLayer::Decode;
        finding.severity = model::IssueSeverity::Warning;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Bounded;
        finding.location.provenance = "avcodec_send_packet errors vs received frames";
        finding.evidence =
            "转码过程中 " + std::to_string(outcome.dropped_video_frames) + " 个视频包/帧无法解码或编码，已被丢弃";
        finding.limitations.push_back("被丢弃帧对应的画面内容无法恢复；只能证明其余帧已重新编码");
        finding.candidate_actions.push_back(model::RepairAction::S08_TolerantTranscode);
        outcome.residual_findings.push_back(std::move(finding));
    }

    model::DamageFinding reencoded;
    reencoded.code = "video.reencoded";
    reencoded.detector = "repair.transcode";
    reencoded.layer = model::DamageLayer::Decode;
    reencoded.severity = model::IssueSeverity::Warning;
    reencoded.confidence = model::DamageConfidence::Confirmed;
    reencoded.location.precision = model::DamagePrecision::Bounded;
    reencoded.location.provenance = "decode -> swscale -> " + std::string(encoder_codec->name);
    reencoded.evidence = "视频轨经过解码与重编码（" + std::to_string(encoded_frames) + " 帧）；不是原码流无损重封装";
    reencoded.limitations.push_back("重编码存在代际画质损失，无法与源画面逐帧等同");
    if (psnr.samples > 0)
        reencoded.limitations.push_back("抽帧 PSNR 均值 " + FormatDb(psnr.average_db) + "（" +
                                        std::to_string(psnr.samples) + " 个样本，亮度平面）");
    else
        reencoded.limitations.push_back("未能完成抽帧 PSNR 比对，画质损失无法量化");
    reencoded.candidate_actions.push_back(model::RepairAction::S08_TolerantTranscode);
    outcome.residual_findings.push_back(std::move(reencoded));

    // 切片 J：重编码丢掉的「源侧属性」与丢帧是两件事 —— 丢帧在画面内容里，
    // 属性丢失在像素格式 / 色彩 / 方向 / 元数据里。不单独落一条 finding，
    // 报告就只能看到「有代际画质损失」，看不到「10-bit 被压成 8-bit」。
    if (outcome.transcode_fidelity.inspected && !outcome.transcode_fidelity.lost_properties.empty()) {
        model::DamageFinding loss;
        loss.code = "video.transcode_property_loss";
        loss.detector = "repair.transcode";
        loss.layer = model::DamageLayer::Decode;
        loss.severity = model::IssueSeverity::Warning;
        loss.confidence = model::DamageConfidence::Confirmed;
        loss.location.precision = model::DamagePrecision::Bounded;
        loss.location.provenance = "source stream / first decoded frame vs encoder pixel format";
        loss.evidence = "重编码前后源侧属性实测：" + outcome.transcode_fidelity.source_pixel_format + " → " +
                        outcome.transcode_fidelity.output_pixel_format + "，共 " +
                        std::to_string(outcome.transcode_fidelity.lost_properties.size()) + " 项未保留";
        for (const auto& item : outcome.transcode_fidelity.lost_properties)
            loss.limitations.push_back(item);
        loss.limitations.push_back("属性丢失是重编码本身造成的，不能通过重新封装或再次修复找回");
        loss.candidate_actions.push_back(model::RepairAction::S08_TolerantTranscode);
        outcome.residual_findings.push_back(std::move(loss));
    }

    return outcome;
}

} // namespace repair
} // namespace videoeye
