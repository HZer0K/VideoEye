#include "TestMediaFactory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace videoeye_test {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string JoinPath(const std::string& directory, const std::string& file_name) {
    return (std::filesystem::path(directory) / file_name).string();
}

void WriteBe32Char(std::vector<char>& bytes, std::size_t offset, uint32_t value) {
    bytes[offset] = static_cast<char>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<char>(value & 0xFF);
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
        if (!dst)
            continue;
        const double freq = 440.0 + 110.0 * ch;
        for (int i = 0; i < frame->nb_samples; ++i) {
            const double t = static_cast<double>(first_sample + i) / static_cast<double>(sample_rate);
            dst[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * freq * t));
        }
    }
}

AVFrame* AllocVideoFrame(int width, int height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame)
        return nullptr;
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
    if (!frame)
        return nullptr;
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

} // namespace

TestMedia CreateTestMedia(const std::string& directory, const std::string& name, const TestMediaSpec& spec) {
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
            if (fmt->pb)
                avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr;
        }
        if (aenc)
            avcodec_free_context(&aenc);
        if (venc)
            avcodec_free_context(&venc);
        media.ok = ok;
        media.error = error;
        if (ok) {
            media.width = spec.width;
            media.height = spec.height;
            media.fps = spec.fps;
            media.fps_den = spec.fps_den;
            media.frame_count = spec.frame_count;
            media.sample_rate = spec.with_audio ? spec.sample_rate : 0;
            media.channels = spec.with_audio ? spec.channels : 0;
            media.duration_seconds = static_cast<double>(spec.frame_count) * spec.fps_den / spec.fps;
        }
        return media;
    };

    // ---- 视频编码器 ----
    // libx264 是 GPL 组件；LGPL 构建里没有它，逐级降级到 OpenH264 / MPEG-4。
    // 指定 video_codec 时不许静默降级 —— 需要 MPEG-4 错误隐藏的样本拿到 H.264
    // 会变成「根本没有帧标记」的假样本。
    const char* video_candidates[] = {"libx264", "libopenh264", "mpeg4"};
    const AVCodec* vcodec = nullptr;
    if (!spec.video_codec.empty()) {
        vcodec = avcodec_find_encoder_by_name(spec.video_codec.c_str());
        if (!vcodec)
            return finish(false, "指定的视频编码器不可用: " + spec.video_codec);
    } else {
        for (const char* candidate : video_candidates) {
            vcodec = avcodec_find_encoder_by_name(candidate);
            if (vcodec)
                break;
        }
    }
    if (!vcodec)
        vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec)
        vcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vcodec)
        return finish(false, "该 FFmpeg 构建没有任何可用的视频编码器");

    venc = avcodec_alloc_context3(vcodec);
    if (!venc)
        return finish(false, "无法分配视频编码器上下文");
    venc->width = spec.width;
    venc->height = spec.height;
    venc->pix_fmt = AV_PIX_FMT_YUV420P;
    venc->time_base = AVRational{spec.fps_den, spec.fps};
    venc->framerate = AVRational{spec.fps, spec.fps_den};
    venc->gop_size = std::max(1, spec.fps / std::max(1, spec.fps_den)); // 约 1 秒一个关键帧
    venc->max_b_frames = spec.max_b_frames; // 0 = 关掉 B 帧: 输出 PTS 与输入一致, 帧数断言才可预测
    venc->bit_rate = 400000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // 快速档 + zerolatency: 去掉 lookahead 与帧重排，生成过程既快又不受线程数影响。
    //
    // ❗ zerolatency 会把 bframes 强制成 0（它就是为了不重排），所以只要调用方要 B 帧
    // 就必须跳过它 —— 否则 "spec.max_b_frames=3" 会静默失效，产出的样本一个 B 帧都没有，
    // 依赖"真实重排"的测试（POC 重建的正例）会得到一个假绿的反例样本。
    if (venc->priv_data) {
        av_opt_set(venc->priv_data, "preset", "veryfast", 0);
        if (spec.max_b_frames <= 0)
            av_opt_set(venc->priv_data, "tune", "zerolatency", 0);
        if (spec.repeat_parameter_sets)
            av_opt_set(venc->priv_data, "x264-params", "repeat-headers=1", 0);
    }
    if (avcodec_open2(venc, vcodec, nullptr) < 0)
        return finish(false, "打开视频编码器失败");

    vst = avformat_new_stream(fmt, nullptr);
    if (!vst)
        return finish(false, "无法创建视频流");
    if (avcodec_parameters_from_context(vst->codecpar, venc) < 0)
        return finish(false, "写入视频流参数失败");
    vst->time_base = venc->time_base;
    media.video_codec = vcodec->name;

    // ---- 音频编码器 ----
    if (spec.with_audio) {
        const AVCodec* acodec = avcodec_find_encoder_by_name("aac");
        if (!acodec)
            acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!acodec)
            return finish(false, "该 FFmpeg 构建没有可用的 AAC 编码器");
        aenc = avcodec_alloc_context3(acodec);
        if (!aenc)
            return finish(false, "无法分配音频编码器上下文");
        aenc->sample_rate = spec.sample_rate;
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP; // 内置 AAC 只吃 FLTP
        av_channel_layout_default(&aenc->ch_layout, spec.channels);
        aenc->bit_rate = 64000;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
            aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aenc, acodec, nullptr) < 0)
            return finish(false, "打开音频编码器失败");

        ast = avformat_new_stream(fmt, nullptr);
        if (!ast)
            return finish(false, "无法创建音频流");
        if (avcodec_parameters_from_context(ast->codecpar, aenc) < 0)
            return finish(false, "写入音频流参数失败");
        ast->time_base = AVRational{1, spec.sample_rate};
        media.audio_codec = acodec->name;
    }

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, media.path.c_str(), AVIO_FLAG_WRITE) < 0)
            return finish(false, "无法创建输出文件");
    }
    if (avformat_write_header(fmt, nullptr) < 0)
        return finish(false, "写入文件头失败");

    // 取出编码器产生的包并交给 muxer（av_interleaved_write_frame 自己负责排序）
    auto drain = [&](AVCodecContext* enc, AVStream* stream) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt)
            return;
        for (;;) {
            const int ret = avcodec_receive_packet(enc, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0)
                break;
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
        const double seconds = static_cast<double>(spec.frame_count) * spec.fps_den / spec.fps;
        const int64_t want = static_cast<int64_t>(std::ceil(seconds * spec.sample_rate));
        total_audio_samples = ((want + audio_frame_size - 1) / audio_frame_size) * audio_frame_size;
    }
    int64_t audio_done = 0;

    auto push_audio_until = [&](int64_t want_samples) {
        while (aenc && audio_done < want_samples && audio_done < total_audio_samples) {
            AVFrame* frame = AllocAudioFrame(aenc, audio_frame_size);
            if (!frame)
                return false;
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
        if (!frame) {
            finish(false, "无法分配视频帧");
            return media;
        }
        FillVideoFrame(frame, i);
        frame->pts = i;
        avcodec_send_frame(venc, frame);
        av_frame_free(&frame);
        drain(venc, vst);

        // 音频补到"不落后于当前画面时间", 让 muxer 拿到基本有序的输入
        const double video_time = static_cast<double>(i) * spec.fps_den / spec.fps;
        const int64_t want = static_cast<int64_t>(video_time * spec.sample_rate);
        if (!push_audio_until(want)) {
            finish(false, "无法分配音频帧");
            return media;
        }
    }
    if (!push_audio_until(total_audio_samples)) {
        finish(false, "无法分配音频帧");
        return media;
    }

    // 排空两个编码器
    avcodec_send_frame(venc, nullptr);
    drain(venc, vst);
    if (aenc) {
        avcodec_send_frame(aenc, nullptr);
        drain(aenc, ast);
    }
    if (av_write_trailer(fmt) < 0)
        return finish(false, "写入文件尾失败");

    return finish(true, std::string());
}

FragmentedTestMedia CreateFragmentedTestMedia(const std::string& directory, const std::string& name,
                                              const TestMediaSpec& spec, bool global_sidx) {
    FragmentedTestMedia result;
    result.init_path = JoinPath(directory, name + "_init.mp4");
    result.segment_path = JoinPath(directory, name + "_seg.ts");

    const TestMedia media = CreateTestMedia(directory, name + "_full", spec);
    if (!media.ok) {
        result.error = "生成基础素材失败: " + media.error;
        return result;
    }

    const std::string fragmented_path = JoinPath(directory, name + "_frag.mp4");
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, media.path.c_str(), nullptr, nullptr) < 0 || !in) {
        result.error = "无法打开基础素材";
        return result;
    }
    if (avformat_find_stream_info(in, nullptr) < 0) {
        avformat_close_input(&in);
        result.error = "基础素材流信息探测失败";
        return result;
    }

    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "mp4", fragmented_path.c_str()) < 0 || !out) {
        avformat_close_input(&in);
        result.error = "无法创建分片输出上下文";
        return result;
    }
    auto cleanup = [&](bool ok, const std::string& error) {
        if (out) {
            if (out->pb)
                avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
        }
        if (in) {
            avformat_close_input(&in);
            in = nullptr;
        }
        result.ok = ok;
        if (!ok)
            result.error = error;
        return result;
    };

    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* dst = avformat_new_stream(out, nullptr);
        if (!dst || avcodec_parameters_copy(dst->codecpar, in->streams[i]->codecpar) < 0) {
            return cleanup(false, "复制流参数失败");
        }
        dst->time_base = in->streams[i]->time_base;
    }
    // empty_moov：moov 里不写样本表，样本全部由 moof/trun 描述；
    // default_base_moof：trun 的 data_offset 相对 moof 起点，截断前缀后仍然有效。
    // global_sidx：在首个 moof 之前写一份覆盖全部分片的 sidx，为"无参考自建初始化段"
    // 提供源侧 timescale 证据（自建器拒绝在缺少该证据时猜测时基）。
    std::string movflags = "empty_moov+default_base_moof+frag_keyframe";
    if (global_sidx)
        movflags += "+global_sidx";
    av_opt_set(out->priv_data, "movflags", movflags.c_str(), 0);

    // 把 avcC 里的 SPS/PPS 以 length-prefixed NAL 形式插到每个关键帧样本前面，
    // 让参数集真正出现在分片载荷里（部分编码器/构建只把参数集写进 extradata）。
    // 自建初始化段只能从 in-band 参数集取证，所以测试样本必须带这一层。
    int parameter_stream_index = -1;
    std::vector<uint8_t> parameter_sets;
    if (spec.repeat_parameter_sets) {
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            const AVCodecParameters* par = in->streams[i]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_VIDEO || par->codec_id != AV_CODEC_ID_H264)
                continue;
            const uint8_t* ed = par->extradata;
            const int ed_size = par->extradata_size;
            if (!ed || ed_size < 8 || ed[0] != 1)
                continue;
            // avcC: configurationVersion(1) profile(1) compat(1) level(1)
            //       lengthSizeMinusOne(1) numOfSPS(1) [sps_len(2) sps...] numOfPPS(1) [pps_len(2) pps...]
            const int length_size = (ed[4] & 0x03) + 1;
            if (length_size != 4)
                continue;
            const auto append = [&parameter_sets](const uint8_t* nal, int size) {
                if (size <= 0)
                    return;
                const uint32_t value = static_cast<uint32_t>(size);
                parameter_sets.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
                parameter_sets.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
                parameter_sets.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
                parameter_sets.push_back(static_cast<uint8_t>(value & 0xFF));
                parameter_sets.insert(parameter_sets.end(), nal, nal + size);
            };
            int pos = 5;
            const int sps_count = ed[pos++] & 0x1F;
            bool parsed = true;
            for (int n = 0; n < sps_count && parsed; ++n) {
                if (pos + 2 > ed_size) {
                    parsed = false;
                    break;
                }
                const int size = (static_cast<int>(ed[pos]) << 8) | ed[pos + 1];
                pos += 2;
                if (pos + size > ed_size) {
                    parsed = false;
                    break;
                }
                append(ed + pos, size);
                pos += size;
            }
            if (!parsed || pos >= ed_size) {
                parameter_sets.clear();
                continue;
            }
            const int pps_count = ed[pos++];
            for (int n = 0; n < pps_count && parsed; ++n) {
                if (pos + 2 > ed_size) {
                    parsed = false;
                    break;
                }
                const int size = (static_cast<int>(ed[pos]) << 8) | ed[pos + 1];
                pos += 2;
                if (pos + size > ed_size) {
                    parsed = false;
                    break;
                }
                append(ed + pos, size);
                pos += size;
            }
            if (!parsed) {
                parameter_sets.clear();
                continue;
            }
            parameter_stream_index = static_cast<int>(i);
            break;
        }
    }

    if (!(out->oformat->flags & AVFMT_NOFILE) && avio_open(&out->pb, fragmented_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        return cleanup(false, "无法创建分片输出文件");
    }
    if (avformat_write_header(out, nullptr) < 0)
        return cleanup(false, "写入分片文件头失败");

    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
        return cleanup(false, "无法分配包");
    uint64_t fragment_duration_ticks = 0;
    uint32_t fragment_timescale = 0;
    uint32_t fragment_track_id = 0;
    while (av_read_frame(in, pkt) >= 0) {
        AVStream* src_stream = in->streams[pkt->stream_index];
        AVStream* dst_stream = out->streams[pkt->stream_index];
        av_packet_rescale_ts(pkt, src_stream->time_base, dst_stream->time_base);
        pkt->pos = -1;
        if (dst_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && pkt->duration > 0) {
            fragment_duration_ticks += static_cast<uint64_t>(pkt->duration);
            fragment_timescale = static_cast<uint32_t>(dst_stream->time_base.den);
            fragment_track_id = static_cast<uint32_t>(dst_stream->id);
        }
        const bool inject = parameter_stream_index == pkt->stream_index && !parameter_sets.empty() &&
                            (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        AVPacket* rebuilt = nullptr;
        AVPacket* to_write = pkt;
        if (inject) {
            rebuilt = av_packet_alloc();
            if (!rebuilt || av_new_packet(rebuilt, pkt->size + static_cast<int>(parameter_sets.size())) < 0) {
                av_packet_free(&rebuilt);
                av_packet_free(&pkt);
                return cleanup(false, "无法构造带参数集的样本");
            }
            std::memcpy(rebuilt->data, parameter_sets.data(), parameter_sets.size());
            std::memcpy(rebuilt->data + parameter_sets.size(), pkt->data, static_cast<std::size_t>(pkt->size));
            rebuilt->pts = pkt->pts;
            rebuilt->dts = pkt->dts;
            rebuilt->duration = pkt->duration;
            rebuilt->stream_index = pkt->stream_index;
            rebuilt->flags = pkt->flags;
            to_write = rebuilt;
        }
        const int write_ret = av_interleaved_write_frame(out, to_write);
        av_packet_free(&rebuilt);
        av_packet_unref(pkt);
        if (write_ret < 0) {
            av_packet_free(&pkt);
            return cleanup(false, "写入分片失败");
        }
    }
    av_packet_free(&pkt);
    if (av_write_trailer(out) < 0)
        return cleanup(false, "写入分片文件尾失败");

    // ---- 切分：前半是初始化段，后半是媒体分片 ----
    // 切在第一个 moof。构建分片后追加的 mfra 属于分片文件尾部，不影响 trun
    // 的 moof 相对偏移，因此保留在媒体段里。
    std::ifstream fragmented(fragmented_path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(fragmented)), std::istreambuf_iterator<char>());
    if (bytes.size() < 16)
        return cleanup(false, "分片文件为空");
    std::size_t split = bytes.size();
    std::size_t pos = 0;
    while (pos + 8 <= bytes.size()) {
        const uint32_t box_size = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[pos])) << 24) |
                                  (static_cast<uint32_t>(static_cast<unsigned char>(bytes[pos + 1])) << 16) |
                                  (static_cast<uint32_t>(static_cast<unsigned char>(bytes[pos + 2])) << 8) |
                                  static_cast<uint32_t>(static_cast<unsigned char>(bytes[pos + 3]));
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(pos) + 4,
                               bytes.begin() + static_cast<std::ptrdiff_t>(pos) + 8);
        if (type == "moof") {
            split = pos;
            break;
        }
        if (box_size < 8)
            break;
        pos += box_size;
    }
    if (split == 0 || split >= bytes.size())
        return cleanup(false, "分片文件里找不到 moof");

    std::ofstream init_out(result.init_path, std::ios::binary | std::ios::trunc);
    init_out.write(bytes.data(), static_cast<std::streamsize>(split));

    std::vector<char> segment(bytes.begin() + static_cast<std::ptrdiff_t>(split), bytes.end());
    if (global_sidx) {
        // FFmpeg 的 global_sidx 在 stream copy 场景会写出 reference_count=0 的空表
        // （本构建实测如此），不能当时基证据。这里按实测的样本时长与 timescale
        // 自造一条覆盖全段的 sidx，让自建初始化段有可核对的源侧证据。
        if (fragment_timescale == 0 || fragment_duration_ticks == 0)
            return cleanup(false, "无法统计分片时基证据");
        const std::size_t sidx_size = 44;
        std::vector<char> sidx(sidx_size, '\0');
        WriteBe32Char(sidx, 0, static_cast<uint32_t>(sidx_size));
        std::memcpy(sidx.data() + 4, "sidx", 4);
        // version 0 / flags 0
        WriteBe32Char(sidx, 8, 0);
        WriteBe32Char(sidx, 12, fragment_track_id); // reference_ID
        WriteBe32Char(sidx, 16, fragment_timescale);
        WriteBe32Char(sidx, 20, 0); // earliest_presentation_time
        WriteBe32Char(sidx, 24, 0); // first_offset：首个 moof 紧跟 sidx
        // reserved(2) + reference_count(2)
        WriteBe32Char(sidx, 28, 1);
        // reference_type=0 + referenced_size，覆盖 sidx 之后的全部媒体段
        WriteBe32Char(sidx, 32, static_cast<uint32_t>(segment.size()));
        WriteBe32Char(sidx, 36, static_cast<uint32_t>(fragment_duration_ticks));
        // starts_with_SAP=1 + SAP_type=1 + SAP_delta_time=0
        WriteBe32Char(sidx, 40, 0x90000000u);
        segment.insert(segment.begin(), sidx.begin(), sidx.end());
    }

    std::ofstream segment_out(result.segment_path, std::ios::binary | std::ios::trunc);
    segment_out.write(segment.data(), static_cast<std::streamsize>(segment.size()));
    if (!init_out || !segment_out)
        return cleanup(false, "写出初始化段/分片失败");

    return cleanup(true, std::string());
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
    int audio_index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* stream = fmt->streams[i];
        if (!stream || !stream->codecpar)
            continue;
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
                    probe.video_duration_seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
                if (stream->start_time != AV_NOPTS_VALUE && stream->time_base.den)
                    probe.video_start_seconds = static_cast<double>(stream->start_time) * av_q2d(stream->time_base);
                probe.video_nb_frames = stream->nb_frames;
                // 方向信息：FFmpeg 8 挂在 codecpar->coded_side_data 上（mp4 的 tkhd matrix）。
                const AVPacketSideData* display = av_packet_side_data_get(
                    stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
                if (display != nullptr && display->size >= 9 * sizeof(int32_t)) {
                    int32_t matrix[9] = {};
                    std::memcpy(matrix, display->data, sizeof(matrix));
                    probe.has_display_matrix = true;
                    probe.video_rotation_degrees = av_display_rotation_get(matrix);
                }
                probe.has_hdr_metadata =
                    av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr ||
                    av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_CONTENT_LIGHT_LEVEL) != nullptr;
            }
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            ++probe.audio_streams;
            if (probe.audio_codec.empty())
                probe.audio_codec = codec ? codec->name : std::string();
            if (audio_index < 0) {
                audio_index = static_cast<int>(i);
                if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den)
                    probe.audio_duration_seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
                if (stream->start_time != AV_NOPTS_VALUE && stream->time_base.den)
                    probe.audio_start_seconds = static_cast<double>(stream->start_time) * av_q2d(stream->time_base);
            }
        }
    }

    // 完整解码一遍视频流。只用容器层的 nb_frames / duration 是不够的 —— 那两个都是
    // 元数据，写坏了照样读得出来；真解码数出来的帧数才是"产物能不能播"的证据。
    if (video_index >= 0) {
        // 帧级损坏标记与「解出帧」分开记：FFmpeg 8.x 上 AV_FRAME_FLAG_CORRUPT
        // 多数解码器恒 0，而 decode_error_flags 由错误隐藏填充。
        auto count_frame = [&probe](AVFrame* f) {
            ++probe.frame_count;
            if (f->decode_error_flags != 0)
                ++probe.marked_frames;
            if (f->flags & AV_FRAME_FLAG_CORRUPT)
                ++probe.corrupt_frames;
            av_frame_unref(f);
        };
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[video_index]->codecpar->codec_id);
        AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (dec && avcodec_parameters_to_context(dec, fmt->streams[video_index]->codecpar) >= 0 &&
            avcodec_open2(dec, codec, nullptr) >= 0) {
            AVPacket* pkt = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            for (;;) {
                const int ret = av_read_frame(fmt, pkt);
                if (ret < 0)
                    break;
                if (pkt->stream_index != video_index) {
                    if (audio_index >= 0 && pkt->stream_index == audio_index)
                        ++probe.audio_packet_count;
                    av_packet_unref(pkt);
                    continue;
                }
                ++probe.video_packet_count;
                if (avcodec_send_packet(dec, pkt) == 0) {
                    for (;;) {
                        const int rr = avcodec_receive_frame(dec, frame);
                        if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF || rr < 0)
                            break;
                        count_frame(frame);
                    }
                }
                av_packet_unref(pkt);
            }
            // 排空解码器里的延迟帧
            avcodec_send_packet(dec, nullptr);
            for (;;) {
                const int rr = avcodec_receive_frame(dec, frame);
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF || rr < 0)
                    break;
                count_frame(frame);
            }
            if (frame)
                av_frame_free(&frame);
            if (pkt)
                av_packet_free(&pkt);
        }
        if (dec)
            avcodec_free_context(&dec);
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
    if (out.is_open())
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    return path;
}

ConcealmentMedia CreateConcealmentMedia(const std::string& directory, const std::string& name) {
    ConcealmentMedia result;
    TestMediaSpec spec;
    spec.with_audio = false;
    spec.video_codec = "mpeg4"; // MPEG-4 Part 2 的错误隐藏会吐出带 decode_error_flags 的帧
    spec.frame_count = 40;
    const TestMedia media = CreateTestMedia(directory, name, spec);
    if (!media.ok) {
        result.error = "生成 MPEG-4 基础素材失败: " + media.error;
        return result;
    }
    result.path = media.path;

    std::ifstream in(media.path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t type = bytes.find("mdat");
    if (type == std::string::npos || type < 4 || bytes.size() < 512) {
        result.error = "找不到 mdat 或样本过小，无法做载荷损坏";
        return result;
    }
    const auto read_be32 = [&bytes](std::size_t at) -> uint32_t {
        return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
               (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
               static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
    };
    const uint32_t box_size = read_be32(type - 4);
    const std::size_t payload_begin = type + 4;
    const std::size_t payload_end =
        box_size >= 8 ? std::min<std::size_t>(payload_begin + box_size - 8, bytes.size()) : bytes.size();
    if (payload_end <= payload_begin + 32) {
        result.error = "mdat 载荷过短";
        return result;
    }

    // 稀疏位翻转：容器 / stsz / stco 全部保持完好，demuxer 仍能逐包交付，
    // 但码流被持续破坏，解码器只能靠错误隐藏吐出带标记的帧。
    std::string damaged = bytes;
    const std::size_t end = payload_end - 16;
    for (std::size_t i = payload_begin + 16; i < end; i += 19)
        damaged[i] = static_cast<char>(static_cast<unsigned char>(damaged[i]) ^ 0xFF);
    std::ofstream out(media.path, std::ios::binary | std::ios::trunc);
    out.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    // ❗ 必须显式 close：moov 在文件尾，流缓冲没刷盘时自检会读到半截文件，
    // demuxer 报 "unknown codec"、一帧都解不出，看起来像「样本没触发帧标记」。
    out.close();
    if (!out) {
        result.error = "写回损坏样本失败";
        return result;
    }

    // 自检：拿不到真实带标记帧就判失败，绝不把假样本交给用例。
    const MediaProbe probe = ProbeMediaFile(result.path);
    if (!probe.opened) {
        result.error = "损坏后的样本无法被 FFmpeg 打开: " + probe.error;
        return result;
    }
    result.decoded_frames = static_cast<uint64_t>(probe.frame_count);
    result.marked_frames = static_cast<uint64_t>(probe.marked_frames);
    if (result.marked_frames == 0) {
        result.error = "样本未触发 decode_error_flags：实测 " + std::to_string(probe.frame_count) +
                       " 帧全部无标记，不能用作帧分类正例";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace videoeye_test
