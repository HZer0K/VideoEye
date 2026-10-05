#include "core/analysis/orchestration/AnalysisPipeline.h"

#include <cmath>
#include <cstring>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "infrastructure/logging/Logger.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace videoeye {
namespace analyzer {
namespace {

// 把 FFmpeg 的声道位置翻译成 BS.1770 需要的角色（决定声道加权）
model::AudioChannelRole AudioChannelRoleOf(AVChannel channel) {
    switch (channel) {
        case AV_CHAN_LOW_FREQUENCY:
            return model::AudioChannelRole::LowFrequency;
        case AV_CHAN_SIDE_LEFT:
        case AV_CHAN_SIDE_RIGHT:
        case AV_CHAN_BACK_LEFT:
        case AV_CHAN_BACK_RIGHT:
        case AV_CHAN_BACK_CENTER:
        case AV_CHAN_TOP_BACK_LEFT:
        case AV_CHAN_TOP_BACK_RIGHT:
            return model::AudioChannelRole::Surround;
        default:
            return model::AudioChannelRole::Front;
    }
}

}  // namespace

AnalysisPipeline::AnalysisPipeline(const AnalysisOptions& options) : options_(options) {}

int AnalysisPipeline::FirstStreamOfType(AVFormatContext* fmt, AVMediaType type) {
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == type) return static_cast<int>(i);
    }
    return -1;
}

// ---- 扫描之前 ----

void AnalysisPipeline::Prepare(AVFormatContext* fmt, model::AnalysisResult& result,
                              double file_duration) {
    // 字幕 / 时码 / 辅助数据轨：只在存在对应流时才有实际工作量（内部按类型过滤）
    if (options_.analyze_subtitle) {
        subtitle_.Reset(options_.subtitle_options);
        subtitle_.RegisterStreams(fmt);
    }
    if (options_.analyze_timecode) {
        timecode_.Reset(options_.timecode_options);
        timecode_.RegisterStreams(fmt, file_duration);
    }
    if (options_.analyze_aux_data) {
        aux_.Reset(options_.aux_data_options);
        aux_.RegisterStreams(fmt);
    }

    PrepareBitstream(fmt, result);

    // ---- 色彩与 HDR 元数据分析（读 AVCodecParameters + coded_side_data，几乎零成本）----
    if (options_.analyze_color_hdr) {
        color_video_stream_index_ = FirstStreamOfType(fmt, AVMEDIA_TYPE_VIDEO);
    }
    if (options_.analyze_color_hdr && color_video_stream_index_ >= 0) {
        color_hdr_.Reset(options_.color_hdr_options);
        color_hdr_.UpdateFromStream(fmt->streams[color_video_stream_index_]);
    }

    // ---- 码率与 GOP 深度分析准备 ----
    if (options_.analyze_bitrate_gop) {
        video_stream_index_ = FirstStreamOfType(fmt, AVMEDIA_TYPE_VIDEO);
    }
    if (options_.analyze_bitrate_gop && video_stream_index_ >= 0) {
        bitrate_gop_.Reset(options_.bitrate_gop_options);
        AVStream* vst = fmt->streams[video_stream_index_];

        if (options_.decode_frame_types) {
            const AVCodec* codec = avcodec_find_decoder(vst->codecpar->codec_id);
            if (codec != nullptr) {
                frame_probe_.decoder = avcodec_alloc_context3(codec);
                if (frame_probe_.decoder != nullptr &&
                    avcodec_parameters_to_context(frame_probe_.decoder, vst->codecpar) >= 0) {
                    frame_probe_.decoder->pkt_timebase = vst->time_base;
                    frame_probe_.frame = av_frame_alloc();
                    if (frame_probe_.frame != nullptr &&
                        avcodec_open2(frame_probe_.decoder, codec, nullptr) == 0) {
                        frame_probe_.use_decoder = true;
                    }
                }
                if (!frame_probe_.use_decoder) {
                    // 打开失败 → 退回 parser
                    if (frame_probe_.frame) { av_frame_free(&frame_probe_.frame); }
                    if (frame_probe_.decoder) { avcodec_free_context(&frame_probe_.decoder); }
                }
            }
        }
        if (!frame_probe_.use_decoder) {
            frame_probe_.parser = av_parser_init(vst->codecpar->codec_id);
            if (frame_probe_.parser != nullptr) {
                // 我们喂的是完整访问单元（一个包 = 一帧），告诉解析器不要做拼接
                frame_probe_.parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
                frame_probe_.parser_ctx = avcodec_alloc_context3(nullptr);
                if (frame_probe_.parser_ctx != nullptr) {
                    avcodec_parameters_to_context(frame_probe_.parser_ctx, vst->codecpar);
                }
            }
        }
    }

    // ---- 音频 QC 准备（解码 + 归一到 float planar）----
    if (options_.analyze_audio_qc) {
        audio_stream_index_ = FirstStreamOfType(fmt, AVMEDIA_TYPE_AUDIO);
    }
    audio_time_base_ =
        (audio_stream_index_ >= 0) ? av_q2d(fmt->streams[audio_stream_index_]->time_base) : 0.0;

    if (options_.analyze_audio_qc && audio_stream_index_ >= 0) {
        audio_qc_.Reset(options_.audio_qc_options);
        AVStream* ast = fmt->streams[audio_stream_index_];
        const AVCodec* codec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (codec != nullptr) {
            audio_probe_.decoder = avcodec_alloc_context3(codec);
            if (audio_probe_.decoder != nullptr &&
                avcodec_parameters_to_context(audio_probe_.decoder, ast->codecpar) >= 0) {
                audio_probe_.decoder->pkt_timebase = ast->time_base;
                if (avcodec_open2(audio_probe_.decoder, codec, nullptr) == 0) {
                    audio_probe_.frame = av_frame_alloc();
                    audio_probe_.ready = (audio_probe_.frame != nullptr);
                }
            }
            if (!audio_probe_.ready) {
                if (audio_probe_.frame) av_frame_free(&audio_probe_.frame);
                if (audio_probe_.decoder) avcodec_free_context(&audio_probe_.decoder);
            }
        }

        if (audio_probe_.ready) {
            AVChannelLayout layout{};
            av_channel_layout_copy(&layout, &audio_probe_.decoder->ch_layout);
            audio_probe_.channels = layout.nb_channels;
            audio_probe_.sample_rate = audio_probe_.decoder->sample_rate;

            for (int c = 0; c < audio_probe_.channels; ++c) {
                const AVChannel ch = av_channel_layout_channel_from_index(&layout, c);
                char name_buf[32] = {0};
                av_channel_name(name_buf, sizeof(name_buf), ch);
                const std::string name = (name_buf[0] != '\0') ? std::string(name_buf)
                                                               : ("Ch" + std::to_string(c + 1));
                audio_probe_.channel_info.emplace_back(name, AudioChannelRoleOf(ch));
            }

            const AVSampleFormat in_fmt = audio_probe_.decoder->sample_fmt;
            const char* fmt_name = av_get_sample_fmt_name(in_fmt);
            audio_qc_.SetStreamInfo(audio_probe_.sample_rate, audio_probe_.channels,
                                    fmt_name ? fmt_name : "",
                                    av_get_bytes_per_sample(in_fmt) * 8);
            audio_qc_.SetChannelInfo(audio_probe_.channel_info);

            // 非 float planar 输出需要 swr 转换（s16 / flt / s32 ...）
            if (in_fmt != AV_SAMPLE_FMT_FLTP && audio_probe_.channels > 0 &&
                audio_probe_.sample_rate > 0) {
                AVChannelLayout out_layout{};
                av_channel_layout_copy(&out_layout, &layout);
                int linesize = 0;
                if (swr_alloc_set_opts2(&audio_probe_.swr, &out_layout, AV_SAMPLE_FMT_FLTP,
                                        audio_probe_.sample_rate, &layout, in_fmt,
                                        audio_probe_.sample_rate, 0, nullptr) >= 0 &&
                    swr_init(audio_probe_.swr) >= 0 &&
                    av_samples_alloc_array_and_samples(&audio_probe_.out_data, &linesize,
                                                       audio_probe_.channels, 8192,
                                                       AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                    audio_probe_.out_frames = 8192;
                } else if (audio_probe_.swr != nullptr) {
                    swr_free(&audio_probe_.swr);
                }
                av_channel_layout_uninit(&out_layout);
            }
            av_channel_layout_uninit(&layout);
        }
    }

    // GOP 统计按流号计数，大小随流数；逐包分发时不再重复分配
    frames_since_key_.assign(static_cast<size_t>(fmt->nb_streams), 0);
}

// 编码码流解析（只读 extradata，不解码；必须在 avformat_close_input 之前）。
// 只取第一条视频流：诊断关心的是主视频的编码参数，多视频流取第一条足够。
void AnalysisPipeline::PrepareBitstream(AVFormatContext* fmt, model::AnalysisResult& result) {
    if (!options_.analyze_bitstream) return;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (st->codecpar->extradata == nullptr || st->codecpar->extradata_size <= 0) continue;

        ContainerMetadata meta;
        meta.codec_name = (i < result.streams.size()) ? result.streams[i].codec_name : "";
        meta.width = st->codecpar->width;
        meta.height = st->codecpar->height;
        meta.bit_depth = (st->codecpar->bits_per_raw_sample > 0)
                             ? st->codecpar->bits_per_raw_sample
                             : 8;
        meta.color_primaries = st->codecpar->color_primaries;
        meta.transfer_characteristics = st->codecpar->color_trc;
        meta.matrix_coefficients = st->codecpar->color_space;
        meta.color_range = st->codecpar->color_range;

        BitstreamAnalyzer bitstream;
        bitstream.SetContainerMetadata(meta);
        model::BitstreamAnalysisResult bs = bitstream.Analyze(
            st->codecpar->extradata,
            static_cast<size_t>(st->codecpar->extradata_size),
            static_cast<int>(st->codecpar->codec_id));
        bs.stream_index = static_cast<int>(i);
        if (bs.analyzed) {
            result.bitstream_analysis = std::move(bs);
            result.bitstream_analyzed = true;
            LOG_INFO("码流解析: codec=" + result.bitstream_analysis.codec_name +
                     " " + std::to_string(result.bitstream_analysis.width) + "x" +
                     std::to_string(result.bitstream_analysis.height) +
                     " 不一致=" + std::to_string(result.bitstream_analysis.inconsistencies.size()));
            break;
        }
    }
}

// ---- 每个包一次 ----

void AnalysisPipeline::OnPacket(AVFormatContext* fmt, AVPacket* pkt,
                                model::AnalysisResult& result, int64_t packet_index) {
    AVStream* st = fmt->streams[pkt->stream_index];
    const double tb = av_q2d(st->time_base);
    const bool is_video = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
    const bool has_pts = (pkt->pts != AV_NOPTS_VALUE);
    const double ts = has_pts ? static_cast<double>(pkt->pts) * tb : -1.0;

    // 时间轴与同步诊断（demux 层：PTS/DTS/B 帧/首帧偏移/关键帧索引）
    {
        model::PacketTiming timing;
        timing.index = static_cast<int>(packet_index);
        timing.stream_index = pkt->stream_index;
        timing.media_type = static_cast<int>(st->codecpar->codec_type);
        timing.pts_ms = has_pts ? ts * 1000.0 : model::kNoTimestamp;
        timing.dts_ms = (pkt->dts != AV_NOPTS_VALUE)
                            ? static_cast<double>(pkt->dts) * tb * 1000.0
                            : model::kNoTimestamp;
        timing.duration_ms = (pkt->duration > 0)
                                 ? static_cast<double>(pkt->duration) * tb * 1000.0
                                 : model::kNoTimestamp;
        timing.pos = pkt->pos;
        timing.size = pkt->size;
        timing.flags = pkt->flags;
        timing.key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        timeline_.OnPacket(timing);
    }

    // 字幕 / 时码 / 辅助数据轨（内部先按流号过滤，非目标流直接返回）
    if (options_.analyze_subtitle) subtitle_.OnPacket(pkt, st);
    if (options_.analyze_timecode) timecode_.OnPacket(pkt, st);
    if (options_.analyze_aux_data) aux_.OnPacket(pkt, st);

    // 音频 QC：解码后归一为 float planar 再喂给分析器
    if (options_.analyze_audio_qc && pkt->stream_index == audio_stream_index_ &&
        audio_probe_.ready) {
        if (avcodec_send_packet(audio_probe_.decoder, pkt) == 0) {
            while (avcodec_receive_frame(audio_probe_.decoder, audio_probe_.frame) >= 0) {
                FeedAudioFrame(audio_probe_.frame);
                av_frame_unref(audio_probe_.frame);
            }
        }
    }

    if (is_video) {
        // 关键帧间隔与 GOP 帧数（逐包扫描就能拿到，不需要解码）
        frames_since_key_[pkt->stream_index] += 1;

        if (pkt->flags & AV_PKT_FLAG_KEY) {
            result.key_frame_count += 1;
            result.streams[pkt->stream_index].key_frame_count += 1;
            if (has_key_ && ts >= 0.0 && last_key_ts_ >= 0.0) {
                const double gap = ts - last_key_ts_;
                if (gap > 0.0) {
                    result.gop_intervals_seconds.push_back(gap);
                    result.gop_frame_sizes.push_back(
                        static_cast<int>(frames_since_key_[pkt->stream_index]));
                    if (gap > result.max_gop_interval_seconds) {
                        result.max_gop_interval_seconds = gap;
                    }
                }
            }
            if (frames_since_key_[pkt->stream_index] > result.max_gop_frames) {
                result.max_gop_frames = static_cast<int>(frames_since_key_[pkt->stream_index]);
            }
            frames_since_key_[pkt->stream_index] = 0;
            if (ts >= 0.0) {
                last_key_ts_ = ts;
                has_key_ = true;
            }
        }

        // 码率与 GOP 深度分析（仅第一条视频流）
        if (options_.analyze_bitrate_gop && pkt->stream_index == video_stream_index_) {
            const double sample_ts = has_pts
                                         ? ts
                                         : ((pkt->dts != AV_NOPTS_VALUE)
                                                ? static_cast<double>(pkt->dts) * tb
                                                : 0.0);
            model::FrameType frame_type = model::FrameType::Unknown;
            bool is_idr = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            bool fed = false;

            if (frame_probe_.use_decoder) {
                if (avcodec_send_packet(frame_probe_.decoder, pkt) == 0) {
                    while (avcodec_receive_frame(frame_probe_.decoder, frame_probe_.frame) >= 0) {
                        frame_type = static_cast<model::FrameType>(frame_probe_.frame->pict_type);
                        // 注: FFmpeg 8.x 已移除 AVFrame::key_frame，统一用 AV_FRAME_FLAG_KEY
                        is_idr = ((frame_probe_.frame->flags & AV_FRAME_FLAG_KEY) != 0);
                        const double frame_ts =
                            (frame_probe_.frame->pts != AV_NOPTS_VALUE)
                                ? static_cast<double>(frame_probe_.frame->pts) * tb
                                : sample_ts;
                        bitrate_gop_.OnFrame(pkt->stream_index, frame_ts, pkt->size,
                                            frame_type, is_idr);
                        fed = true;
                        av_frame_unref(frame_probe_.frame);
                    }
                }
            } else if (frame_probe_.parser != nullptr) {
                uint8_t* out_data = nullptr;
                int out_size = 0;
                av_parser_parse2(frame_probe_.parser, frame_probe_.parser_ctx, &out_data,
                                 &out_size, pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
                if (frame_probe_.parser->pict_type != AV_PICTURE_TYPE_NONE) {
                    frame_type = static_cast<model::FrameType>(frame_probe_.parser->pict_type);
                    if (frame_probe_.parser->key_frame >= 1) {
                        is_idr = true;
                    } else if (frame_probe_.parser->key_frame == 0) {
                        is_idr = false;
                    }
                    fed = true;
                }
            }

            if (fed) {
                bitrate_gop_.OnFrame(pkt->stream_index, sample_ts, pkt->size, frame_type,
                                     is_idr);
            } else {
                bitrate_gop_.OnPacket(pkt->stream_index, sample_ts, pkt->size, is_idr);
            }
        }
    }

    // 色彩与 HDR：包级 side data 补充 + 需要时解码首帧读 AVFrame side data
    if (options_.analyze_color_hdr && is_video &&
        pkt->stream_index == color_video_stream_index_) {
        if (pkt->side_data != nullptr && pkt->side_data_elems > 0) {
            color_hdr_.UpdateFromPacket(pkt, pkt->stream_index);
        }
        if (color_hdr_.NeedsFrameProbe() && !color_probe_.failed &&
            color_probe_.frames_read < options_.color_hdr_options.max_probe_frames) {
            if (color_probe_.EnsureOpen(st)) {
                if (avcodec_send_packet(color_probe_.decoder, pkt) == 0) {
                    while (avcodec_receive_frame(color_probe_.decoder, color_probe_.frame) >= 0) {
                        color_hdr_.UpdateFromFrame(color_probe_.frame);
                        av_frame_unref(color_probe_.frame);
                        ++color_probe_.frames_read;
                        if (color_probe_.frames_read >=
                            options_.color_hdr_options.max_probe_frames) {
                            break;
                        }
                    }
                }
            }
        }
    }
}

// ---- 扫描结束后的钩子 ----

void AnalysisPipeline::BeforeClose(AVFormatContext* fmt) {
    // 部分封装的 HDR 元数据要读到数据包后才补充进 codecpar，关闭输入前再刷一次
    if (options_.analyze_color_hdr && color_video_stream_index_ >= 0) {
        color_hdr_.UpdateFromStream(fmt->streams[color_video_stream_index_]);
    }
}

void AnalysisPipeline::FlushAudio() {
    if (!options_.analyze_audio_qc || !audio_probe_.ready) return;

    avcodec_send_packet(audio_probe_.decoder, nullptr);
    while (avcodec_receive_frame(audio_probe_.decoder, audio_probe_.frame) >= 0) {
        FeedAudioFrame(audio_probe_.frame);
        av_frame_unref(audio_probe_.frame);
    }
    if (audio_probe_.swr != nullptr && audio_probe_.out_data != nullptr &&
        audio_probe_.channels > 0) {
        std::vector<const float*> planes(static_cast<size_t>(audio_probe_.channels));
        int got = swr_convert(audio_probe_.swr, audio_probe_.out_data, audio_probe_.out_frames,
                              nullptr, 0);
        while (got > 0) {
            for (int c = 0; c < audio_probe_.channels; ++c) {
                planes[static_cast<size_t>(c)] =
                    reinterpret_cast<const float*>(audio_probe_.out_data[c]);
            }
            audio_qc_.OnSamples(planes.data(), got, -1.0);
            got = swr_convert(audio_probe_.swr, audio_probe_.out_data, audio_probe_.out_frames,
                              nullptr, 0);
        }
    }
}

void AnalysisPipeline::ReleaseProbes() {
    frame_probe_.Release();
    color_probe_.Release();
    if (audio_probe_.ready) audio_probe_.Release();
}

// ---- 内部 ----

// 喂一帧解码后的音频给 QC 分析器（内部按 100 ms 步进出块）
void AnalysisPipeline::FeedAudioFrame(AVFrame* frame) {
    if (!audio_probe_.ready || frame == nullptr || frame->nb_samples <= 0) return;
    const int channels = frame->ch_layout.nb_channels;
    if (channels <= 0) return;
    if (frame->sample_rate != audio_probe_.sample_rate || channels != audio_probe_.channels) {
        audio_probe_.rate_changed = true;   // 参数中途变化（罕见），该帧丢弃
        return;
    }
    double ts = (frame->pts != AV_NOPTS_VALUE && audio_time_base_ > 0.0)
                    ? static_cast<double>(frame->pts) * audio_time_base_
                    : -1.0;

    std::vector<const float*> planes(static_cast<size_t>(channels));
    if (frame->format == AV_SAMPLE_FMT_FLTP) {
        for (int c = 0; c < channels; ++c) {
            planes[static_cast<size_t>(c)] = reinterpret_cast<const float*>(frame->data[c]);
        }
        audio_qc_.OnSamples(planes.data(), frame->nb_samples, ts);
        return;
    }
    if (audio_probe_.swr == nullptr || audio_probe_.out_data == nullptr) return;

    int got = swr_convert(audio_probe_.swr, audio_probe_.out_data, audio_probe_.out_frames,
                          const_cast<const uint8_t**>(frame->data), frame->nb_samples);
    while (got > 0) {
        for (int c = 0; c < channels; ++c) {
            planes[static_cast<size_t>(c)] =
                reinterpret_cast<const float*>(audio_probe_.out_data[c]);
        }
        audio_qc_.OnSamples(planes.data(), got, ts);
        ts = -1.0;   // 后续块沿用内部时钟
        got = swr_convert(audio_probe_.swr, audio_probe_.out_data, audio_probe_.out_frames,
                          nullptr, 0);
    }
}

}  // namespace analyzer
}  // namespace videoeye
