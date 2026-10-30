// 码流一致性（端到端）：从 AnalysisPipeline 入口跑**真实编码样本**，而不是直接调解析器。
//
// 为什么需要这一层（docs/audit/CODEC_AUDIT_2026-10-24.md 第五节 Step 0）
// ---------------------------------------------------------------------
// 现有的解析器用例几乎都是"逐位拼出参数集，断言解析出来的数等于拼进去的数"。
// 这类用例有个共同盲区：**生成器和解析器可能犯同一个错**。383×215 那个错值
// 能存活很久，就是因为测试只断言"解析成功 + 等于生成器写的值"。
//
// 这一层补两个东西：
//   1) 输入是 ffmpeg/libx264 真正编出来的文件（tests/corpus/codec/），不是我们自己拼的位；
//   2) 期望值来自**其它工具**（ffprobe），硬编码在这里并注明获取命令 —— 见下方
//      kSamples 的 expected_* 字段。
//
// 还有一个只有端到端才能回答的问题：巡检器到底被**启用**过没有。`analyzed == true`
// 只说明"跑过解析"，`packets_scanned > 0` 才说明包真的喂进去了。以前踩过
// "修完解析器但入口没接线 → 结果永远是空的，所有单测却全绿"。
//
// 期望值的来源（重新采集用这条命令，不要照抄实现里的常量）：
//   ffprobe -v error -select_streams v:0 -count_frames \
//     -show_entries stream=codec_name,width,height,nb_read_frames -of default=nw=1 <文件>
//
// 语料由 tests/corpus/generate_real_corpus.py 生成并入库；缺失时 SKIP 而不是 FAIL
// （没跑过生成器的机器不该红，CI 上样本随仓库走所以是必跑的）。

#include <gtest/gtest.h>

#include <cctype>
#include <cstdio>
#include <string>

#include "core/analysis/orchestration/detail/AnalysisPipeline.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/BitstreamInfo.h"

#include "tests/support/FrameOrderTestSupport.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace {

using namespace videoeye;
using videoeye_test::CodecSample;
using videoeye_test::SkipIfMissing;

// 一份样本的"语义黄金值"。全部来自 ffprobe（见文件头命令），不是从实现里抄的。
struct Sample {
    const char* file = "";
    const char* codec_name = ""; // BitstreamAnalysisResult::codec_name 的口径（小写）
    int width = 0;
    int height = 0;
    int frames = 0; // ffprobe -count_frames 的 nb_read_frames
    // 巡检器是否该在这一编码上启用（VideoEsInspector::Reset 只认 h264/hevc/av1）
    bool es_inspector_enabled = false;
    // slice QP 是否有实现（目前只有 H.264 做了）
    bool slice_qp_supported = false;
};

const Sample kSamples[] = {
    {"real_x264_bframes_gop.mp4", "h264", 320, 176, 30, true, true},
    {"real_x265_bframes_gop.mp4", "hevc", 192, 108, 40, true, false},
    {"real_vp9_altref_gop.webm", "vp9", 160, 96, 50, false, false},
    {"real_av1_altref_gop.mp4", "av1", 176, 96, 60, true, false},
};

// 走一遍完整流水线：开文件 → 填流摘要 → Prepare → 逐包 OnPacket → BeforeClose。
//
// 流摘要（result.streams）按 AnalysisInputSession::BuildStreamDigests 的口径手工填：
// 这里只需要主轨的类型与编码，够 Prepare 选主轨、够 GOP 记账按流归集。
bool RunThroughPipeline(const std::string& path, model::AnalysisResult& result, std::string& why) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        why = "avformat_open_input 失败";
        return false;
    }
    if (avformat_find_stream_info(format, nullptr) < 0) {
        avformat_close_input(&format);
        why = "avformat_find_stream_info 失败";
        return false;
    }

    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVStream* stream = format->streams[i];
        model::StreamDigest digest;
        digest.index = static_cast<int>(i);
        digest.media_type = stream->codecpar->codec_type;
        digest.codec_name = avcodec_get_name(stream->codecpar->codec_id);
        digest.width = stream->codecpar->width;
        digest.height = stream->codecpar->height;
        digest.sample_rate = stream->codecpar->sample_rate;
        digest.channels = stream->codecpar->ch_layout.nb_channels;
        digest.packet_count = static_cast<int64_t>(stream->nb_frames);
        result.streams.push_back(digest);
    }

    AnalysisOptions options;
    AnalysisPipeline pipeline{options};
    const double duration = format->duration > 0 ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0;
    pipeline.Prepare(format, result, duration);

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        pipeline.ReleaseProbes();
        avformat_close_input(&format);
        why = "av_packet_alloc 失败";
        return false;
    }
    int64_t index = 0;
    while (av_read_frame(format, packet) >= 0) {
        pipeline.OnPacket(format, packet, result, index++);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    pipeline.BeforeClose(format, result);
    pipeline.ReleaseProbes();
    avformat_close_input(&format);
    return true;
}

// 打印一行的诊断（断言挂了时不用再猜实际值是多少）
void Dump(const Sample& sample, const model::AnalysisResult& result) {
    const model::BitstreamAnalysisResult& bs = result.bitstream_analysis;
    const model::SliceSeiSummary& es = bs.slice_sei;
    std::printf("[conformance] %-30s analyzed=%d codec=%s %dx%d slice_sei(analyzed=%d scanned=%d frames=%d "
                "i=%d p=%d b=%d qp_valid=%d)\n",
                sample.file, bs.analyzed ? 1 : 0, bs.codec_name.c_str(), bs.width, bs.height, es.analyzed ? 1 : 0,
                es.packets_scanned, es.slices.frames, es.slices.type_i, es.slices.type_p, es.slices.type_b,
                es.slices.qp_valid ? 1 : 0);
}

// 每个样本一条用例：跑完整流水线，然后把"解析出来的"与 ffprobe 的"黄金值"对账。
class CodecConformance : public ::testing::TestWithParam<Sample> {};

TEST_P(CodecConformance, MatchesFfprobeAndRunsTheInspector) {
    const Sample& sample = GetParam();
    const std::string path = CodecSample(sample.file);
    SkipIfMissing(path, sample.file);

    model::AnalysisResult result;
    std::string why;
    ASSERT_TRUE(RunThroughPipeline(path, result, why)) << why;
    Dump(sample, result);

    const model::BitstreamAnalysisResult& bs = result.bitstream_analysis;

    // ---- 1) 码流解析真的跑过，且认得这个编码 ----
    EXPECT_TRUE(bs.analyzed) << sample.file << " 没有执行码流解析";
    EXPECT_EQ(bs.codec_name, sample.codec_name) << sample.file;

    // ---- 2) 语义值与 ffprobe 对账（这才是"其它工具给的期望值"）----
    EXPECT_EQ(bs.width, sample.width) << sample.file << " 宽度应等于 ffprobe 的值";
    EXPECT_EQ(bs.height, sample.height) << sample.file << " 高度应等于 ffprobe 的值";

    // ---- 3) 巡检器：该启用的必须真的扫到包，不该启用的必须明确未启用 ----
    const model::SliceSeiSummary& es = bs.slice_sei;
    if (sample.es_inspector_enabled) {
        EXPECT_TRUE(es.analyzed) << sample.file << " 巡检器没跑";
        EXPECT_GT(es.packets_scanned, 0) << sample.file
                                         << " 巡检器一个包都没收到 —— 入口可能没接线（只看 analyzed 是发现不了的）";
        EXPECT_EQ(es.codec_name, sample.codec_name) << sample.file;
    } else {
        EXPECT_FALSE(es.analyzed) << sample.file << " 该编码没有巡检实现，不该产出结果";
        EXPECT_EQ(es.packets_scanned, 0) << sample.file;
    }

    // ---- 4) slice 统计与 QP 的口径 ----
    // ❗ 只有实现了 slice 解析的编码才有 frames/type_* 可断言。HEVC / AV1 的巡检
    //    会喂包（packets_scanned > 0）但 supported=false —— 它们只是"包进来了"，
    //    没有逐 slice 的结论，别把 H.264 的口径套上去。
    if (sample.slice_qp_supported) {
        EXPECT_TRUE(es.slices.supported) << sample.file;
        EXPECT_GT(es.slices.frames, 0) << sample.file;
        EXPECT_GT(es.slices.type_i, 0) << sample.file << " 真实样本首帧必是 I，I 帧计数不该为 0";
        // 硬对账：slice 帧数必须等于 ffprobe 数出来的帧数。这条能抓住"少读一位还能
        // 凑出一个看似合法的值"那类畸形 —— 只断言 >0 是抓不住的。
        EXPECT_EQ(es.slices.frames, sample.frames) << sample.file << " slice 帧数与 ffprobe 的 nb_read_frames 不一致";
        // 样本是 libx264 -bf 3 编的，没有 B 帧的样本没法证明重排相关逻辑。
        EXPECT_GT(es.slices.type_b, 0) << sample.file << " 该样本应含 B 帧（-bf 3）";
        EXPECT_TRUE(es.slices.qp_valid) << sample.file << " 有 SPS/PPS 的真实 H.264 应能算出 QP";
    } else if (sample.es_inspector_enabled) {
        EXPECT_FALSE(es.slices.supported) << sample.file << " 该编码的 slice QP 尚未实现";
        EXPECT_FALSE(es.slices.unsupported_reason.empty())
            << sample.file << " 不支持要写清理由，不能留空让读者以为「没 QP」";
        EXPECT_EQ(es.slices.frames, 0) << sample.file << " 没实现 slice 解析就不该凭空出帧数";
    }
}

// 用例名只能用字母数字下划线：文件名里的 '.' 要换掉，否则 gtest 拒绝编译期生成名字。
std::string SampleName(const ::testing::TestParamInfo<Sample>& info) {
    std::string name = info.param.file;
    for (char& ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0)
            ch = '_';
    }
    return name;
}

INSTANTIATE_TEST_SUITE_P(RealSamples, CodecConformance, ::testing::ValuesIn(kSamples), SampleName);

} // namespace
