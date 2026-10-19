// 码率 / GOP 深度分析的全链路回归测试（复查 P0：重复计数）
//
// 缺陷（已修）: AnalysisPipeline 在解码路径里，avcodec_receive_frame 循环内已按
// 每个解码帧调用一次 OnFrame，循环外又凭 fed 标志再调一次；parser 路径同样经
// fed 二次调用。于是帧数 ≈ 2×、字节数虚高、GOP 被腰斩、I/P/B 比例与异常判断
// 全部失真（详见 BitrateGopAnalyzer.h 的输入契约注释）。
//
// 修复: 输入改成显式事件（OnFrame / OnPacketBytes / OnPacket），每个语义帧只进
// 一次统计，"重复"在结构上不再可能。
//
// 这里不喂合成样本（那是 test_bitrate_gop_analyzer.cpp 的活），而是用
// TestMediaFactory 现场生成的确定性 H.264+AAC MP4（128x72 / 25fps / 40 帧 /
// gop_size=fps 每秒一个关键帧 / 无 B 帧）跑**完整分析**（AnalysisEngine::Run），
// 分别覆盖 decode_frame_types=true（解码路径）与 false（parser 路径，UI 默认）。
//
// 期望值不抄常量: 测试内现算两组**独立事实**，与被测路径零共享代码 —— 旧实现下
// 帧数会是期望的 2 倍，本测试因此对"重复计数"类回归直接红。
//
// 两组事实分工:
//   * DemuxVideo   —— 包级事实（视频包数 / 包字节和 / 关键包数），parser 路径的基准;
//   * DecodeVideo  —— 解码级事实（真实帧数 / 真实关键帧数），解码路径的基准。
// 两者必须分开: 一包不保证产出一帧（本样本末包送进解码器后既没当场出帧、flush
// 也拿不到 —— 解码器侧的行为，不是统计口径），所以"包数 == 帧数"只是 parser 路径
// 的语义，不能拿来要求解码路径。反过来，解码产出的帧数才是帧级统计的真值。

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/domain/model/AnalysisResult.h"

#include "tests/support/TestMediaFactory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

namespace {

using videoeye::AnalysisEngine;
using videoeye::AnalysisOptions;
using videoeye::model::AnalysisResult;
using videoeye::model::AnalysisStatus;

// ---- 独立的 demux 事实（与 AnalysisPipeline 零共享代码）----
struct DemuxFacts {
    bool ok = false;
    int video_stream = -1;
    int64_t video_packets = 0;
    int64_t video_bytes = 0;
    int64_t key_packets = 0;
};

DemuxFacts DemuxVideo(const std::string& path) {
    DemuxFacts facts;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt) return facts;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return facts;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar &&
            fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            facts.video_stream = static_cast<int>(i);
            break;
        }
    }
    if (facts.video_stream < 0) {
        avformat_close_input(&fmt);
        return facts;
    }
    AVPacket* pkt = av_packet_alloc();
    while (pkt != nullptr && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == facts.video_stream) {
            ++facts.video_packets;
            facts.video_bytes += pkt->size;
            if (pkt->flags & AV_PKT_FLAG_KEY) ++facts.key_packets;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    facts.ok = facts.video_packets > 0;
    return facts;
}

// ---- 独立的解码事实（与 AnalysisPipeline 零共享代码）----
// 解码路径的统计真值：真实解码出多少帧、其中多少帧是关键帧。
struct DecodedFacts {
    bool ok = false;
    int64_t frames = 0;
    int64_t key_frames = 0;
};

DecodedFacts DecodeVideo(const std::string& path) {
    DecodedFacts facts;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt) return facts;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return facts;
    }
    int video_stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar &&
            fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = static_cast<int>(i);
            break;
        }
    }
    if (video_stream < 0) {
        avformat_close_input(&fmt);
        return facts;
    }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[video_stream]->codecpar->codec_id);
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!dec || avcodec_parameters_to_context(dec, fmt->streams[video_stream]->codecpar) < 0 ||
        avcodec_open2(dec, codec, nullptr) < 0) {
        if (dec) avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return facts;
    }
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&]() {
        while (avcodec_receive_frame(dec, frame) >= 0) {
            ++facts.frames;
            if ((frame->flags & AV_FRAME_FLAG_KEY) != 0) ++facts.key_frames;
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == video_stream && avcodec_send_packet(dec, pkt) == 0) drain();
        av_packet_unref(pkt);
    }
    // EOF flush: 解码器里压着的重排序帧在这里收干净
    if (avcodec_send_packet(dec, nullptr) == 0) drain();

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    facts.ok = facts.frames > 0;
    return facts;
}

// ---- 跑一次完整分析，只开码率/GOP 维度 ----
// 其余维度与本次回归无关（音频 QC / 色彩 HDR / 码流解析 / 样本表 / 字幕 / 时码 /
// 辅助数据 / 流媒体包），关掉求快求稳: 本测试只钉"帧数 / 字节 / GOP 不重复计数"。
AnalysisResult RunBitrateGopAnalysis(const std::string& path, bool decode_frame_types) {
    AnalysisResult result;
    videoeye::AnalysisCallbacks callbacks;
    callbacks.on_failed = [](const std::string&) {};
    callbacks.on_finished = [](bool, const AnalysisResult&) {};

    AnalysisOptions options;
    options.analyze_bitrate_gop = true;
    options.decode_frame_types = decode_frame_types;
    options.analyze_audio_qc = false;
    options.analyze_color_hdr = false;
    options.analyze_bitstream = false;
    options.analyze_mp4_sample_table = false;
    options.analyze_streaming_package = false;
    options.analyze_subtitle = false;
    options.analyze_timecode = false;
    options.analyze_aux_data = false;

    AnalysisEngine engine;
    engine.Run(path, options, callbacks, &result);
    return result;
}

// 生成一份确定性样本（工厂每次remove后重写，内容一致）
std::string MakeMedia() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "videoeye_bitrate_gop_regression").string();
    std::filesystem::create_directories(dir);
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "clip");
    EXPECT_TRUE(media.ok) << media.error;
    return media.path;
}

// 公共断言: 帧数 / 字节 / GOP 三类核心统计与独立事实逐一对齐
// expected_frames / expected_gops 由调用方按路径语义给出（解码路径用解码事实，
// parser 路径用包级事实），"不重复计数"的硬约束在这里统一收口。
void ExpectNoDoubleCounting(const AnalysisResult& result, const DemuxFacts& facts,
                            int64_t expected_frames, int64_t expected_gops) {
    const videoeye::model::BitrateGopAnalysis& gop = result.bitrate_gop;

    // 总帧数 == 该路径的独立帧数真值。重复计数缺陷下这里是 2 倍。
    EXPECT_EQ(gop.total_frames, expected_frames);
    // 结构性上界: 任何路径都不该比"视频包数"还多 —— 这条是纯防重复的红线，
    // 与"解码器丢没丢最后一帧"无关。
    EXPECT_LE(gop.total_frames, facts.video_packets);
    // 总字节 == 视频包字节和（一个字节都不能多，也不能少）
    EXPECT_EQ(gop.total_bytes, facts.video_bytes);
    // GOP 数 == 独立事实里的关键帧数
    ASSERT_EQ(gop.gops.size(), static_cast<size_t>(expected_gops));
    // I 帧数 == 关键帧数（该样本只有 IDR 是关键帧）
    EXPECT_EQ(gop.i_frame_count, expected_gops);

    // 帧类型三分账必须闭合
    EXPECT_EQ(gop.i_frame_count + gop.p_frame_count + gop.b_frame_count +
                  gop.unknown_frame_count,
              gop.total_frames);
    EXPECT_TRUE(gop.frame_types_known);

    // 每个 GOP 的帧数之和 == 总帧数（GOP 边界没被重复切）
    int64_t gop_frames = 0;
    for (const auto& g : gop.gops) gop_frames += g.frame_count;
    EXPECT_EQ(gop_frames, gop.total_frames);

    // 码率汇总自洽（曲线非空、峰值不低于均值(5% 容差)、未触发降采样）
    EXPECT_GT(gop.avg_bitrate_kbps, 0.0);
    // 峰值不低于均值是**统计意义上**的不变量，这里不能写成严格 >=：
    // peak 是滑动窗口口径（窗口内字节 / 窗口长度），avg 是 total_bytes / duration。
    // 本样本只有 40 帧、2 个 I 帧，而 x264 的 keyint(25 帧) 恰好等于默认窗口长度
    // (1.0s)，于是每个窗口恒定只含 1 个 I 帧，avg 按全时长分摊却相当于 1.25 个 ——
    // 短样本下 peak 会略低于 avg（FFmpeg 7.1 + x264 实测 101.3 vs 103.9，约 -2.5%）。
    // 字节分布还随本地 x264/FFmpeg 版本浮动（Win 17418B vs Linux 20786B），所以给
    // 5% 容差：仍然能兜住真正的异常（重复计数 / 窗口口径写错会让偏差远大于此）。
    EXPECT_GE(gop.peak_bitrate_kbps, gop.avg_bitrate_kbps * 0.95);
    EXPECT_FALSE(gop.bitrate_points.empty());
    EXPECT_FALSE(gop.downsampled);
    // avg ≈ total_bytes * 8 / 1000 / duration（duration 由首末帧时间戳推得，10% 容差）
    if (gop.duration_seconds > 0.0) {
        const double expect_avg =
            static_cast<double>(gop.total_bytes) * 8.0 / 1000.0 / gop.duration_seconds;
        EXPECT_NEAR(gop.avg_bitrate_kbps, expect_avg, expect_avg * 0.10);
    }
}

}  // namespace

// ---- 解码路径（decode_frame_types=true）----
TEST(BitrateGopPipelineRegression, DecoderPathCountsEachFrameExactlyOnce) {
    const std::string path = MakeMedia();
    const DemuxFacts facts = DemuxVideo(path);
    const DecodedFacts decoded = DecodeVideo(path);
    ASSERT_TRUE(facts.ok);
    ASSERT_TRUE(decoded.ok);
    ASSERT_GT(facts.video_packets, 0);
    ASSERT_GT(decoded.key_frames, 0);

    const AnalysisResult result = RunBitrateGopAnalysis(path, /*decode_frame_types=*/true);
    ASSERT_EQ(result.scan_status, AnalysisStatus::Complete) << result.error_message;
    ExpectNoDoubleCounting(result, facts, decoded.frames, decoded.key_frames);
}

// ---- parser 路径（decode_frame_types=false，UI 默认）----
// parser 按"一个完整包 = 一帧"记账，所以这里的帧数真值是包数。
TEST(BitrateGopPipelineRegression, ParserPathCountsEachFrameExactlyOnce) {
    const std::string path = MakeMedia();
    const DemuxFacts facts = DemuxVideo(path);
    ASSERT_TRUE(facts.ok);
    ASSERT_GT(facts.video_packets, 0);
    ASSERT_GT(facts.key_packets, 0);

    const AnalysisResult result = RunBitrateGopAnalysis(path, /*decode_frame_types=*/false);
    ASSERT_EQ(result.scan_status, AnalysisStatus::Complete) << result.error_message;
    ExpectNoDoubleCounting(result, facts, facts.video_packets, facts.key_packets);
}

// ---- 两条路径都必须落在"包数"这个上界内 ----
// 字节数与 GOP 划分与路径无关（同一份字节、同一组关键帧位置），必须逐项相同；
// 帧数允许差在解码器自己的产出上（见文件头对末包的说明），但两条路径都不能
// 越过视频包数 —— 一旦越过就是重复计数回来了。
TEST(BitrateGopPipelineRegression, NeitherPathExceedsPacketCount) {
    const std::string path = MakeMedia();
    const DemuxFacts facts = DemuxVideo(path);
    const DecodedFacts decoded = DecodeVideo(path);
    ASSERT_TRUE(facts.ok);

    const AnalysisResult decoded_run = RunBitrateGopAnalysis(path, true);
    const AnalysisResult parsed_run = RunBitrateGopAnalysis(path, false);
    ASSERT_EQ(decoded_run.scan_status, AnalysisStatus::Complete) << decoded_run.error_message;
    ASSERT_EQ(parsed_run.scan_status, AnalysisStatus::Complete) << parsed_run.error_message;

    EXPECT_LE(decoded_run.bitrate_gop.total_frames, facts.video_packets);
    EXPECT_LE(parsed_run.bitrate_gop.total_frames, facts.video_packets);
    // 字节是"这一路所有视频包的总和"，与帧归属无关，两条路径必须一致
    EXPECT_EQ(decoded_run.bitrate_gop.total_bytes, facts.video_bytes);
    EXPECT_EQ(parsed_run.bitrate_gop.total_bytes, facts.video_bytes);
    ASSERT_EQ(decoded_run.bitrate_gop.gops.size(), parsed_run.bitrate_gop.gops.size());
    // 解码路径的帧数就是独立解码事实的帧数（真值对齐）
    EXPECT_EQ(decoded_run.bitrate_gop.total_frames, decoded.frames);
}
