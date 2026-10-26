#pragma once

// 全文件分析的**分析维度集合**: 准备（建分析器 / 注册流 / 打开解码器）、
// 逐包分发、关闭前后的收尾钩子。
//
// 为什么单独成类: 以前这六七个维度是 Run() 里散落的三段 —— 创建段（找流号、开解码器、
// 配 swr）、分发段（每包依次喂给各分析器）、收尾段（关上下文前刷 HDR、flush 音频）。
// 于是"加一个维度"要在 Run() 里同时改创建、分发、收尾三处，而这三处相距两百多行，
// 漏掉任何一处都不会报错，只会静默少一个维度的数据。收进来之后，维度只有本文件一个
// 落点：加维度 = 加一个成员 + Prepare 里一段 + OnPacket 里一段。
//
// 边界（不要越）:
//   * 本类**不持有** AVFormatContext，也不负责打开/关闭它 —— 生命周期归
//     AnalysisInputSession，关闭时点由 Run() 决定（BeforeClose 要在 close 前跑）。
//   * 本类不决定取消、不发回调、不算进度 —— 那些归 Run() / PacketScanLoop。
//   * 本类**不拥有** model::AnalysisResult，只是往里写维度结果；结果对象的唯一
//     归属是 Run()，收尾归 AnalysisResultAssembler。
//
// 顺序约束（搬动时不要重排）:
//   Prepare()  ->  OnPacket() * N  ->  BeforeClose()  ->  FlushAudio()
//              ->  FlushFrameProbe()  ->  ReleaseProbes()  ->  (外部关闭上下文)
//              ->  Assembler 收尾
//   * BeforeClose 必须在关上下文之前: HDR 元数据要读到包后才补进 codecpar；
//   * FlushAudio 必须在 ReleaseProbes 之前: 释放后 ready 就变 false，再 flush 就是空转；
//   * FlushFrameProbe 同样要在 ReleaseProbes 之前: 帧类型解码器到 EOF 可能还压着
//     几帧（帧重排序延迟），不 flush 则"总帧数 < 视频包数"，I/P/B 与 GOP 统计失真；
//   * 各分析器的 Finish() 在 Assembler 里跑，ReleaseProbes 之后才安全。

#include <cstdint>
#include <vector>

#include "core/analysis/codec/FrameOrderTracker.h"
#include "core/analysis/codec/InBandParameterSets.h"
#include "core/analysis/codec/ParameterSetTracker.h"
#include "core/analysis/codec/VideoEsInspector.h"
#include "core/analysis/diagnostics/AuxDataAnalyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/diagnostics/TimecodeAnalyzer.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/analysis/quality/AudioBitstreamAnalyzer.h"
#include "core/analysis/quality/AudioQcAnalyzer.h"
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/analysis/quality/ColorHdrAnalyzer.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/ParameterTimeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace videoeye {

// 音频 QC 的**每条轨**通路：解码器 + 到 float planar 的转换（swr）+ 分析器。
//
// 为什么是每轨一套而不是共用一个解码器：不同音轨的编码/采样率/声道布局本来就可以
// 不同（国配 AAC 立体声 + 原声 AC-3 5.1 是常见组合），共用一个 AVCodecContext
// 会在切轨时把参数改掉，产出的响度/相位数据互相污染。
//
// ❗ 解码器输出本身就是 FLTP 时直接用 AVFrame::extended_data，省掉一次无谓的拷贝
// （data[] 只有 8 个指针，超过 8 声道时后面全是 nullptr）。
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
        if (swr)
            swr_free(&swr);
        if (frame)
            av_frame_free(&frame);
        if (decoder)
            avcodec_free_context(&decoder);
        ready = false;
    }
};

// 一条参与 PCM QC 的音轨（流号 + 解码通路 + 独立分析器）
struct AudioQcTrack {
    int stream_index = -1;
    // 该流的时基（秒/单位）。逐轨存而不是共用一个成员：不同轨可以是不同 time_base。
    double time_base = 0.0;
    AudioQcProbe probe;
    AudioQcAnalyzer analyzer;
};

// 帧类型探测：优先用解码器（准确但慢），否则用 codec parser（几乎零成本）。
// 两者都拿不到时，调用方退回 OnPacket()，只按 AV_PKT_FLAG_KEY 识别 I 帧。
struct FrameTypeProbe {
    AVCodecContext* parser_ctx = nullptr; // 仅供 av_parser_parse2 使用的参数上下文
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    bool use_decoder = false;

    void Release() {
        if (parser)
            av_parser_close(parser);
        if (parser_ctx)
            avcodec_free_context(&parser_ctx);
        if (frame)
            av_frame_free(&frame);
        if (decoder)
            avcodec_free_context(&decoder);
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

    bool Ready() const {
        return decoder != nullptr && frame != nullptr && !failed;
    }

    // 惰性打开解码器；打开失败会被记住，不会重复尝试
    bool EnsureOpen(AVStream* stream) {
        if (failed)
            return false;
        if (opened)
            return Ready();
        opened = true;
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec != nullptr) {
            decoder = avcodec_alloc_context3(codec);
            if (decoder != nullptr && avcodec_parameters_to_context(decoder, stream->codecpar) >= 0) {
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
        if (frame)
            av_frame_free(&frame);
        if (decoder)
            avcodec_free_context(&decoder);
    }
};

class AnalysisPipeline {
public:
    explicit AnalysisPipeline(const AnalysisOptions& options);

    // ---- 扫描之前：建分析器、注册流、解析 extradata、打开解码器 ----
    // 必须在 avformat_close_input 之前（要读 codecpar / extradata），只调用一次。
    // file_duration 来自容器头，供时码轨"没有流级时长时回退"使用。
    void Prepare(AVFormatContext* fmt, model::AnalysisResult& result, double file_duration);

    // ---- 每个包一次 ----
    // 要求 pkt->stream_index 已校验合法。分发顺序：时间轴 -> 字幕/时码/辅助 ->
    // 音频解码 -> 关键帧与 GOP -> 色彩与 HDR。顺序本身不影响正确性，但保持固定顺序
    // 便于对照日志。
    void OnPacket(AVFormatContext* fmt, AVPacket* pkt, model::AnalysisResult& result, int64_t packet_index);

    // ---- 扫描结束后的三个钩子（顺序见文件头注释）----
    // 关闭输入上下文之前的最后一眼：部分封装的 HDR 元数据要读到包后才补进 codecpar。
    // 关上下文之前跑：部分封装的 HDR 元数据要读到包后才补进 codecpar。
    // result 用于回填 slice/SEI 巡检的汇总结果（见 FinishVideoEsInspection）。
    void BeforeClose(AVFormatContext* fmt, model::AnalysisResult& result);
    // flush 音频解码器与 swr 缓存里残留的样本（不碰 AVFormatContext）。
    void FlushAudio();
    // flush 帧类型探测解码器：把 EOF 时仍压在解码器里的帧收干净（见文件头注释）。
    // 这些帧的字节从待认领池里认领（它们的包早就按字节入过账了）——
    // 帧数与帧类型必须完整，字节也要归位，否则平均帧大小与 GOP 大小偏小。
    void FlushFrameProbe();
    // 释放解码器 / parser / swr。可重复调用（各 probe 的 Release 自带幂等）。
    void ReleaseProbes();

    // ---- 收尾阶段（AnalysisResultAssembler）要读的中间态 ----
    TimelineAnalyzer& timeline() {
        return timeline_;
    }
    SubtitleAnalyzer& subtitle() {
        return subtitle_;
    }
    TimecodeAnalyzer& timecode() {
        return timecode_;
    }
    AuxDataAnalyzer& aux_data() {
        return aux_;
    }
    ColorHdrAnalyzer& color_hdr() {
        return color_hdr_;
    }
    BitrateGopAnalyzer& bitrate_gop() {
        return bitrate_gop_;
    }
    // 主音轨的 QC 分析器。没有音频流时返回一个空分析器（Finish 后 analyzed=false），
    // 这样收尾代码不必到处判空 —— 但调用方仍应先用 audio_stream_index() 判断有没有音轨。
    AudioQcAnalyzer& audio_qc();
    // 逐轨收尾用：本次实际参与 QC 的每条音轨（顺序 = 主轨在前，其余按流号）
    // 非 const 重载：收尾阶段 Assembler 要对每轨的 analyzer 调 Finish()（会改状态）
    const std::vector<AudioQcTrack>& audio_tracks() const {
        return audio_tracks_;
    }
    std::vector<AudioQcTrack>& audio_tracks() {
        return audio_tracks_;
    }
    AudioBitstreamAnalyzer& audio_bitstream() {
        return audio_bitstream_;
    }
    // 参数变化时间线（每条视频轨一份，按流号顺序）
    const std::vector<model::ParameterTimeline>& parameter_timelines() const {
        return parameter_timelines_;
    }
    // 帧级显示顺序时间线（H.264 / HEVC 走 POC，VP9 走显示事件序，
    // AV1 走 order_hint；其余编码为空，对应维度标不可用）
    const std::vector<model::FrameOrderTimeline>& frame_order_timelines() const {
        return frame_order_timelines_;
    }

    int video_stream_index() const {
        return video_stream_index_;
    }
    int audio_stream_index() const {
        return audio_stream_index_;
    }
    int color_video_stream_index() const {
        return color_video_stream_index_;
    }
    // 音频 QC 是否真的解出过码（决定"未执行音频 QC"该给哪条说明）
    bool audio_decoder_ready() const;
    bool audio_rate_changed() const;
    // 文件里的音轨总数（含因上限/不支持而被跳过的），给覆盖率的"分母"用。
    // 注意与 audio_tracks().size() 区分：后者是**实际参与 QC** 的条数（"分子"）。
    int audio_tracks_total() const {
        return audio_tracks_total_;
    }

private:
    // 只读 extradata 的码流解析（不解码）。优先主视频，跳过封面图片。
    // 里头还挂着参数集追踪与 POC 追踪的创建 —— 那两条有自己的开关，
    // 不归 analyze_bitstream 管（见 cpp 里的说明）。
    void PrepareBitstream(AVFormatContext* fmt, model::AnalysisResult& result);
    // PrepareBitstream 里"只属于码流解析维度"的那两段（extradata 参数集解析 +
    // slice/SEI 巡检器初始化）。video_order：主视频排最前、其余视频轨跟在后面。
    void PrepareBitstreamAnalysis(AVFormatContext* fmt, model::AnalysisResult& result,
                                  const std::vector<unsigned>& video_order);
    // 为一条音频流建解码通路 + 分析器。成功返回该轨在 audio_tracks_ 里的下标。
    int SetupAudioTrack(AVFormatContext* fmt, int stream_index);
    // 解码后归一为 float planar 再喂给该轨的分析器（内部按 100 ms 步进出块）
    void FeedAudioFrame(AudioQcTrack& track, AVFrame* frame);
    // 把一个包的音频样本喂给指定音轨（发送 + 收帧）
    void PumpAudioPacket(AudioQcTrack& track, const AVPacket* pkt);

    // ---- slice/SEI 巡检（C4）----
    // 把已解析的参数集注入巡检器：H.264 的 slice QP 需要 PPS 的 init_qp 作基准。
    // 带内路径在 OnPacket 里解出参数集后要**再调一次**。
    void ApplyParameterSetsToEsInspector(const model::BitstreamAnalysisResult& bs);
    void FinishVideoEsInspection(model::AnalysisResult& result);

    const AnalysisOptions& options_;

    // 诊断维度
    TimelineAnalyzer timeline_;
    SubtitleAnalyzer subtitle_;
    TimecodeAnalyzer timecode_;
    AuxDataAnalyzer aux_;

    // 质量维度
    ColorHdrAnalyzer color_hdr_;
    BitrateGopAnalyzer bitrate_gop_;
    // 多音轨 PCM QC：每条参与分析的音轨一个元素（主轨恒在首位）。
    // 以前是单个 audio_qc_ + 单个 audio_probe_，只能分析主音轨 ——
    // 国配轨爆音、评论轨反相这类问题在主轨上根本看不出来。
    std::vector<AudioQcTrack> audio_tracks_;
    // 没有音频流时 audio_qc() 要返回一个可写引用；这个空分析器 Finish 后 analyzed=false
    AudioQcAnalyzer empty_audio_qc_;
    // 音频位流参数（D1）：只读帧头不解码，所以不需要 probe，也没有 ready 概念。
    AudioBitstreamAnalyzer audio_bitstream_;

    // 上面三个维度各自要开的解码通路（RAII，见各自 Release）
    ColorFrameProbe color_probe_;
    FrameTypeProbe frame_probe_;

    // 参数集全程追踪：每条视频轨一个（多节目 TS 每条节目独立 PID、独立参数集）
    struct ParameterTrackerSlot {
        int stream_index = -1;
        double time_base = 0.0;
        ParameterSetTracker tracker;
    };
    std::vector<ParameterTrackerSlot> parameter_trackers_;
    // 收尾后写进 result 的时间线（BeforeClose 里填充）
    std::vector<model::ParameterTimeline> parameter_timelines_;

    // 帧级显示顺序追踪（H.264 / HEVC / VP9 / AV1；每条视频轨一个）。
    // FrameOrderTracker 内部按 codec_id 选实现，这里不需要判断编码。
    struct OrderTrackerSlot {
        int stream_index = -1;
        double time_base = 0.0;
        FrameOrderTracker tracker;
    };
    std::vector<OrderTrackerSlot> order_trackers_;
    std::vector<model::FrameOrderTimeline> frame_order_timelines_;
    // 本次音轨总数 / 实际参与 QC 的条数（给 DimensionStatus 的覆盖说明用）
    int audio_tracks_total_ = 0;
    int audio_tracks_limit_note_ = 0;

    // 各维度盯的流号（Prepare 里解析一次，逐包分发时不再遍历流列表）
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    int color_video_stream_index_ = -1;
    // 带内参数集探测（extradata 为空的封装：TS / 裸流 / fMP4 媒体分片 / WebM-VP9）。
    //
    // ❗ **每条视频轨一个 probe**（C3）：多节目 TS 的每条节目是独立 PID、独立参数集，
    // 一条 probe 只能跟一条流 —— 以前只有一个成员，第二条节目永远没人分析。
    // 各自独立收工：成功或放弃后立刻从列表里摘掉，不让后面的包白跑一遍循环。
    struct InBandProbeSlot {
        int stream_index = -1;
        InBandParameterSetProbe probe;
    };
    std::vector<InBandProbeSlot> inband_probes_;
    // 还有 probe 在跑时为 true：逐包分发的短路判据（比每次 empty() 便宜，
    // 也避免"列表空了还进循环"这个容易复发的手误）
    bool bitstream_inband_active_ = false;
    // slice header / SEI 巡检（C4）：逐包喂主视频轨的字节，收尾汇总成 QP 分布与
    // SEI 元数据。与上面的参数集解析是两条独立链路（输入来源不同），别合并。
    VideoEsInspector es_inspector_;
    bool es_inspector_configured_ = false;
    double audio_time_base_ = 0.0;
    // 视频流时基（FlushFrameProbe 把 flush 出来的帧 pts 换算成秒用）
    double video_time_base_ = 0.0;

    // GOP 统计的中间态：跨包累积、按流分别记账，所以只能是成员
    std::vector<int64_t> frames_since_key_;
    std::vector<double> last_key_ts_;
};

} // namespace videoeye
