#include "core/repair/RepairTrialDecode.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/repair/RepairBitstreamResync.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace videoeye {
namespace repair {
namespace {

constexpr std::uint32_t kMaxNalBytes = 8u * 1024u * 1024u;

std::string AvErrorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

// H.264 包内是否含 IDR（NAL type 5）。
// 只认 4 字节长度前缀；其它形态返回 supported=false，绝不按 4 字节硬套。
bool H264PacketHasIdr(const AVPacket* packet, bool* supported, std::string* error) {
    *supported = true;
    if (packet->size < 5) {
        *error = "包过小，容纳不下长度前缀与 NAL 头";
        return false;
    }
    const std::uint8_t* data = packet->data;
    const int size = packet->size;
    int pos = 0;
    bool saw_vcl = false;
    while (pos + 4 <= size) {
        const std::uint32_t nal_size =
            (static_cast<std::uint32_t>(data[pos]) << 24) | (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
            (static_cast<std::uint32_t>(data[pos + 2]) << 8) | static_cast<std::uint32_t>(data[pos + 3]);
        if (nal_size < 2 || nal_size > kMaxNalBytes || nal_size > static_cast<std::uint32_t>(size - pos - 4)) {
            *error = "NAL 长度前缀与包长不自洽（offset=" + std::to_string(pos) + "）";
            return false;
        }
        const std::uint8_t nal_type = static_cast<std::uint8_t>(data[pos + 4] & 0x1F);
        if (nal_type == 5)
            return true;
        if (nal_type == 1)
            saw_vcl = true;
        pos += 4 + static_cast<int>(nal_size);
    }
    *error = saw_vcl ? "包内只有非 IDR 分片（缺少随机访问点）" : "包内没有有效 NAL";
    return false;
}

} // namespace

RepairTrialDecodeResult TrialDecodeVideoAt(const std::string& path, double at_seconds,
                                           const RepairTrialDecodeOptions& options, const std::atomic<bool>* cancel,
                                           const RepairRunBudget* budget) {
    RepairTrialDecodeResult result;
    result.seek_seconds = at_seconds;
    // Checkpoint() 返回 true = 已取消 / 已超预算，此时不能开始任何解码。
    if (budget != nullptr && budget->Checkpoint()) {
        result.error = budget->Canceled() ? "本次修复已取消，未执行试解码" : "预算已用尽，未执行试解码";
        return result;
    }
    if (cancel != nullptr && cancel->load()) {
        result.error = "本次修复已取消，未执行试解码";
        return result;
    }

    AVFormatContext* fmt = nullptr;
    const int open_ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (open_ret < 0 || fmt == nullptr) {
        result.error = "试解码无法打开源文件: " + AvErrorText(open_ret);
        return result;
    }
    const int info_ret = avformat_find_stream_info(fmt, nullptr);
    if (info_ret < 0) {
        result.error = "试解码读取流信息失败: " + AvErrorText(info_ret);
        avformat_close_input(&fmt);
        return result;
    }

    int video_index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i] != nullptr && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_index = static_cast<int>(i);
            break;
        }
    }
    if (video_index < 0) {
        result.error = "没有视频轨，无法做试解码";
        avformat_close_input(&fmt);
        return result;
    }
    const AVStream* stream = fmt->streams[video_index];
    if (stream->codecpar->codec_id != AV_CODEC_ID_H264) {
        // 没试过就绝不能报"通过"。
        result.error = "第一版试解码只覆盖 H.264";
        avformat_close_input(&fmt);
        return result;
    }

    const int64_t target_us = static_cast<int64_t>(std::max(0.0, at_seconds) * AV_TIME_BASE);
    int seek_ret = AVERROR(ENOSYS);
    if (stream->time_base.num > 0 && stream->time_base.den > 0) {
        const int64_t target_ticks = av_rescale_q(target_us, AV_TIME_BASE_Q, stream->time_base);
        seek_ret = av_seek_frame(fmt, video_index, target_ticks, AVSEEK_FLAG_BACKWARD);
    }
    if (seek_ret < 0)
        seek_ret = av_seek_frame(fmt, -1, target_us, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0) {
        result.error = "试解码 seek 失败: " + AvErrorText(seek_ret);
        avformat_close_input(&fmt);
        return result;
    }
    avformat_flush(fmt);

    // 收同流视频包。首包必须是关键帧，否则说明恢复点落在 GOP 中间。
    std::vector<AVPacket*> packets;
    const int max_video = options.max_video_packets > 0 ? options.max_video_packets : 8;
    const int max_read = options.max_packets_read > 0 ? options.max_packets_read : 256;
    bool first_is_key = false;
    for (int i = 0; i < max_read && static_cast<int>(packets.size()) < max_video; ++i) {
        if (budget != nullptr && budget->Checkpoint())
            break;
        if (cancel != nullptr && cancel->load())
            break;
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr)
            break;
        const int read_ret = av_read_frame(fmt, packet);
        if (read_ret < 0) {
            av_packet_free(&packet);
            break;
        }
        if (packet->stream_index != video_index) {
            av_packet_free(&packet);
            continue;
        }
        if (packets.empty()) {
            first_is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            const double seconds = static_cast<double>(packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts) *
                                   stream->time_base.num / stream->time_base.den;
            result.first_packet_seconds = seconds;
            if (std::isfinite(seconds) && std::abs(seconds - at_seconds) > options.tolerance_seconds) {
                av_packet_free(&packet);
                result.error = "恢复后的首包时间 " + std::to_string(seconds) + " s 与目标 " +
                               std::to_string(at_seconds) + " s 偏差过大";
                break;
            }
        }
        packets.push_back(packet);
    }
    if (packets.empty()) {
        if (result.error.empty())
            result.error = "试解码未取到任何视频包";
        avformat_close_input(&fmt);
        return result;
    }
    result.attempted = true;
    result.video_packets = static_cast<int>(packets.size());
    if (!first_is_key) {
        result.error = "恢复后的首包不是关键帧（容器标记）";
        for (AVPacket* p : packets)
            av_packet_free(&p);
        avformat_close_input(&fmt);
        return result;
    }

    // 首包必须真的是 IDR：容器标记可以错，NAL 类型不会。
    bool supported = false;
    std::string parse_error;
    const bool has_idr = H264PacketHasIdr(packets.front(), &supported, &parse_error);
    if (!supported) {
        result.error = parse_error;
        for (AVPacket* p : packets)
            av_packet_free(&p);
        avformat_close_input(&fmt);
        return result;
    }
    if (!has_idr) {
        result.error = "首包不可解码：" + parse_error;
        for (AVPacket* p : packets)
            av_packet_free(&p);
        avformat_close_input(&fmt);
        return result;
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        result.attempted = false;
        result.error = "找不到 H.264 解码器";
        for (AVPacket* p : packets)
            av_packet_free(&p);
        avformat_close_input(&fmt);
        return result;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    if (ctx == nullptr || frame == nullptr) {
        result.attempted = false;
        result.error = "分配解码上下文 / 帧失败";
        if (ctx != nullptr)
            avcodec_free_context(&ctx);
        if (frame != nullptr)
            av_frame_free(&frame);
        for (AVPacket* p : packets)
            av_packet_free(&p);
        avformat_close_input(&fmt);
        return result;
    }
    int rc = avcodec_parameters_to_context(ctx, stream->codecpar);
    if (rc >= 0)
        rc = avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) {
        result.attempted = false;
        result.error = "解码器初始化失败: " + AvErrorText(rc);
        avcodec_free_context(&ctx);
        av_frame_free(&frame);
        for (AVPacket* p : packets)
            av_packet_free(&p);
        avformat_close_input(&fmt);
        return result;
    }
    ctx->thread_count = 1; // 短窗口验证不需要多线程，避免线程池开销与不确定性

    for (AVPacket* packet : packets) {
        rc = avcodec_send_packet(ctx, packet);
        if (rc < 0 && rc != AVERROR(EAGAIN))
            break;
        for (;;) {
            const int recv = avcodec_receive_frame(ctx, frame);
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF)
                break;
            if (recv < 0) {
                if (result.error.empty())
                    result.error = "解码报错: " + AvErrorText(recv);
                break;
            }
            ++result.frames_decoded;
            if ((frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 || frame->decode_error_flags != 0)
                ++result.corrupt_frames;
            av_frame_unref(frame);
        }
    }
    // 冲刷尾部：B 帧重排时最后几帧要 flush 才出来。
    avcodec_send_packet(ctx, nullptr);
    for (;;) {
        const int recv = avcodec_receive_frame(ctx, frame);
        if (recv < 0)
            break;
        ++result.frames_decoded;
        if ((frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 || frame->decode_error_flags != 0)
            ++result.corrupt_frames;
        av_frame_unref(frame);
    }

    if (result.frames_decoded <= 0 && result.error.empty())
        result.error = "解码器未输出任何帧";
    else if (result.corrupt_frames > 0)
        result.error = "解出 " + std::to_string(result.frames_decoded) + " 帧，其中 " +
                       std::to_string(result.corrupt_frames) + " 帧带损坏标记";
    result.ok = result.frames_decoded > 0 && result.corrupt_frames == 0;
    if (result.ok) {
        result.evidence = "从 " + std::to_string(result.first_packet_seconds) + " s 起试解码 " +
                          std::to_string(result.video_packets) + " 个包，解出 " +
                          std::to_string(result.frames_decoded) + " 帧且无损坏标记";
    }

    avcodec_free_context(&ctx);
    av_frame_free(&frame);
    for (AVPacket* p : packets)
        av_packet_free(&p);
    avformat_close_input(&fmt);
    return result;
}

} // namespace repair
} // namespace videoeye
