// 分析流水线的边界回归：主轨选择、封面图、按流 GOP 记账、容器位深未知、
// 以及超过 8 声道时 AVFrame::data[] 越界（必须走 extended_data）。
//
// 前四条是"metadata 层看不见、只在特定文件上错"的口径问题；最后一条是
// 真正的 UB —— AV_NUM_DATA_POINTERS 只有 8，12 声道 S16P 解码后
// data[8..11] 全为 nullptr，读它等于解引用空指针。

#include <gtest/gtest.h>

#include <cstdlib>
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
    static_assert(!std::is_copy_constructible<VisualFrameProbe>::value, "视觉 QC 的 probe 必须禁拷贝");
    static_assert(std::is_move_constructible<AudioQcProbe>::value, "probe 必须可移动（vector 扩容要用）");
    static_assert(std::is_move_constructible<FrameTypeProbe>::value, "probe 必须可移动");
    static_assert(std::is_move_constructible<ColorFrameProbe>::value, "probe 必须可移动");
    static_assert(std::is_move_constructible<VisualFrameProbe>::value, "视觉 QC 的 probe 必须可移动");

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

// ==========================================================================
// 视觉画质 QC：离线抽帧必须真的走到解码出来的画面上。
//
// 用 rawvideo + RGB24 合成全黑帧：不需要真码流，解码器输出的就是喂进去的像素，
// 这样"抽帧 -> BuildSample -> Feed"整条链路才被测到（而不是只测分析器算法）。
// 10 帧 @25fps 的黑场跨越 0.36s，超过默认 0.3s 阈值 —— 必须落一条 BlackFrame。
// ==========================================================================
TEST(AnalysisPipelineEdges, VisualQcAnalyzesDecodedFramesAndReportsBlackSegment) {
    using videoeye::model::VisualDefectReport;
    using videoeye::model::VisualDefectType;

    PipelineCase test;
    ASSERT_TRUE(test.format != nullptr);
    AVStream* video = test.Add(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_RAWVIDEO);
    ASSERT_TRUE(video != nullptr);
    constexpr int kWidth = 64;
    constexpr int kHeight = 36;
    constexpr int kFrames = 10;
    video->codecpar->width = kWidth;
    video->codecpar->height = kHeight;
    video->codecpar->format = AV_PIX_FMT_RGB24;
    video->avg_frame_rate = AVRational{25, 1};
    video->r_frame_rate = AVRational{25, 1};

    test.options.analyze_visual_qc = true;
    // 离线全帧：不抽样，覆盖率应当等于解码帧数
    test.options.visual_qc_options.preset = videoeye::model::VisualSamplingPreset::OfflineFull;
    test.pipeline.Prepare(test.format, test.result, 1.0);
    ASSERT_GE(test.pipeline.visual_video_stream_index(), 0);

    const int frame_bytes = kWidth * kHeight * 3;
    for (int i = 0; i < kFrames; ++i) {
        AVPacket* packet = av_packet_alloc();
        ASSERT_TRUE(packet != nullptr);
        if (av_new_packet(packet, frame_bytes) < 0) {
            av_packet_free(&packet);
            FAIL() << "无法分配 rawvideo 帧";
            return;
        }
        std::memset(packet->data, 0, static_cast<size_t>(frame_bytes));
        packet->stream_index = video->index;
        packet->pts = packet->dts = i * 40; // 25fps，时基 1/1000
        packet->duration = 40;
        test.pipeline.OnPacket(test.format, packet, test.result, 0);
        av_packet_free(&packet);
    }
    test.pipeline.FlushVisual();

    EXPECT_EQ(test.pipeline.visual_frames_decoded(), kFrames) << "每个包都应解出一帧";
    const VisualDefectReport report = test.pipeline.visual_qc().Snapshot();
    ASSERT_TRUE(report.analyzed) << "全帧分析后必须标记已分析";
    EXPECT_EQ(report.analyzed_frames, kFrames) << "逐帧档位不应抽样";
    EXPECT_GE(report.CountByType(VisualDefectType::BlackFrame), 1) << "0.36s 黑场应落一条 BlackFrame";
}

// ==========================================================================
// 逐秒桶的有界保留
//
// 桶按时间无限增长（24 小时 / 1 秒粒度 = 86400 个），而三条逐秒曲线最多只画
// kMaxCurvePoints 个点。上限之后必须**停止新增**并显式置 truncated，但
// last_bucket_index 仍要跟着走 —— 否则时长兜底会把素材算短。
// ==========================================================================
TEST(AnalysisPipelineEdges, ScanBucketsStopGrowingAtCapButRememberLastIndex) {
    const int64_t cap = static_cast<int64_t>(videoeye::ScanBuckets::kMaxBuckets);
    videoeye::ScanBuckets buckets;
    for (int64_t i = 0; i < cap; ++i) {
        videoeye::ScanBucket* slot = buckets.Acquire(i);
        ASSERT_NE(slot, nullptr) << "上限之前的每个桶都应创建成功: " << i;
        slot->total_bytes = 1;
    }
    EXPECT_EQ(static_cast<int64_t>(buckets.by_second.size()), cap);
    EXPECT_FALSE(buckets.truncated);
    EXPECT_EQ(buckets.last_bucket_index, cap - 1);

    // 超过上限：不再新增、置 truncated，last_bucket_index 继续更新
    for (int64_t i = 0; i < 5; ++i) {
        EXPECT_EQ(buckets.Acquire(cap + i), nullptr);
    }
    EXPECT_TRUE(buckets.truncated);
    EXPECT_EQ(static_cast<int64_t>(buckets.by_second.size()), cap);
    EXPECT_EQ(buckets.last_bucket_index, cap + 4);

    // 已存在的桶在截断后仍可累加（前段曲线不受影响）
    videoeye::ScanBucket* existing = buckets.Acquire(0);
    ASSERT_NE(existing, nullptr);
    existing->total_bytes += 1;
    EXPECT_EQ(existing->total_bytes, 2);
}

// 时长兜底必须用 last_bucket_index（含被丢弃的桶），不能用 rbegin()：
// 截断后最后一个留在 map 里的桶远早于真实末尾，用它算时长会把素材算短。
TEST(AnalysisPipelineEdges, TruncatedBucketsStillYieldFullMeasuredDuration) {
    const int64_t cap = static_cast<int64_t>(videoeye::ScanBuckets::kMaxBuckets);
    constexpr double interval = 0.5;
    videoeye::ScanBuckets buckets;
    for (int64_t i = 0; i < cap; ++i)
        ASSERT_NE(buckets.Acquire(i), nullptr);
    // 之后还有 10 秒的数据（桶被丢弃，但结束时间必须仍然知道）
    for (int64_t i = 0; i < 20; ++i)
        EXPECT_EQ(buckets.Acquire(cap + i), nullptr);

    videoeye::model::AnalysisResult result; // duration_seconds 为 0 -> 走桶兜底
    videoeye::AnalysisOptions options = QuietOptions();
    videoeye::AnalysisResultAssembler assembler(result, options);
    const double measured = assembler.FinalizeBuckets(buckets, interval);

    EXPECT_TRUE(result.series_truncated);
    EXPECT_EQ(result.total_bitrate_kbps.Size(), static_cast<size_t>(cap));
    // 结束时刻 = (last_bucket_index + 1) * interval，覆盖被丢弃的尾部
    EXPECT_NEAR(measured, static_cast<double>(cap + 20) * interval, 1e-9);
}

// 裸流 / 无 STREAMINFO 的 FLAC 等容器不声明时长。此时必须把逐桶实测时长写回结果，
// 否则 QC 的 container.duration_invalid（< 0.01s，Critical）会把合法素材判成"不通过"，
// 报告里的时长也会一直是 0。逐流码率同理：先补时长再算，避免停在 0 触发 low_bpp 误报。
TEST(AnalysisPipelineEdges, MissingContainerDurationFallsBackToMeasuredDuration) {
    constexpr double interval = 1.0;
    videoeye::ScanBuckets buckets;
    for (int64_t i = 0; i < 10; ++i)
        ASSERT_NE(buckets.Acquire(i), nullptr);

    videoeye::model::AnalysisResult result; // duration_seconds == 0 -> 走桶兜底
    videoeye::model::StreamDigest video;
    video.index = 0;
    video.media_type = 0;       // AVMEDIA_TYPE_VIDEO
    video.byte_count = 1250000; // 10s @ 1 Mbps
    result.streams.push_back(video);

    videoeye::AnalysisResultAssembler assembler(result, QuietOptions());
    const double measured = assembler.FinalizeBuckets(buckets, interval);

    EXPECT_NEAR(measured, 10.0, 1e-9);
    EXPECT_DOUBLE_EQ(result.duration_seconds, measured);
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_DOUBLE_EQ(result.streams[0].duration_seconds, measured);
    EXPECT_EQ(result.streams[0].bitrate_bps, 1000000);
}

// ============================================================================
// 真实 HDR 样本端到端（env-gated）
//
// 用法：VIDEOEYE_HDR_SAMPLE_DIR=<目录> ctest -R AnalysisPipelineEdgeTests
// 不设环境变量时 SKIP：这些大样本不随仓库分发。这里钉的不是"能打开文件"，
// 而是 MP4 的 HDR 静态元数据**只在解码帧 side data 上**这条真实链路：
//   * hdr10-720p / hdr10-video-with-sdr-container：CLL 与 mastering 都取自帧侧；
//   * hlg-1080p：HLG 本就没有静态元数据，不能因此判成"HDR 元数据不完整"。
// 旧版遗留 videoeye-cli.exe（9/27）测出的 CLL=0 是历史二进制，不能当真值。
// ============================================================================
namespace {

AnalysisOptions HdrSampleOptions() {
    AnalysisOptions options;
    options.analyze_bitrate_gop = false;
    options.analyze_audio_qc = false;
    options.analyze_audio_bitstream = false;
    options.analyze_color_hdr = true;
    options.analyze_visual_qc = false;
    options.analyze_bitstream = false;
    options.analyze_parameter_timeline = false;
    options.analyze_frame_order = false;
    options.analyze_mp4_sample_table = false;
    options.analyze_streaming_package = false;
    options.analyze_subtitle = false;
    options.analyze_timecode = false;
    options.analyze_aux_data = false;
    options.analyze_ts_transport = false;
    options.analyze_legacy_container = false;
    return options;
}

bool AnalyzeHdrSample(const std::string& path, model::AnalysisResult& result) {
    AnalysisOptions options = HdrSampleOptions();
    AnalysisPipeline pipeline(options);
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0)
        return false;
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr || avformat_find_stream_info(format, nullptr) < 0) {
        if (packet != nullptr)
            av_packet_free(&packet);
        avformat_close_input(&format);
        return false;
    }

    result.file_path = path;
    result.duration_seconds = static_cast<double>(format->duration) / 1000000.0;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVStream* stream = format->streams[i];
        model::StreamDigest digest;
        digest.index = static_cast<int>(i);
        digest.media_type = stream->codecpar->codec_type;
        digest.codec_name = avcodec_get_name(stream->codecpar->codec_id);
        digest.width = stream->codecpar->width;
        digest.height = stream->codecpar->height;
        result.streams.push_back(digest);
    }
    pipeline.SetSourcePath(path);
    pipeline.Prepare(format, result, result.duration_seconds);

    int64_t packet_index = 0;
    while (av_read_frame(format, packet) >= 0) {
        pipeline.OnPacket(format, packet, result, packet_index++);
        av_packet_unref(packet);
    }
    pipeline.BeforeClose(format, result);
    pipeline.ReleaseProbes();

    AnalysisResultAssembler assembler(result, options);
    assembler.FinalizeColorHdr(pipeline.color_hdr(), pipeline.color_video_stream_index());
    assembler.FinalizeDimensionStatus(0, 0);
    av_packet_free(&packet);
    avformat_close_input(&format);
    return result.color_hdr.analyzed;
}

const model::DimensionStatus* ColorHdrDimension(const model::AnalysisResult& result) {
    return result.FindDimension(model::AnalysisDimension::ColorHdr);
}

std::string HdrSamplePath(const char* name) {
    const char* dir = std::getenv("VIDEOEYE_HDR_SAMPLE_DIR");
    return dir == nullptr ? std::string() : (std::string(dir) + "/" + name);
}

} // namespace

TEST(ColorHdrRealSampleTest, Hdr10PlusReadsFrameSideDataAndFlagsPlaceholderCll) {
    const std::string path = HdrSamplePath("hdr10-720p.mp4");
    if (path.empty())
        GTEST_SKIP() << "set VIDEOEYE_HDR_SAMPLE_DIR to run real HDR sample checks";
    model::AnalysisResult result;
    ASSERT_TRUE(AnalyzeHdrSample(path, result)) << path;

    // 真值：SEI 里 mastering 齐备，但 CLL SEI 两项都是 0（ffprobe 6.1 与 FFmpeg 8.1 一致）。
    EXPECT_EQ(result.color_hdr.hdr.format, model::HdrFormat::Hdr10Plus);
    EXPECT_TRUE(result.color_hdr.hdr.mastering_display.Complete());
    EXPECT_TRUE(result.color_hdr.hdr.content_light.present);
    EXPECT_FALSE(result.color_hdr.hdr.content_light.HasAny());
}

TEST(ColorHdrRealSampleTest, Hdr10InSdrContainerKeepsContentLight) {
    const std::string path = HdrSamplePath("hdr10-video-with-sdr-container.mp4");
    if (path.empty())
        GTEST_SKIP() << "set VIDEOEYE_HDR_SAMPLE_DIR to run real HDR sample checks";
    model::AnalysisResult result;
    ASSERT_TRUE(AnalyzeHdrSample(path, result)) << path;

    // 真值：SEI 的 mastering 只有色度坐标（has_luminance=0），CLL 1000/200 完整。
    EXPECT_EQ(result.color_hdr.hdr.format, model::HdrFormat::Hdr10Plus);
    EXPECT_TRUE(result.color_hdr.hdr.mastering_display.present);
    EXPECT_FALSE(result.color_hdr.hdr.mastering_display.Complete());
    EXPECT_EQ(result.color_hdr.hdr.content_light.max_cll, 1000u);
    EXPECT_EQ(result.color_hdr.hdr.content_light.max_fall, 200u);
}

TEST(ColorHdrRealSampleTest, HlgNeedsNoStaticMetadata) {
    const std::string path = HdrSamplePath("hlg-1080p.mp4");
    if (path.empty())
        GTEST_SKIP() << "set VIDEOEYE_HDR_SAMPLE_DIR to run real HDR sample checks";
    model::AnalysisResult result;
    ASSERT_TRUE(AnalyzeHdrSample(path, result)) << path;

    EXPECT_EQ(result.color_hdr.hdr.format, model::HdrFormat::Hlg);
    const model::DimensionStatus* dim = ColorHdrDimension(result);
    ASSERT_NE(dim, nullptr);
    EXPECT_TRUE(dim->conclusive);
    EXPECT_TRUE(dim->failure_reason.empty());
}

TEST(ColorHdrRealSampleTest, SdrFileNeedsNoHdrStaticMetadata) {
    const std::string path = HdrSamplePath("bt2020-sdr.mp4");
    if (path.empty())
        GTEST_SKIP() << "set VIDEOEYE_HDR_SAMPLE_DIR to run real HDR sample checks";
    model::AnalysisResult result;
    ASSERT_TRUE(AnalyzeHdrSample(path, result)) << path;

    EXPECT_EQ(result.color_hdr.hdr.format, model::HdrFormat::Sdr);
    EXPECT_FALSE(result.color_hdr.StaticMetadataComplete()); // SDR 本来就没有静态元数据
    const model::DimensionStatus* dim = ColorHdrDimension(result);
    ASSERT_NE(dim, nullptr);
    EXPECT_TRUE(dim->conclusive);
    EXPECT_TRUE(dim->failure_reason.empty());
}

// ---- 分析器层：完整性优先级替换（真实样本里占位 side data 并不总出现，得造数据钉住）----
#include "core/analysis/quality/ColorHdrAnalyzer.h"
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
}

namespace {

void AddPacketCll(AVPacket* packet, unsigned cll, unsigned fall) {
    AVContentLightMetadata data{};
    data.MaxCLL = cll;
    data.MaxFALL = fall;
    AVPacketSideData* side = av_packet_side_data_new(&packet->side_data, &packet->side_data_elems,
                                                     AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof(data), 0);
    ASSERT_NE(side, nullptr);
    std::memcpy(side->data, &data, sizeof(data));
}

void AddPacketMastering(AVPacket* packet, const AVMasteringDisplayMetadata& data) {
    AVPacketSideData* side = av_packet_side_data_new(&packet->side_data, &packet->side_data_elems,
                                                     AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof(data), 0);
    ASSERT_NE(side, nullptr);
    std::memcpy(side->data, &data, sizeof(data));
}

void AddPacketDolbyVision(AVPacket* packet, const AVDOVIDecoderConfigurationRecord& data) {
    AVPacketSideData* side =
        av_packet_side_data_new(&packet->side_data, &packet->side_data_elems, AV_PKT_DATA_DOVI_CONF, sizeof(data), 0);
    ASSERT_NE(side, nullptr);
    std::memcpy(side->data, &data, sizeof(data));
}

void AddFrameCll(AVFrame* frame, unsigned cll, unsigned fall) {
    AVContentLightMetadata data{};
    data.MaxCLL = cll;
    data.MaxFALL = fall;
    AVBufferRef* buf = av_buffer_alloc(sizeof(data));
    ASSERT_NE(buf, nullptr);
    std::memcpy(buf->data, &data, sizeof(data));
    ASSERT_NE(
        av_frame_side_data_add(&frame->side_data, &frame->nb_side_data, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, &buf, 0),
        nullptr);
    av_buffer_unref(&buf); // 引用已转交 side data
}

void AddFrameMastering(AVFrame* frame, const AVMasteringDisplayMetadata& data) {
    AVBufferRef* buf = av_buffer_alloc(sizeof(data));
    ASSERT_NE(buf, nullptr);
    std::memcpy(buf->data, &data, sizeof(data));
    ASSERT_NE(av_frame_side_data_add(&frame->side_data, &frame->nb_side_data, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                     &buf, 0),
              nullptr);
    av_buffer_unref(&buf);
}

} // namespace

// 先到的包侧占位 CLL 0/0 + mastering 全空，不能永久挡住解码帧上的真值。
TEST(ColorHdrAnalyzerTest, CompleteFrameSideDataReplacesPlaceholderPacketSideData) {
    ColorHdrAnalyzer analyzer;
    analyzer.Reset();

    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    AddPacketCll(packet, 0, 0);
    AVMasteringDisplayMetadata placeholder{};
    AddPacketMastering(packet, placeholder);
    analyzer.UpdateFromPacket(packet, 0);
    ASSERT_TRUE(analyzer.result().hdr.content_light.present);
    EXPECT_FALSE(analyzer.result().hdr.content_light.HasAny());
    ASSERT_TRUE(analyzer.result().hdr.mastering_display.present);
    EXPECT_FALSE(analyzer.result().hdr.mastering_display.Complete());
    av_packet_free(&packet);

    AVFrame* frame = av_frame_alloc();
    ASSERT_NE(frame, nullptr);
    AddFrameCll(frame, 1000, 400);
    AVMasteringDisplayMetadata full{};
    full.has_primaries = full.has_luminance = 1;
    full.display_primaries[0][0] = AVRational{68, 100};
    full.display_primaries[0][1] = AVRational{32, 100};
    full.display_primaries[1][0] = AVRational{265, 1000};
    full.display_primaries[1][1] = AVRational{69, 100};
    full.display_primaries[2][0] = AVRational{15, 100};
    full.display_primaries[2][1] = AVRational{6, 100};
    full.white_point[0] = AVRational{3127, 10000};
    full.white_point[1] = AVRational{3290, 10000};
    full.max_luminance = AVRational{1000, 1};
    full.min_luminance = AVRational{1, 1000};
    AddFrameMastering(frame, full);
    analyzer.UpdateFromFrame(frame);
    av_frame_free(&frame);

    EXPECT_EQ(analyzer.result().hdr.content_light.max_cll, 1000u);
    EXPECT_EQ(analyzer.result().hdr.content_light.max_fall, 400u);
    EXPECT_TRUE(analyzer.result().hdr.mastering_display.Complete());
    EXPECT_NEAR(analyzer.result().hdr.mastering_display.max_luminance, 1000.0, 1e-9);
    bool tracked = false;
    for (const std::string& note : analyzer.result().notes) {
        if (note.find("替换此前占位/不完整数据") != std::string::npos)
            tracked = true;
    }
    EXPECT_TRUE(tracked) << "替换过程必须留下来源轨迹";
}

// DV 配置记录也可能先读到 profile=0 的占位，后续完整配置要能覆盖；
// RPU-only 帧数据仍要保留 rpu_present，不能被配置记录抹掉。
TEST(ColorHdrAnalyzerTest, DolbyVisionConfigReplacesPlaceholderButKeepsRpu) {
    ColorHdrAnalyzer analyzer;
    analyzer.Reset();

    AVDOVIDecoderConfigurationRecord dv{};
    dv.dv_profile = 0;
    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    AddPacketDolbyVision(packet, dv);
    analyzer.UpdateFromPacket(packet, 0);
    EXPECT_TRUE(analyzer.result().hdr.dolby_vision.present);
    EXPECT_FALSE(analyzer.result().hdr.dolby_vision.profile_known);
    av_packet_free(&packet);

    packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    dv.dv_profile = 8;
    dv.dv_bl_signal_compatibility_id = 4;
    AddPacketDolbyVision(packet, dv);
    analyzer.UpdateFromPacket(packet, 0);
    av_packet_free(&packet);

    EXPECT_TRUE(analyzer.result().hdr.dolby_vision.profile_known);
    EXPECT_EQ(analyzer.result().hdr.dolby_vision.profile, 8);
    EXPECT_EQ(analyzer.result().hdr.dolby_vision.ProfileText(), "8.4");
}
