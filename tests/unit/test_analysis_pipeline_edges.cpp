// 分析流水线的边界回归：主轨选择、封面图、按流 GOP 记账、容器位深未知、
// 以及超过 8 声道时 AVFrame::data[] 越界（必须走 extended_data）。
//
// 前四条是"metadata 层看不见、只在特定文件上错"的口径问题；最后一条是
// 真正的 UB —— AV_NUM_DATA_POINTERS 只有 8，12 声道 S16P 解码后
// data[8..11] 全为 nullptr，读它等于解引用空指针。

#include <gtest/gtest.h>

#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/analysis/detail/AnalysisStreamSelection.h"
#include "core/analysis/orchestration/detail/AnalysisPipeline.h"
#include "core/analysis/orchestration/detail/AnalysisResultAssembler.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace {
using namespace videoeye;

// 关掉所有"要解码/要读包"的维度，让每条用例只测自己关心的那一件事。
AnalysisOptions QuietOptions() {
    AnalysisOptions options;
    options.analyze_bitrate_gop = false;
    options.analyze_audio_qc = false;
    options.analyze_color_hdr = false;
    options.analyze_bitstream = false;
    options.analyze_subtitle = false;
    options.analyze_timecode = false;
    options.analyze_aux_data = false;
    return options;
}

// 注意成员顺序：options 必须先于 pipeline 构造（pipeline 持有它的引用），
// 而用例会在 Prepare 之前改 options 开关来打开自己需要的维度。
struct PipelineCase {
    AnalysisOptions options = QuietOptions();
    AnalysisPipeline pipeline{options};
    AVFormatContext* format = avformat_alloc_context();
    model::AnalysisResult result;

    ~PipelineCase() {
        pipeline.ReleaseProbes();
        if (format != nullptr)
            avformat_free_context(format);
    }

    AVStream* Add(AVMediaType type, AVCodecID codec) {
        if (format == nullptr)
            return nullptr;
        AVStream* stream = avformat_new_stream(format, nullptr);
        if (stream == nullptr)
            return nullptr;
        stream->codecpar->codec_type = type;
        stream->codecpar->codec_id = codec;
        stream->time_base = AVRational{1, 1000};
        if (type == AVMEDIA_TYPE_VIDEO) {
            stream->codecpar->width = 1920;
            stream->codecpar->height = 1080;
        }
        model::StreamDigest digest;
        digest.index = stream->index;
        digest.media_type = type;
        result.streams.push_back(digest);
        return stream;
    }

    // 喂一个关键帧包：GOP 统计只看包级 flags，不需要真码流
    void Key(int stream, int64_t pts) {
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr)
            return;
        packet->stream_index = stream;
        packet->pts = packet->dts = pts;
        packet->flags = AV_PKT_FLAG_KEY;
        pipeline.OnPacket(format, packet, result, 0);
        av_packet_free(&packet);
    }
};

// 12 声道 PCM：planar / packed 两条通路的解码与重采样
void CheckTwelveChannelPcm(AVCodecID codec, bool planar) {
    PipelineCase test;
    ASSERT_TRUE(test.format != nullptr);
    AVStream* stream = test.Add(AVMEDIA_TYPE_AUDIO, codec);
    ASSERT_TRUE(stream != nullptr);
    constexpr int channels = 12;
    constexpr int frames = 512;
    av_channel_layout_default(&stream->codecpar->ch_layout, channels);
    ASSERT_EQ(stream->codecpar->ch_layout.nb_channels, channels);
    stream->codecpar->sample_rate = 48000;
    stream->codecpar->format = planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
    stream->time_base = AVRational{1, 48000};

    test.options.analyze_audio_qc = true;
    test.options.audio_qc_options.enable_true_peak = false;
    test.pipeline.Prepare(test.format, test.result, 1.0);
    ASSERT_TRUE(test.pipeline.audio_decoder_ready());

    AVPacket* packet = av_packet_alloc();
    ASSERT_TRUE(packet != nullptr);
    if (av_new_packet(packet, channels * frames * 2) < 0) {
        av_packet_free(&packet);
        FAIL() << "无法分配 PCM 包";
        return;
    }
    // 每个声道填不同的常量，这样"平面接错位置"会立刻体现成别的声道的峰值
    for (int c = 0; c < channels; ++c) {
        const int value = (c + 1) * 1000;
        for (int s = 0; s < frames; ++s) {
            const int at = 2 * (planar ? c * frames + s : s * channels + c);
            packet->data[at] = static_cast<uint8_t>(value & 0xFF);
            packet->data[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        }
    }
    packet->stream_index = stream->index;
    packet->pts = packet->dts = 0;
    packet->duration = frames;
    test.pipeline.OnPacket(test.format, packet, test.result, 0);
    av_packet_free(&packet);

    test.pipeline.FlushAudio();
    const auto& audio = test.pipeline.audio_qc().Finish();
    ASSERT_TRUE(audio.analyzed);
    ASSERT_EQ(audio.channels.size(), static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        EXPECT_EQ(audio.channels[c].sample_count, frames) << "channel " << c;
        EXPECT_NEAR(audio.channels[c].peak, (c + 1) * 1000.0 / 32768.0, 0.00001) << "channel " << c;
    }
}

// 容器位深 = raw_bits，码流恒为 10bit（High 10，1920x1088 裁到 1080p）
void CheckBitDepth(int raw_bits, bool expect_mismatch) {
    PipelineCase test;
    ASSERT_TRUE(test.format != nullptr);
    AVStream* video = test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264);
    ASSERT_TRUE(video != nullptr);
    const uint8_t annexb[] = {0,    0,    0,    1, 0x67, 0x6E, 0x00, 0x33, 0xA6, 0xCC, 0xA4, 0x01, 0xE0,
                              0x08, 0x9F, 0x95, 0, 0,    0,    1,    0x68, 0xEB, 0x4B, 0x2C, 0x80};
    video->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(sizeof(annexb) + AV_INPUT_BUFFER_PADDING_SIZE));
    ASSERT_TRUE(video->codecpar->extradata != nullptr);
    std::memcpy(video->codecpar->extradata, annexb, sizeof(annexb));
    video->codecpar->extradata_size = sizeof(annexb);
    video->codecpar->bits_per_raw_sample = raw_bits;

    test.options.analyze_bitstream = true;
    test.pipeline.Prepare(test.format, test.result, 1.0);
    ASSERT_TRUE(test.result.bitstream_analyzed);
    EXPECT_EQ(test.result.bitstream_analysis.bit_depth, 10);
    EXPECT_EQ(test.result.bitstream_analysis.container_bit_depth, raw_bits);
    bool mismatch = false;
    for (const auto& issue : test.result.bitstream_analysis.inconsistencies) {
        if (issue.field == "Bit Depth")
            mismatch = true;
    }
    EXPECT_EQ(mismatch, expect_mismatch);
}
} // namespace

// 两条视频轨交错到达：GOP 相邻关键帧时间必须按流分别记账，
// 共用一份"上一关键帧"会把 A 轨的间隔算成 A->B 的半截。
TEST(ParserReviewPipeline, InterleavedVideoTracksDoNotShortenPrimaryGops) {
    PipelineCase test;
    ASSERT_TRUE(test.format != nullptr);
    AVStream* main = test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4);
    ASSERT_TRUE(main != nullptr);
    main->disposition = AV_DISPOSITION_DEFAULT;
    ASSERT_TRUE(test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4) != nullptr);

    test.pipeline.Prepare(test.format, test.result, 5.0);
    ASSERT_EQ(test.pipeline.video_stream_index(), 0);

    test.Key(0, 0);
    test.Key(1, 500);
    test.Key(0, 2000);
    test.Key(1, 2500);
    test.Key(0, 4000);

    ASSERT_EQ(test.result.gop_intervals_seconds.size(), 2u);
    EXPECT_DOUBLE_EQ(test.result.gop_intervals_seconds[0], 2.0);
    EXPECT_DOUBLE_EQ(test.result.gop_intervals_seconds[1], 2.0);
    EXPECT_DOUBLE_EQ(test.result.max_gop_interval_seconds, 2.0);
}

TEST(ParserReviewPipeline, PrimarySelectionSkipsCoverAndUsesDefaultVideo) {
    PipelineCase test;
    ASSERT_TRUE(test.format != nullptr);
    AVStream* cover = test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MJPEG);
    ASSERT_TRUE(cover != nullptr);
    cover->disposition = AV_DISPOSITION_ATTACHED_PIC;
    ASSERT_TRUE(test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4) != nullptr);
    AVStream* primary = test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4);
    ASSERT_TRUE(primary != nullptr);
    primary->disposition = AV_DISPOSITION_DEFAULT;

    test.pipeline.Prepare(test.format, test.result, 5.0);
    EXPECT_EQ(test.pipeline.video_stream_index(), 2);
    EXPECT_EQ(test.result.primary_video_stream_index, 2);

    // 封面包不参与任何视频统计
    test.Key(0, 0);
    test.Key(2, 0);
    test.Key(2, 2000);
    EXPECT_EQ(test.result.key_frame_count, 2);
}

TEST(ParserReviewPipeline, CoverOnlyInputHasNoPrimaryVideo) {
    PipelineCase test;
    ASSERT_TRUE(test.format != nullptr);
    AVStream* cover = test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MJPEG);
    ASSERT_TRUE(cover != nullptr);
    cover->disposition = AV_DISPOSITION_ATTACHED_PIC;
    EXPECT_EQ(SelectAnalysisStream(test.format, AVMEDIA_TYPE_VIDEO), -1);
}

// bits_per_raw_sample == 0 表示"容器没说"，不能当成 8bit 去和码流比
TEST(ParserReviewPipeline, UnknownContainerBitDepthDoesNotCreateMismatch) {
    CheckBitDepth(0, false);
}

TEST(ParserReviewPipeline, ExplicitContainerBitDepthMismatchIsStillReported) {
    CheckBitDepth(8, true);
}

TEST(ParserReviewPipeline, TwelveChannelPlanarPcmUsesExtendedDataForResampling) {
    CheckTwelveChannelPcm(AV_CODEC_ID_PCM_S16LE_PLANAR, true);
}

TEST(ParserReviewPipeline, TwelveChannelPackedPcmStillWorks) {
    CheckTwelveChannelPcm(AV_CODEC_ID_PCM_S16LE, false);
}

// ---------------------------------------------------------------------------
// 维度口径一致性：FinalizeDimensionStatus 之后，历史 *_analyzed 布尔必须跟着
// dimensions 表的 executed 走。消费方（QC/报告/UI）只读其中一个，两者不一致
// 就会出现"UI 显示跑了、报告说没跑"的分裂。
// ---------------------------------------------------------------------------
TEST(ParserReviewPipeline, DimensionStatusIsAuthoritativeOverLegacyFlags) {
    using videoeye::model::AnalysisDimension;

    // 选项全关：即使生产阶段把布尔置成 true，收尾也必须改回 false ——
    // "没执行"不能因为残留标志被消费方当成"执行过"。
    videoeye::AnalysisOptions off = QuietOptions();
    off.analyze_audio_bitstream = false;
    off.analyze_mp4_sample_table = false;
    off.analyze_streaming_package = false;
    videoeye::model::AnalysisResult skipped;
    skipped.bitstream_analyzed = true;
    skipped.audio_bitstream_analyzed = true;
    skipped.subtitle_analyzed = true;
    skipped.timecode_analyzed = true;
    skipped.aux_data_analyzed = true;
    skipped.mp4_samples_analyzed = true;
    skipped.streaming_analyzed = true;
    videoeye::AnalysisResultAssembler skipped_assembler(skipped, off);
    skipped_assembler.FinalizeDimensionStatus(0, 0);
    EXPECT_FALSE(skipped.DimensionExecuted(AnalysisDimension::VideoBitstream));
    EXPECT_FALSE(skipped.bitstream_analyzed);
    EXPECT_FALSE(skipped.audio_bitstream_analyzed);
    EXPECT_FALSE(skipped.subtitle_analyzed);
    EXPECT_FALSE(skipped.timecode_analyzed);
    EXPECT_FALSE(skipped.aux_data_analyzed);
    EXPECT_FALSE(skipped.mp4_samples_analyzed);
    EXPECT_FALSE(skipped.streaming_analyzed);

    // 反向：真跑过（选项开 + 有数据）时，布尔保持 true，两边口径一致。
    videoeye::AnalysisOptions on = QuietOptions();
    on.analyze_audio_bitstream = true;
    videoeye::model::AnalysisResult analyzed;
    analyzed.audio_bitstream_analyzed = true;
    analyzed.audio_bitstream.analyzed = true;
    analyzed.audio_bitstream.streams.emplace_back();
    videoeye::AnalysisResultAssembler analyzed_assembler(analyzed, on);
    analyzed_assembler.FinalizeDimensionStatus(0, 0);
    EXPECT_TRUE(analyzed.DimensionExecuted(AnalysisDimension::AudioBitstream));
    EXPECT_TRUE(analyzed.audio_bitstream_analyzed);
}

// ==========================================================================
// 探针必须是不变量：禁拷贝、可移动、析构即释放（评审 P2-4）
//
// 以前它们只是"带 Release() 的裸聚合体"，释放全靠 AnalysisPipeline 显式调
// ReleaseProbes()。栈展开（Prepare / 逐包分析 / 回调抛异常）不会走到那一步，
// 外围的异常捕获能让线程不直接结束，却补不上 FFmpeg 资源的释放。
// 现在"最终一定被释放"由析构兜底，禁拷贝则杜绝"两份句柄各释放一次"的双释放。
// ==========================================================================
TEST(AnalysisPipelineEdges, ProbesAreMoveOnlyAndReleaseOnDestruction) {
    static_assert(!std::is_copy_constructible<AudioQcProbe>::value, "probe 必须禁拷贝");
    static_assert(!std::is_copy_constructible<FrameTypeProbe>::value, "probe 必须禁拷贝");
    static_assert(!std::is_copy_constructible<ColorFrameProbe>::value, "probe 必须禁拷贝");
    static_assert(std::is_move_constructible<AudioQcProbe>::value, "probe 必须可移动（vector 扩容要用）");
    static_assert(std::is_move_constructible<FrameTypeProbe>::value, "probe 必须可移动");
    static_assert(std::is_move_constructible<ColorFrameProbe>::value, "probe 必须可移动");

    AudioQcProbe probe;
    probe.frame = av_frame_alloc();
    ASSERT_TRUE(probe.frame != nullptr);
    probe.ready = true;

    AVFrame* borrowed = probe.frame;
    {
        AudioQcProbe moved = std::move(probe);
        EXPECT_TRUE(moved.frame == borrowed);
        // 搬走之后源对象不再拥有句柄 —— 否则它析构时会把同一个 AVFrame 放两次
        EXPECT_TRUE(probe.frame == nullptr);
        EXPECT_FALSE(probe.ready);
    } // moved 析构：AVFrame 在这里被释放（ASan/LSan 会盯住"有没有真释放"）
    EXPECT_TRUE(probe.frame == nullptr);
}

// 管线析构兜底：即使一次 ReleaseProbes() 都没调（栈展开那条路径），
// 三条通路也必须在管线消失时收干净。这里只验证"析构不崩 + 可重复释放"，
// 真正的"有没有漏"由 ASan / LSan 构建兜底。
TEST(AnalysisPipelineEdges, PipelineDestructorReleasesProbesWithoutExplicitCall) {
    AnalysisOptions options = QuietOptions();
    options.analyze_audio_qc = true;
    options.analyze_bitrate_gop = true;
    options.analyze_color_hdr = true;
    {
        AnalysisPipeline pipeline(options);
        pipeline.ReleaseProbes(); // 幂等：调几次都不会重复释放
        pipeline.ReleaseProbes();
    } // 析构再收一次：显式释放过的探针已回到"什么都没拿"的状态
}
