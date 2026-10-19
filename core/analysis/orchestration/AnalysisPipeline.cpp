#include "core/analysis/orchestration/AnalysisPipeline.h"

#include <cmath>
#include <cstring>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/analysis/detail/AnalysisStreamSelection.h"
#include "infrastructure/logging/Logger.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace videoeye {
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

// 容器侧 metadata 快照，供码流层做"码流 vs 容器"比对。
// extradata 与带内两条路径共用：两边都拿同一份快照，比对口径才一致。
ContainerMetadata ContainerMetadataFromStream(const AVStream* st,
                                             const model::AnalysisResult& result,
                                             unsigned stream_index) {
    ContainerMetadata meta;
    meta.codec_name = (stream_index < result.streams.size())
                          ? result.streams[stream_index].codec_name
                          : std::string();
    meta.width = st->codecpar->width;
    meta.height = st->codecpar->height;
    // bits_per_raw_sample 为 0 表示"容器没说"，不能当成 8bit：
    // 10bit H.264/HEVC 的 codecpar 也常常是 0，硬填 8 会给每个 10bit 文件
    // 制造一条假的不一致告警。0 由码流层按"未知"跳过比对。
    meta.bit_depth = (st->codecpar->bits_per_raw_sample > 0)
                         ? st->codecpar->bits_per_raw_sample
                         : 0;
    meta.color_primaries = st->codecpar->color_primaries;
    meta.transfer_characteristics = st->codecpar->color_trc;
    meta.matrix_coefficients = st->codecpar->color_space;
    meta.color_range = st->codecpar->color_range;
    return meta;
}

}  // namespace

AnalysisPipeline::AnalysisPipeline(const AnalysisOptions& options) : options_(options) {}

// ---- 扫描之前 ----

void AnalysisPipeline::Prepare(AVFormatContext* fmt, model::AnalysisResult& result,
                              double file_duration) {
    // 主轨一次定下来，各维度共用：封面图（ATTACHED_PIC）不算视频轨，
    // 有多条视频轨时以容器的 DEFAULT 标记为准（av_find_best_stream 的口径）。
    video_stream_index_ = SelectAnalysisStream(fmt, AVMEDIA_TYPE_VIDEO);
    audio_stream_index_ = SelectAnalysisStream(fmt, AVMEDIA_TYPE_AUDIO);
    result.primary_video_stream_index = video_stream_index_;
    result.primary_audio_stream_index = audio_stream_index_;

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

    // ---- 时间轴与同步诊断（逐包，常开）----
    // 只把 AnalysisOptions 暴露的那一项透传进来：逐包 delta 曲线的点数上限。
    // 分析器自己的默认值已经是有限值，这里给的是"上限可配"的入口。
    {
        TimelineAnalyzer::Options timeline_options;
        timeline_options.max_curve_points = options_.timeline_max_curve_points;
        timeline_.SetOptions(timeline_options);
    }

    PrepareBitstream(fmt, result);

    // ---- 色彩与 HDR 元数据分析（读 AVCodecParameters + coded_side_data，几乎零成本）----
    color_video_stream_index_ = options_.analyze_color_hdr ? video_stream_index_ : -1;
    if (options_.analyze_color_hdr && color_video_stream_index_ >= 0) {
        color_hdr_.Reset(options_.color_hdr_options);
        color_hdr_.UpdateFromStream(fmt->streams[color_video_stream_index_]);
    }

    // ---- 码率与 GOP 深度分析准备 ----
    if (options_.analyze_bitrate_gop && video_stream_index_ >= 0) {
        video_time_base_ = av_q2d(fmt->streams[video_stream_index_]->time_base);
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
    last_key_ts_.assign(static_cast<size_t>(fmt->nb_streams), -1.0);
}

// 编码码流解析（只读 extradata，不解码；必须在 avformat_close_input 之前）。
// 优先分析主视频轨；主轨的编码没有解析器时，允许退到其它视频轨（并保留
// stream_index，让报告能说明到底解析的是哪一条）。封面图不参与。
//
// extradata 为空时（TS / 裸流 / fMP4 媒体分片）转成"带内探测"：
// 参数集只写在码流里，Prepare 阶段还没读到包，所以这里只登记流号，
// 真正的攒包与解析在 OnPacket 里逐包推进（见 InBandParameterSets.h）。
void AnalysisPipeline::PrepareBitstream(AVFormatContext* fmt, model::AnalysisResult& result) {
    // 每次 Prepare 都从干净状态开始：同一个 pipeline 对象理论上可被复用，
    // 上一份文件的带内探测状态（流号/缓冲区）不能漏到下一份里。
    inband_stream_index_ = -1;
    inband_probe_.Reset(0);
    if (!options_.analyze_bitstream) return;

    // 主视频排最前，其余视频轨按流号跟在后面
    std::vector<unsigned> video_order;
    if (video_stream_index_ >= 0) {
        video_order.push_back(static_cast<unsigned>(video_stream_index_));
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (static_cast<int>(i) != video_stream_index_ && IsAnalysisVideoStream(fmt->streams[i])) {
            video_order.push_back(i);
        }
    }

    for (unsigned i : video_order) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->extradata == nullptr || st->codecpar->extradata_size <= 0) continue;

        BitstreamAnalyzer bitstream;
        bitstream.SetContainerMetadata(ContainerMetadataFromStream(st, result, i));
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

    if (result.bitstream_analyzed) return;

    // ---- 带内兜底：extradata 缺失的封装 ----
    // 注意用 continue 而不是 return：第一条视频流可能是没有解析器的编码
    // （如 MPEG-2），但第二条是 H.264 —— 那种文件仍然应该能分析。
    for (unsigned i : video_order) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->extradata != nullptr && st->codecpar->extradata_size > 0) continue;

        inband_probe_.Reset(static_cast<int>(st->codecpar->codec_id));
        if (!inband_probe_.enabled()) continue;
        inband_probe_.SetContainerMetadata(ContainerMetadataFromStream(st, result, i));
        inband_stream_index_ = static_cast<int>(i);
        LOG_INFO("码流解析: 流 #" + std::to_string(i) +
                 " 无 extradata，改为带内参数集探测");
        return;
    }
}


// ---- 每个包一次 ----

void AnalysisPipeline::OnPacket(AVFormatContext* fmt, AVPacket* pkt,
                                model::AnalysisResult& result, int64_t packet_index) {
    AVStream* st = fmt->streams[pkt->stream_index];
    const double tb = av_q2d(st->time_base);
    // 封面图走的是"一帧 MJPEG 视频流"，不该进任何视频统计（GOP/码率/色彩/时间轴）
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !IsAnalysisVideoStream(st)) return;
    const bool is_video = IsAnalysisVideoStream(st);
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
            // GOP 间隔按流各自记账：多视频轨交错到达时，共用一份"上一关键帧时间"
            // 会把两条轨的间隔算成互相之间的半截 GOP。
            double& last_key_ts = last_key_ts_[pkt->stream_index];
            const bool is_primary = pkt->stream_index == video_stream_index_;
            if (is_primary && ts >= 0.0 && last_key_ts >= 0.0) {
                const double gap = ts - last_key_ts;
                if (gap > 0.0) {
                    result.gop_intervals_seconds.push_back(gap);
                    result.gop_frame_sizes.push_back(
                        static_cast<int>(frames_since_key_[pkt->stream_index]));
                    if (gap > result.max_gop_interval_seconds) {
                        result.max_gop_interval_seconds = gap;
                    }
                }
            }
            // max_gop_frames 与 gop_intervals_seconds 同口径：只统计主视频轨
            if (is_primary && frames_since_key_[pkt->stream_index] > result.max_gop_frames) {
                result.max_gop_frames = static_cast<int>(frames_since_key_[pkt->stream_index]);
            }
            frames_since_key_[pkt->stream_index] = 0;
            if (ts >= 0.0) {
                last_key_ts = ts;
            }
        }

        // 码率与 GOP 深度分析（仅第一条视频流）
        //
        // 输入建模成显式事件，**每个语义单元只进一次统计**（曾因解码路径在
        // receive 循环内外各调一次 OnFrame、parser 路径经 fed 二次调用，导致
        // 帧数/字节数/GOP/帧类型比例全部重复计数，见 BitrateGopAnalyzer.h）:
        //   * OnPacketBytes        —— 包字节按到达时间入包级账，并进待认领池
        //   * OnFramesClaimPending —— 解码器/parser 产出的帧来认领池里的字节（帧级账）
        //   * OnPacket             —— 无帧类型探测时的包级回退（一包一帧，类型未知）
        // 两条账分开是硬要求：解码有延迟时"本次送入包的字节"不属于"本次收到的帧"，
        // 拿它顶替会让 GOP 字节之和远小于总字节、I 帧大小归属到错误的包。
        if (options_.analyze_bitrate_gop && pkt->stream_index == video_stream_index_) {
            const double sample_ts = has_pts
                                         ? ts
                                         : ((pkt->dts != AV_NOPTS_VALUE)
                                                ? static_cast<double>(pkt->dts) * tb
                                                : 0.0);
            bool is_idr = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

            if (frame_probe_.use_decoder) {
                if (avcodec_send_packet(frame_probe_.decoder, pkt) == 0) {
                    // 字节**先**按包到达时间入账（码率曲线与总字节是包级事实），
                    // 并进待认领池：解码器可能压着好几包才吐一帧，此刻出的帧属于
                    // 更早的包，绝不能拿"本包字节"顶替它们的帧大小 —— 那会让
                    // GOP 字节之和远小于总字节、I 帧大小也归属到错误的包。
                    bitrate_gop_.OnPacketBytes(pkt->stream_index, sample_ts, pkt->size,
                                              /*awaiting_frame=*/true);
                    // 再收本包解码产出的全部帧：一包可能产出多帧，也可能一帧都
                    // 没有（帧重排序延迟）—— 两者都不按"一包一帧"猜。
                    std::vector<BitrateGopAnalyzer::FrameEvent> decoded;
                    while (avcodec_receive_frame(frame_probe_.decoder, frame_probe_.frame) >= 0) {
                        // 注: FFmpeg 8.x 已移除 AVFrame::key_frame，统一用 AV_FRAME_FLAG_KEY
                        const bool frame_is_idr =
                            (frame_probe_.frame->flags & AV_FRAME_FLAG_KEY) != 0;
                        const double frame_ts =
                            (frame_probe_.frame->pts != AV_NOPTS_VALUE)
                                ? static_cast<double>(frame_probe_.frame->pts) * tb
                                : sample_ts;
                        decoded.push_back({frame_ts,
                                           static_cast<model::FrameType>(
                                               frame_probe_.frame->pict_type),
                                           frame_is_idr});
                        av_frame_unref(frame_probe_.frame);
                    }
                    if (!decoded.empty()) {
                        // 待认领池里的字节（含本包）按出帧数均分给这批帧 ——
                        // 总量守恒，帧级账 + 未关联字节 == 包级账。
                        bitrate_gop_.OnFramesClaimPending(pkt->stream_index, decoded);
                    }
                    // decoded 为空 = 帧延迟：字节留在待认领池里，等后续包产出帧时
                    // 一起认领。绝不能在这里补一帧，否则会和真正的解码输出重复计数。
                } else {
                    // 包没进解码器（send 失败）：不会有帧来认领，按回退路径记一帧
                    bitrate_gop_.OnPacket(pkt->stream_index, sample_ts, pkt->size, is_idr);
                }
            } else if (frame_probe_.parser != nullptr) {
                uint8_t* out_data = nullptr;
                int out_size = 0;
                av_parser_parse2(frame_probe_.parser, frame_probe_.parser_ctx, &out_data,
                                 &out_size, pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
                // 与解码路径同构：字节先入账并进待认领池，parser 吐出的帧再来认领。
                // 这样"parser 攒了几个包才凑出一帧"时，那几个包的字节也归到这一帧，
                // 而不是留在池外变成"未关联字节"。
                bitrate_gop_.OnPacketBytes(pkt->stream_index, sample_ts, pkt->size,
                                          /*awaiting_frame=*/true);
                if (out_size > 0) {
                    // parser 吐出了一个完整帧（PARSER_FLAG_COMPLETE_FRAMES 下
                    // 一次调用对应一帧）
                    model::FrameType frame_type = model::FrameType::Unknown;
                    if (frame_probe_.parser->pict_type != AV_PICTURE_TYPE_NONE) {
                        frame_type =
                            static_cast<model::FrameType>(frame_probe_.parser->pict_type);
                        if (frame_probe_.parser->key_frame >= 1) {
                            is_idr = true;
                        } else if (frame_probe_.parser->key_frame == 0) {
                            is_idr = false;
                        }
                    }
                    std::vector<BitrateGopAnalyzer::FrameEvent> events{
                        {sample_ts, frame_type, is_idr}};
                    bitrate_gop_.OnFramesClaimPending(pkt->stream_index, events);
                }
                // 本包不含完整帧（裸流/TS 分片场景）：字节留在池里，不猜帧。
            } else {
                // 解码器与 parser 都不可用：包级回退（一包一帧，类型未知）
                bitrate_gop_.OnPacket(pkt->stream_index, sample_ts, pkt->size, is_idr);
            }
        }

        // 带内参数集（extradata 为空的封装）：攒到参数集出现为止。
        // 放在 GOP/码率之后是因为那两段不依赖参数集；这里一旦成功就回填结果并收工，
        // 之后的包不再进这个分支（inband_stream_index_ 置 -1）。
        if (inband_stream_index_ >= 0 && pkt->stream_index == inband_stream_index_ &&
            !result.bitstream_analyzed) {
            if (inband_probe_.Feed(pkt->data, pkt->size)) {
                result.bitstream_analysis = inband_probe_.result();
                result.bitstream_analysis.stream_index = pkt->stream_index;
                result.bitstream_analyzed = true;
                inband_stream_index_ = -1;
                LOG_INFO("码流解析(带内): codec=" + result.bitstream_analysis.codec_name +
                         " " + std::to_string(result.bitstream_analysis.width) + "x" +
                         std::to_string(result.bitstream_analysis.height) +
                         " 用包数=" + std::to_string(inband_probe_.packets()) +
                         " 不一致=" +
                         std::to_string(result.bitstream_analysis.inconsistencies.size()));
            } else if (inband_probe_.given_up()) {
                // 放弃要留日志：否则用户只看到"码流分析没结果"，查不出是没找到参数集
                LOG_INFO("带内参数集探测放弃: " + inband_probe_.GiveUpReason());
                inband_stream_index_ = -1;
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

// EOF 时帧类型解码器里可能还压着几帧（帧重排序延迟）。这些帧的归属包在扫描期
// 已按"0 帧产出"走过 OnPacketBytes（字节已入账、躺在待认领池里），这里把帧补进
// 统计并让它们认领池里剩下的字节 —— 不 flush 的话"总帧数 < 视频包数"，I/P/B
// 比例与 GOP 统计失真；不认领的话这些帧就是 0 字节，平均帧大小与 GOP 大小偏小。
void AnalysisPipeline::FlushFrameProbe() {
    if (!options_.analyze_bitrate_gop || !frame_probe_.use_decoder) return;
    if (video_stream_index_ < 0) return;
    if (frame_probe_.decoder == nullptr || frame_probe_.frame == nullptr) return;
    if (avcodec_send_packet(frame_probe_.decoder, nullptr) != 0) return;
    std::vector<BitrateGopAnalyzer::FrameEvent> pending;
    while (avcodec_receive_frame(frame_probe_.decoder, frame_probe_.frame) >= 0) {
        const bool is_idr = (frame_probe_.frame->flags & AV_FRAME_FLAG_KEY) != 0;
        const double frame_ts =
            (frame_probe_.frame->pts != AV_NOPTS_VALUE && video_time_base_ > 0.0)
                ? static_cast<double>(frame_probe_.frame->pts) * video_time_base_
                : -1.0;
        pending.push_back({frame_ts,
                           static_cast<model::FrameType>(frame_probe_.frame->pict_type),
                           is_idr});
        av_frame_unref(frame_probe_.frame);
    }
    // 池里剩下的字节（最后几个包）归这批帧：它们的包确实已经全部送进了解码器。
    if (!pending.empty()) bitrate_gop_.OnFramesClaimPending(video_stream_index_, pending);
}

// ---- 内部 ----

// 喂一帧解码后的音频给 QC 分析器（内部按 100 ms 步进出块）
void AnalysisPipeline::FeedAudioFrame(AVFrame* frame) {
    if (!audio_probe_.ready || frame == nullptr || frame->nb_samples <= 0) return;
    const int channels = frame->ch_layout.nb_channels;
    // extended_data 在声道数 > AV_NUM_DATA_POINTERS(8) 时才是完整的那一份：
    // data[] 只有前 8 个平面，9~N 声道全是 nullptr（12 声道 S16P 实测如此）。
    if (channels <= 0 || frame->extended_data == nullptr) return;
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
            planes[static_cast<size_t>(c)] =
                reinterpret_cast<const float*>(frame->extended_data[c]);
        }
        audio_qc_.OnSamples(planes.data(), frame->nb_samples, ts);
        return;
    }
    if (audio_probe_.swr == nullptr || audio_probe_.out_data == nullptr) return;

    int got = swr_convert(audio_probe_.swr, audio_probe_.out_data, audio_probe_.out_frames,
                          const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
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

}  // namespace videoeye
