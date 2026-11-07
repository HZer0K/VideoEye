#include "core/repair/RepairTolerantTranscode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <vector>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
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
#include <libswscale/swscale.h>
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
        progress(percent, stage::Executing(), detail);
}

std::string FormatDb(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << value << " dB";
    return out.str();
}

// 预算 / 取消统一出口：区分"用户取消"与"超出时间预算"。
bool StopRequested(const std::atomic<bool>* cancel, const RepairRunBudget* budget, bool* canceled, bool* timed_out) {
    if (budget != nullptr) {
        if (!budget->Checkpoint())
            return false;
        *canceled = budget->Canceled();
        *timed_out = !*canceled;
        return true;
    }
    if (infrastructure::Checkpoint(cancel)) {
        *canceled = true;
        return true;
    }
    return false;
}

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

    bool Open(const std::string& path, std::string* error) {
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0 || fmt_ == nullptr) {
            *error = "无法打开源文件: " + path;
            return false;
        }
        if (avformat_find_stream_info(fmt_, nullptr) < 0) {
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
    AVFrame* Next(uint64_t* skipped_packets) {
        if (eof_)
            return nullptr;
        av_frame_unref(frame_);
        for (;;) {
            const int ret = avcodec_receive_frame(decoder_, frame_);
            if (ret == 0) {
                ++decoded_frames_;
                return frame_;
            }
            if (ret == AVERROR_EOF) {
                eof_ = true;
                return nullptr;
            }
            if (ret != AVERROR(EAGAIN)) {
                // 帧级解码错误：跳过并继续尝试下一包，不能把整个输入当成 EOF。
                if (!SendNext(skipped_packets)) {
                    eof_ = true;
                    return nullptr;
                }
                continue;
            }
            if (!SendNext(skipped_packets)) {
                // 输入结束：冲刷解码器。
                avcodec_send_packet(decoder_, nullptr);
                eof_ = true;
                while (avcodec_receive_frame(decoder_, frame_) == 0) {
                    ++decoded_frames_;
                    return frame_;
                }
                return nullptr;
            }
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
    bool SendNext(uint64_t* skipped_packets) {
        for (;;) {
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
    int stream_index_ = -1;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    uint64_t decoded_frames_ = 0;
    bool eof_ = false;
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
};

PsnrResult MeasurePsnr(const std::string& input_path, const std::string& output_path, const std::atomic<bool>* cancel) {
    PsnrResult result;
    FrameReader input;
    FrameReader output;
    std::string error;
    if (!input.Open(input_path, &error) || !output.Open(output_path, &error)) {
        result.note = error;
        return result;
    }

    uint64_t pair_index = 0;
    uint64_t skipped_input = 0;
    uint64_t skipped_output = 0;
    double sum = 0.0;
    uint64_t measured = 0;

    for (;;) {
        if (infrastructure::Checkpoint(cancel))
            break;
        AVFrame* in_frame = input.Next(&skipped_input);
        AVFrame* out_frame = output.Next(&skipped_output);
        if (in_frame == nullptr || out_frame == nullptr)
            break;
        const bool sample = (pair_index < 16) || (pair_index % 100 == 0);
        if (sample && measured < 32) {
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
    note << "（亮度平面，非全量逐帧比对）";
    result.note = note.str();
    return result;
}

} // namespace

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

    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, request.source_path.c_str(), nullptr, nullptr) < 0 || in == nullptr) {
        outcome.error = "无法打开源文件（转码前读取失败）";
        return outcome;
    }
    if (avformat_find_stream_info(in, nullptr) < 0) {
        avformat_close_input(&in);
        outcome.error = "无法读取源文件媒体信息";
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

    const AVCodec* encoder_codec = avcodec_find_encoder_by_name("libx264");
    if (encoder_codec == nullptr)
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
    for (;;) {
        const int read_ret = av_read_frame(in, packet);
        if (read_ret < 0) {
            // 冲刷解码器取剩余帧。
            avcodec_send_packet(decoder, nullptr);
            if (avcodec_receive_frame(decoder, frame) == 0)
                first_frame = frame;
            break;
        }
        if (packet->stream_index != video_index) {
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

    if (first_frame == nullptr || first_frame->width <= 0 || first_frame->height <= 0) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_free_context(out);
        avcodec_free_context(&decoder);
        avformat_close_input(&in);
        outcome.error = "源视频没有可解码帧（编码层已损坏，无法转码）";
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
    encoder->time_base = AVRational{1, fps.num > 0 ? fps.num : 25};
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
    sws = sws_getContext(width, height, static_cast<AVPixelFormat>(first_frame->format), width, height, encoder_format,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);

    // 写一个编码包。
    uint64_t encoded_frames = 0;
    uint64_t video_packets = 0;
    uint64_t audio_packets = 0;
    uint64_t bytes_written = 0;
    bool write_failed = false;
    std::string write_error;
    const double fps_value = fps.num > 0 && fps.den > 0 ? static_cast<double>(fps.num) / fps.den : 25.0;

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
            const int written = encoded->size;
            if (av_interleaved_write_frame(out, encoded) < 0) {
                write_failed = true;
                write_error = "写入编码包失败";
                return false;
            }
            ++video_packets;
            bytes_written += static_cast<uint64_t>(std::max(written, 0));
        }
    };

    auto encode_frame = [&](AVFrame* source) -> bool {
        if (sws == nullptr) {
            ++outcome.dropped_video_frames;
            return true;
        }
        if (av_frame_get_buffer(converted, 0) < 0) {
            ++outcome.dropped_video_frames;
            return true;
        }
        sws_scale(sws, (const uint8_t* const*)source->data, source->linesize, 0, height, converted->data,
                  converted->linesize);
        converted->pts = static_cast<int64_t>(encoded_frames);
        if (avcodec_send_frame(encoder, converted) < 0) {
            ++outcome.dropped_video_frames;
        } else {
            ++encoded_frames;
        }
        av_frame_unref(converted);
        return drain_encoder();
    };

    // ---- 解码 → 转码 → 封装 ----
    bool canceled = false;
    bool timed_out = false;
    if (encode_frame(first_frame) && !write_failed) {
        for (;;) {
            if (StopRequested(cancel, budget, &canceled, &timed_out))
                break;
            const int read_ret = av_read_frame(in, packet);
            if (read_ret < 0) {
                if (read_ret != AVERROR_EOF)
                    outcome.read_error = true;
                avcodec_send_packet(decoder, nullptr);
                while (!write_failed && avcodec_receive_frame(decoder, frame) == 0) {
                    if (!encode_frame(frame))
                        break;
                    av_frame_unref(frame);
                }
                break;
            }
            if (packet->stream_index == video_index) {
                if (avcodec_send_packet(decoder, packet) < 0) {
                    ++dropped_decode_packets;
                } else {
                    while (!write_failed && avcodec_receive_frame(decoder, frame) == 0) {
                        if (!encode_frame(frame))
                            break;
                        av_frame_unref(frame);
                    }
                }
            } else {
                const int out_index =
                    packet->stream_index >= 0 && packet->stream_index < static_cast<int>(audio_map.size())
                        ? audio_map[static_cast<std::size_t>(packet->stream_index)]
                        : -1;
                if (out_index >= 0) {
                    AVStream* dst = out->streams[out_index];
                    av_packet_rescale_ts(packet, in->streams[packet->stream_index]->time_base, dst->time_base);
                    packet->stream_index = out_index;
                    const int written = packet->size;
                    if (av_interleaved_write_frame(out, packet) < 0) {
                        write_failed = true;
                        write_error = "写入音频包失败";
                    } else {
                        ++audio_packets;
                        bytes_written += static_cast<uint64_t>(std::max(written, 0));
                    }
                }
            }
            av_packet_unref(packet);
            if (write_failed)
                break;
            if ((video_packets + audio_packets) % 200 == 0)
                Emit(progress, 80, "已转码 " + std::to_string(encoded_frames) + " 帧");
        }
    }

    if (!write_failed) {
        avcodec_send_frame(encoder, nullptr);
        if (!drain_encoder())
            write_failed = true;
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
    if (av_write_trailer(out) < 0 && !write_failed)
        write_failed = true;
    avio_closep(&out->pb);
    avformat_free_context(out);
    avcodec_free_context(&encoder);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&in);

    outcome.canceled = canceled;
    outcome.timed_out = timed_out;
    outcome.packets_read = encoded_frames + dropped_decode_packets;
    outcome.packets_written = video_packets + audio_packets;
    outcome.bytes_written = bytes_written;
    outcome.transcoded_frames = encoded_frames;
    outcome.dropped_video_frames = dropped_decode_packets + outcome.dropped_video_frames;
    outcome.output_end_seconds = fps_value > 0.0 ? static_cast<double>(encoded_frames) / fps_value : 0.0;

    if (write_failed && outcome.packets_written == 0) {
        outcome.error = write_error.empty() ? "转码写入失败" : write_error;
        return outcome;
    }
    if (canceled || timed_out) {
        outcome.ok = outcome.packets_written > 0;
        outcome.error = canceled ? "转码阶段被取消" : "转码阶段超出预算";
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
    const PsnrResult psnr = MeasurePsnr(request.source_path, paths.part_path, cancel);
    outcome.psnr_samples = psnr.samples;
    outcome.psnr_average_db = psnr.average_db;
    outcome.transcode_note += "；" + psnr.note;

    model::RepairActionExecution record;
    record.action = model::RepairAction::S08_TolerantTranscode;
    record.attempted = true;
    record.succeeded = true;
    record.packets_read = outcome.packets_read;
    record.packets_written = outcome.packets_written;
    record.bytes_written = outcome.bytes_written;
    record.output_start_seconds = 0.0;
    record.output_end_seconds = outcome.output_end_seconds;
    std::ostringstream detail;
    detail << "容忍式转码：解码并编码 " << encoded_frames << " 帧（" << std::string(encoder_codec->name) << "），跳过 "
           << outcome.dropped_video_frames << " 个解码失败包/帧；音频包 " << audio_packets << " 个";
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
        audio_target.required = false;
        outcome.verification_targets.push_back(std::move(audio_target));
    }

    {
        model::RepairTrackConservation video_record;
        video_record.track_id = video_track_id;
        video_record.input_stream_index = video_index;
        video_record.output_stream_index = video_out_index;
        video_record.kind = "video";
        video_record.packets_written = video_packets;
        video_record.status = "unknown";
        video_record.relation = "reencoded";
        video_record.note = "转码后样本数与源不可直接比对";
        outcome.track_conservation.push_back(std::move(video_record));
    }

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

    return outcome;
}

} // namespace repair
} // namespace videoeye
