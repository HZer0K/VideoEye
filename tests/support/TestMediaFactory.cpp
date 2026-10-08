#include "TestMediaFactory.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace videoeye_test {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string JoinPath(const std::string& directory, const std::string& file_name) {
    return (std::filesystem::path(directory) / file_name).string();
}

// 画面: 亮度是 (x,y,帧号) 的确定性线性组合, 色度随帧号缓慢漂移。
// 不做"纯色 + 全黑": 那样 H.264 会整体压成 skip 块, 导出的帧数/时长断言就变得
// 毫无区分度（全黑 1 帧和全黑 40 帧长得一样）。
void FillVideoFrame(AVFrame* frame, int index) {
    for (int y = 0; y < frame->height; ++y) {
        uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; ++x) {
            row[x] = static_cast<uint8_t>((x * 2 + y * 3 + index * 5) & 0xFF);
        }
    }
    const int cx = frame->width / 2;
    const int cy = frame->height / 2;
    for (int y = 0; y < cy; ++y) {
        uint8_t* u = frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1];
        uint8_t* v = frame->data[2] + static_cast<ptrdiff_t>(y) * frame->linesize[2];
        for (int x = 0; x < cx; ++x) {
            u[x] = static_cast<uint8_t>(128 + ((x + index) & 31));
            v[x] = static_cast<uint8_t>(128 + ((y + index) & 31));
        }
    }
}

// 音频: 每声道一个固定频率的正弦（左 440Hz / 右 550Hz ...），幅度 0.25。
// 用正弦而不是随机噪声: 噪声在 AAC 下码率飙升，且解码端没有可校验的确定性。
void FillAudioFrame(AVFrame* frame, int channels, int sample_rate, int64_t first_sample) {
    for (int ch = 0; ch < channels && ch < frame->ch_layout.nb_channels; ++ch) {
        float* dst = reinterpret_cast<float*>(frame->data[ch]);
        if (!dst) continue;
        const double freq = 440.0 + 110.0 * ch;
        for (int i = 0; i < frame->nb_samples; ++i) {
            const double t = static_cast<double>(first_sample + i) / static_cast<double>(sample_rate);
            dst[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * freq * t));
        }
    }
}

AVFrame* AllocVideoFrame(int width, int height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

AVFrame* AllocAudioFrame(const AVCodecContext* enc, int nb_samples) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = enc->sample_fmt;
    frame->sample_rate = enc->sample_rate;
    if (av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    frame->nb_samples = nb_samples;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

}  // namespace

TestMedia CreateTestMedia(const std::string& directory, const std::string& name,
                          const TestMediaSpec& spec) {
    TestMedia media;
    media.path = JoinPath(directory, name + ".mp4");
    std::error_code ec;
    std::filesystem::remove(media.path, ec);

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, nullptr, media.path.c_str()) < 0 || !fmt) {
        media.error = "无法按扩展名创建输出上下文 (.mp4)";
        return media;
    }

    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;

    auto finish = [&](bool ok, const std::string& error) {
        if (fmt) {
            if (fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr;
        }
        if (aenc) avcodec_free_context(&aenc);
        if (venc) avcodec_free_context(&venc);
        media.ok = ok;
        media.error = error;
        if (ok) {
            media.width = spec.width;
            media.height = spec.height;
            media.fps = spec.fps;
            media.frame_count = spec.frame_count;
            media.sample_rate = spec.with_audio ? spec.sample_rate : 0;
            media.channels = spec.with_audio ? spec.channels : 0;
            media.duration_seconds = static_cast<double>(spec.frame_count) / spec.fps;
        }
        return media;
    };

    // ---- 视频编码器 ----
    // libx264 是 GPL 组件；LGPL 构建里没有它，逐级降级到 OpenH264 / MPEG-4。
    const char* video_candidates[] = {"libx264", "libopenh264", "mpeg4"};
    const AVCodec* vcodec = nullptr;
    for (const char* candidate : video_candidates) {
        vcodec = avcodec_find_encoder_by_name(candidate);
        if (vcodec) break;
    }
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vcodec) return finish(false, "该 FFmpeg 构建没有任何可用的视频编码器");

    venc = avcodec_alloc_context3(vcodec);
    if (!venc) return finish(false, "无法分配视频编码器上下文");
    venc->width = spec.width;
    venc->height = spec.height;
    venc->pix_fmt = AV_PIX_FMT_YUV420P;
    venc->time_base = AVRational{1, spec.fps};
    venc->framerate = AVRational{spec.fps, 1};
    venc->gop_size = spec.fps;      // 1 秒一个关键帧
    venc->max_b_frames = 0;         // 关掉 B 帧: 输出 PTS 与输入一致, 帧数断言才可预测
    venc->bit_rate = 400000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // 快速档 + zerolatency: 去掉 lookahead 与帧重排，生成过程既快又不受线程数影响。
    // (preset 是 libx264 私有选项, 换到其它编码器时 av_opt_set 返回错误, 忽略即可。)
    if (venc->priv_data) {
        av_opt_set(venc->priv_data, "preset", "veryfast", 0);
        av_opt_set(venc->priv_data, "tune", "zerolatency", 0);
    }
    if (avcodec_open2(venc, vcodec, nullptr) < 0) return finish(false, "打开视频编码器失败");

    vst = avformat_new_stream(fmt, nullptr);
    if (!vst) return finish(false, "无法创建视频流");
    if (avcodec_parameters_from_context(vst->codecpar, venc) < 0)
        return finish(false, "写入视频流参数失败");
    vst->time_base = venc->time_base;
    media.video_codec = vcodec->name;

    // ---- 音频编码器 ----
    if (spec.with_audio) {
        const AVCodec* acodec = avcodec_find_encoder_by_name("aac");
        if (!acodec) acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!acodec) return finish(false, "该 FFmpeg 构建没有可用的 AAC 编码器");
        aenc = avcodec_alloc_context3(acodec);
        if (!aenc) return finish(false, "无法分配音频编码器上下文");
        aenc->sample_rate = spec.sample_rate;
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;  // 内置 AAC 只吃 FLTP
        av_channel_layout_default(&aenc->ch_layout, spec.channels);
        aenc->bit_rate = 64000;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aenc, acodec, nullptr) < 0) return finish(false, "打开音频编码器失败");

        ast = avformat_new_stream(fmt, nullptr);
        if (!ast) return finish(false, "无法创建音频流");
        if (avcodec_parameters_from_context(ast->codecpar, aenc) < 0)
            return finish(false, "写入音频流参数失败");
        ast->time_base = AVRational{1, spec.sample_rate};
        media.audio_codec = acodec->name;
    }

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, media.path.c_str(), AVIO_FLAG_WRITE) < 0)
            return finish(false, "无法创建输出文件");
    }
    if (avformat_write_header(fmt, nullptr) < 0) return finish(false, "写入文件头失败");

    // 取出编码器产生的包并交给 muxer（av_interleaved_write_frame 自己负责排序）
    auto drain = [&](AVCodecContext* enc, AVStream* stream) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return;
        for (;;) {
            const int ret = avcodec_receive_packet(enc, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) break;
            pkt->stream_index = stream->index;
            av_packet_rescale_ts(pkt, enc->time_base, stream->time_base);
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    };

    const int audio_frame_size = aenc ? aenc->frame_size : 0;
    // 音频总采样数向上取整到整帧: AAC 只接受整帧, 尾部补的那一小段由编码器自己吞掉。
    int64_t total_audio_samples = 0;
    if (aenc && audio_frame_size > 0) {
        const double seconds = static_cast<double>(spec.frame_count) / spec.fps;
        const int64_t want = static_cast<int64_t>(std::ceil(seconds * spec.sample_rate));
        total_audio_samples = ((want + audio_frame_size - 1) / audio_frame_size) * audio_frame_size;
    }
    int64_t audio_done = 0;

    auto push_audio_until = [&](int64_t want_samples) {
        while (aenc && audio_done < want_samples && audio_done < total_audio_samples) {
            AVFrame* frame = AllocAudioFrame(aenc, audio_frame_size);
            if (!frame) return false;
            FillAudioFrame(frame, spec.channels, spec.sample_rate, audio_done);
            frame->pts = audio_done;
            avcodec_send_frame(aenc, frame);
            av_frame_free(&frame);
            audio_done += audio_frame_size;
            drain(aenc, ast);
        }
        return true;
    };

    for (int i = 0; i < spec.frame_count; ++i) {
        AVFrame* frame = AllocVideoFrame(spec.width, spec.height);
        if (!frame) { finish(false, "无法分配视频帧"); return media; }
        FillVideoFrame(frame, i);
        frame->pts = i;
        avcodec_send_frame(venc, frame);
        av_frame_free(&frame);
        drain(venc, vst);

        // 音频补到"不落后于当前画面时间", 让 muxer 拿到基本有序的输入
        const double video_time = static_cast<double>(i) / spec.fps;
        const int64_t want = static_cast<int64_t>(video_time * spec.sample_rate);
        if (!push_audio_until(want)) { finish(false, "无法分配音频帧"); return media; }
    }
    if (!push_audio_until(total_audio_samples)) { finish(false, "无法分配音频帧"); return media; }

    // 排空两个编码器
    avcodec_send_frame(venc, nullptr);
    drain(venc, vst);
    if (aenc) {
        avcodec_send_frame(aenc, nullptr);
        drain(aenc, ast);
    }
    if (av_write_trailer(fmt) < 0) return finish(false, "写入文件尾失败");

    return finish(true, std::string());
}

MediaProbe ProbeMediaFile(const std::string& path) {
    MediaProbe probe;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt) {
        probe.error = "FFmpeg 无法重新打开产物: " + path;
        return probe;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        probe.error = "FFmpeg 无法探测产物流信息";
        avformat_close_input(&fmt);
        return probe;
    }
    probe.opened = true;
    if (fmt->duration != AV_NOPTS_VALUE)
        probe.duration_seconds = static_cast<double>(fmt->duration) / 1e6;

    int video_index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (!stream || !stream->codecpar) continue;
        const AVMediaType type = stream->codecpar->codec_type;
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (type == AVMEDIA_TYPE_VIDEO) {
            ++probe.video_streams;
            if (video_index < 0) {
                video_index = static_cast<int>(i);
                probe.width = stream->codecpar->width;
                probe.height = stream->codecpar->height;
                probe.video_codec = codec ? codec->name : std::string();
                if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den)
                    probe.video_duration_seconds =
                        static_cast<double>(stream->duration) * av_q2d(stream->time_base);
                if (stream->start_time != AV_NOPTS_VALUE && stream->time_base.den)
                    probe.video_start_seconds =
                        static_cast<double>(stream->start_time) * av_q2d(stream->time_base);
                probe.video_nb_frames = stream->nb_frames;
            }
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            ++probe.audio_streams;
            if (probe.audio_codec.empty()) probe.audio_codec = codec ? codec->name : std::string();
        }
    }

    // 完整解码一遍视频流。只用容器层的 nb_frames / duration 是不够的 —— 那两个都是
    // 元数据，写坏了照样读得出来；真解码数出来的帧数才是"产物能不能播"的证据。
    if (video_index >= 0) {
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[video_index]->codecpar->codec_id);
        AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (dec && avcodec_parameters_to_context(dec, fmt->streams[video_index]->codecpar) >= 0 &&
            avcodec_open2(dec, codec, nullptr) >= 0) {
            AVPacket* pkt = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            for (;;) {
                const int ret = av_read_frame(fmt, pkt);
                if (ret < 0) break;
                if (pkt->stream_index != video_index) { av_packet_unref(pkt); continue; }
                ++probe.video_packet_count;
                if (avcodec_send_packet(dec, pkt) == 0) {
                    for (;;) {
                        const int rr = avcodec_receive_frame(dec, frame);
                        if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF || rr < 0) break;
                        ++probe.frame_count;
                        av_frame_unref(frame);
                    }
                }
                av_packet_unref(pkt);
            }
            // 排空解码器里的延迟帧
            avcodec_send_packet(dec, nullptr);
            for (;;) {
                const int rr = avcodec_receive_frame(dec, frame);
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF || rr < 0) break;
                ++probe.frame_count;
                av_frame_unref(frame);
            }
            if (frame) av_frame_free(&frame);
            if (pkt) av_packet_free(&pkt);
        }
        if (dec) avcodec_free_context(&dec);
    }

    avformat_close_input(&fmt);
    return probe;
}

std::string CreateCorruptMediaFile(const std::string& directory, const std::string& name) {
    const std::string path = JoinPath(directory, name + ".mp4");
    std::vector<char> junk(2048);
    for (std::size_t i = 0; i < junk.size(); ++i)
        junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out.is_open()) out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    return path;
}

}  // namespace videoeye_test
