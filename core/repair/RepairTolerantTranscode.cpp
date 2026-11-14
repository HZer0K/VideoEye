#include "core/repair/RepairTolerantTranscode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/repair/RepairAudioTranscode.h"
#include "core/repair/RepairSampleScan.h"
#include "core/repair/RepairSilenceFill.h"
#include "core/repair/RepairTranscodeFidelity.h"
#include "core/repair/RepairTranscodeTracks.h"
#include "core/repair/RepairUtils.h"
#include "core/repair/RepairVideoGapFill.h"
#include "infrastructure/concurrency/Cancellation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace videoeye {
namespace repair {
namespace {

using Clock = std::chrono::steady_clock;

// 测试专用故障注入，默认全关（见 RepairTolerantTranscode.h）。
std::atomic<int> g_inject_write_failure_nth{0};
std::atomic<bool> g_inject_trailer_failure{false};
// 切片 V：强制把某条音频轨的容器接受性判成拒绝（-1 = 关，见头文件说明）。
std::atomic<int> g_inject_audio_container_rejected{-1};

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

// ---- 切片 V：音频轨「解码 → 重采样 → 重编码」链 ----
//
// 只在目标容器拒绝原编码时启用，目的很单一：把这条音轨**留下来**，而不是丢掉。
//
// 四条硬约束（都对应一个报告字段）：
//   * 时间轴：每帧位置由源 PTS 换算到编码器采样时基后**各自定位**，不按序号累加。
//     源侧真有空洞时位置照旧 —— 要么按用户显式启用补静音（切片 W，逐处记账、
//     单独计数），要么把位置直接跳过去并记进 timeline_gaps。两条路都不会悄悄
//     拉长音频：跳过是保留空洞，补静音是**合成内容**，两者都进报告。
//   * 末帧：编码器要求整块（aac 1024 采样），不足的部分用静音补齐，补齐的采样数
//     单独计数上报 —— 「补齐以便编码」和「填补内容」是两件事，不能混。
//   * 任何失败都返回 false 并写明原因；是否降级由调用方决定。
class AudioTranscodeChain {
public:
    using Sink = std::function<bool(AVPacket*)>;

    AudioTranscodeChain() = default;
    ~AudioTranscodeChain() {
        av_frame_free(&encoded_frame_);
        av_frame_free(&filtered_);
        av_frame_free(&decoded_);
        av_packet_free(&packet_);
        if (fifo_ != nullptr)
            av_audio_fifo_free(fifo_);
        if (swr_ != nullptr)
            swr_free(&swr_);
        avcodec_free_context(&decoder_);
        avcodec_free_context(&encoder_); // 由 AudioReencodeTarget 移交的所有权
    }
    AudioTranscodeChain(const AudioTranscodeChain&) = delete;
    AudioTranscodeChain& operator=(const AudioTranscodeChain&) = delete;

    // encoder 的所有权转移给本对象。source 是源侧编码参数（解码器初值）。
    bool Init(AVCodecContext* encoder, const AVCodecParameters* source, double origin_seconds, int input_index,
              std::string* error) {
        encoder_ = encoder;
        input_stream_index = input_index;
        origin_seconds_ = origin_seconds;
        if (encoder_ == nullptr || source == nullptr) {
            *error = "缺少音频编码器或源参数";
            return false;
        }
        // 切片 W：判据的容差与换算都用**编码器**参数（重采样之后才是产物时基）。
        silence_policy_.sample_rate = encoder_->sample_rate;
        silence_policy_.block_samples = encoder_->frame_size > 0 ? encoder_->frame_size : 0;
        // 编码块长度不可得（flac 等不定长编码器）时，报告里的判据依据必须写明
        // 用的是 20 ms 兜底容差，不能让读者以为是按真实块长算的。
        silence_basis = "判据：相邻音频帧 PTS 间隔 vs " +
                        (encoder_->frame_size > 0 ? "编码块 " + std::to_string(encoder_->frame_size) + " 采样"
                                                  : std::string("20 ms 兜底容差（该编码器未声明帧长）")) +
                        "（采样率 " + std::to_string(encoder_->sample_rate) + " Hz）；容差半个编码块";
        source_time_base_ = encoder_->time_base; // 调用方随后用 SetSourceTimeBase 换成源流时基
        const AVCodec* decoder_codec = avcodec_find_decoder(source->codec_id);
        if (decoder_codec == nullptr) {
            *error = "没有可用的音频解码器（" + std::string(avcodec_get_name(source->codec_id)) + "）";
            return false;
        }
        decoder_ = avcodec_alloc_context3(decoder_codec);
        if (decoder_ == nullptr || avcodec_parameters_to_context(decoder_, source) < 0) {
            *error = "无法初始化音频解码器";
            return false;
        }
        decoder_->thread_count = 0;
        if (avcodec_open2(decoder_, decoder_codec, nullptr) < 0) {
            *error = "无法打开音频解码器（编码参数可能已损坏）";
            return false;
        }
        packet_ = av_packet_alloc();
        decoded_ = av_frame_alloc();
        filtered_ = av_frame_alloc();
        encoded_frame_ = av_frame_alloc();
        if (packet_ == nullptr || decoded_ == nullptr || filtered_ == nullptr || encoded_frame_ == nullptr) {
            *error = "无法分配音频转码缓冲";
            return false;
        }
        encoder_name = encoder_->codec->name != nullptr ? encoder_->codec->name : std::string();
        source_codec = avcodec_get_name(source->codec_id);
        return true;
    }

    void SetSourceTimeBase(AVRational time_base) {
        source_time_base_ = time_base;
    }

    // 切片 W：是否在源时间轴空洞里补静音。必须在 Init 之前设置（Init 里要按
    // 编码器参数算容差）；Init 之后设置会拿不到编码块长度。
    void ConfigureSilenceFill(bool enabled, bool merge_contiguous) {
        silence_policy_.fill_enabled = enabled;
        silence_policy_.merge_contiguous = merge_contiguous;
    }

    AVRational EncoderTimeBase() const {
        return encoder_->time_base;
    }

    bool FeedPacket(const AVPacket* packet, const Sink& sink, std::string* error) {
        for (;;) {
            const int ret = avcodec_send_packet(decoder_, packet);
            if (ret == AVERROR(EAGAIN)) {
                if (!DrainDecoded(sink, error))
                    return false;
                continue;
            }
            if (ret < 0) {
                // 单个包解不动不该整链失败：记一笔，继续吃后面的包。
                ++dropped_packets;
                return true;
            }
            return DrainDecoded(sink, error);
        }
    }

    bool Flush(const Sink& sink, std::string* error) {
        // 1) 解码器排空（送 nullptr），把剩下的帧全部喂进重采样。
        avcodec_send_packet(decoder_, nullptr);
        if (!DrainDecoded(sink, error))
            return false;
        // 2) 重采样器里可能还压着几个采样，必须冲出来，否则末段会被截掉。
        if (swr_ != nullptr) {
            for (;;) {
                av_frame_unref(filtered_);
                if (!PrepareFiltered(encoder_->frame_size > 0 ? encoder_->frame_size : 4096, error))
                    return false;
                const int converted = swr_convert(swr_, filtered_->data, filtered_->nb_samples, nullptr, 0);
                if (converted <= 0)
                    break;
                filtered_->nb_samples = converted;
                if (!WriteToFifo(filtered_, error))
                    return false;
            }
        }
        // 3) 编码器要求整块时补齐末帧 —— 补的是静音，单独计数，不混进「恢复内容」。
        if (!FlushFifo(sink, error))
            return false;
        if (avcodec_send_frame(encoder_, nullptr) < 0) {
            *error = "音频编码器排空失败";
            return false;
        }
        return ReceivePackets(sink, error);
    }

    int input_stream_index = -1;
    int output_stream_index = -1;
    std::string encoder_name;
    // 源编码名（初始化时固化）：结论层要用它写「原编码 → 目标编码」，
    // 而那时输入上下文可能已经关闭。
    std::string source_codec;
    uint64_t decoded_frames = 0;
    uint64_t encoded_packets = 0;
    uint64_t dropped_packets = 0;
    // 位置被跳过（没补静音）的空洞次数。补过静音的空洞不计在这里 —— 那是
    // 另一个动作，混成一个计数器会让报告说不清「到底补了几处」。
    uint64_t timeline_gaps = 0;
    int64_t tail_padding_samples = 0;
    // 切片 W：补进源时间轴空洞的静音采样数（**不含**上面的末帧块对齐补齐）。
    int64_t silence_samples_inserted = 0;
    // 切片 W：逐处音频空洞的判定账。填充率与「为什么没填」都在这里，报告只打印。
    std::vector<model::RepairSyntheticAudioGap> silence_gaps;
    std::string silence_basis;
    // 含合成采样、因此整块不计入样本账本的编码包数。
    int64_t synthetic_packets_excluded = 0;
    // 切片 W：当前这个编码包里含合成静音采样吗？FIFO 里静音在前、真实采样在后
    // （PushSilence 在写真实帧之前调用），所以整块静音一定是纯合成的，混合只可能
    // 出现在边界那一块 —— 两者都按「含合成内容」处理，账本整块排除。
    // -1 表示「本次补过静音，但块与包的对应关系已错位」，同样按含合成内容算。
    bool CurrentPacketHasSyntheticSamples() const {
        return current_packet_synthetic_ != 0;
    }
    /// 首个输出包的源侧起点（秒，已按统一原点平移），报告用它核对音画偏差。
    std::optional<double> first_output_seconds;

private:
    bool EnsureResampler(const AVFrame* frame, std::string* error) {
        if (swr_ != nullptr)
            return true;
        AVChannelLayout in_layout{};
        if (frame->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&in_layout, &frame->ch_layout);
        } else {
            av_channel_layout_default(&in_layout,
                                      decoder_->ch_layout.nb_channels > 0 ? decoder_->ch_layout.nb_channels : 2);
        }
        const int in_rate = frame->sample_rate > 0 ? frame->sample_rate : encoder_->sample_rate;
        const int ret = swr_alloc_set_opts2(
            &swr_, &encoder_->ch_layout, static_cast<AVSampleFormat>(encoder_->sample_fmt), encoder_->sample_rate,
            &in_layout, static_cast<AVSampleFormat>(frame->format), in_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        if (ret < 0 || swr_ == nullptr || swr_init(swr_) < 0) {
            *error = "无法初始化音频重采样器";
            return false;
        }
        return true;
    }

    bool PrepareFiltered(int nb_samples, std::string* error) {
        filtered_->format = encoder_->sample_fmt;
        filtered_->sample_rate = encoder_->sample_rate;
        if (av_channel_layout_copy(&filtered_->ch_layout, &encoder_->ch_layout) < 0) {
            *error = "无法设置重采样输出声道布局";
            return false;
        }
        filtered_->nb_samples = nb_samples;
        if (av_frame_get_buffer(filtered_, 0) < 0) {
            *error = "无法分配重采样输出帧缓冲";
            return false;
        }
        return true;
    }

    bool DrainDecoded(const Sink& sink, std::string* error) {
        for (;;) {
            const int ret = avcodec_receive_frame(decoder_, decoded_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return true;
            if (ret < 0) {
                *error = "音频解码失败: " + AvErrorText(ret);
                av_frame_unref(decoded_);
                return false;
            }
            ++decoded_frames;
            const bool ok = ConsumeFrame(sink, error);
            av_frame_unref(decoded_);
            if (!ok)
                return false;
        }
    }

    bool ConsumeFrame(const Sink& sink, std::string* error) {
        if (!EnsureResampler(decoded_, error))
            return false;
        if (decoded_->pts != AV_NOPTS_VALUE) {
            const double seconds = std::max(0.0, static_cast<double>(decoded_->pts) * av_q2d(source_time_base_));
            const double relative = std::max(0.0, seconds - origin_seconds_);
            const int64_t want = std::llround(relative * encoder_->sample_rate);
            if (!positioned_) {
                next_pts_ = want;
                positioned_ = true;
            } else if (want > next_pts_) {
                // 源侧时间戳跳了一段。两种处理都必须留痕（切片 W）：
                //   补静音 = 用合成内容占住这段位置，产物时间轴与源一致；
                //   跳过   = 位置直接放过去，产物里留一段没有采样覆盖的空洞。
                // 旧实现只有后者，且一句都不报。
                const SilenceFillPlan plan =
                    PlanSilenceFill(next_pts_, want, silence_policy_, silence_samples_inserted);
                if (plan.verdict == SilenceFillVerdict::NoGap) {
                    // 容差内的取整抖动：不改位置、不记账（补它等于往产物里灌噪声）。
                } else {
                    model::RepairSyntheticAudioGap gap = plan.gap;
                    gap.after_audio_frames = decoded_frames;
                    gap.text = SilenceFillGapText(gap);
                    silence_gaps.push_back(gap);
                    if (plan.verdict == SilenceFillVerdict::Fill) {
                        if (!PushSilence(plan.gap.missing_samples, sink, error))
                            return false;
                        silence_samples_inserted += plan.gap.missing_samples;
                        // next_pts_ 不跳：静音已经占住原位，后续采样的位置自然与源一致。
                    } else {
                        ++timeline_gaps;
                        next_pts_ = want;
                    }
                }
            }
        }
        const int capacity = swr_get_out_samples(swr_, decoded_->nb_samples);
        if (capacity <= 0)
            return true;
        av_frame_unref(filtered_);
        if (!PrepareFiltered(capacity, error))
            return false;
        const int converted = swr_convert(swr_, filtered_->data, capacity,
                                          const_cast<const uint8_t**>(decoded_->extended_data), decoded_->nb_samples);
        if (converted < 0) {
            *error = "音频重采样失败: " + AvErrorText(converted);
            return false;
        }
        if (converted == 0)
            return true;
        filtered_->nb_samples = converted;
        if (!WriteToFifo(filtered_, error))
            return false;
        return DrainFifo(sink, false, error);
    }

    bool WriteToFifo(AVFrame* frame, std::string* error) {
        if (fifo_ == nullptr) {
            fifo_ = av_audio_fifo_alloc(static_cast<AVSampleFormat>(encoder_->sample_fmt),
                                        encoder_->ch_layout.nb_channels, 1);
            if (fifo_ == nullptr) {
                *error = "无法分配音频帧缓冲队列";
                return false;
            }
        }
        if (av_audio_fifo_write(fifo_, reinterpret_cast<void**>(frame->extended_data), frame->nb_samples) <
            frame->nb_samples) {
            *error = "音频帧写入缓冲队列失败";
            return false;
        }
        return true;
    }

    // 切片 W：把 samples 个静音采样写进编码队列。分块写（每块一个编码块长度），
    // 免得为 5 s 的空洞一次性分配几 MB 缓冲；块内内容由 av_samples_set_silence
    // 一次性填 0，不参与任何「与源对应」的记账。
    bool PushSilence(int64_t samples, const Sink& sink, std::string* error) {
        const int chunk = encoder_->frame_size > 0 ? encoder_->frame_size : 4096;
        while (samples > 0) {
            const int take = static_cast<int>(std::min<int64_t>(samples, chunk));
            av_frame_unref(filtered_);
            if (!PrepareFiltered(take, error))
                return false;
            if (av_samples_set_silence(filtered_->data, 0, take, encoder_->ch_layout.nb_channels,
                                       static_cast<AVSampleFormat>(encoder_->sample_fmt)) < 0) {
                *error = "无法生成静音填充帧";
                return false;
            }
            if (!WriteToFifo(filtered_, error))
                return false;
            silence_in_fifo_ += take;
            samples -= take;
        }
        // 队列里够整块的就编出去：空洞可能有 5 s，全压在 FIFO 里等下一个源帧
        // 才排空，内存和对齐都没必要这么拖。
        return DrainFifo(sink, false, error);
    }

    // 把队列里够整块的采样编码出去。pad_last=true 时把不足一块的尾巴用静音补满。
    bool DrainFifo(const Sink& sink, bool pad_last, std::string* error) {
        if (fifo_ == nullptr)
            return true;
        const int frame_size = encoder_->frame_size;
        for (;;) {
            const int available = av_audio_fifo_size(fifo_);
            if (frame_size <= 0) {
                // 不定长编码器（flac 等）：有多少喂多少。
                if (available <= 0)
                    return true;
                if (!EncodeOneBlock(available, sink, error))
                    return false;
                return true;
            }
            if (available < frame_size)
                break;
            if (!EncodeOneBlock(frame_size, sink, error))
                return false;
        }
        if (!pad_last)
            return true;
        const int left = av_audio_fifo_size(fifo_);
        if (left <= 0)
            return true;
        if (frame_size <= 0)
            return true;
        // 补静音到整块：这不是「填补内容」，只是编码器的块对齐要求。
        av_frame_unref(encoded_frame_);
        encoded_frame_->format = encoder_->sample_fmt;
        encoded_frame_->sample_rate = encoder_->sample_rate;
        if (av_channel_layout_copy(&encoded_frame_->ch_layout, &encoder_->ch_layout) < 0) {
            *error = "无法设置音频编码帧声道布局";
            return false;
        }
        encoded_frame_->nb_samples = frame_size;
        encoded_frame_->pts = next_pts_;
        if (av_frame_get_buffer(encoded_frame_, 0) < 0) {
            *error = "无法分配音频编码帧缓冲";
            return false;
        }
        if (av_samples_set_silence(encoded_frame_->data, 0, frame_size, encoder_->ch_layout.nb_channels,
                                   static_cast<AVSampleFormat>(encoder_->sample_fmt)) < 0) {
            *error = "无法填充末帧静音";
            return false;
        }
        if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(encoded_frame_->data), left) < left) {
            *error = "读取音频缓冲队列末段失败";
            return false;
        }
        // 这一块里有多少是合成静音：末块从前缀读 real/silence 混合，也可能整块都是
        // 之前补进来的静音（空洞刚好在结尾）。两种都按「含合成内容」记。
        const int64_t synthetic = std::min<int64_t>(silence_in_fifo_, left);
        silence_in_fifo_ -= synthetic;
        block_synthetic_samples_.push_back(static_cast<int>(synthetic));
        tail_padding_samples += frame_size - left;
        next_pts_ += frame_size;
        return SendFrame(encoded_frame_, sink, error);
    }

    bool FlushFifo(const Sink& sink, std::string* error) {
        return DrainFifo(sink, true, error);
    }

    bool EncodeOneBlock(int nb_samples, const Sink& sink, std::string* error) {
        av_frame_unref(encoded_frame_);
        encoded_frame_->format = encoder_->sample_fmt;
        encoded_frame_->sample_rate = encoder_->sample_rate;
        if (av_channel_layout_copy(&encoded_frame_->ch_layout, &encoder_->ch_layout) < 0) {
            *error = "无法设置音频编码帧声道布局";
            return false;
        }
        encoded_frame_->nb_samples = nb_samples;
        encoded_frame_->pts = next_pts_;
        if (av_frame_get_buffer(encoded_frame_, 0) < 0) {
            *error = "无法分配音频编码帧缓冲";
            return false;
        }
        if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(encoded_frame_->data), nb_samples) < nb_samples) {
            *error = "读取音频缓冲队列失败";
            return false;
        }
        const int64_t synthetic = std::min<int64_t>(silence_in_fifo_, nb_samples);
        silence_in_fifo_ -= synthetic;
        block_synthetic_samples_.push_back(static_cast<int>(synthetic));
        next_pts_ += nb_samples;
        return SendFrame(encoded_frame_, sink, error);
    }

    bool SendFrame(AVFrame* frame, const Sink& sink, std::string* error) {
        for (;;) {
            const int ret = avcodec_send_frame(encoder_, frame);
            if (ret == AVERROR(EAGAIN)) {
                if (!ReceivePackets(sink, error))
                    return false;
                continue;
            }
            if (ret < 0) {
                *error = "音频编码失败: " + AvErrorText(ret);
                return false;
            }
            return ReceivePackets(sink, error);
        }
    }

    bool ReceivePackets(const Sink& sink, std::string* error) {
        for (;;) {
            const int ret = avcodec_receive_packet(encoder_, packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return true;
            if (ret < 0) {
                *error = "读取音频编码器输出失败: " + AvErrorText(ret);
                return false;
            }
            ++encoded_packets;
            // 切片 W：包与块的对应关系按发送顺序取；编码器多吐 / 少吐包时会与队列
            // 错位，此后的归因都不可信 —— 保守起见后面的包一律按「含合成内容」算，
            // 宁可少记恢复样本，也不把合成静音算成恢复出来的内容。
            current_packet_synthetic_ = 0;
            if (!block_synthetic_samples_.empty()) {
                current_packet_synthetic_ = block_synthetic_samples_.front();
                block_synthetic_samples_.pop_front();
            } else if (silence_samples_inserted > 0) {
                current_packet_synthetic_ = -1;
            }
            if (current_packet_synthetic_ != 0)
                ++synthetic_packets_excluded;
            if (!sink(packet_)) {
                av_packet_unref(packet_);
                return false;
            }
            av_packet_unref(packet_);
        }
    }

    AVCodecContext* decoder_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVFrame* decoded_ = nullptr;
    AVFrame* filtered_ = nullptr;
    AVFrame* encoded_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVRational source_time_base_{0, 1};
    double origin_seconds_ = 0.0;
    int64_t next_pts_ = 0;
    bool positioned_ = false;
    // 切片 W：FIFO 里还压着多少合成静音采样；每个编码块各含多少合成采样
    // （按发送顺序排队，与编码器输出的包一一对应）。
    int64_t silence_in_fifo_ = 0;
    std::deque<int> block_synthetic_samples_;
    int64_t current_packet_synthetic_ = 0;
    // 切片 W：空洞判据的参数。sample_rate / block_samples 由 Init 按编码器填，
    // fill_enabled / merge_contiguous 由 ConfigureSilenceFill 填。
    SilenceFillPolicy silence_policy_;
};

// 切片 V：解码链能不能起来，必须在**决策之前**问清楚。
// 决策表一旦写进报告就不能再改口，而「能编码成 aac」不等于「源音频解得开」——
// 编码器与解码器是两条独立的能力。这里只做一次开/关，开销可以忽略。
bool CanOpenAudioDecoder(const AVCodecParameters* parameters, std::string* error) {
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) {
        *error = "没有可用的音频解码器（" + std::string(avcodec_get_name(parameters->codec_id)) + "）";
        return false;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        *error = "无法分配音频解码器上下文";
        return false;
    }
    bool ok = avcodec_parameters_to_context(context, parameters) >= 0 && avcodec_open2(context, codec, nullptr) >= 0;
    if (!ok)
        *error = "音频解码器打不开（编码参数可能已损坏）";
    avcodec_free_context(&context);
    return ok;
}

// 切片 V：容器拒绝的音频轨 → 实测出来的转码目标（含已打开的编码器上下文）。
// S08 有十几个早退 return，用一个有析构的容器兜住，别指望每个出口都记得释放。
class AudioReencodeTargets {
public:
    ~AudioReencodeTargets() {
        for (auto& entry : by_input_)
            avcodec_free_context(&entry.second.encoder);
    }
    AudioReencodeTargets(const AudioReencodeTargets&) = delete;
    AudioReencodeTargets& operator=(const AudioReencodeTargets&) = delete;
    AudioReencodeTargets() = default;

    AudioReencodeTarget& For(int input_index) {
        return by_input_[input_index];
    }
    AudioReencodeTarget* Find(int input_index) {
        const auto it = by_input_.find(input_index);
        return it == by_input_.end() ? nullptr : &it->second;
    }
    // 取走编码器上下文（所有权转移给音频转码链）。
    AVCodecContext* Take(int input_index) {
        const auto it = by_input_.find(input_index);
        if (it == by_input_.end())
            return nullptr;
        AVCodecContext* encoder = it->second.encoder;
        it->second.encoder = nullptr;
        return encoder;
    }

private:
    std::map<int, AudioReencodeTarget> by_input_;
};

AVRational SourceFrameRate(const AVStream* stream) {
    AVRational fps = stream->avg_frame_rate;
    // 切片 U 实测：avg_frame_rate 是「帧数 ÷ 总时长」，源中间缺一段时它会显著偏小。
    // 25 fps 的 40 帧片子里空掉 12 帧（0.48 s）之后 avg 掉到 19.23，而 r_frame_rate
    // （时间戳的最小公倍帧率，即名义帧率）仍是 25。拿 19.23 当 CFR 帧率会给每帧
    // 多算 12 ms：整条视频被拉慢 30%，而报告只会说「按推定帧率重建」。
    //
    // 只在名义帧率**明显高于**平均值（> 10%）时改用名义帧率 —— 那正是「有空档/
    // 丢帧」的签名。两者接近时仍沿用 avg（VFR 源的平均值更有代表性）。
    const AVRational nominal = stream->r_frame_rate;
    if (fps.num > 0 && fps.den > 0 && nominal.num > 0 && nominal.den > 0) {
        const double avg_value = av_q2d(fps);
        const double nominal_value = av_q2d(nominal);
        if (avg_value > 0.0 && nominal_value > avg_value * 1.10)
            return nominal;
        return fps;
    }
    if (fps.num <= 0 || fps.den <= 0)
        fps = stream->r_frame_rate;
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

void SetTranscodeAudioContainerRejectedForTesting(int input_stream_index) {
    g_inject_audio_container_rejected.store(input_stream_index, std::memory_order_relaxed);
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

    // ---- 切片 T：先给每条输入轨做决策，再按决策建输出 ----
    //
    // 旧实现把「主视频轨重编码 + 每条音频轨按原码流复制」写死在流程里，三种后果
    // 都是实测确认过的：第二视频轨 / 封面 / 字幕 / 数据轨无声消失；音频轨被目标
    // 容器拒绝时整份产物写头失败；「这条轨为什么重编码」从来没有落过账。
    const std::string output_ext = paths.extension.empty() ? LowerExtension(paths.part_path) : paths.extension;
    const char* format_name = FormatNameForExtension(output_ext);

    std::vector<TranscodeTrackProbe> probes;
    probes.reserve(in->nb_streams);
    // 切片 V：被目标容器拒绝的音频轨，在这里实测出一条能落地的重编码目标。
    // 编码器上下文随目标一起留着 —— 后面建输出流要用同一份 extradata。
    AudioReencodeTargets audio_targets;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* stream = in->streams[i];
        TranscodeTrackProbe probe;
        probe.input_stream_index = static_cast<int>(i);
        probe.kind = TranscodeTrackKindName(stream->codecpar->codec_type,
                                            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0);
        probe.source_codec = avcodec_get_name(stream->codecpar->codec_id);
        probe.discarded = stream->discard == AVDISCARD_ALL;
        // 容器接受性只对音频轨有决策意义，且必须实测：avformat_query_codec 会
        // 对 mpegts 这类没实现 query 的封装器返回垃圾值，按它判会误删轨。
        if (probe.kind == "audio" && !probe.discarded) {
            probe.container_fit = ProbeTrackFitsContainer(format_name, stream->codecpar, &probe.container_fit_detail);
            if (g_inject_audio_container_rejected.load(std::memory_order_relaxed) == probe.input_stream_index) {
                // 注入必须留下痕迹：万一哪次真跑进来，报告里能一眼看出这个结论不是实测的。
                probe.container_fit = TranscodeContainerFit::Rejected;
                probe.container_fit_detail = "被强制判为拒绝（测试注入 SetTranscodeAudioContainerRejectedForTesting）";
            }
            probe.audio_reencode_allowed = request.reencode_audio_to_fit_container;
            if (probe.container_fit == TranscodeContainerFit::Rejected) {
                if (!probe.audio_reencode_allowed) {
                    probe.audio_reencode_note = "本次未允许重编码（请求选项 reencode_audio_to_fit_container=false）";
                } else {
                    AudioReencodeTarget target = SelectAudioReencodeTarget(format_name, stream->codecpar);
                    probe.audio_reencode_note = AudioReencodeAttemptsText(target.attempts);
                    std::string decoder_error;
                    if (target.found && !CanOpenAudioDecoder(stream->codecpar, &decoder_error)) {
                        // 编码目标测出来了，但源音频根本解不开 —— 决策必须落回 drop，
                        // 不能写「将重编码」然后运行时才发现做不到。
                        probe.audio_reencode_note += "；" + decoder_error;
                        avcodec_free_context(&target.encoder);
                        target.found = false;
                    }
                    if (target.found) {
                        probe.audio_reencode_encoder = target.encoder_name;
                        probe.audio_reencode_codec = target.codec_name;
                        probe.audio_reencode_settings = AudioEncodeSettingsText(target.settings);
                        audio_targets.For(probe.input_stream_index) = std::move(target);
                    }
                }
            }
        }
        probes.push_back(std::move(probe));
    }
    // 「该轨有没有编码层损伤」只能来自诊断层的证据，不能从「我们要重编码」反推。
    const int primary_video_probe = SelectPrimaryVideoIndex(probes);
    for (auto& probe : probes) {
        if (probe.input_stream_index != primary_video_probe)
            continue;
        for (const auto& finding : diagnosis.findings) {
            if (finding.layer != model::DamageLayer::Decode)
                continue;
            if (finding.location.stream_index && *finding.location.stream_index == probe.input_stream_index) {
                probe.decode_damage_detected = true;
                break;
            }
        }
    }
    const TranscodeTrackPlan track_plan = PlanTranscodeTracks(probes);
    // 决策表的落账入口只留这一个：后面 `outcome.transcode_fidelity = InspectTranscodeFidelity(...)`
    // 会整份替换结构体（实测过一次：早写的决策表被那次替换静默抹掉，报告里一条都不剩），
    // 所以必须在替换之后再调一次，而不是在两处各写一遍赋值。
    const auto apply_track_plan = [&outcome, &track_plan](int input_streams) {
        outcome.transcode_fidelity.track_decisions = track_plan.decisions;
        outcome.transcode_fidelity.track_decision_input_streams = input_streams;
        outcome.transcode_fidelity.track_decision_summary = track_plan.summary;
        outcome.transcode_fidelity.track_decision_note = track_plan.note;
    };
    // 决策表在任何早退路径上都要带走：转码失败时「哪些轨本来会被丢掉」同样是事实。
    apply_track_plan(static_cast<int>(in->nb_streams));
    for (const auto& decision : track_plan.decisions) {
        if (decision.action == "drop")
            outcome.omitted_items.push_back(TranscodeTrackOmittedText(decision));
    }
    // 输入流下标 → 决策动作（copy / transcode / drop）。后面建输出流、缓存首帧前的
    // 音频包都只看这张表，不再各自重判一次。
    std::vector<std::string> track_action(in->nb_streams);
    for (const auto& decision : track_plan.decisions) {
        if (decision.input_stream_index >= 0 && decision.input_stream_index < static_cast<int>(in->nb_streams))
            track_action[static_cast<std::size_t>(decision.input_stream_index)] = decision.action;
    }

    const int video_index = track_plan.primary_video_index;
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
            // 只缓存「决策 = 复制或重编码」的轨：被丢弃的轨（容器拒绝且转不了码的音频 /
            // 字幕 / 数据）缓存起来既无用，还会把 pending 预算吃光，最后误报「前缀可能缺失」。
            //
            // ⚠️ 重编码轨**也必须**在这里缓存。切片 V 之前只有 copy 轨会进这个缓冲，
            // 于是「首帧探测期间读到的音频包」对重编码轨是直接丢掉的 —— 实测后果是
            // 产物音频从 0.58 s 才开始（正好等于探测消耗的时长），而整份产物仍报成功。
            const std::size_t index = static_cast<std::size_t>(std::max(packet->stream_index, 0));
            const bool will_consume =
                index < track_action.size() && (track_action[index] == "copy" || track_action[index] == "transcode");
            if (will_consume && !pending_nonvideo.Push(packet, &pending_bytes, kMaxPendingBytes, kMaxPendingPackets)) {
                pending_overflow = true;
            }
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
    // 切片 U：「帧时长」这个数只允许来自源侧证据。SourceFrameRate 在没有证据时
    // 会兜底 25 fps —— 拿兜底值去把空洞秒数换算成帧数是纯粹的编造，所以这里
    // 单独记下证据是否真的存在，供黑帧填补的判据拒绝使用兜底值。
    const bool frame_rate_from_source = (video_in->avg_frame_rate.num > 0 && video_in->avg_frame_rate.den > 0) ||
                                        (video_in->r_frame_rate.num > 0 && video_in->r_frame_rate.den > 0);

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

    // ---- 切片 Q：容器级 side data 的继承（方向矩阵 / HDR 静态元数据）----
    //
    // 这两份都是**流级** side data，不跟着像素走。手机竖拍的片子是「画面横着存 +
    // 矩阵声明 90°」，只重编码像素会让产物躺倒。旧实现两份都不拷，报告写
    // 「未继承」—— 恰好蒙对，但那是从「我们没拷」推出来的，产物侧一次没量过。
    //
    // muxer 在 avformat_write_header 时从输出流的 codecpar->coded_side_data 取
    // 矩阵写 tkhd、取 HDR 元数据写 mdcv / clli，所以必须赶在写头之前挂上。
    // ❗ 拷贝成功 ≠ 产物真的有：真正有没有由收尾处打开产物实测，不拿意图当结论。
    bool display_matrix_attached = false;
    bool hdr_metadata_attached = false;
    // HDR 元数据有位深前提：10 bit 源压成 8 bit 产物后，照搬 HDR 标记会让播放器把
    // 一个 SDR 画面按 HDR 渲染。这里主动不拷，并把「为什么没拷」记进结论层。
    const bool hdr_inheritable = TranscodeHdrInheritable(
        TranscodeBitDepth(static_cast<AVPixelFormat>(first_frame->format)), TranscodeBitDepth(encoder_format));
    bool hdr_inherit_blocked = false;
    {
        std::size_t matrix_size = 0;
        const std::uint8_t* matrix = SourceDisplayMatrix(video_in->codecpar, first_frame, &matrix_size);
        if (matrix != nullptr) {
            AVPacketSideData* copied =
                av_packet_side_data_new(&video_out->codecpar->coded_side_data, &video_out->codecpar->nb_coded_side_data,
                                        AV_PKT_DATA_DISPLAYMATRIX, matrix_size, 0);
            if (copied != nullptr) {
                std::memcpy(copied->data, matrix, matrix_size);
                display_matrix_attached = true;
            }
        }
        const bool source_has_hdr =
            av_packet_side_data_get(video_in->codecpar->coded_side_data, video_in->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr ||
            av_packet_side_data_get(video_in->codecpar->coded_side_data, video_in->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_CONTENT_LIGHT_LEVEL) != nullptr;
        if (source_has_hdr && !hdr_inheritable)
            hdr_inherit_blocked = true;
        for (const AVPacketSideDataType type :
             {AV_PKT_DATA_MASTERING_DISPLAY_METADATA, AV_PKT_DATA_CONTENT_LIGHT_LEVEL}) {
            if (!hdr_inheritable)
                break;
            const AVPacketSideData* source = av_packet_side_data_get(video_in->codecpar->coded_side_data,
                                                                     video_in->codecpar->nb_coded_side_data, type);
            if (source == nullptr || source->size == 0)
                continue;
            AVPacketSideData* copied = av_packet_side_data_new(
                &video_out->codecpar->coded_side_data, &video_out->codecpar->nb_coded_side_data, type, source->size, 0);
            if (copied != nullptr) {
                std::memcpy(copied->data, source->data, source->size);
                hdr_metadata_attached = true;
            }
        }
    }

    // 音频：按决策表决定复制哪些轨、重编码哪些轨。drop 的轨（容器拒绝且没有
    // 可用重编码目标 / 源侧已标记丢弃）不再尝试复制 —— 旧实现一律复制，其中
    // 一条被拒就让整份产物写头失败。
    std::vector<int> audio_map(in->nb_streams, -1);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* src = in->streams[i];
        if (src->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        const bool copy_track = i < track_action.size() && track_action[i] == "copy";
        const bool reencode_track = i < track_action.size() && track_action[i] == "transcode";
        if (!copy_track && !reencode_track)
            continue;
        AVStream* dst = avformat_new_stream(out, nullptr);
        if (dst == nullptr) {
            outcome.omitted_items.push_back("音频流 #" + std::to_string(i) + " 无法建立输出流，产物未保留");
            continue;
        }
        std::string output_codec;
        if (copy_track) {
            if (avcodec_parameters_copy(dst->codecpar, src->codecpar) < 0) {
                outcome.omitted_items.push_back("音频流 #" + std::to_string(i) + " 无法复制编码参数，产物未保留");
                continue;
            }
            dst->time_base = src->time_base;
            dst->disposition = src->disposition;
            output_codec = avcodec_get_name(src->codecpar->codec_id);
        } else {
            // 重编码轨的输出参数必须来自**已经开起来的编码器**：extradata 要与
            // 真实码流一致，自己拼一份 AAC AudioSpecificConfig 就是编造。
            const AudioReencodeTarget* target = audio_targets.Find(static_cast<int>(i));
            if (target == nullptr || target->encoder == nullptr ||
                avcodec_parameters_from_context(dst->codecpar, target->encoder) < 0) {
                outcome.omitted_items.push_back("音频流 #" + std::to_string(i) +
                                                " 无法取到重编码后的编码参数，产物未保留");
                continue;
            }
            dst->time_base = target->encoder->time_base;
            output_codec = target->codec_name;
        }
        dst->codecpar->codec_tag = 0;
        av_dict_copy(&dst->metadata, src->metadata, 0);
        audio_map[i] = static_cast<int>(out->nb_streams) - 1;
        stream_meta[audio_map[i]] = {"audio", output_codec};
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
        // 旧文案「音频编码参数可能不被目标容器接受」是猜的：它把「我们没查」写成
        // 了「大概是这个原因」。现在被拒的轨已经在决策阶段剔除，这里只能如实说
        // 清楚「是否还有被拒轨没剔除」。
        const bool any_rejected = std::any_of(probes.begin(), probes.end(), [](const TranscodeTrackProbe& probe) {
            return probe.container_fit == TranscodeContainerFit::Rejected;
        });
        outcome.error = std::string("写入封装头失败（") + format_name + "）；" +
                        (any_rejected ? "已按试写头实测剔除被拒音频轨，写头仍然失败，原因不在轨编码兼容性"
                                      : "逐轨实测没有被目标容器拒绝的轨道，失败原因不在轨编码兼容性");
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

    // ---- 切片 V：为被容器拒绝、且实测出了目标的音频轨建转码链 ----
    //
    // 「编码器存在 / 参数能开 / 容器接受 / 解码器能开」四条都已经在决策之前实测过，
    // 这里再失败基本只剩内存不足。此时**不能**留一条永远没有包的空音轨（那等于
    // 让产物「看起来保留了音轨」），按硬错误中止并把原因写清。
    std::map<int, std::unique_ptr<AudioTranscodeChain>> audio_chains;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        if (i >= track_action.size() || track_action[i] != "transcode" || audio_map[i] < 0)
            continue;
        AVCodecContext* audio_encoder = audio_targets.Take(static_cast<int>(i));
        if (audio_encoder == nullptr)
            continue;
        auto chain = std::make_unique<AudioTranscodeChain>();
        // 切片 W：转码链手里有编码器，是唯一能造出「源里不存在的声音」的路径。
        // 是否真的造由用户开关决定（默认关），判定仍在链内按源 PTS 逐帧做。
        chain->ConfigureSilenceFill(request.fill_audio_gaps_with_silence,
                                    request.gap_policy == model::RepairGapPolicy::MergeContiguous);
        std::string chain_error;
        if (!chain->Init(audio_encoder, in->streams[i]->codecpar, origin_seconds, static_cast<int>(i), &chain_error)) {
            // 编码器上下文的所有权在 Init 的第一行就转移给链了（失败时由链的析构
            // 释放）。这里**不能**再 free 一次 —— 那是对同一块内存二次释放。
            outcome.error = "音频重编码链无法建立（流 #" + std::to_string(i) + "）：" + chain_error;
            av_write_trailer(out);
            avio_closep(&out->pb);
            avformat_free_context(out);
            av_packet_free(&encoded);
            av_frame_free(&converted);
            avcodec_free_context(&encoder);
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&decoder);
            avformat_close_input(&in);
            return outcome;
        }
        chain->SetSourceTimeBase(in->streams[i]->time_base);
        chain->output_stream_index = audio_map[i];
        audio_chains[static_cast<int>(i)] = std::move(chain);
    }

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
    // 整份替换之后把轨级决策补回来（理由见上面 apply_track_plan 的注释）。
    apply_track_plan(static_cast<int>(in->nb_streams));
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

    // ---- 切片 U / Y：时间轴空洞探测与合成填补 ----
    //
    // 只要解码链跑起来就探测（探测是证据，插帧是动作，两件事分开：用户没启用
    // 时报告也要能说「源里确实有 2 处空洞」，不能因为没插帧就当没发生）。
    VideoGapFillPolicy video_gap_policy;
    video_gap_policy.frame_seconds = frame_seconds;
    video_gap_policy.frame_rate_from_source = frame_rate_from_source;
    video_gap_policy.fill_enabled = request.fill_video_gaps;
    video_gap_policy.content = request.video_gap_fill_content;
    video_gap_policy.merge_contiguous = request.gap_policy == model::RepairGapPolicy::MergeContiguous;
    model::RepairSyntheticFill& synthetic_fill = outcome.synthetic_fill;
    synthetic_fill.inspected = false;
    synthetic_fill.requested = video_gap_policy.fill_enabled;
    synthetic_fill.content = VideoGapFillContentCode(request.video_gap_fill_content);
    synthetic_fill.basis =
        "判据：相邻源解码帧 PTS 间隔 vs 标称帧率 " + std::to_string(fps.num) + "/" + std::to_string(fps.den) +
        " fps（" + (frame_rate_from_source ? "源侧声明" : "非源侧证据，不做填补") +
        "）；容差 1.5 帧，且间隔须为帧时长的整数倍；填补内容：" + VideoGapFillContentName(synthetic_fill.content);
    // 上一个「有源时间戳」的解码帧（秒）。用它和当前帧比出间隔。
    std::optional<double> last_source_video_seconds;
    uint64_t decoded_video_frames = 0;
    double synthetic_seconds_accum = 0.0;
    // 合成帧在编码器时基上的 PTS。drain_encoder 按 PTS 精确归因（帧序号唯一
    // 且单调，不会与真实帧撞号），据此把合成帧排除在样本账本之外。
    std::unordered_set<int64_t> synthetic_video_pts;
    // 切片 Y：`converted` 里当前是否躺着一幅**真实解码画面**（上一次 sws_scale
    // 的产物）。重复帧填补直接复用它 —— 探测发生在当前帧 sws_scale 之前，此刻
    // `converted` 装的正是上一帧，不需要额外留一份拷贝。
    // 首帧之前 / 上一帧在转换阶段就被丢弃时它是 false：那时缓冲里是未初始化内存，
    // 拿去当画面重复就是往产物里写垃圾。
    bool have_picture_in_converted = false;
    // 复用一块黑帧缓冲：每帧 av_frame_make_writable 保证编码器还持着引用时
    // 不会读到我们正在改的内存。
    AVFrame* black_frame = av_frame_alloc();
    if (black_frame != nullptr) {
        black_frame->format = encoder_format;
        black_frame->width = width;
        black_frame->height = height;
        // av_image_fill_black 要的是 ptrdiff_t 行宽数组，AVFrame::linesize 是 int[8]，
        // 两个类型在 64 位下不能隐式转换（C2664），必须显式摊平一次。
        ptrdiff_t black_linesize[4] = {0, 0, 0, 0};
        bool black_ok = av_frame_get_buffer(black_frame, 0) >= 0;
        if (black_ok) {
            for (int plane = 0; plane < 4; ++plane)
                black_linesize[plane] = black_frame->linesize[plane];
            black_ok =
                av_image_fill_black(black_frame->data, black_linesize, encoder_format,
                                    encoder->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG,
                                    width, height) >= 0;
        }
        if (!black_ok)
            av_frame_free(&black_frame);
    }

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
            // 切片 U：合成黑帧的归因必须在 rescale 之前抓 —— 集合里存的都是
            // 编码器时基上的帧序号，换算成封装器时基就对不上了。
            const bool is_synthetic_frame =
                encoded->pts != AV_NOPTS_VALUE && synthetic_video_pts.count(encoded->pts) > 0;
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
            // 样本账本记的是「与源对应的已恢复样本」。合成黑帧没有源侧对应样本，
            // 记进去会让逐轨覆盖把填补的空档算成"恢复出来的内容" —— 那正是
            // 切片 U 明令禁止的事。它们单独记在 synthetic_fill 里。
            if (!is_synthetic_frame) {
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

    // 切片 Y：把 `converted` 里的上一帧画面重复 count 次，返回**实际**送进去的帧数。
    //
    // 复用同一个 AVFrame 反复送编码器：每次 make_writable 会按引用计数决定要不要
    // 换缓冲，但内容始终是同一幅画面 —— 这正是「冻结」的语义。占位方式与黑帧完全
    // 一致（共用 encoded_frames 序号），所以时间轴效果也一致。
    //
    // **前置条件**：调用前 `converted` 里必须是一幅真实解码画面（见
    // have_picture_in_converted）。这一条由判定层保证：每次 PlanVideoGapFill 之前
    // 都把 policy.previous_frame_available 赋成 have_picture_in_converted，没有画面
    // 时判定结果是 FillWithBlackFallback，根本不会走到这里。
    // 这里**不再重复判一次**：真走到没有画面的话，静默返回 0 会被上层记成「编码器
    // 拒收」，那是个假理由 —— 留一个说不清的兜底，比把不变量写清楚更糟。
    auto encode_repeated_frames = [&](int64_t count) -> int64_t {
        if (count <= 0)
            return 0;
        int64_t sent = 0;
        for (int64_t i = 0; i < count; ++i) {
            if (av_frame_make_writable(converted) < 0) {
                resource_failed = true;
                resource_error = "重复帧缓冲不可写（无法分配新的输出帧缓冲）";
                break;
            }
            converted->pts = static_cast<int64_t>(encoded_frames) + video_pts_offset_frames;
            const int64_t assigned = converted->pts;
            if (avcodec_send_frame(encoder, converted) < 0)
                break; // 编码器拒收：不重试、不假装插进去了
            synthetic_video_pts.insert(assigned);
            ++encoded_frames;
            ++sent;
            if (!drain_encoder())
                break;
            if (size_capped || write_failed || resource_failed)
                break;
        }
        return sent;
    };

    // 切片 U：把 count 个合成黑帧送进编码器，返回**实际**送进去的帧数。
    //
    // PTS 与真实帧共用同一个 encoded_frames 序号，所以填充帧是真的占住了时间轴上
    // 的那几格，后续真实帧自然顺延 —— 这就是「把空洞补回去」的全部含义，没有别的
    // 隐式时间轴改写。返回值和 count 不等时必须如实上报（编码器拒收 / 写失败）。
    auto encode_black_frames = [&](int64_t count) -> int64_t {
        if (black_frame == nullptr || count <= 0)
            return 0;
        int64_t sent = 0;
        for (int64_t i = 0; i < count; ++i) {
            if (av_frame_make_writable(black_frame) < 0) {
                resource_failed = true;
                resource_error = "合成黑帧缓冲不可写（无法分配新的输出帧缓冲）";
                break;
            }
            black_frame->pts = static_cast<int64_t>(encoded_frames) + video_pts_offset_frames;
            const int64_t assigned = black_frame->pts;
            if (avcodec_send_frame(encoder, black_frame) < 0)
                break; // 编码器拒收：不重试、不假装插进去了
            synthetic_video_pts.insert(assigned);
            ++encoded_frames;
            ++sent;
            if (!drain_encoder()) {
                // 写盘失败已经由 drain_encoder 记好原因，这里只停止继续插帧。
                break;
            }
            if (size_capped || write_failed || resource_failed)
                break;
        }
        return sent;
    };

    // 判为可填之后按请求的内容落地。返回实际插入帧数；`fallback_black` 表示
    // 「本该重复却只能填黑帧」（调用方据此累计 fallback_black_frames）。
    // 放在两个填充函数之后：lambda 按值/引用捕获，必须先声明后使用。
    auto encode_gap_fill = [&](int64_t count, bool fallback_black) -> int64_t {
        const bool repeat = video_gap_policy.content == model::VideoGapFillContent::PreviousFrame && !fallback_black;
        return repeat ? encode_repeated_frames(count) : encode_black_frames(count);
    };

    auto encode_frame = [&](AVFrame* source) -> bool {
        // ---- 切片 U / Y：先按源时间戳探测空洞，再决定要不要在它前面补内容 ----
        //
        // 探测与插帧是两件事：探测是证据（用户没启用也要如实进报告），插帧是
        // 动作（只有显式启用才做）。缺源时间戳时探测不完整，单独计数上报。
        if (source->pts != AV_NOPTS_VALUE) {
            const double source_seconds = static_cast<double>(source->pts) * av_q2d(video_in->time_base);
            synthetic_fill.inspected = true;
            if (last_source_video_seconds) {
                // 「有没有可重复的上一帧」是**逐处**的事实，不是整轮的属性：第一处
                // 空洞之前那一帧若在转换阶段被丢弃，这一处就只能退回黑帧。
                video_gap_policy.previous_frame_available = have_picture_in_converted;
                VideoGapFillPlan plan = PlanVideoGapFill(*last_source_video_seconds, source_seconds, video_gap_policy,
                                                         synthetic_fill.frames_inserted);
                if (plan.verdict != VideoGapFillVerdict::NoGap) {
                    plan.gap.after_decoded_frames = decoded_video_frames;
                    ++synthetic_fill.gaps_detected;
                    const bool attempted = plan.verdict == VideoGapFillVerdict::Fill ||
                                           plan.verdict == VideoGapFillVerdict::FillWithBlackFallback;
                    if (attempted) {
                        const int64_t sent = encode_gap_fill(
                            plan.gap.missing_frames, plan.verdict == VideoGapFillVerdict::FillWithBlackFallback);
                        plan.gap.inserted_frames = sent;
                        synthetic_fill.frames_inserted += sent;
                        synthetic_seconds_accum += static_cast<double>(sent) * frame_seconds;
                        if (plan.verdict == VideoGapFillVerdict::FillWithBlackFallback)
                            synthetic_fill.fallback_black_frames += sent;
                        if (sent != plan.gap.missing_frames) {
                            plan.verdict = VideoGapFillVerdict::EncoderRejected;
                            plan.gap.action = "not_filled";
                            plan.gap.reason_code = VideoGapFillReasonCode(plan.verdict, video_gap_policy.fill_enabled);
                            // 内容类型跟着**实际送进去的**走：退回黑帧的那一处即使
                            // 只送进一半，也不许写成「重复画面」。
                            plan.gap.synthetic_kind = plan.gap.synthetic_kind.empty()
                                                          ? VideoGapFillKind(plan.verdict, video_gap_policy.content)
                                                          : plan.gap.synthetic_kind;
                        }
                    }
                    if (plan.gap.inserted_frames <= 0)
                        ++synthetic_fill.gaps_refused;
                    plan.gap.text = VideoGapFillGapText(plan.gap);
                    synthetic_fill.gaps.push_back(plan.gap);
                    if (write_failed || resource_failed)
                        return false;
                }
            }
            last_source_video_seconds = source_seconds;
        } else {
            ++synthetic_fill.frames_without_source_pts;
        }
        ++decoded_video_frames;

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
        // 切片 Y：从这一刻起 `converted` 里装的是真实画面，可以拿来重复。
        // 探测发生在下一帧的入口处，那时 sws_scale 还没覆盖它 —— 所以不必另存一份。
        have_picture_in_converted = true;
        // encoder->time_base = av_inv_q(fps)，PTS 每帧 +1 tick = 1/fps 秒。
        converted->pts = static_cast<int64_t>(encoded_frames) + video_pts_offset_frames;
        if (avcodec_send_frame(encoder, converted) < 0) {
            ++outcome.dropped_video_frames;
        } else {
            ++encoded_frames;
        }
        return drain_encoder();
    };

    // ---- 切片 W：码流复制轨的空洞探测（只探测，不填补）----
    //
    // 复制路径不解码，所以手里没有采样；但包 PTS 与包时长是可测的：相邻包之间
    // 缺了多久算得出来。补不了（没有这条编码的编码器，造不出采样），但「源里
    // 缺了 0.28 s」必须能报出来 —— 不报的话「没补静音」会被读成「没有空洞」。
    std::map<int, model::RepairSyntheticAudioFill> copied_audio_fills;
    std::map<int, double> copied_audio_end_seconds;
    std::map<int, int64_t> copied_audio_packets;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        if (audio_map[i] < 0)
            continue;
        if (i < track_action.size() && track_action[i] != "copy")
            continue;
        model::RepairSyntheticAudioFill fill;
        fill.inspected = true;
        fill.requested = request.fill_audio_gaps_with_silence;
        fill.coding_mode = "copy";
        fill.input_stream_index = static_cast<int>(i);
        const AVCodecParameters* parameters = in->streams[i]->codecpar;
        fill.codec = parameters != nullptr ? avcodec_get_name(parameters->codec_id) : std::string();
        fill.basis = "判据：相邻音频包 PTS + 包时长（码流复制路径不解码），采样率 " +
                     std::to_string(parameters != nullptr ? parameters->sample_rate : 0) + " Hz";
        copied_audio_fills[static_cast<int>(i)] = std::move(fill);
    }

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
        // 切片 W：空洞探测必须在改 PTS 之前做 —— 之后 pts 已按统一原点平移、
        // 包时长也被 rescale 到输出时基，秒数容易算错。
        if (pkt->pts != AV_NOPTS_VALUE) {
            const auto detection = copied_audio_fills.find(in_index);
            if (detection != copied_audio_fills.end()) {
                model::RepairSyntheticAudioFill& fill = detection->second;
                const double tb = av_q2d(src->time_base);
                const double start = static_cast<double>(pkt->pts) * tb;
                const int rate = src->codecpar != nullptr ? src->codecpar->sample_rate : 0;
                const int block = src->codecpar != nullptr ? src->codecpar->frame_size : 0;
                // 编码一包最多承载多少秒。只有编解码器声明了帧长（aac 1024 等）时才知道；
                // 不定长 / 可变帧长的编码器（flac、opus）不能拿它当判据。
                const double nominal = (rate > 0 && block > 0) ? static_cast<double>(block) / rate : 0.0;
                double duration = pkt->duration > 0 ? static_cast<double>(pkt->duration) * tb : 0.0;
                if (duration <= 0.0)
                    duration = nominal;
                const int64_t ordinal = copied_audio_packets[in_index]++;
                SilenceFillPolicy policy;
                policy.sample_rate = rate;
                policy.block_samples = block > 0 ? block : 0;
                policy.fill_enabled = request.fill_audio_gaps_with_silence;
                policy.merge_contiguous = request.gap_policy == model::RepairGapPolicy::MergeContiguous;
                // 复制轨没有编码器：判据照跑，动作只能是「未填补 + 说明原因」。
                policy.reencode_available = false;
                const auto record_gap = [&](int64_t missing_samples, double position_seconds) {
                    if (missing_samples <= 0)
                        return;
                    const SilenceFillPlan plan = PlanSilenceFill(0, missing_samples, policy, 0);
                    if (plan.verdict == SilenceFillVerdict::NoGap)
                        return;
                    model::RepairSyntheticAudioGap gap = plan.gap;
                    gap.source_position_seconds = position_seconds;
                    gap.after_audio_frames = static_cast<uint64_t>(ordinal);
                    gap.text = SilenceFillGapText(gap);
                    fill.gaps.push_back(gap);
                };
                // 形态一：容器把空洞并进了前一个样本的时长 —— 这一包声称的时长远超
                // 编码一包能承载的时长（实测：一个 AAC 包声称 0.302 s ≈ 13 个编码块，
                // 实际只携带 1 块）。只按 PTS 找洞会完全看不见这种形态。
                const bool inflated = nominal > 0.0 && duration > nominal * 1.5;
                if (inflated)
                    record_gap(std::llround((duration - nominal) * rate), start + nominal);
                // 形态二：样本表按真实时长记，空洞留在时间轴上（PTS 直接跳过去）。
                const auto last = copied_audio_end_seconds.find(in_index);
                if (!inflated && nominal > 0.0 && last != copied_audio_end_seconds.end())
                    record_gap(std::llround((start - last->second) * rate), last->second);
                if (duration > 0.0) {
                    copied_audio_end_seconds[in_index] = start + duration;
                } else {
                    // 缺时长的包会让后面每个包都被误判成「洞」：丢掉基线，别猜。
                    ++fill.packets_without_duration;
                    copied_audio_end_seconds.erase(in_index);
                }
            }
        }
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

    // ---- 切片 V：重编码音频的写包入口 ----
    //
    // 与 copy 路径共用同一套预算 / 故障注入 / 账本口径，差别只有两点：时间戳来自
    // 编码器时基；账本里必须写明这条轨的字节**不是**源字节（重编码产物）。
    auto MakeAudioSink = [&](AudioTranscodeChain* chain) -> AudioTranscodeChain::Sink {
        return [&, chain](AVPacket* pkt) -> bool {
            if (size_capped)
                return true;
            pkt->stream_index = chain->output_stream_index;
            AVStream* dst = out->streams[chain->output_stream_index];
            av_packet_rescale_ts(pkt, chain->EncoderTimeBase(), dst->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) {
                // 夹到 0 的理由与 copy 路径一致：负 PTS 只可能来自编码器 priming。
                const double seconds = std::max(0.0, static_cast<double>(pkt->pts) * av_q2d(dst->time_base));
                if (!observed_audio_start_seconds || seconds < *observed_audio_start_seconds)
                    observed_audio_start_seconds = seconds;
                if (!chain->first_output_seconds)
                    chain->first_output_seconds = seconds;
            }
            const int written = pkt->size;
            ++write_attempts;
            if (ShouldInjectWriteFailure(write_attempts)) {
                write_failed = true;
                write_error = "写入音频包失败（故障注入）";
                return false;
            }
            if (av_interleaved_write_frame(out, pkt) < 0) {
                write_failed = true;
                write_error = "写入重编码音频包失败";
                return false;
            }
            {
                // 切片 W：含合成静音的编码包整块不进账本。账本的口径是「与源对应的
                // 已恢复样本」，静音不是从源解出来的。宁可少记（混合块里的真实采样
                // 也一起排除，每处空洞最多少记一个编码块），也不让补进来的时长抬高
                // 恢复率 —— 这条与视频黑帧的处理同一个理由。
                if (!chain->CurrentPacketHasSyntheticSamples()) {
                    model::RecoveredSample sample;
                    sample.size = static_cast<uint64_t>(std::max(written, 0));
                    sample.track_id = 0;
                    sample.stream_index = chain->output_stream_index;
                    sample.config_id = "s08_reencoded_audio";
                    sample.decode_ordinal = sample_ledger.CountForTrack(chain->output_stream_index);
                    sample.timestamp_evidence = model::SampleTimestampEvidence::Inferred;
                    sample.inference_basis = "S08 音频重编码（原编码不被目标容器接受，编码器=" + chain->encoder_name +
                                             "），PTS 按统一原点平移重建";
                    sample.confidence = model::SampleConfidence::Inferred;
                    sample_ledger.Add(sample);
                }
            }
            bytes_written += static_cast<uint64_t>(std::max(written, 0));
            if (max_output_bytes > 0 && bytes_written > max_output_bytes)
                size_capped = true;
            return true;
        };
    };
    std::map<int, AudioTranscodeChain::Sink> audio_sinks;
    for (auto& entry : audio_chains)
        audio_sinks[entry.first] = MakeAudioSink(entry.second.get());

    // 非视频包的分发：重编码轨进链，复制轨直接写。两条路的失败语义一致（写盘失败
    // 会置 write_failed），所以调用点不需要区分。
    auto feed_audio_packet = [&](AVPacket* pkt) -> bool {
        const auto it = audio_chains.find(pkt->stream_index);
        if (it == audio_chains.end())
            return write_audio_packet(pkt);
        AudioTranscodeChain* chain = it->second.get();
        std::string chain_error;
        if (!chain->FeedPacket(pkt, audio_sinks.at(pkt->stream_index), &chain_error)) {
            write_failed = true;
            write_error = chain_error;
            return false;
        }
        return true;
    };

    // 回放首帧探测期间缓存的音频包，之后才继续读 `in`。
    for (AVPacket* cached : pending_nonvideo.items()) {
        if (!feed_audio_packet(cached))
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
                feed_audio_packet(packet);
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
    // 切片 V：把重编码音频链排空。解码器、重采样器、编码器里各有一层缓冲，
    // 少冲一层就会静默少掉末段采样 —— 那种「产物能播但结尾被切」最难看出来。
    // 写失败 / 截断时不再写盘：产物已经不打算交付了。
    if (!write_failed && !resource_failed && !size_capped) {
        for (auto& entry : audio_chains) {
            std::string chain_error;
            if (!entry.second->Flush(audio_sinks.at(entry.first), &chain_error)) {
                write_failed = true;
                write_error = chain_error;
                break;
            }
        }
    }
    // 逐帧实测的动态分辨率帧数并进同一个保真结论，写在所有返回路径之前：
    // 后面失败 / 取消 / 截断的分支也得带上这份实测，不能退回去用空表自证。
    // 注意：这里还不算最终结论 —— 方向矩阵 / HDR 的继承要等产物写完后实测定论，
    // 所以 transcode_note 里的项数统计放在收尾处（见 FinalizeSideDataInheritance 之后）。
    FinalizeTranscodeFidelity(outcome.transcode_fidelity, scale_state.resolution_changed_frames);

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
    // 切片 U：恢复比例需要源侧时长基线。S08 以前根本不填这个字段，于是转码产物
    // 永远只拿到「缺少源侧时长基线，未给出恢复比例」—— 空着不是「没问题」，
    // 是结论缺失；而「填补不许抬高恢复比例」这条也只有在有基线时才可验证。
    {
        double source_duration = in->duration > 0 ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            const AVStream* stream = in->streams[i];
            if (stream->duration <= 0)
                continue;
            const bool counted = (stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ||
                                 audio_map[i] >= 0;
            if (counted)
                source_duration =
                    std::max(source_duration, static_cast<double>(stream->duration) * av_q2d(stream->time_base));
        }
        outcome.source_duration_seconds = source_duration;
    }
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
    av_frame_free(&black_frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&in);

    // ---- 切片 Q：打开产物实测两份容器级 side data 到底进没进去 ----
    //
    // 「拷了」和「产物里真的有」是两件事：muxer 可能不支持该盒，也可能因为
    // 参数不合规静默丢弃。结论必须来自产物本身，不拿意图当事实。
    // 只在写成功时测：写失败 / 截断的产物可能连 moov 都不完整，打开失败本身
    // 不该被读成「产物缺少方向信息」。
    bool side_data_measured = false;
    bool output_has_matrix = false;
    bool output_has_hdr = false;
    double output_rotation = 0.0;
    if (!write_failed) {
        AVFormatContext* verify = nullptr;
        if (avformat_open_input(&verify, paths.part_path.c_str(), nullptr, nullptr) >= 0) {
            for (unsigned i = 0; i < verify->nb_streams; ++i) {
                const AVStream* stream = verify->streams[i];
                if (stream->codecpar == nullptr || stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                    continue;
                const AVPacketSideData* display = av_packet_side_data_get(
                    stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
                if (display != nullptr && display->size >= 9 * sizeof(std::int32_t)) {
                    std::int32_t matrix[9] = {};
                    std::memcpy(matrix, display->data, sizeof(matrix));
                    output_has_matrix = true;
                    output_rotation = av_display_rotation_get(matrix);
                }
                output_has_hdr =
                    av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr ||
                    av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_CONTENT_LIGHT_LEVEL) != nullptr;
                side_data_measured = true;
                break;
            }
            avformat_close_input(&verify);
        }
    }
    outcome.transcode_fidelity.display_matrix_attached = display_matrix_attached;
    outcome.transcode_fidelity.hdr_metadata_attached = hdr_metadata_attached;
    outcome.transcode_fidelity.hdr_inherit_blocked_by_bit_depth = hdr_inherit_blocked;
    FinalizeSideDataInheritance(outcome.transcode_fidelity, side_data_measured, output_has_matrix, output_rotation,
                                output_has_hdr);
    // 丢失 / 继承项数在这里才齐全（方向与 HDR 的结论刚落地），统计只能放这里。
    // 轨级决策的汇总也放在这里：早写会带上一个悬空的分隔符（那时 transcode_note
    // 还是空的），而且它和上面那批结论是同一次「收尾统计」。
    outcome.transcode_note += "；轨道决策=" + track_plan.summary;
    if (outcome.transcode_fidelity.inspected) {
        const model::RepairTranscodeFidelity& fidelity = outcome.transcode_fidelity;
        outcome.transcode_note += "；源侧保真探测=" + fidelity.source_pixel_format + "→" +
                                  fidelity.output_pixel_format + "，丢失属性 " +
                                  std::to_string(fidelity.lost_properties.size()) + " 项，继承 " +
                                  std::to_string(fidelity.preserved_properties.size()) + " 项";
        if (!fidelity.unverified_properties.empty())
            outcome.transcode_note += "，未实测 " + std::to_string(fidelity.unverified_properties.size()) + " 项";
    }

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
    // 切片 U：合成填补的收尾渲染。放在失败 / 取消分支**之前** —— 探测到的空洞
    // 与已插入的帧数是既成事实，后面转码失败也不能把它们从报告里抹掉。
    synthetic_fill.seconds_inserted = synthetic_seconds_accum;
    synthetic_fill.summary = VideoGapFillSummary(synthetic_fill);
    synthetic_fill.note = VideoGapFillBasisNote(synthetic_fill);
    if (synthetic_fill.frames_inserted > 0) {
        outcome.transcode_note += "；合成黑帧=" + std::to_string(synthetic_fill.frames_inserted) + " 帧（" +
                                  FormatSeconds(synthetic_seconds_accum) + " s），不是从源恢复的内容";
        // 样本账本记的是「与源对应的已恢复样本」，合成帧不在里面；而
        // packets_written 必须与账本总数严格相等（上层守恒会逐项核对），
        // 所以这两个数字必然不同，必须说出来，不能让读者自己猜。
        synthetic_fill.note += "；合成帧已写入产物但未计入样本账本与写入包数（它们没有源侧对应样本），" +
                               std::string("故写入包数比实际封装包数少 ") +
                               std::to_string(synthetic_fill.frames_inserted) + " 个";
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
    outcome.audio_reencoded = !audio_chains.empty();
    // 逐轨写清「这条音轨到底做了什么」。少写一项都会变成「音轨被改过了但没人知道」：
    // 原编码 → 目标编码、编解码帧数、源时间戳跳变、末帧补齐的采样数、解不动的包。
    for (const auto& entry : audio_chains) {
        const AudioTranscodeChain& chain = *entry.second;
        std::string line = "音频流 #" + std::to_string(entry.first) + "：原编码 " + chain.source_codec + " → " +
                           chain.encoder_name + "（解码 " + std::to_string(chain.decoded_frames) + " 帧 / 编码 " +
                           std::to_string(chain.encoded_packets) + " 包）";
        if (chain.timeline_gaps > 0 || chain.silence_samples_inserted > 0) {
            const int64_t filled_gaps =
                static_cast<int64_t>(chain.silence_gaps.size()) - static_cast<int64_t>(chain.timeline_gaps);
            if (filled_gaps > 0) {
                line += "；源时间戳有 " + std::to_string(filled_gaps) + " 处空洞已补入 " +
                        std::to_string(chain.silence_samples_inserted) + " 采样合成静音（合成内容，单独记账）";
            }
            if (chain.timeline_gaps > 0) {
                line += "；另有 " + std::to_string(chain.timeline_gaps) +
                        " 处跳变按源位置跳过、未补静音（原因见「音频合成填补」逐条说明）";
            }
        }
        if (chain.synthetic_packets_excluded > 0) {
            line += "；其中 " + std::to_string(chain.synthetic_packets_excluded) +
                    " 个编码包含合成静音，未计入样本账本（含静音的编码块整块排除，恢复率因此偏保守）";
        }
        if (chain.tail_padding_samples > 0) {
            line += "；末帧不足一个编码块，静音补齐 " + std::to_string(chain.tail_padding_samples) +
                    " 采样（编码器块对齐要求，不是内容填补）";
        }
        if (chain.decoded_frames == 0)
            line += "；源侧没有解码出任何音频帧（这条输出流是空的，不能读成「音轨保住了」）";
        if (chain.dropped_packets > 0)
            line += "；" + std::to_string(chain.dropped_packets) + " 个源音频包解码失败已跳过";
        outcome.transcode_note += "；" + line;
    }

    // ---- 切片 W：音频合成填补逐轨落账 ----
    //
    // 顺序按输入流下标，报告里一眼能对上「轨道决策」。复制轨只有探测结论
    // （补不了），转码轨才有动作与计数 —— 两种都进同一份逐轨账，读者才不会
    // 因为「某轨没条目」而以为那条轨没有空洞。
    // 注意：这里**不能**再用 in->nb_streams —— 输入上下文已经关闭，读的是释放后的内存
    // （实测：紧接在这一行之后的字符串拷贝就 0xc0000005）。用关闭前固化的流数。
    for (unsigned i = 0; i < input_stream_count; ++i) {
        model::RepairSyntheticAudioFill fill;
        const auto chain_it = audio_chains.find(static_cast<int>(i));
        if (chain_it != audio_chains.end()) {
            const AudioTranscodeChain& chain = *chain_it->second;
            fill.inspected = true;
            fill.requested = request.fill_audio_gaps_with_silence;
            fill.coding_mode = "transcode";
            fill.input_stream_index = static_cast<int>(i);
            fill.codec = chain.source_codec;
            fill.basis = chain.silence_basis;
            fill.gaps = chain.silence_gaps;
            // 秒数由逐处 gap_seconds 求和，不另算一遍：两处各算一份迟早漂移。
            for (const auto& gap : fill.gaps) {
                if (gap.inserted_samples <= 0)
                    continue;
                fill.samples_inserted += gap.inserted_samples;
                fill.seconds_inserted += gap.gap_seconds;
            }
        } else {
            const auto copy_it = copied_audio_fills.find(static_cast<int>(i));
            if (copy_it == copied_audio_fills.end())
                continue;
            fill = copy_it->second;
            if (pending_overflow) {
                // 首帧探测阶段的缓存溢出会让相邻包之间出现一段**我们自己造成的**
                // 缺口：那段不能被写成「源里有空洞」。
                fill.basis += "；首帧探测阶段的包缓存溢出（见残余风险），前缀未完整回放，该段不做空洞判定";
            }
        }
        fill.gaps_detected = static_cast<int64_t>(fill.gaps.size());
        for (const auto& gap : fill.gaps) {
            if (gap.inserted_samples <= 0)
                ++fill.gaps_refused;
        }
        fill.summary = SilenceFillSummary(fill);
        fill.note = SilenceFillBasisNote(fill);
        outcome.synthetic_audio_tracks.push_back(std::move(fill));
    }

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
        const bool reencoded = audio_chains.find(static_cast<int>(i)) != audio_chains.end();
        audio_target.note = reencoded ? "S08 转码产物保留音频轨（原编码不被目标容器接受，已重编码）"
                                      : "S08 转码产物保留音频轨（码流复制，PTS 按统一原点平移）";
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
