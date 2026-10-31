#include "core/analysis/orchestration/detail/AnalysisPipeline.h"

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
ContainerMetadata ContainerMetadataFromStream(const AVStream* st, const model::AnalysisResult& result,
                                              unsigned stream_index) {
    ContainerMetadata meta;
    meta.codec_name = (stream_index < result.streams.size()) ? result.streams[stream_index].codec_name : std::string();
    meta.width = st->codecpar->width;
    meta.height = st->codecpar->height;
    // bits_per_raw_sample 为 0 表示"容器没说"，不能当成 8bit：
    // 10bit H.264/HEVC 的 codecpar 也常常是 0，硬填 8 会给每个 10bit 文件
    // 制造一条假的不一致告警。0 由码流层按"未知"跳过比对。
    meta.bit_depth = (st->codecpar->bits_per_raw_sample > 0) ? st->codecpar->bits_per_raw_sample : 0;
    meta.color_primaries = st->codecpar->color_primaries;
    meta.transfer_characteristics = st->codecpar->color_trc;
    meta.matrix_coefficients = st->codecpar->color_space;
    meta.color_range = st->codecpar->color_range;
    return meta;
}

// 媒体包里 NAL 长度前缀的宽度（1..4）。返回 0 = 未知，交给巡检器逐包判定。
//
// 来源是配置记录本身（ISO/IEC 14496-15）：
//   * avcC：第 5 个字节（index 4）的低 2 位 = lengthSizeMinusOne
//   * hvcC：第 22 个字节（index 21）的低 2 位 = lengthSizeMinusOne
// Annex B 的 extradata（TS / 裸流的 SPS+PPS）没有长度前缀这个概念，返回 0。
int NalLengthSizeFromExtradata(const AVCodecParameters* par) {
    if (par == nullptr || par->extradata == nullptr || par->extradata_size <= 0)
        return 0;
    const uint8_t* d = par->extradata;
    const int n = par->extradata_size;
    // 起始码开头 = Annex B extradata，不是长度前缀流
    if (n >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1)))
        return 0;
    if (par->codec_id == AV_CODEC_ID_H264 && n >= 5 && d[0] == 1 && (d[4] & 0xFC) == 0xFC) {
        return (d[4] & 0x03) + 1;
    }
    if (par->codec_id == AV_CODEC_ID_HEVC && n >= 22) {
        return (d[21] & 0x03) + 1;
    }
    return 0;
}

} // namespace

AnalysisPipeline::AnalysisPipeline(const AnalysisOptions& options) : options_(options) {
}

AnalysisPipeline::~AnalysisPipeline() {
    // 兜底释放（评审 P2-4）：正常/已知错误分支都会显式 ReleaseProbes()，这里收的是
    // "栈展开"那条路径 —— Prepare() / 逐包分析 / 回调抛异常时不会走到显式释放。
    // ReleaseProbes() 本身幂等，重复调用没有副作用。
    ReleaseProbes();
}

// ---- 扫描之前 ----

void AnalysisPipeline::Prepare(AVFormatContext* fmt, model::AnalysisResult& result, double file_duration) {
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
        timeline_options.preferred_video_stream_index = video_stream_index_;
        timeline_options.preferred_audio_stream_index = audio_stream_index_;
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
                    if (frame_probe_.frame != nullptr && avcodec_open2(frame_probe_.decoder, codec, nullptr) == 0) {
                        frame_probe_.use_decoder = true;
                    }
                }
                if (!frame_probe_.use_decoder) {
                    // 打开失败 → 退回 parser
                    if (frame_probe_.frame) {
                        av_frame_free(&frame_probe_.frame);
                    }
                    if (frame_probe_.decoder) {
                        avcodec_free_context(&frame_probe_.decoder);
                    }
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
    //
    // 多音轨：每条参与 QC 的音频流各建一套解码通路 + 分析器（见 AudioQcTrack）。
    // 主轨恒排第一 —— result.audio_qc 仍然取主轨那一份，既有消费方不用改。
    audio_tracks_.clear();
    audio_tracks_total_ = 0;
    audio_tracks_limit_note_ = 0;
    audio_time_base_ = (audio_stream_index_ >= 0) ? av_q2d(fmt->streams[audio_stream_index_]->time_base) : 0.0;

    if (options_.analyze_audio_qc) {
        // 先数一遍音频流总数（用于 DimensionStatus 的覆盖说明：覆盖 M/N 条）
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* st = fmt->streams[i];
            if (st != nullptr && st->codecpar != nullptr && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                ++audio_tracks_total_;
        }

        const int max_tracks = (options_.audio_qc_max_tracks > 0) ? options_.audio_qc_max_tracks : 1;
        int planned = 0;
        if (audio_stream_index_ >= 0) {
            SetupAudioTrack(fmt, audio_stream_index_);
            ++planned;
        }
        if (options_.audio_qc_all_tracks) {
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                const int index = static_cast<int>(i);
                if (index == audio_stream_index_)
                    continue;
                const AVStream* st = fmt->streams[i];
                if (st == nullptr || st->codecpar == nullptr || st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;
                if (planned >= max_tracks) {
                    // 超出上限必须留痕：静默少测几条轨正是"抽样扫描"最容易演变成假通过的地方
                    ++audio_tracks_limit_note_;
                    continue;
                }
                SetupAudioTrack(fmt, index);
                ++planned;
            }
        }
        if (audio_tracks_limit_note_ > 0) {
            LOG_INFO("音频 QC: 有 " + std::to_string(audio_tracks_limit_note_) + " 条音轨超过上限(" +
                     std::to_string(max_tracks) + ")，未参与 QC");
        }
    }

    // ---- 音频位流参数（D1）----
    // 与音频 QC 解耦：这条链路只读帧头，不碰解码器。因此**即使解码器打不开**
    // （缺解码库、专有编码）也照样能拿到 ADTS/AC-3/Opus 的声明参数。
    if (options_.analyze_audio_bitstream) {
        audio_bitstream_.Reset(options_.audio_bitstream_options);
        audio_bitstream_.RegisterStreams(fmt);
    }

    // GOP 统计按流号计数，大小随流数；逐包分发时不再重复分配
    frames_since_key_.assign(static_cast<size_t>(fmt->nb_streams), 0);
    last_key_ts_.assign(static_cast<size_t>(fmt->nb_streams), -1.0);
}

// 为一条音频流建解码通路 + 分析器，并挂进 audio_tracks_。
//
// 返回该轨在 audio_tracks_ 里的下标；建不起来（找不到解码器 / 打不开 / 参数非法）
// 返回 -1。返回 -1 不等于"这条轨没问题"：调用方把它计入 total 而不计入 analyzed，
// 覆盖说明里就会显示"覆盖 M/N 条"。
int AnalysisPipeline::SetupAudioTrack(AVFormatContext* fmt, int stream_index) {
    if (fmt == nullptr || stream_index < 0 || stream_index >= static_cast<int>(fmt->nb_streams))
        return -1;
    AVStream* ast = fmt->streams[stream_index];
    if (ast == nullptr || ast->codecpar == nullptr || ast->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return -1;

    audio_tracks_.push_back(AudioQcTrack{});
    AudioQcTrack& track = audio_tracks_.back();
    track.stream_index = stream_index;
    track.time_base = av_q2d(ast->time_base);
    track.analyzer.Reset(options_.audio_qc_options);

    const AVCodec* codec = avcodec_find_decoder(ast->codecpar->codec_id);
    if (codec == nullptr) {
        LOG_INFO("音频 QC: 流#" + std::to_string(stream_index) + " 没有可用解码器，跳过该轨");
        return static_cast<int>(audio_tracks_.size()) - 1;
    }

    AudioQcProbe& probe = track.probe;
    probe.decoder = avcodec_alloc_context3(codec);
    if (probe.decoder != nullptr && avcodec_parameters_to_context(probe.decoder, ast->codecpar) >= 0) {
        probe.decoder->pkt_timebase = ast->time_base;
        if (avcodec_open2(probe.decoder, codec, nullptr) == 0) {
            probe.frame = av_frame_alloc();
            probe.ready = (probe.frame != nullptr);
        }
    }
    if (!probe.ready) {
        if (probe.frame)
            av_frame_free(&probe.frame);
        if (probe.decoder)
            avcodec_free_context(&probe.decoder);
        LOG_INFO("音频 QC: 流#" + std::to_string(stream_index) + " 解码器打开失败，跳过该轨");
        return static_cast<int>(audio_tracks_.size()) - 1;
    }

    AVChannelLayout layout{};
    av_channel_layout_copy(&layout, &probe.decoder->ch_layout);
    probe.channels = layout.nb_channels;
    probe.sample_rate = probe.decoder->sample_rate;

    for (int c = 0; c < probe.channels; ++c) {
        const AVChannel ch = av_channel_layout_channel_from_index(&layout, c);
        char name_buf[32] = {0};
        av_channel_name(name_buf, sizeof(name_buf), ch);
        const std::string name = (name_buf[0] != '\0') ? std::string(name_buf) : ("Ch" + std::to_string(c + 1));
        probe.channel_info.emplace_back(name, AudioChannelRoleOf(ch));
    }

    const AVSampleFormat in_fmt = probe.decoder->sample_fmt;
    const char* fmt_name = av_get_sample_fmt_name(in_fmt);
    track.analyzer.SetStreamInfo(probe.sample_rate, probe.channels, fmt_name ? fmt_name : "",
                                 av_get_bytes_per_sample(in_fmt) * 8);
    track.analyzer.SetChannelInfo(probe.channel_info);

    // 非 float planar 输出需要 swr 转换（s16 / flt / s32 ...）
    if (in_fmt != AV_SAMPLE_FMT_FLTP && probe.channels > 0 && probe.sample_rate > 0) {
        AVChannelLayout out_layout{};
        av_channel_layout_copy(&out_layout, &layout);
        int linesize = 0;
        if (swr_alloc_set_opts2(&probe.swr, &out_layout, AV_SAMPLE_FMT_FLTP, probe.sample_rate, &layout, in_fmt,
                                probe.sample_rate, 0, nullptr) >= 0 &&
            swr_init(probe.swr) >= 0 &&
            av_samples_alloc_array_and_samples(&probe.out_data, &linesize, probe.channels, 8192, AV_SAMPLE_FMT_FLTP,
                                               0) >= 0) {
            probe.out_frames = 8192;
        } else if (probe.swr != nullptr) {
            swr_free(&probe.swr);
        }
        av_channel_layout_uninit(&out_layout);
    }
    av_channel_layout_uninit(&layout);
    return static_cast<int>(audio_tracks_.size()) - 1;
}

AudioQcAnalyzer& AnalysisPipeline::audio_qc() {
    // 主轨恒在首位（Prepare 里先建它）
    if (!audio_tracks_.empty())
        return audio_tracks_.front().analyzer;
    return empty_audio_qc_;
}

bool AnalysisPipeline::audio_decoder_ready() const {
    return !audio_tracks_.empty() && audio_tracks_.front().probe.ready;
}

bool AnalysisPipeline::audio_rate_changed() const {
    for (const AudioQcTrack& track : audio_tracks_) {
        if (track.probe.rate_changed)
            return true;
    }
    return false;
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
    inband_probes_.clear();
    bitstream_inband_active_ = false;
    result.bitstream_streams.clear();
    // slice/SEI 巡检器同样要清零，而且**必须在这里无条件清**：以前只在带内分支
    // Reset，于是"上一份文件的 slice 统计"会被 FinishVideoEsInspection 写进这一份，
    // 旧 inspector 还会继续吃新文件的包。（es_inspector_configured_ 这个成员本来
    // 就是为这件事准备的，一直没用 —— 现在用起来了。）
    es_inspector_.Reset(0);
    es_inspector_configured_ = false;

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

    // ---- 本函数自己的两个维度 ----
    //
    // 下面的"参数集全程追踪"与"帧级 POC 追踪"**不属于**码流解析维度：它们只是
    // 借这条路径拿一份参数集（extradata 或带内），各有各的开关。以前整个函数
    // 开头就 `if (!options_.analyze_bitstream) return;`，于是关掉码流解析会同时
    // 关掉这两条 —— 一个开关管了三件事，且界面上无从察觉（时间线一律是空的）。
    if (options_.analyze_bitstream)
        PrepareBitstreamAnalysis(fmt, result, video_order);

    // ---- 参数集全程追踪（每条视频轨一个 tracker）----
    //
    // 必须放在 `if (result.bitstream_analyzed) return;` **之前**：恰恰是 extradata
    // 解析成功的那条主力路径（MP4/MKV）才最需要看"中途有没有换配置"，
    // 放在 return 之后就又变成"只有裸流才追踪"。
    //
    // 与带内 probe 的关系：probe 负责"第一次解析成功"，tracker 负责"之后每一次变化"。
    // 两者互不依赖 —— tracker 不做完整参数集解析，只算摘要 + 读 SPS 宽高。
    parameter_trackers_.clear();
    parameter_timelines_.clear();
    if (options_.analyze_parameter_timeline) {
        for (unsigned i : video_order) {
            const AVStream* st = fmt->streams[i];
            if (st == nullptr || st->codecpar == nullptr)
                continue;
            ParameterTrackerSlot slot;
            slot.stream_index = static_cast<int>(i);
            slot.time_base = av_q2d(st->time_base);
            slot.tracker.Reset(static_cast<int>(st->codecpar->codec_id), static_cast<int>(i));
            if (!slot.tracker.enabled())
                continue;
            slot.tracker.SetFraming(ParameterSetTracker::Framing::Auto, NalLengthSizeFromExtradata(st->codecpar));
            // 与 POC 追踪同理：先登记 extradata 里的参数集当基线。少了这一步，
            // MP4/MKV 上"参数集登记表"是空的，而它空着的时候和"全程没有变化"
            // 长得一模一样 —— 这是最难发现的一类静默失效。
            if (st->codecpar->extradata != nullptr && st->codecpar->extradata_size > 0) {
                slot.tracker.FeedExtradata(st->codecpar->extradata, static_cast<size_t>(st->codecpar->extradata_size));
            }
            parameter_trackers_.push_back(std::move(slot));
        }
        if (parameter_trackers_.empty()) {
            LOG_INFO("参数变化追踪: 没有可追踪的视频轨（编码需为 H.264 / HEVC / AV1）");
        }
    }

    // ---- 帧级显示顺序追踪（H.264 / HEVC / VVC / VP9 / AV1）----
    //
    // 与参数集追踪同样是"extradata 路径也要跑"，所以放在 return 之前。
    // 按编码分派交给 FrameOrderTracker：没有实现的编码会
    // enabled()==false，这里直接不建槽 —— 上层据 frame_order_timelines 为空
    // 把维度标成"不可用"，而不是给一条"0 帧、无重排"的假结论。
    order_trackers_.clear();
    frame_order_timelines_.clear();
    if (options_.analyze_frame_order) {
        for (unsigned i : video_order) {
            const AVStream* st = fmt->streams[i];
            if (st == nullptr || st->codecpar == nullptr)
                continue;
            OrderTrackerSlot slot;
            slot.stream_index = static_cast<int>(i);
            slot.time_base = av_q2d(st->time_base);
            slot.tracker.Reset(static_cast<int>(st->codecpar->codec_id), static_cast<int>(i));
            if (!slot.tracker.enabled())
                continue;
            slot.tracker.SetFraming(FrameOrderTracker::Framing::Auto);
            slot.tracker.SetLengthPrefixBytes(NalLengthSizeFromExtradata(st->codecpar));
            // ❗ 先喂 extradata：MP4/MKV 的参数集只写在 avcC / hvcC 里，码流里
            // 一个 SPS 都没有（libx26x 在 AV_CODEC_FLAG_GLOBAL_HEADER 下会关掉
            // repeat-headers）。少了这一步，"帧顺序"在主力封装上恒为"不可用"，
            // 而原因会被记成"扫到 slice 之前没有 SPS"，指向完全错误的方向。
            if (st->codecpar->extradata != nullptr && st->codecpar->extradata_size > 0) {
                slot.tracker.FeedExtradata(st->codecpar->extradata, static_cast<size_t>(st->codecpar->extradata_size));
            }
            LOG_INFO("帧级顺序追踪: 流 #" + std::to_string(i) + " 用 " + slot.tracker.codec_name() +
                     " 解析，顺序值来源 " + slot.tracker.order_source_name());
            order_trackers_.push_back(std::move(slot));
        }
        if (order_trackers_.empty()) {
            LOG_INFO("帧级顺序追踪: 没有可追踪的视频轨（当前覆盖 H.264 / HEVC / VVC（POC）、"
                     "VP9（显示事件序）与 AV1（order_hint））");
        }
    }

    if (!options_.analyze_bitstream || result.bitstream_analyzed)
        return;

    // ---- 带内兜底：extradata 缺失的封装（TS / 裸流 / fMP4 分片 / WebM-VP9）----
    //
    // ❗ 注意用 continue 而不是 return：第一条视频流可能是没有解析器的编码
    // （如 MPEG-2），但第二条是 H.264 —— 那种文件仍然应该能分析。
    //
    // ❗ 而且每条轨都要挂一个 probe：多节目 TS 的每条节目是独立的 PID、独立的
    // 参数集，一条 probe 只能覆盖一条流 —— 以前只有 inband_probe_ 一个成员，
    // 第二条节目彻底没人看（这正是 C3 里"多节目/多角度漏"的一半成因）。
    for (unsigned i : video_order) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->extradata != nullptr && st->codecpar->extradata_size > 0)
            continue;

        InBandParameterSetProbe probe;
        probe.Reset(static_cast<int>(st->codecpar->codec_id));
        if (!probe.enabled())
            continue;
        probe.SetContainerMetadata(ContainerMetadataFromStream(st, result, i));
        inband_probes_.push_back({static_cast<int>(i), std::move(probe)});
        LOG_INFO("码流解析: 流 #" + std::to_string(i) + " 无 extradata，改为带内参数集探测");
    }
    if (inband_probes_.empty())
        return;
    // 只有真的有人在探测时才让逐包分发进那段循环
    bitstream_inband_active_ = true;
}

// 码流解析维度自己的准备动作：extradata 参数集解析 + slice/SEI 巡检器初始化。
//
// 与 PrepareBitstream 分开是因为这两段**只**属于 analyze_bitstream 维度，
// 而 PrepareBitstream 里还挂着参数集追踪与 POC 追踪 —— 那两条有自己的开关，
// 不能因为"关掉了码流解析"就一起停掉（见 PrepareBitstream 里的同名注释）。
void AnalysisPipeline::PrepareBitstreamAnalysis(AVFormatContext* fmt, model::AnalysisResult& result,
                                                const std::vector<unsigned>& video_order) {
    // ---- libavcodec extradata 路径：逐条视频轨都解析一次 ----
    //
    // 以前这里 `break` 在第一个成功的轨道上 —— TS 多节目 / 多机位 MP4 里第二条
    // 视频轨（往往才是真正出问题的那条）永远不会被看一眼。现在所有轨道各产一份
    // 独立结果：主轨（第一个成功的）照样写 bitstream_analysis，全量写
    // bitstream_streams。
    //
    // ❗ 复杂的"谁当主轨"优先级不要在这里重复判断：video_order 已经把主轨排在第一，
    // 遇到的第一个 analyzed 结果天然就是主轨的结论。
    for (unsigned i : video_order) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->extradata == nullptr || st->codecpar->extradata_size <= 0)
            continue;

        BitstreamAnalyzer bitstream;
        bitstream.SetContainerMetadata(ContainerMetadataFromStream(st, result, i));
        model::BitstreamAnalysisResult bs =
            bitstream.Analyze(st->codecpar->extradata, static_cast<size_t>(st->codecpar->extradata_size),
                              static_cast<int>(st->codecpar->codec_id));
        bs.stream_index = static_cast<int>(i);
        if (!bs.analyzed)
            continue;

        if (!result.bitstream_analyzed) {
            result.bitstream_analysis = bs; // 主轨（第一个成功的）
            result.bitstream_analyzed = true;
            LOG_INFO("码流解析: codec=" + bs.codec_name + " 流#" + std::to_string(i) + " " + std::to_string(bs.width) +
                     "x" + std::to_string(bs.height) + " 不一致=" + std::to_string(bs.inconsistencies.size()));
        } else {
            LOG_INFO("码流解析(附加视频轨 #" + std::to_string(i) + "): codec=" + bs.codec_name + " " +
                     std::to_string(bs.width) + "x" + std::to_string(bs.height));
        }
        result.bitstream_streams.push_back(std::move(bs));
    }

    // ---- slice header / SEI 巡检（C4）----
    //
    // ❗ 这一段必须放在 `if (result.bitstream_analyzed) return;` **之前**。
    // 以前它在之后，于是凡是 extradata 解析成功的文件（MP4/MOV/大多数封装）
    // 巡检器压根没启用过 —— slice/SEI 这条功能在主力路径上是零产出，而不是
    // "数据不全"。顺序也决定了参数集从哪儿来：
    //   * extradata 路径：参数集已经解出来了，直接注入，QP 立刻可算；
    //   * 带内路径：Prepare 阶段还没有参数集，等 OnPacket 里探到再补
    //     （见 ApplyParameterSetsToEsInspector 的调用点）。
    if (video_stream_index_ >= 0) {
        const AVCodecParameters* par = fmt->streams[video_stream_index_]->codecpar;
        es_inspector_.Reset(static_cast<int>(par->codec_id));
        // 媒体包的 NAL 封装方式。MP4/MOV/fMP4 是 `[N 字节长度][NAL]`，
        // 一条起始码都没有 —— 只按 Annex B 抽会得到"零个 NAL"，而零个 NAL
        // 在外观上和"这段流没有 slice/SEI"完全一样，属于静默失效。
        // 宽度未知时交给 Auto 逐包判定（先看起始码，再验证长度前缀链）。
        es_inspector_.SetNalFraming(VideoEsInspector::NalFraming::Auto, NalLengthSizeFromExtradata(par));
        if (result.bitstream_analyzed && result.bitstream_analysis.stream_index == video_stream_index_) {
            ApplyParameterSetsToEsInspector(result.bitstream_analysis);
            es_inspector_configured_ = true;
        }
    }
}

// 把已解析的参数集注入 slice/SEI 巡检器。带内路径（TS 等）在 OnPacket 里
// 解出参数集之后会**再调一次** —— 那时才能真正算出 QP。
void AnalysisPipeline::ApplyParameterSetsToEsInspector(const model::BitstreamAnalysisResult& bs) {
    if (bs.has_h264) {
        es_inspector_.SetH264ParameterSets(bs.h264_sps, bs.h264_pps);
    } else if (bs.has_hevc) {
        es_inspector_.SetHevcParameterSets(bs.hevc_sps, bs.hevc_pps);
    }
}

void AnalysisPipeline::FinishVideoEsInspection(model::AnalysisResult& result) {
    if (!es_inspector_.enabled())
        return;
    es_inspector_.Finish();
    // 参数集没拿到时 H.264 的 QP 一定算不出来（slice header 的位宽全依赖 SPS/PPS）。
    // 留一条日志，免得排查时看到"slice 计数有、QP 却是空"只能靠猜。
    if (!es_inspector_configured_ && es_inspector_.result().slices.supported) {
        LOG_INFO("slice/SEI 巡检: 未拿到参数集（无 extradata 且带内未探到），slice QP 不可用");
    }
    if (result.bitstream_analyzed) {
        result.bitstream_analysis.slice_sei = es_inspector_.result();
    }
}

// ---- 每个包一次 ----

void AnalysisPipeline::OnPacket(AVFormatContext* fmt, AVPacket* pkt, model::AnalysisResult& result,
                                int64_t packet_index) {
    AVStream* st = fmt->streams[pkt->stream_index];
    const double tb = av_q2d(st->time_base);
    // 封面图走的是"一帧 MJPEG 视频流"，不该进任何视频统计（GOP/码率/色彩/时间轴）
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !IsAnalysisVideoStream(st))
        return;
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
        timing.dts_ms =
            (pkt->dts != AV_NOPTS_VALUE) ? static_cast<double>(pkt->dts) * tb * 1000.0 : model::kNoTimestamp;
        timing.duration_ms =
            (pkt->duration > 0) ? static_cast<double>(pkt->duration) * tb * 1000.0 : model::kNoTimestamp;
        timing.pos = pkt->pos;
        timing.size = pkt->size;
        timing.flags = pkt->flags;
        timing.key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        timeline_.OnPacket(timing);
    }

    // 字幕 / 时码 / 辅助数据轨（内部先按流号过滤，非目标流直接返回）
    if (options_.analyze_subtitle)
        subtitle_.OnPacket(pkt, st);
    if (options_.analyze_timecode)
        timecode_.OnPacket(pkt, st);
    if (options_.analyze_aux_data)
        aux_.OnPacket(pkt, st);

    // 音频位流头（D1）：与下面的解码通路是两条独立链路，这里只读包字节。
    if (options_.analyze_audio_bitstream)
        audio_bitstream_.OnPacket(pkt, st);

    // 音频 QC：解码后归一为 float planar 再喂给分析器。
    //
    // 多音轨时这里要按包所属流号分派 —— 以前只有 `pkt->stream_index == audio_stream_index_`
    // 一个条件，第二条音轨的包被直接丢掉，于是"只测主轨"这件事连日志都没有。
    if (options_.analyze_audio_qc && !audio_tracks_.empty()) {
        for (AudioQcTrack& track : audio_tracks_) {
            if (track.stream_index != pkt->stream_index || !track.probe.ready)
                continue;
            PumpAudioPacket(track, pkt);
        }
    }

    if (is_video) {
        // 关键帧间隔与 GOP 帧数（逐包扫描就能拿到，不需要解码）
        frames_since_key_[pkt->stream_index] += 1;

        // ---- slice header / SEI 巡检（C4）----
        // 只盯主视频轨，且只看前若干个包：QP 分布与 SEI 密度进一两个完整 GOP
        // 就收敛了，往后只是重复。Inspector 内部有 max_packets 上限，到点自己收工。
        if (es_inspector_.enabled() && pkt->stream_index == video_stream_index_) {
            es_inspector_.Feed(pkt->data, pkt->size);
        }

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
                    result.gop_frame_sizes.push_back(static_cast<int>(frames_since_key_[pkt->stream_index]));
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
            const double sample_ts =
                has_pts ? ts : ((pkt->dts != AV_NOPTS_VALUE) ? static_cast<double>(pkt->dts) * tb : 0.0);
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
                        const bool frame_is_idr = (frame_probe_.frame->flags & AV_FRAME_FLAG_KEY) != 0;
                        const double frame_ts = (frame_probe_.frame->pts != AV_NOPTS_VALUE)
                                                    ? static_cast<double>(frame_probe_.frame->pts) * tb
                                                    : sample_ts;
                        decoded.push_back(
                            {frame_ts, static_cast<model::FrameType>(frame_probe_.frame->pict_type), frame_is_idr});
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
                av_parser_parse2(frame_probe_.parser, frame_probe_.parser_ctx, &out_data, &out_size, pkt->data,
                                 pkt->size, pkt->pts, pkt->dts, pkt->pos);
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
                        frame_type = static_cast<model::FrameType>(frame_probe_.parser->pict_type);
                        if (frame_probe_.parser->key_frame >= 1) {
                            is_idr = true;
                        } else if (frame_probe_.parser->key_frame == 0) {
                            is_idr = false;
                        }
                    }
                    std::vector<BitrateGopAnalyzer::FrameEvent> events{{sample_ts, frame_type, is_idr}};
                    bitrate_gop_.OnFramesClaimPending(pkt->stream_index, events);
                }
                // 本包不含完整帧（裸流/TS 分片场景）：字节留在池里，不猜帧。
            } else {
                // 解码器与 parser 都不可用：包级回退（一包一帧，类型未知）
                bitrate_gop_.OnPacket(pkt->stream_index, sample_ts, pkt->size, is_idr);
            }
        }

        // 参数集全程追踪：与上面的带内 probe 是两条独立链路。
        // probe 攒到第一次成功就收工；这里逐包看参数集有没有变（含 extradata 路径）。
        // 成本只有"每包一次 NAL 扫描"，所以放在最外层无条件跑。
        for (ParameterTrackerSlot& slot : parameter_trackers_) {
            if (slot.stream_index != pkt->stream_index)
                continue;
            double pts_seconds = -1.0;
            if (pkt->pts != AV_NOPTS_VALUE && slot.time_base > 0.0)
                pts_seconds = static_cast<double>(pkt->pts) * slot.time_base;
            slot.tracker.Feed(pkt->data, static_cast<size_t>(pkt->size), pts_seconds, packet_index, pkt->pos);
        }

        // 帧级显示顺序：H.264 / HEVC / VP9 / AV1 轨，同样在 extradata 路径上也要扫。
        // 时间戳用 PTS（显示顺序的时间），不是 DTS —— 这一层就是来比对显示顺序的。
        for (OrderTrackerSlot& slot : order_trackers_) {
            if (slot.stream_index != pkt->stream_index)
                continue;
            const bool has_time = (pkt->pts != AV_NOPTS_VALUE && slot.time_base > 0.0);
            const double pts_seconds = has_time ? static_cast<double>(pkt->pts) * slot.time_base : 0.0;
            slot.tracker.Feed(pkt->data, static_cast<size_t>(pkt->size), has_time, pts_seconds, packet_index);
        }

        // 带内参数集（extradata 为空的封装）：攒到参数集出现为止。
        //
        // 放在 GOP/码率之后是因为那两段不依赖参数集。每条视频轨一个 probe：
        // 多节目 TS / 多机位流里每条节目有自己的 SPS/VPS，主轨收工不代表别的轨
        // 就该放弃 —— 所以这里是"各自独立收工"，而不是"第一个成功就全部停下"。
        //
        // 第一个成功的结果（= video_order 里最早的那条）回填 bitstream_analysis
        // 当主结论，其余继续 append 到 bitstream_streams。
        if (!inband_probes_.empty() && bitstream_inband_active_) {
            for (auto it = inband_probes_.begin(); it != inband_probes_.end();) {
                auto& slot = *it;
                if (slot.probe.done() || slot.probe.given_up() || slot.stream_index != pkt->stream_index) {
                    ++it;
                    continue;
                }
                if (slot.probe.Feed(pkt->data, pkt->size)) {
                    model::BitstreamAnalysisResult bs = slot.probe.result();
                    bs.stream_index = slot.stream_index;
                    if (!result.bitstream_analyzed) {
                        result.bitstream_analysis = bs;
                        result.bitstream_analyzed = true;
                        LOG_INFO("码流解析(带内): codec=" + bs.codec_name + " " + std::to_string(bs.width) + "x" +
                                 std::to_string(bs.height) + " 流#" + std::to_string(slot.stream_index) +
                                 " 用包数=" + std::to_string(slot.probe.packets()) +
                                 " 不一致=" + std::to_string(bs.inconsistencies.size()));
                    } else {
                        LOG_INFO("码流解析(带内, 附加视频轨 #" + std::to_string(slot.stream_index) +
                                 "): codec=" + bs.codec_name);
                    }
                    // 带内路径刚拿到参数集 —— 趁热补给巡检器，让后面的包能算出 QP。
                    // ❗ 只补给主视频轨：巡检器只盯主轨，把附加轨的参数集塞进去
                    // 会让 slice header 按另一条流的位宽去解，算出来的 QP 是垃圾。
                    if (slot.stream_index == video_stream_index_) {
                        ApplyParameterSetsToEsInspector(bs);
                        es_inspector_configured_ = true;
                    }
                    result.bitstream_streams.push_back(std::move(bs));
                    it = inband_probes_.erase(it);
                    continue;
                }
                if (slot.probe.given_up()) {
                    // 放弃要留日志：否则用户只看到"码流分析没结果"，查不出是没找到参数集
                    LOG_INFO("带内参数集探测放弃(流#" + std::to_string(slot.stream_index) +
                             "): " + slot.probe.GiveUpReason());
                    it = inband_probes_.erase(it);
                    continue;
                }
                ++it;
            }
            if (inband_probes_.empty())
                bitstream_inband_active_ = false;
        }
    }

    // 色彩与 HDR：包级 side data 补充 + 需要时解码首帧读 AVFrame side data
    if (options_.analyze_color_hdr && is_video && pkt->stream_index == color_video_stream_index_) {
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
                        if (color_probe_.frames_read >= options_.color_hdr_options.max_probe_frames) {
                            break;
                        }
                    }
                }
            }
        }
    }
}

// ---- 扫描结束后的钩子 ----

void AnalysisPipeline::BeforeClose(AVFormatContext* fmt, model::AnalysisResult& result) {
    // 部分封装的 HDR 元数据要读到数据包后才补充进 codecpar，关闭输入前再刷一次
    if (options_.analyze_color_hdr && color_video_stream_index_ >= 0) {
        color_hdr_.UpdateFromStream(fmt->streams[color_video_stream_index_]);
    }

    // slice / SEI 巡检收尾：汇总 QP 分布与 SEI 元数据，回填主轨的码流结果。
    // 挂在 BeforeClose 是因为它不依赖任何解码上下文 —— 只要在关YYYY前跑掉即可，
    // 而 BeforeClose 是"关之前最后还能读到 exporter 状态"的那个点。
    FinishVideoEsInspection(result);

    // 参数变化时间线收尾：这里要算"引用了从未传输的参数集"——只有扫完全部包
    // 才能确定某份 PPS 到底出现过没有，所以只能在收尾时结账。
    parameter_timelines_.clear();
    for (ParameterTrackerSlot& slot : parameter_trackers_) {
        slot.tracker.Finish();
        const model::ParameterTimeline& timeline = slot.tracker.result();
        parameter_timelines_.push_back(timeline);
        if (!timeline.changes.empty() || timeline.missing_reference_count > 0) {
            LOG_INFO("参数变化追踪(流#" + std::to_string(slot.stream_index) + "): " + timeline.Summary());
        }
    }

    // 帧级显示顺序收尾：reorder / GOP / 重复 / 缺口都在这里结账
    frame_order_timelines_.clear();
    for (OrderTrackerSlot& slot : order_trackers_) {
        // 拷一份再落：FrameOrderTimeline 不带流号（domain 不认 AVStream），
        // 而报告要按流分节展示，流号只能在这里补。编码名同理（只用于文案）。
        model::FrameOrderTimeline timeline = slot.tracker.Finish();
        timeline.stream_index = slot.stream_index;
        timeline.codec = slot.tracker.codec_name();
        frame_order_timelines_.push_back(timeline);
        LOG_INFO("帧级顺序(流#" + std::to_string(slot.stream_index) + "): " + timeline.Summary());
    }
}

void AnalysisPipeline::FlushAudio() {
    if (!options_.analyze_audio_qc)
        return;

    for (AudioQcTrack& track : audio_tracks_) {
        AudioQcProbe& probe = track.probe;
        if (!probe.ready)
            continue;

        avcodec_send_packet(probe.decoder, nullptr);
        while (avcodec_receive_frame(probe.decoder, probe.frame) >= 0) {
            FeedAudioFrame(track, probe.frame);
            av_frame_unref(probe.frame);
        }
        if (probe.swr != nullptr && probe.out_data != nullptr && probe.channels > 0) {
            std::vector<const float*> planes(static_cast<size_t>(probe.channels));
            int got = swr_convert(probe.swr, probe.out_data, probe.out_frames, nullptr, 0);
            while (got > 0) {
                for (int c = 0; c < probe.channels; ++c) {
                    planes[static_cast<size_t>(c)] = reinterpret_cast<const float*>(probe.out_data[c]);
                }
                track.analyzer.OnSamples(planes.data(), got, -1.0);
                got = swr_convert(probe.swr, probe.out_data, probe.out_frames, nullptr, 0);
            }
        }
    }
}

void AnalysisPipeline::ReleaseProbes() {
    frame_probe_.Release();
    color_probe_.Release();
    for (AudioQcTrack& track : audio_tracks_) {
        if (track.probe.ready)
            track.probe.Release();
    }
}

// EOF 时帧类型解码器里可能还压着几帧（帧重排序延迟）。这些帧的归属包在扫描期
// 已按"0 帧产出"走过 OnPacketBytes（字节已入账、躺在待认领池里），这里把帧补进
// 统计并让它们认领池里剩下的字节 —— 不 flush 的话"总帧数 < 视频包数"，I/P/B
// 比例与 GOP 统计失真；不认领的话这些帧就是 0 字节，平均帧大小与 GOP 大小偏小。
void AnalysisPipeline::FlushFrameProbe() {
    if (!options_.analyze_bitrate_gop || !frame_probe_.use_decoder)
        return;
    if (video_stream_index_ < 0)
        return;
    if (frame_probe_.decoder == nullptr || frame_probe_.frame == nullptr)
        return;
    if (avcodec_send_packet(frame_probe_.decoder, nullptr) != 0)
        return;
    std::vector<BitrateGopAnalyzer::FrameEvent> pending;
    while (avcodec_receive_frame(frame_probe_.decoder, frame_probe_.frame) >= 0) {
        const bool is_idr = (frame_probe_.frame->flags & AV_FRAME_FLAG_KEY) != 0;
        const double frame_ts = (frame_probe_.frame->pts != AV_NOPTS_VALUE && video_time_base_ > 0.0)
                                    ? static_cast<double>(frame_probe_.frame->pts) * video_time_base_
                                    : -1.0;
        pending.push_back({frame_ts, static_cast<model::FrameType>(frame_probe_.frame->pict_type), is_idr});
        av_frame_unref(frame_probe_.frame);
    }
    // 池里剩下的字节（最后几个包）归这批帧：它们的包确实已经全部送进了解码器。
    if (!pending.empty())
        bitrate_gop_.OnFramesClaimPending(video_stream_index_, pending);
}

// ---- 内部 ----

// 把一个音频包送进指定音轨的解码器，并把解出的帧全部喂给该轨的分析器。
void AnalysisPipeline::PumpAudioPacket(AudioQcTrack& track, const AVPacket* pkt) {
    AudioQcProbe& probe = track.probe;
    if (!probe.ready)
        return;
    const int send_ret = avcodec_send_packet(probe.decoder, pkt);
    // EAGAIN 表示解码器还压着包，不是错误；其余负值才是"这个包解不了"。
    if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
        track.analyzer.NoteDecodeErrorPacket();
        return;
    }
    // ❗ 三种收尾必须分开记账：以前只写 `>= 0` 就继续，解码失败被当成
    // "这包没有帧"，结果里只留下"已分析"，看不出覆盖不足。
    while (true) {
        const int recv_ret = avcodec_receive_frame(probe.decoder, probe.frame);
        if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF)
            break;
        if (recv_ret < 0) {
            track.analyzer.NoteDecodeErrorFrame();
            break;
        }
        FeedAudioFrame(track, probe.frame);
        av_frame_unref(probe.frame);
    }
}

// 喂一帧解码后的音频给 QC 分析器（内部按 100 ms 步进出块）
void AnalysisPipeline::FeedAudioFrame(AudioQcTrack& track, AVFrame* frame) {
    AudioQcProbe& probe = track.probe;
    if (!probe.ready || frame == nullptr || frame->nb_samples <= 0)
        return;
    const int channels = frame->ch_layout.nb_channels;
    // extended_data 在声道数 > AV_NUM_DATA_POINTERS(8) 时才是完整的那一份：
    // data[] 只有前 8 个平面，9~N 声道全是 nullptr（12 声道 S16P 实测如此）。
    if (channels <= 0 || frame->extended_data == nullptr) {
        track.analyzer.NoteDroppedFrame(frame->nb_samples);
        return;
    }
    if (frame->sample_rate != probe.sample_rate || channels != probe.channels) {
        probe.rate_changed = true; // 参数中途变化（罕见），该帧丢弃
        track.analyzer.NoteDroppedFrame(frame->nb_samples);
        return;
    }
    // 时间轴换算必须用**本轨**的时基：不同音轨的 time_base 可以不同，
    // 共用一个成员会把第二轨的 PTS 换算成完全错的秒数（响度曲线的时间轴会歪）。
    double ts = (frame->pts != AV_NOPTS_VALUE && track.time_base > 0.0)
                    ? static_cast<double>(frame->pts) * track.time_base
                    : -1.0;

    std::vector<const float*> planes(static_cast<size_t>(channels));
    if (frame->format == AV_SAMPLE_FMT_FLTP) {
        for (int c = 0; c < channels; ++c) {
            planes[static_cast<size_t>(c)] = reinterpret_cast<const float*>(frame->extended_data[c]);
        }
        track.analyzer.OnSamples(planes.data(), frame->nb_samples, ts);
        return;
    }
    if (probe.swr == nullptr || probe.out_data == nullptr) {
        // 重采样器建不起来：非 FLTP 的帧一个都进不了统计，必须记账
        track.analyzer.NoteDroppedFrame(frame->nb_samples);
        return;
    }

    int got = swr_convert(probe.swr, probe.out_data, probe.out_frames,
                          const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    while (got > 0) {
        for (int c = 0; c < channels; ++c) {
            planes[static_cast<size_t>(c)] = reinterpret_cast<const float*>(probe.out_data[c]);
        }
        track.analyzer.OnSamples(planes.data(), got, ts);
        ts = -1.0; // 后续块沿用内部时钟
        got = swr_convert(probe.swr, probe.out_data, probe.out_frames, nullptr, 0);
    }
}

} // namespace videoeye
