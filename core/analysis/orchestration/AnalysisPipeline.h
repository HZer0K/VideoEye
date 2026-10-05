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
//              ->  ReleaseProbes()  ->  (外部关闭上下文)  ->  Assembler 收尾
//   * BeforeClose 必须在关上下文之前: HDR 元数据要读到包后才补进 codecpar；
//   * FlushAudio 必须在 ReleaseProbes 之前: 释放后 ready 就变 false，再 flush 就是空转；
//   * 各分析器的 Finish() 在 Assembler 里跑，ReleaseProbes 之后才安全。

#include <cstdint>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/diagnostics/AuxDataAnalyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/diagnostics/TimecodeAnalyzer.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/analysis/quality/AudioQcAnalyzer.h"
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/analysis/quality/ColorHdrAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace videoeye {

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
    void OnPacket(AVFormatContext* fmt, AVPacket* pkt, model::AnalysisResult& result,
                  int64_t packet_index);

    // ---- 扫描结束后的三个钩子（顺序见文件头注释）----
    // 关闭输入上下文之前的最后一眼：部分封装的 HDR 元数据要读到包后才补进 codecpar。
    void BeforeClose(AVFormatContext* fmt);
    // flush 音频解码器与 swr 缓存里残留的样本（不碰 AVFormatContext）。
    void FlushAudio();
    // 释放解码器 / parser / swr。可重复调用（各 probe 的 Release 自带幂等）。
    void ReleaseProbes();

    // ---- 收尾阶段（AnalysisResultAssembler）要读的中间态 ----
    TimelineAnalyzer& timeline() { return timeline_; }
    SubtitleAnalyzer& subtitle() { return subtitle_; }
    TimecodeAnalyzer& timecode() { return timecode_; }
    AuxDataAnalyzer& aux_data() { return aux_; }
    ColorHdrAnalyzer& color_hdr() { return color_hdr_; }
    BitrateGopAnalyzer& bitrate_gop() { return bitrate_gop_; }
    AudioQcAnalyzer& audio_qc() { return audio_qc_; }

    int video_stream_index() const { return video_stream_index_; }
    int audio_stream_index() const { return audio_stream_index_; }
    int color_video_stream_index() const { return color_video_stream_index_; }
    // 音频 QC 是否真的解出过码（决定"未执行音频 QC"该给哪条说明）
    bool audio_decoder_ready() const { return audio_probe_.ready; }
    bool audio_rate_changed() const { return audio_probe_.rate_changed; }

private:
    // 只读 extradata 的码流解析（不解码）。只取第一条有 extradata 的视频流。
    void PrepareBitstream(AVFormatContext* fmt, model::AnalysisResult& result);
    int FirstStreamOfType(AVFormatContext* fmt, AVMediaType type);
    // 解码后归一为 float planar 再喂给 AudioQcAnalyzer（内部按 100 ms 步进出块）
    void FeedAudioFrame(AVFrame* frame);

    const AnalysisOptions& options_;

    // 诊断维度
    TimelineAnalyzer timeline_;
    SubtitleAnalyzer subtitle_;
    TimecodeAnalyzer timecode_;
    AuxDataAnalyzer aux_;

    // 质量维度
    ColorHdrAnalyzer color_hdr_;
    BitrateGopAnalyzer bitrate_gop_;
    AudioQcAnalyzer audio_qc_;

    // 上面三个维度各自要开的解码通路（RAII，见各自 Release）
    ColorFrameProbe color_probe_;
    FrameTypeProbe frame_probe_;
    AudioQcProbe audio_probe_;

    // 各维度盯的流号（Prepare 里解析一次，逐包分发时不再遍历流列表）
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    int color_video_stream_index_ = -1;
    double audio_time_base_ = 0.0;

    // GOP 统计的中间态：跨包累积，所以只能是成员
    std::vector<int64_t> frames_since_key_;
    double last_key_ts_ = -1.0;
    bool has_key_ = false;
};

}  // namespace videoeye
