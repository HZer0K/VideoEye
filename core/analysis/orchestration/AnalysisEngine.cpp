#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
// 下面这些分析器是本 cpp 真正要用到的执行者。以前是 AnalysisResult.h 顺带把它们
// 全带进来的 —— 那个头文件现在只认 domain 的结果类型，于是"谁用谁 include"。
#include "core/analysis/diagnostics/AuxDataAnalyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/diagnostics/TimecodeAnalyzer.h"
#include "core/analysis/quality/AudioQcAnalyzer.h"
#include "core/analysis/quality/ColorHdrAnalyzer.h"
#include "core/analysis/streaming/SegmentQcAnalyzer.h"
// 打开 / 探测 / 中断 / 取消 / 容器级事实 / 流摘要 都在 AnalysisInputSession 里；
// FFmpeg 中断回调本身住在 core/ffmpeg_io/FfmpegInterrupt.h（叶子模块），由会话去 include。
#include "core/analysis/orchestration/AnalysisInputSession.h"
#include "core/media/probe/FileProbe.h"
#include "core/media/streaming/ManifestText.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include "core/analysis/quality/BitrateGopAnalyzer.h"
}

namespace videoeye {
namespace analyzer {
namespace {

constexpr int kProgressMinIntervalMs = 100;
constexpr int kProgressMinPackets = 2000;
// ---- FFmpeg 中断机制 ----
// AvInterruptState / AvIoInterruptCallback / kOpenTimeoutUs / kProbeTimeoutUs
// 已抽到 core/ffmpeg_io/FfmpegInterrupt.h 供所有分析器共享（见上方 include）。
// 打开/探测阶段对它们的具体用法（含 moov 顺序扫描、MP4 sample table、流摘要）
// 已随 AnalysisInputSession 一起搬走，见 core/analysis/orchestration/AnalysisInputSession.cpp。

// 流媒体清单的扩展名。清单是纯文本，扩展名是唯一的识别手段
// （魔数检测对 "#EXTM3U" / "<MPD" 无能为力）。
bool IsStreamingManifestExtension(const std::string& ext) {
    return ext == "m3u8" || ext == "m3u" || ext == "mpd";
}

struct Bucket {
    int64_t total_bytes = 0;
    int64_t video_bytes = 0;
    int64_t video_frames = 0;
};

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

// 音频 QC 的解码通路：解码器 + 到 float planar 的转换（swr）。
// 解码器输出本身就是 FLTP 时直接用 AVFrame::data，省掉一次无谓的拷贝。
struct AudioQcProbe {
    bool ready = false;
    bool rate_changed = false;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    uint8_t** out_data = nullptr;
    int out_frames = 0;
    int channels = 0;
    int sample_rate = 0;
    std::vector<model::AudioChannelInfo> channel_info;

    void Release() {
        if (out_data) {
            av_freep(&out_data[0]);
            av_freep(&out_data);
        }
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (decoder) avcodec_free_context(&decoder);
        ready = false;
    }
};

// 帧类型探测：优先用解码器（准确但慢），否则用 codec parser（几乎零成本）。
// 两者都拿不到时，调用方退回 OnPacket()，只按 AV_PKT_FLAG_KEY 识别 I 帧。
struct FrameTypeProbe {
    AVCodecContext* parser_ctx = nullptr;    // 仅供 av_parser_parse2 使用的参数上下文
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    bool use_decoder = false;

    void Release() {
        if (parser) av_parser_close(parser);
        if (parser_ctx) avcodec_free_context(&parser_ctx);
        if (frame) av_frame_free(&frame);
        if (decoder) avcodec_free_context(&decoder);
        parser = nullptr;
        parser_ctx = nullptr;
        frame = nullptr;
        decoder = nullptr;
        use_decoder = false;
    }
};

// 色彩与 HDR 的帧级兜底探测：容器/码流层信息不全时，解码前几帧读 AVFrame side data
// （HDR10+/DV RPU 这类动态元数据通常只在解码帧上出现）
struct ColorFrameProbe {
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    bool opened = false;
    bool failed = false;
    int frames_read = 0;

    bool Ready() const { return decoder != nullptr && frame != nullptr && !failed; }

    // 惰性打开解码器；打开失败会被记住，不会重复尝试
    bool EnsureOpen(AVStream* stream) {
        if (failed) return false;
        if (opened) return Ready();
        opened = true;
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec != nullptr) {
            decoder = avcodec_alloc_context3(codec);
            if (decoder != nullptr &&
                avcodec_parameters_to_context(decoder, stream->codecpar) >= 0) {
                decoder->pkt_timebase = stream->time_base;
                frame = av_frame_alloc();
                if (frame != nullptr && avcodec_open2(decoder, codec, nullptr) == 0) {
                    return true;
                }
            }
        }
        Release();
        failed = true;
        return false;
    }

    void Release() {
        if (frame) av_frame_free(&frame);
        if (decoder) avcodec_free_context(&decoder);
    }
};

// 回报通道（AnalysisCallbacks）与终态写法（MarkFailed / Notify* / ResultSink /
// kNoFfmpegErrorCode）已搬到 core/analysis/orchestration/AnalysisTerminalState.{h,cpp} ——
// 引擎拆开之后每一片都要用，留在某一个 cpp 的匿名命名空间里就只能被抄走一份。

}  // namespace

bool AnalysisEngine::IsCancelledExit(int ret) const {
    return IsCancelRequested() && ret == AVERROR_EXIT;
}

void AnalysisEngine::Cancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

void AnalysisEngine::Reset() {
    cancel_requested_.store(false, std::memory_order_release);
}

bool AnalysisEngine::IsCancelRequested() const {
    return cancel_requested_.load(std::memory_order_acquire);
}

bool AnalysisEngine::CancelRequested() const {
    if (active_cancel_source_ && active_cancel_source_->load(std::memory_order_acquire))
        return true;
    return cancel_requested_.load(std::memory_order_acquire);
}

// 返回可变指针而不是 const: 调用方（FFmpeg 的 AvInterruptState）要往里置位，
// 而"能不能改"这件事由谁能调 Cancel() 决定，跟"这次读的是哪颗标志"是两回事。
// 本方法是 const 的事实不变 —— 它只读自己，不因多返回一个可写引用就获得改自身的权限。
std::atomic<bool>* AnalysisEngine::CancelSource() const {
    if (active_cancel_source_)
        return active_cancel_source_.get();
    return const_cast<std::atomic<bool>*>(&cancel_requested_);
}

void AnalysisEngine::Run(const std::string& file_path, const AnalysisOptions& options,
                         const AnalysisCallbacks& callbacks, model::AnalysisResult* out_result,
                         std::shared_ptr<std::atomic<bool>> cancel_source) {
    VE_PERF("AnalysisEngine::Run");
    // 必须第一件事就挂上: 下面的"流媒体清单分流"会立刻走到另一条路径，
    // 那里读的是同一个 CancelSource()，晚一步挂就等于本次 Run 没接外部取消源。
    CancelSourceScope cancel_scope(*this, std::move(cancel_source));
    model::AnalysisResult result;
    result.file_path = file_path;
    // 声明在 result 之后：析构顺序反过来，回传时 result 仍然活着。
    const ResultSink result_sink(out_result, &result);

    // 提取小写扩展名（供"扩展名与实际容器不符"规则使用）
    {
        const size_t slash = file_path.find_last_of("/\\");
        const size_t dot = file_path.find_last_of('.');
        if (dot != std::string::npos &&
            (slash == std::string::npos || dot > slash)) {
            result.file_extension = file_path.substr(dot + 1);
            std::transform(result.file_extension.begin(), result.file_extension.end(),
                           result.file_extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
    }

    // ---- 流媒体清单分流（必须在 avformat_open_input 之前）----
    // FFmpeg 会把 .m3u8 / .mpd 当成 HLS / DASH 播放列表去发网络请求，
    // 离线分析时既不可控（卡在网络超时），也拿不到有意义的时长/码率序列。
    // 所以这里在打开任何 IO 之前就分流到自研的清单解析器。
    if (options.analyze_streaming_package && IsStreamingManifestExtension(result.file_extension) &&
        file_path.find("://") == std::string::npos) {
        RunStreamingManifest(file_path, options, result, callbacks);
        return;
    }

    // ---- 打开 / 探测 / 容器级事实 / 流摘要（AnalysisInputSession）----
    //
    // 取消源交进去而不是自备一颗: 引擎接了外部令牌时，FFmpeg 的中断回调必须看同一颗，
    // 否则"用户点取消"对 avformat_open_input 是隐形的。
    AnalysisInputSession input(file_path, options, CancelSource());
    const AnalysisInputSession::Outcome outcome = input.Open(result);
    if (outcome == AnalysisInputSession::Outcome::Failed) {
        // 失败终态（含 error_message / scan_error_code）已由会话写进 result，这里只发回调。
        // 文案一律取 result.error_message：另拼一份就会出现"回调说的"和"结果里写的"不一致。
        NotifyFailed(callbacks, result.error_message);
        return;
    }
    if (outcome == AnalysisInputSession::Outcome::Cancelled) {
        NotifyCancelled(callbacks, result);
        return;
    }
    // 会话仍然持有上下文，**关闭的时点由本函数决定**（色彩 HDR 要在关闭前刷一次，
    // 字幕 / 时码的 Finish 要在关闭后跑）。
    AVFormatContext* fmt = input.context();
    // 来自容器头的总时长；流摘要与下面几处"没有流级时长时回退"的场合都要用
    const double file_duration = result.duration_seconds;

    // ---- 字幕 / 时码 / 辅助数据轨准备（只在存在对应流时才有实际工作量）----
    SubtitleAnalyzer subtitle_analyzer;
    TimecodeAnalyzer timecode_analyzer;
    AuxDataAnalyzer aux_analyzer;
    if (options.analyze_subtitle) {
        subtitle_analyzer.Reset(options.subtitle_options);
        subtitle_analyzer.RegisterStreams(fmt);
    }
    if (options.analyze_timecode) {
        timecode_analyzer.Reset(options.timecode_options);
        timecode_analyzer.RegisterStreams(fmt, file_duration);
    }
    if (options.analyze_aux_data) {
        aux_analyzer.Reset(options.aux_data_options);
        aux_analyzer.RegisterStreams(fmt);
    }

    // ---- 编码码流解析（只读 extradata，不解码；必须在 avformat_close_input 之前）----
    // 只取第一条视频流：诊断关心的是主视频的编码参数，多视频流取第一条足够。
    if (options.analyze_bitstream) {
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

    // ---- 色彩与 HDR 元数据分析（读 AVCodecParameters + coded_side_data，几乎零成本）----
    int color_video_stream_index = -1;
    if (options.analyze_color_hdr) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                color_video_stream_index = static_cast<int>(i);
                break;
            }
        }
    }
    ColorHdrAnalyzer color_hdr;
    ColorFrameProbe color_probe;
    if (options.analyze_color_hdr && color_video_stream_index >= 0) {
        color_hdr.Reset(options.color_hdr_options);
        color_hdr.UpdateFromStream(fmt->streams[color_video_stream_index]);
    }

    // ---- 码率与 GOP 深度分析准备 ----
    int video_stream_index = -1;
    if (options.analyze_bitrate_gop) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = static_cast<int>(i);
                break;
            }
        }
    }

    BitrateGopAnalyzer bitrate_gop;
    FrameTypeProbe probe;
    if (options.analyze_bitrate_gop && video_stream_index >= 0) {
        bitrate_gop.Reset(options.bitrate_gop_options);
        AVStream* vst = fmt->streams[video_stream_index];

        if (options.decode_frame_types) {
            const AVCodec* codec = avcodec_find_decoder(vst->codecpar->codec_id);
            if (codec != nullptr) {
                probe.decoder = avcodec_alloc_context3(codec);
                if (probe.decoder != nullptr &&
                    avcodec_parameters_to_context(probe.decoder, vst->codecpar) >= 0) {
                    probe.decoder->pkt_timebase = vst->time_base;
                    probe.frame = av_frame_alloc();
                    if (probe.frame != nullptr &&
                        avcodec_open2(probe.decoder, codec, nullptr) == 0) {
                        probe.use_decoder = true;
                    }
                }
                if (!probe.use_decoder) {
                    // 打开失败 → 退回 parser
                    if (probe.frame) { av_frame_free(&probe.frame); }
                    if (probe.decoder) { avcodec_free_context(&probe.decoder); }
                }
            }
        }
        if (!probe.use_decoder) {
            probe.parser = av_parser_init(vst->codecpar->codec_id);
            if (probe.parser != nullptr) {
                // 我们喂的是完整访问单元（一个包 = 一帧），告诉解析器不要做拼接
                probe.parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
                probe.parser_ctx = avcodec_alloc_context3(nullptr);
                if (probe.parser_ctx != nullptr) {
                    avcodec_parameters_to_context(probe.parser_ctx, vst->codecpar);
                }
            }
        }
    }

    // ---- 音频 QC 准备（解码 + 归一到 float planar）----
    int audio_stream_index = -1;
    if (options.analyze_audio_qc) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = static_cast<int>(i);
                break;
            }
        }
    }
    const double audio_time_base =
        (audio_stream_index >= 0) ? av_q2d(fmt->streams[audio_stream_index]->time_base) : 0.0;

    AudioQcAnalyzer audio_qc;
    AudioQcProbe audio_probe;
    if (options.analyze_audio_qc && audio_stream_index >= 0) {
        audio_qc.Reset(options.audio_qc_options);
        AVStream* ast = fmt->streams[audio_stream_index];
        const AVCodec* codec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (codec != nullptr) {
            audio_probe.decoder = avcodec_alloc_context3(codec);
            if (audio_probe.decoder != nullptr &&
                avcodec_parameters_to_context(audio_probe.decoder, ast->codecpar) >= 0) {
                audio_probe.decoder->pkt_timebase = ast->time_base;
                if (avcodec_open2(audio_probe.decoder, codec, nullptr) == 0) {
                    audio_probe.frame = av_frame_alloc();
                    audio_probe.ready = (audio_probe.frame != nullptr);
                }
            }
            if (!audio_probe.ready) {
                if (audio_probe.frame) av_frame_free(&audio_probe.frame);
                if (audio_probe.decoder) avcodec_free_context(&audio_probe.decoder);
            }
        }

        if (audio_probe.ready) {
            AVChannelLayout layout{};
            av_channel_layout_copy(&layout, &audio_probe.decoder->ch_layout);
            audio_probe.channels = layout.nb_channels;
            audio_probe.sample_rate = audio_probe.decoder->sample_rate;

            for (int c = 0; c < audio_probe.channels; ++c) {
                const AVChannel ch = av_channel_layout_channel_from_index(&layout, c);
                char name_buf[32] = {0};
                av_channel_name(name_buf, sizeof(name_buf), ch);
                const std::string name = (name_buf[0] != '\0') ? std::string(name_buf)
                                                               : ("Ch" + std::to_string(c + 1));
                audio_probe.channel_info.emplace_back(name, AudioChannelRoleOf(ch));
            }

            const AVSampleFormat in_fmt = audio_probe.decoder->sample_fmt;
            const char* fmt_name = av_get_sample_fmt_name(in_fmt);
            audio_qc.SetStreamInfo(audio_probe.sample_rate, audio_probe.channels,
                                   fmt_name ? fmt_name : "",
                                   av_get_bytes_per_sample(in_fmt) * 8);
            audio_qc.SetChannelInfo(audio_probe.channel_info);

            // 非 float planar 输出需要 swr 转换（s16 / flt / s32 ...）
            if (in_fmt != AV_SAMPLE_FMT_FLTP && audio_probe.channels > 0 &&
                audio_probe.sample_rate > 0) {
                AVChannelLayout out_layout{};
                av_channel_layout_copy(&out_layout, &layout);
                int linesize = 0;
                if (swr_alloc_set_opts2(&audio_probe.swr, &out_layout, AV_SAMPLE_FMT_FLTP,
                                        audio_probe.sample_rate, &layout, in_fmt,
                                        audio_probe.sample_rate, 0, nullptr) >= 0 &&
                    swr_init(audio_probe.swr) >= 0 &&
                    av_samples_alloc_array_and_samples(&audio_probe.out_data, &linesize,
                                                       audio_probe.channels, 8192,
                                                       AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                    audio_probe.out_frames = 8192;
                } else if (audio_probe.swr != nullptr) {
                    swr_free(&audio_probe.swr);
                }
                av_channel_layout_uninit(&out_layout);
            }
            av_channel_layout_uninit(&layout);
        }
    }

    // 喂一帧解码后的音频给 QC 分析器（内部按 100 ms 步进出块）
    auto feed_audio_frame = [&](AVFrame* frame) {
        if (!audio_probe.ready || frame == nullptr || frame->nb_samples <= 0) return;
        const int channels = frame->ch_layout.nb_channels;
        if (channels <= 0) return;
        if (frame->sample_rate != audio_probe.sample_rate || channels != audio_probe.channels) {
            audio_probe.rate_changed = true;   // 参数中途变化（罕见），该帧丢弃
            return;
        }
        double ts = (frame->pts != AV_NOPTS_VALUE && audio_time_base > 0.0)
                        ? static_cast<double>(frame->pts) * audio_time_base
                        : -1.0;

        std::vector<const float*> planes(static_cast<size_t>(channels));
        if (frame->format == AV_SAMPLE_FMT_FLTP) {
            for (int c = 0; c < channels; ++c) {
                planes[static_cast<size_t>(c)] = reinterpret_cast<const float*>(frame->data[c]);
            }
            audio_qc.OnSamples(planes.data(), frame->nb_samples, ts);
            return;
        }
        if (audio_probe.swr == nullptr || audio_probe.out_data == nullptr) return;

        int got = swr_convert(audio_probe.swr, audio_probe.out_data, audio_probe.out_frames,
                              const_cast<const uint8_t**>(frame->data), frame->nb_samples);
        while (got > 0) {
            for (int c = 0; c < channels; ++c) {
                planes[static_cast<size_t>(c)] =
                    reinterpret_cast<const float*>(audio_probe.out_data[c]);
            }
            audio_qc.OnSamples(planes.data(), got, ts);
            ts = -1.0;   // 后续块沿用内部时钟
            got = swr_convert(audio_probe.swr, audio_probe.out_data, audio_probe.out_frames,
                              nullptr, 0);
        }
    };

    // ---- 逐包扫描 ----
    const double interval = (options.sample_interval_seconds > 0.0) ? options.sample_interval_seconds : 1.0;
    std::map<int64_t, Bucket> buckets;
    std::vector<int64_t> frames_since_key(fmt->nb_streams, 0);
    double last_key_ts = -1.0;
    bool has_key = false;

    AVPacket* pkt = av_packet_alloc();
    int64_t packet_index = 0;
    int64_t last_pos = 0;
    TimelineAnalyzer timeline_analyzer;

    auto last_progress = std::chrono::steady_clock::now();
    NotifyProgress(callbacks, 0.0, "扫描数据包");

    // 逐包扫描的统一收尾：取消和真·IO 错误都要走同一份清理，
    // 免得以后改一处漏一处（漏 avformat_close_input 就是句柄泄漏）。
    //
    // 关上下文必须走 input.Close()：会话自己也持着这枚指针，直接 avformat_close_input(&fmt)
    // 只会清掉这里的局部变量，会话析构时会对已释放的上下文再关一次。
    auto teardown_scan = [&]() {
        av_packet_free(&pkt);
        input.Close();
        fmt = nullptr;
        probe.Release();
        color_probe.Release();
        if (options.analyze_audio_qc && audio_probe.ready) audio_probe.Release();
    };

    {
    VE_PERF("逐包扫描(全文件 demux + 音频解码 + GOP)");
    while (true) {
        if (CancelRequested()) {
            result.scan_status = model::AnalysisStatus::Cancelled;
            break;
        }
        const int ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                result.scan_status = model::AnalysisStatus::Complete;
            } else {
                // 读取数据包阶段出现错误（文件截断 / IO 错误 / 网络中断）。
                // 这与"完整扫到 EOF"不同：必须作为失败处理，不能把半成品当完整 QC 报告。
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
                // 取消标记的轮询在循环开头，但 av_read_frame 可能在两次轮询之间
                // 就被中断回调打断（网络/磁盘 IO 里根本轮不到那次检查），
                // 这种"取消 + AVERROR_EXIT"必须报成 Cancelled 而不是半成品失败。
                if (IsCancelledExit(ret)) {
                    teardown_scan();
                    NotifyCancelled(callbacks, result);
                    return;
                }
                result.scan_error_code = ret;  // 真 AVERROR 优先，MarkFailed 不会覆盖它
                MarkFailed(result, "读取数据包失败（文件可能截断或 IO 错误）: " +
                                       std::string(errbuf));
                teardown_scan();
                NotifyFailed(callbacks, result.error_message);
                return;
            }
            break;  // EOF
        }

        if (pkt->stream_index < 0 ||
            pkt->stream_index >= static_cast<int>(fmt->nb_streams)) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* st = fmt->streams[pkt->stream_index];
        model::StreamDigest& digest = result.streams[pkt->stream_index];
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
            timeline_analyzer.OnPacket(timing);
        }

        // 字幕 / 时码 / 辅助数据轨（内部先按流号过滤，非目标流直接返回）
        if (options.analyze_subtitle) subtitle_analyzer.OnPacket(pkt, st);
        if (options.analyze_timecode) timecode_analyzer.OnPacket(pkt, st);
        if (options.analyze_aux_data) aux_analyzer.OnPacket(pkt, st);

        // 音频 QC：解码后归一为 float planar 再喂给分析器
        if (options.analyze_audio_qc && pkt->stream_index == audio_stream_index &&
            audio_probe.ready) {
            if (avcodec_send_packet(audio_probe.decoder, pkt) == 0) {
                while (avcodec_receive_frame(audio_probe.decoder, audio_probe.frame) >= 0) {
                    feed_audio_frame(audio_probe.frame);
                    av_frame_unref(audio_probe.frame);
                }
            }
        }

        result.total_packets += 1;
        result.total_bytes += pkt->size;
        digest.packet_count += 1;
        digest.byte_count += pkt->size;
        if (pkt->size > result.max_packet_bytes) result.max_packet_bytes = pkt->size;
        if (pkt->dts == AV_NOPTS_VALUE) result.packets_missing_dts += 1;

        if (is_video) {
                result.video_packets += 1;
            digest.frame_count += 1;
            frames_since_key[pkt->stream_index] += 1;

            if (pkt->flags & AV_PKT_FLAG_KEY) {
                result.key_frame_count += 1;
                digest.key_frame_count += 1;
                if (has_key && ts >= 0.0 && last_key_ts >= 0.0) {
                    const double gap = ts - last_key_ts;
                    if (gap > 0.0) {
                        result.gop_intervals_seconds.push_back(gap);
                        result.gop_frame_sizes.push_back(
                            static_cast<int>(frames_since_key[pkt->stream_index]));
                        if (gap > result.max_gop_interval_seconds) {
                            result.max_gop_interval_seconds = gap;
                        }
                    }
                }
                if (frames_since_key[pkt->stream_index] > result.max_gop_frames) {
                    result.max_gop_frames = static_cast<int>(frames_since_key[pkt->stream_index]);
                }
                frames_since_key[pkt->stream_index] = 0;
                if (ts >= 0.0) {
                    last_key_ts = ts;
                    has_key = true;
                }
            }

            // 码率与 GOP 深度分析（仅第一条视频流）
            if (options.analyze_bitrate_gop && pkt->stream_index == video_stream_index) {
                const double sample_ts = has_pts
                                             ? ts
                                             : ((pkt->dts != AV_NOPTS_VALUE)
                                                    ? static_cast<double>(pkt->dts) * tb
                                                    : 0.0);
                model::FrameType frame_type = model::FrameType::Unknown;
                bool is_idr = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                bool fed = false;

                if (probe.use_decoder) {
                    if (avcodec_send_packet(probe.decoder, pkt) == 0) {
                        while (avcodec_receive_frame(probe.decoder, probe.frame) >= 0) {
                            frame_type = static_cast<model::FrameType>(probe.frame->pict_type);
                            // 注: FFmpeg 8.x 已移除 AVFrame::key_frame，统一用 AV_FRAME_FLAG_KEY
                            is_idr = ((probe.frame->flags & AV_FRAME_FLAG_KEY) != 0);
                            const double frame_ts =
                                (probe.frame->pts != AV_NOPTS_VALUE)
                                    ? static_cast<double>(probe.frame->pts) * tb
                                    : sample_ts;
                            bitrate_gop.OnFrame(pkt->stream_index, frame_ts, pkt->size,
                                                frame_type, is_idr);
                            fed = true;
                            av_frame_unref(probe.frame);
                        }
                    }
                } else if (probe.parser != nullptr) {
                    uint8_t* out_data = nullptr;
                    int out_size = 0;
                    av_parser_parse2(probe.parser, probe.parser_ctx, &out_data, &out_size,
                                     pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
                    if (probe.parser->pict_type != AV_PICTURE_TYPE_NONE) {
                        frame_type = static_cast<model::FrameType>(probe.parser->pict_type);
                        if (probe.parser->key_frame >= 1) {
                            is_idr = true;
                        } else if (probe.parser->key_frame == 0) {
                            is_idr = false;
                        }
                        fed = true;
                    }
                }

                if (fed) {
                    bitrate_gop.OnFrame(pkt->stream_index, sample_ts, pkt->size, frame_type,
                                        is_idr);
                } else {
                    bitrate_gop.OnPacket(pkt->stream_index, sample_ts, pkt->size, is_idr);
                }
            }

        // 色彩与 HDR：包级 side data 补充 + 需要时解码首帧读 AVFrame side data
        if (options.analyze_color_hdr && is_video &&
            pkt->stream_index == color_video_stream_index) {
            if (pkt->side_data != nullptr && pkt->side_data_elems > 0) {
                color_hdr.UpdateFromPacket(pkt, pkt->stream_index);
            }
            if (color_hdr.NeedsFrameProbe() && !color_probe.failed &&
                color_probe.frames_read < options.color_hdr_options.max_probe_frames) {
                if (color_probe.EnsureOpen(st)) {
                    if (avcodec_send_packet(color_probe.decoder, pkt) == 0) {
                        while (avcodec_receive_frame(color_probe.decoder, color_probe.frame) >= 0) {
                            color_hdr.UpdateFromFrame(color_probe.frame);
                            av_frame_unref(color_probe.frame);
                            ++color_probe.frames_read;
                            if (color_probe.frames_read >=
                                options.color_hdr_options.max_probe_frames) {
                                break;
                            }
                        }
                    }
                }
            }
        }

        // 逐秒桶：码率 + 帧率
        if (ts >= 0.0) {
            const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval));
            Bucket& bucket = buckets[bucket_index];
            bucket.video_bytes += pkt->size;
            bucket.video_frames += 1;
        }
        }

        if (ts >= 0.0) {
            const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval));
            buckets[bucket_index].total_bytes += pkt->size;
        }

        last_pos = (pkt->pos > 0) ? pkt->pos : last_pos;
        av_packet_unref(pkt);

        // 进度上报（限频）
        ++packet_index;
        if (options.max_packets > 0 && packet_index >= options.max_packets) {
            // 命中包数上限：只完成了抽样扫描，不是完整结果。
            if (result.scan_status == model::AnalysisStatus::Complete) {
                result.scan_status = model::AnalysisStatus::Sampled;
            }
            break;
        }
        if (packet_index % kProgressMinPackets == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count() >=
                kProgressMinIntervalMs) {
                last_progress = now;
                double percent = 0.0;
                if (result.file_size_bytes > 0 && last_pos > 0) {
                    percent = 100.0 * static_cast<double>(last_pos) /
                              static_cast<double>(result.file_size_bytes);
                } else if (result.duration_seconds > 0.0 && ts > 0.0) {
                    percent = 100.0 * ts / result.duration_seconds;
                }
                percent = std::clamp(percent, 0.0, 99.0);
                NotifyProgress(callbacks, percent,
                                      "扫描数据包 " + std::to_string(packet_index) + " 个");
            }
        }
    }
    }
    av_packet_free(&pkt);

    // 部分封装的 HDR 元数据要读到数据包后才补充进 codecpar，关闭输入前再刷一次
    if (options.analyze_color_hdr && color_video_stream_index >= 0) {
        color_hdr.UpdateFromStream(fmt->streams[color_video_stream_index]);
    }
    // 上下文归输入会话所有，关闭也走它（会话析构时同样只会看到空指针）
    input.Close();
    probe.Release();
    color_probe.Release();

    // ---- 音频解码收尾（flush 解码器与 swr 缓存）----
    const bool audio_decoder_ready = audio_probe.ready;
    const bool audio_rate_changed = audio_probe.rate_changed;
    if (options.analyze_audio_qc && audio_decoder_ready) {
        avcodec_send_packet(audio_probe.decoder, nullptr);
        while (avcodec_receive_frame(audio_probe.decoder, audio_probe.frame) >= 0) {
            feed_audio_frame(audio_probe.frame);
            av_frame_unref(audio_probe.frame);
        }
        if (audio_probe.swr != nullptr && audio_probe.out_data != nullptr &&
            audio_probe.channels > 0) {
            std::vector<const float*> planes(static_cast<size_t>(audio_probe.channels));
            int got = swr_convert(audio_probe.swr, audio_probe.out_data, audio_probe.out_frames,
                                  nullptr, 0);
            while (got > 0) {
                for (int c = 0; c < audio_probe.channels; ++c) {
                    planes[static_cast<size_t>(c)] =
                        reinterpret_cast<const float*>(audio_probe.out_data[c]);
                }
                audio_qc.OnSamples(planes.data(), got, -1.0);
                got = swr_convert(audio_probe.swr, audio_probe.out_data, audio_probe.out_frames,
                                  nullptr, 0);
            }
        }
        audio_probe.Release();
    }

    // ---- 汇总 ----
    if (result.total_packets > 0) {
        result.avg_packet_bytes = static_cast<double>(result.total_bytes) /
                                  static_cast<double>(result.total_packets);
    }
    const double measured_duration = (result.duration_seconds > 0.0)
                                         ? result.duration_seconds
                                         : (buckets.empty() ? 0.0
                                                            : (static_cast<double>(buckets.rbegin()->first) + 1.0) * interval);
    if (result.overall_bitrate_bps <= 0 && measured_duration > 0.0) {
        result.overall_bitrate_bps = static_cast<int64_t>(result.total_bytes * 8 / measured_duration);
    }
    for (auto& digest : result.streams) {
        if (digest.bitrate_bps <= 0 && digest.duration_seconds > 0.0) {
            digest.bitrate_bps = static_cast<int64_t>(digest.byte_count * 8 / digest.duration_seconds);
        }
        if (digest.duration_seconds <= 0.0) digest.duration_seconds = measured_duration;
    }

    const double kbps_per_byte_per_sec = 8.0 / 1000.0 / interval;
    result.total_bitrate_kbps.name = "total_bitrate";
    result.total_bitrate_kbps.unit = "kbps";
    result.video_bitrate_kbps.name = "video_bitrate";
    result.video_bitrate_kbps.unit = "kbps";
    result.video_fps.name = "video_fps";
    result.video_fps.unit = "fps";
    result.total_bitrate_kbps.Reserve(buckets.size());
    result.video_bitrate_kbps.Reserve(buckets.size());
    result.video_fps.Reserve(buckets.size());
    for (const auto& [index, bucket] : buckets) {
        const double t = static_cast<double>(index) * interval;
        result.total_bitrate_kbps.Add(t, static_cast<double>(bucket.total_bytes) * kbps_per_byte_per_sec);
        result.video_bitrate_kbps.Add(t, static_cast<double>(bucket.video_bytes) * kbps_per_byte_per_sec);
        result.video_fps.Add(t, static_cast<double>(bucket.video_frames) / interval);
    }

    // ---- 时间轴与同步汇总 ----
    timeline_analyzer.Finish();
    result.timeline = timeline_analyzer.result();

    if (result.max_gop_frames == 0 && !result.gop_frame_sizes.empty()) {
        result.max_gop_frames = *std::max_element(result.gop_frame_sizes.begin(),
                                                  result.gop_frame_sizes.end());
    }

    // 码率与 GOP 深度分析收尾（必须在 probe.Release() 之后、发信号之前）
    if (options.analyze_bitrate_gop && video_stream_index >= 0) {
        result.bitrate_gop = bitrate_gop.Finish();
        LOG_INFO("码率与 GOP 分析: frames=" + std::to_string(result.bitrate_gop.total_frames) +
                 " gops=" + std::to_string(result.bitrate_gop.gops.size()) +
                 " anomalies=" + std::to_string(result.bitrate_gop.anomalies.size()));
    }

    // 音频 QC 收尾（与上面共用同一次 demux）
    if (options.analyze_audio_qc && audio_stream_index >= 0) {
        const model::StreamDigest* audio =
            (audio_stream_index < static_cast<int>(result.streams.size()))
                ? &result.streams[static_cast<size_t>(audio_stream_index)]
                : nullptr;
        const model::StreamDigest* video = result.FirstVideoStream();
        audio_qc.SetDurations(audio ? audio->duration_seconds : 0.0, result.duration_seconds,
                              video ? video->duration_seconds : 0.0, video != nullptr);
        result.audio_qc = audio_qc.Finish();
        if (!result.audio_qc.analyzed) {
            result.audio_qc.notes.push_back(audio_decoder_ready
                                                ? "音频解码未产出 PCM，未执行音频 QC"
                                                : "音频解码器打开失败，未执行音频 QC");
        }
        if (audio_rate_changed) {
            result.audio_qc.notes.push_back("音频采样率/声道数中途改变，部分样本未参与统计");
        }
        LOG_INFO("音频 QC: " + result.audio_qc.ToString());
    }

    // 色彩与 HDR 元数据收尾（必须在 color_probe.Release() 之后、发信号之前）
    if (options.analyze_color_hdr && color_video_stream_index >= 0) {
        result.color_hdr = color_hdr.Finish();
        LOG_INFO("色彩与 HDR: " + result.color_hdr.ToString());
    }

    // 字幕轨收尾（必须在 avformat_close_input 之后、发信号之前）
    if (options.analyze_subtitle) {
        // 用实测时长兜底：容器没给时长时用桶估算值，否则"超出媒体时长"会全漏
        const double cue_limit = (result.duration_seconds > 0.0) ? result.duration_seconds
                                                                 : measured_duration;
        subtitle_analyzer.Finish(cue_limit);
        result.subtitle = subtitle_analyzer.result();
        result.subtitle_analyzed = result.subtitle.analyzed && !result.subtitle.streams.empty();
        LOG_INFO("字幕分析: streams=" + std::to_string(result.subtitle.streams.size()) +
                 " cues=" + std::to_string(result.subtitle.cues.size()) +
                 " issues=" + std::to_string(result.subtitle.issues.size()));
    }

    // 时码与章节收尾
    if (options.analyze_timecode) {
        timecode_analyzer.Finish();
        result.timecode = timecode_analyzer.result();
        result.timecode_analyzed = result.timecode.analyzed;
        LOG_INFO("时码与章节: tracks=" + std::to_string(result.timecode.tracks.size()) +
                 " 首帧时码=" + (result.timecode.has_primary ? result.timecode.primary.ToString()
                                                             : std::string("无")) +
                 " chapters=" + std::to_string(result.timecode.chapters.size()));
    }

    // 辅助数据轨收尾
    if (options.analyze_aux_data) {
        aux_analyzer.Finish();
        result.aux_data = aux_analyzer.result();
        result.aux_data_analyzed = result.aux_data.analyzed;
        LOG_INFO("辅助数据轨: streams=" + std::to_string(result.aux_data.streams.size()) +
                 " scte35=" + std::to_string(result.aux_data.scte35_cue_count) +
                 " metadata=" + std::to_string(result.aux_data.metadata.size()));
    }

    result.scanned_packets = result.total_packets;
    LOG_INFO("全文件分析完成: packets=" + std::to_string(result.total_packets) +
             " duration=" + std::to_string(result.duration_seconds) +
             " status=" + std::string(ToString(result.scan_status)));
    NotifyProgress(callbacks, 100.0, "分析完成");
    // 第二个参数沿用旧语义（true = 到达终态而非被取消），Failed 不会走到这里。
    NotifyFinished(callbacks, result.scan_status != model::AnalysisStatus::Cancelled, result);
}

void AnalysisEngine::RunStreamingManifest(const std::string& file_path,
                                          const AnalysisOptions& options,
                                          model::AnalysisResult& result,
                                          const AnalysisCallbacks& callbacks) {
    VE_PERF("AnalysisEngine::RunStreamingManifest");
    const bool is_dash = (result.file_extension == "mpd");
    result.container_format = is_dash ? "dash" : "hls";

    // 清单解析的全部循环（逐行 / 逐分片 / 逐时间轴条目 / 逐分片落盘探测）都会轮询它。
    // 没有这条通道时，一个几十万行 EXTINF 的 m3u8 会让"取消"和"重新扫描"都点不动 ——
    // QtAnalysisController 启动新扫描会 join 旧线程，而旧线程正卡在解析循环里。
    const std::atomic<bool>* cancel = CancelSource();

    model::StreamingPackageResult& pkg = result.streaming_package;
    bool ok = false;
    {
        VE_PERF("流媒体清单解析");
        if (is_dash) {
            DashManifestAnalyzer dash;
            ok = dash.AnalyzeFile(file_path, pkg, DashManifestOptions{}, cancel);
        } else {
            HlsManifestAnalyzer hls;
            ok = hls.AnalyzeFile(file_path, pkg, HlsManifestOptions{}, cancel);
        }
    }

    // 取消优先于失败判定：解析被中断时 pkg 里只有半份数据，此时报"无法解析清单"
    // 会把用户主动取消说成文件有问题。与逐包扫描路径一致：保留已扫到的部分，
    // 以 scan_status=Cancelled + completed=false 收尾。
    if (CancelRequested()) {
        result.scan_status = model::AnalysisStatus::Cancelled;
        NotifyProgress(callbacks, 100.0, "已取消");
        NotifyFinished(callbacks, false, result);
        return;
    }

    if (!ok) {
        // 清单解析失败也是失败终态：pkg 里只有半截数据，scan_status 必须跟着改成
        // Failed（默认值 Complete 会让"回调报失败 + 结果报完成"同时成立）。
        MarkFailed(result, pkg.error_message.empty() ? ("无法解析清单: " + file_path)
                                                     : pkg.error_message);
        NotifyFailed(callbacks, result.error_message);
        return;
    }

    {
        VE_PERF("SegmentQcAnalyzer::Analyze");
        // 必须接住返回值: Analyze 是三态的, 丢掉它、下面再无条件置 streaming_analyzed /
        // Complete, 等于把"QC 阶段失败"和"QC 阶段被取消"一律报成完整成功 ——
        // 界面上会显示一份只有半截 issues 的分析结果为"分析完成"。
        const SegmentQcAnalyzer::StageStatus qc =
            SegmentQcAnalyzer::Analyze(pkg, options.streaming_package_options, cancel);
        if (qc == SegmentQcAnalyzer::StageStatus::kFailed) {
            // 走失败收尾: pkg 里已经填了 error_message / issues, 交给 NotifyFailed 报出去。
            // 与清单解析失败同理: 状态必须一起改, 否则结果是 Failed 的回调 + Complete 的对象。
            MarkFailed(result, pkg.error_message.empty() ? ("分片级校验失败: " + file_path)
                                                         : pkg.error_message);
            NotifyFailed(callbacks, result.error_message);
            return;
        }
        if (qc == SegmentQcAnalyzer::StageStatus::kCancelled) {
            result.scan_status = model::AnalysisStatus::Cancelled;
            NotifyProgress(callbacks, 100.0, "已取消");
            NotifyFinished(callbacks, false, result);
            return;
        }
    }
    if (CancelRequested()) {
        result.scan_status = model::AnalysisStatus::Cancelled;
        NotifyProgress(callbacks, 100.0, "已取消");
        NotifyFinished(callbacks, false, result);
        return;
    }
    result.streaming_analyzed = true;

    int64_t manifest_size = 0;
    utils::manifest::FileSizeOf(file_path, manifest_size);
    result.file_size_bytes = manifest_size;
    // 时长取清单声明值：分片本体不 demux，拿不到更精确的数字
    if (is_dash) {
        result.duration_seconds = pkg.media_presentation_duration_s;
    } else {
        double longest = 0.0;
        for (const model::MediaPlaylistInfo& pl : pkg.playlists) {
            if (pl.total_duration_seconds > longest) longest = pl.total_duration_seconds;
        }
        result.duration_seconds = longest;
    }
    result.seekable = true;
    result.scan_status = model::AnalysisStatus::Complete;

    LOG_INFO("流媒体清单分析完成: kind=" + std::to_string(static_cast<int>(pkg.kind)) +
             " ladder=" + std::to_string(pkg.ladder.size()) +
             " segments=" + std::to_string(pkg.TotalSegments()) +
             " issues=" + std::to_string(pkg.issues.size()));
    NotifyProgress(callbacks, 100.0, "清单分析完成");
    NotifyFinished(callbacks, true, result);
}

} // namespace analyzer
} // namespace videoeye
