#include <gtest/gtest.h>

// ============================================================
// 阶段 6 / 切片 T：S08 的轨级决策（copy / transcode / drop）
//
// 旧实现的策略是一句隐含假设：「主视频轨重编码 + 每条音频轨按原码流复制」，
// 三种后果都是实测确认的：
//   * 第二视频轨 / 封面 / 字幕 / 数据轨**无声消失**，报告里一个字都没有；
//   * 音频轨一律复制参数，目标容器拒绝其中一条时 avformat_write_header 整体失败，
//     整个转码连带失败，文案只说「音频编码参数可能不被目标容器接受」；
//   * 「这条轨为什么重编码」从来没有落过账。
//
// 这一组用例盯住四件事：
//   1. 每条输入轨恰好一条决策（覆盖自证，下标也要唯一）；
//   2. 容器接受性是**实测**出来的（内存试写头），不是查表 / 按扩展名推断；
//   3. 主视频轨的重编码理由必须区分「该轨有解码层损伤证据」与「模式选择」，
//      没有证据时不许暗示这是修复所必需；
//   4. 决策真的进报告（TXT/HTML/JSON 同源），被丢弃的轨出现在「未保留」清单里。
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "core/domain/model/RepairRequest.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairTranscodeTracks.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/TestMediaFactory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace {

using namespace videoeye;
using namespace videoeye::model;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_tracks_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return 0;
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

repair::TranscodeTrackProbe Probe(int index, const std::string& kind, const std::string& codec) {
    repair::TranscodeTrackProbe probe;
    probe.input_stream_index = index;
    probe.kind = kind;
    probe.source_codec = codec;
    return probe;
}

const RepairTranscodeTrackDecision* FindDecision(const repair::TranscodeTrackPlan& plan, int index) {
    for (const auto& decision : plan.decisions) {
        if (decision.input_stream_index == index)
            return &decision;
    }
    return nullptr;
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = RepairVerificationLevel::V1_FrameSample;
    request.write_report = true;
    return request;
}

// 真实编码器的轨道参数：容器接受性只能靠真实参数去试写头，手工拼的
// AVCodecParameters（没有 extradata / frame_size）会得到与真实转码不同的结论。
struct EncodedParameters {
    AVCodecContext* context = nullptr;
    AVCodecParameters* parameters = nullptr;

    ~EncodedParameters() {
        if (parameters != nullptr)
            avcodec_parameters_free(&parameters);
        if (context != nullptr)
            avcodec_free_context(&context);
    }

    bool Build(AVCodecID codec_id, int sample_rate, int channels) {
        const AVCodec* codec = avcodec_find_encoder(codec_id);
        if (codec == nullptr)
            return false;
        context = avcodec_alloc_context3(codec);
        if (context == nullptr)
            return false;
        context->sample_rate = sample_rate;
        av_channel_layout_default(&context->ch_layout, channels);
        context->sample_fmt = codec->sample_fmts != nullptr ? codec->sample_fmts[0] : AV_SAMPLE_FMT_S16;
        context->bit_rate = 128000;
        if (avcodec_open2(context, codec, nullptr) < 0)
            return false;
        parameters = avcodec_parameters_alloc();
        if (parameters == nullptr)
            return false;
        return avcodec_parameters_from_context(parameters, context) >= 0;
    }
};

// ---- 覆盖自证 ----

TEST(RepairTranscodeTracks, EveryInputStreamGetsExactlyOneDecision) {
    std::vector<repair::TranscodeTrackProbe> probes = {
        Probe(0, "video", "h264"),
        Probe(1, "audio", "aac"),
        Probe(2, "subtitle", "mov_text"),
        Probe(3, "data", "bin_data"),
    };
    probes[1].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    ASSERT_EQ(plan.decisions.size(), probes.size());
    EXPECT_TRUE(plan.complete);
    EXPECT_EQ(plan.primary_video_index, 0);
    EXPECT_EQ(plan.copy_count, 1u);
    EXPECT_EQ(plan.transcode_count, 1u);
    EXPECT_EQ(plan.drop_count, 2u);
    for (const auto& probe : probes) {
        const RepairTranscodeTrackDecision* decision = FindDecision(plan, probe.input_stream_index);
        ASSERT_NE(decision, nullptr) << "流 #" << probe.input_stream_index << " 没有决策";
        EXPECT_FALSE(decision->action.empty());
        EXPECT_FALSE(decision->reason_code.empty());
        EXPECT_FALSE(decision->reason.empty());
        EXPECT_FALSE(decision->text.empty()) << "报告层只打印 text，不许留空";
    }
}

// 下标重复时条数照样对得上，但会有一条轨被报两次、另一条彻底消失 ——
// 「条数相等」不足以自证覆盖，所以还要查下标唯一性。
TEST(RepairTranscodeTracks, DuplicateIndexBreaksCoverageProof) {
    std::vector<repair::TranscodeTrackProbe> probes = {
        Probe(0, "video", "h264"),
        Probe(1, "audio", "aac"),
        Probe(1, "audio", "aac"),
    };
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);
    EXPECT_FALSE(plan.complete) << "同一输入流出现两条决策，覆盖自证必须失败";
    EXPECT_NE(plan.summary.find("不一致"), std::string::npos) << plan.summary;
}

// ---- 主视频轨选择 ----

TEST(RepairTranscodeTracks, CoverArtIsNeverThePrimaryVideoTrack) {
    std::vector<repair::TranscodeTrackProbe> probes = {
        Probe(0, "attached_pic", "mjpeg"),
        Probe(1, "video", "h264"),
        Probe(2, "audio", "aac"),
    };
    probes[2].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    EXPECT_EQ(plan.primary_video_index, 1) << "封面图（attached_pic）不能被选成主视频轨";
    const RepairTranscodeTrackDecision* cover = FindDecision(plan, 0);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->action, "drop");
    EXPECT_EQ(cover->reason_code, "cover_art_not_rebuilt");
    const RepairTranscodeTrackDecision* video = FindDecision(plan, 1);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->action, "transcode");
}

TEST(RepairTranscodeTracks, NoVideoTrackMeansNoPrimary) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "audio", "aac")};
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);
    EXPECT_EQ(plan.primary_video_index, -1);
    // 没有主视频轨时音频决策照旧给出（调用方会以「没有可转码的视频轨」收尾），
    // 但绝不能凭空造一条视频决策。
    EXPECT_EQ(plan.transcode_count, 0u);
}

// ---- 音频：容器接受性决定 copy / drop ----

TEST(RepairTranscodeTracks, AudioRejectedByContainerIsDroppedNotCopied) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "audio", "flac")};
    probes[1].container_fit = repair::TranscodeContainerFit::Rejected;
    probes[1].container_fit_detail = "封装器返回 Invalid argument";
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* audio = FindDecision(plan, 1);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->action, "drop") << "容器拒绝的轨不能照旧复制 → 整份产物写头失败";
    EXPECT_EQ(audio->reason_code, "audio_drop_container_rejected");
    EXPECT_NE(audio->reason.find("写头失败"), std::string::npos) << audio->reason;
    // 实测原因要带出来：读者需要知道这是封装器判的，不是我们的猜测。
    EXPECT_NE(audio->reason.find("封装器返回 Invalid argument"), std::string::npos) << audio->reason;
}

TEST(RepairTranscodeTracks, AudioWithoutMeasurementIsCopiedAndSaysSo) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "audio", "aac")};
    probes[1].container_fit = repair::TranscodeContainerFit::Unknown;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* audio = FindDecision(plan, 1);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->action, "copy");
    EXPECT_EQ(audio->reason_code, "audio_copy_container_unprobed") << "没实测就说「接受」是假覆盖";
    EXPECT_NE(audio->reason.find("未能实测"), std::string::npos) << audio->reason;
}

TEST(RepairTranscodeTracks, AudioAcceptedByContainerReportsTheMeasurement) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "audio", "aac")};
    probes[1].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);
    const RepairTranscodeTrackDecision* audio = FindDecision(plan, 1);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->action, "copy");
    EXPECT_EQ(audio->reason_code, "audio_copy_container_ok");
    EXPECT_NE(audio->reason.find("试写头接受"), std::string::npos) << audio->reason;
}

// ---- 主视频轨：重编码的理由必须来自证据 ----

TEST(RepairTranscodeTracks, VideoReasonDistinguishesDamageFromModeChoice) {
    std::vector<repair::TranscodeTrackProbe> with_damage = {Probe(0, "video", "h264")};
    with_damage[0].decode_damage_detected = true;
    const repair::TranscodeTrackPlan damaged = repair::PlanTranscodeTracks(with_damage);
    ASSERT_EQ(damaged.decisions.size(), 1u);
    EXPECT_EQ(damaged.decisions[0].action, "transcode");
    EXPECT_EQ(damaged.decisions[0].reason_code, "video_reencode_decode_damage");

    std::vector<repair::TranscodeTrackProbe> without_damage = {Probe(0, "video", "h264")};
    const repair::TranscodeTrackPlan clean = repair::PlanTranscodeTracks(without_damage);
    ASSERT_EQ(clean.decisions.size(), 1u);
    EXPECT_EQ(clean.decisions[0].action, "transcode");
    EXPECT_EQ(clean.decisions[0].reason_code, "video_reencode_mode_choice");
    // 反向守护：没有损伤证据时不许把重编码写成「该轨坏了」。
    EXPECT_NE(clean.decisions[0].reason.find("未检出编码层损伤证据"), std::string::npos) << clean.decisions[0].reason;
    EXPECT_NE(clean.decisions[0].reason.find("不是该轨必需的修复"), std::string::npos) << clean.decisions[0].reason;
}

// ---- 非音视频轨 ----

TEST(RepairTranscodeTracks, EveryNonCarriedTrackKindIsNamedInTheDropReason) {
    std::vector<repair::TranscodeTrackProbe> probes = {
        Probe(0, "video", "h264"),     Probe(1, "subtitle", "subrip"), Probe(2, "data", "bin_data"),
        Probe(3, "attachment", "ttf"), Probe(4, "other", "unknown"),
    };
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);
    const char* expected_codes[] = {"subtitle_not_carried", "data_not_carried", "attachment_not_carried",
                                    "unclassified_not_carried"};
    for (int index = 1; index <= 4; ++index) {
        const RepairTranscodeTrackDecision* decision = FindDecision(plan, index);
        ASSERT_NE(decision, nullptr) << "流 #" << index;
        EXPECT_EQ(decision->action, "drop");
        EXPECT_EQ(decision->reason_code, expected_codes[index - 1]) << decision->reason;
    }
}

TEST(RepairTranscodeTracks, DiscardedStreamHasItsOwnReason) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "audio", "aac")};
    probes[1].discarded = true;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);
    const RepairTranscodeTrackDecision* audio = FindDecision(plan, 1);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->action, "drop");
    EXPECT_EQ(audio->reason_code, "stream_marked_discard") << "源侧已丢弃与容器拒绝是两件事，不能共用一个理由码";
}

// ---- 渲染：唯一入口 ----

TEST(RepairTranscodeTracks, DecisionTextAndOmittedTextHaveStableShape) {
    model::RepairTranscodeTrackDecision decision;
    decision.input_stream_index = 2;
    decision.kind = "audio";
    decision.source_codec = "flac";
    decision.action = "drop";
    decision.reason = "目标容器拒绝该编码";

    const std::string text = repair::TranscodeTrackDecisionText(decision);
    EXPECT_EQ(text, "流 #2（audio, flac）→ 未保留：目标容器拒绝该编码");
    const std::string omitted = repair::TranscodeTrackOmittedText(decision);
    EXPECT_EQ(omitted, "流 #2（audio, flac）：目标容器拒绝该编码")
        << "「未保留」清单已有前缀，短形式不许再拼一次动作词";
}

// ---- 容器接受性：必须来自实测 ----

TEST(RepairTranscodeTracks, ContainerProbeMeasuresTheRealMuxer) {
    EncodedParameters aac;
    ASSERT_TRUE(aac.Build(AV_CODEC_ID_AAC, 44100, 2)) << "本构建缺少 AAC 编码器";
    std::string detail;
    EXPECT_EQ(repair::ProbeTrackFitsContainer("mp4", aac.parameters, &detail), repair::TranscodeContainerFit::Accepted)
        << detail;

    EncodedParameters flac;
    if (!flac.Build(AV_CODEC_ID_FLAC, 44100, 2)) {
        GTEST_SKIP() << "本构建没有 FLAC 编码器，无法构造「容器拒绝」的实测样本";
    }
    detail.clear();
    EXPECT_EQ(repair::ProbeTrackFitsContainer("mp4", flac.parameters, &detail), repair::TranscodeContainerFit::Accepted)
        << "FLAC 在 MP4 里是支持的，不能凭印象判拒绝：" << detail;
    detail.clear();
    EXPECT_EQ(repair::ProbeTrackFitsContainer("mov", flac.parameters, &detail), repair::TranscodeContainerFit::Rejected)
        << "FLAC 只在 MP4 受支持，mov 必须实测为拒绝";
    // 封装器的具体理由走 av_log（不抓日志），返回值只有错误码；这里钉的是
    // 「必须带回一个非空、能说明是封装器判的」的实测原因，不是它的原话。
    EXPECT_NE(detail.find("封装器返回"), std::string::npos) << detail;

    // 没有这个封装器 → 只能报「没测成」，不能报接受或拒绝。
    detail.clear();
    EXPECT_EQ(repair::ProbeTrackFitsContainer("no_such_muxer", aac.parameters, &detail),
              repair::TranscodeContainerFit::Unknown);
    EXPECT_FALSE(detail.empty());
}

// ---- 端到端：多视频轨必须逐条交代 ----

TEST(RepairTranscodeTracks, S08AccountsForTheSecondVideoTrack) {
    const std::string dir = MakeTempDir("e2e");
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 20;
    spec.extra_video_tracks = 1;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "two_video", spec);
    ASSERT_TRUE(media.ok) << media.error;
    const auto source_probe = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source_probe.opened) << source_probe.error;
    ASSERT_EQ(source_probe.video_streams, 2) << "语料必须先真的带上两条视频轨";

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    // 1. 决策表覆盖全部输入流，且没有把第二视频轨当成主轨去重建。
    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    ASSERT_EQ(fidelity.track_decision_input_streams, 3) << "源侧 2 视频 + 1 音频";
    ASSERT_EQ(fidelity.track_decisions.size(), 3u);
    const RepairTranscodeTrackDecision* extra = nullptr;
    for (const auto& decision : fidelity.track_decisions) {
        if (decision.kind == "video" && decision.input_stream_index != 0)
            extra = &decision;
    }
    ASSERT_NE(extra, nullptr) << "第二视频轨没有决策";
    EXPECT_EQ(extra->action, "drop");
    EXPECT_EQ(extra->reason_code, "extra_video_single_chain");
    EXPECT_EQ(fidelity.track_decisions[0].action, "transcode") << "主视频轨必须被重建";

    // 2. 产物只有一条视频轨 —— 报告说「未保留」，产物里也不能有。
    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    EXPECT_EQ(output_probe.video_streams, 1);

    // 3. 未保留项进 omitted_items（这是报告里「未保留」清单的唯一来源）。
    const bool omitted_mentions_second_video =
        std::any_of(result.fidelity.omitted_items.begin(), result.fidelity.omitted_items.end(),
                    [](const std::string& item) { return item.find("第二视频轨") != std::string::npos; });
    EXPECT_TRUE(omitted_mentions_second_video) << "第二视频轨被丢了却不在未保留清单里";

    // 4. 三份报告同源：汇总行 + 未保留行各一次，不重复也不遗漏。
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("轨道决策"), std::string::npos) << "报告缺少轨道决策行";
        EXPECT_NE(body.find("第二视频轨"), std::string::npos) << "报告没有交代第二视频轨的去向";
        // 丢轨理由会出现在「未保留」清单、「已知限制」与「明确未恢复项」三处
        // （后两者引用的都是同一条结论）。决策表**不能**再加一份：决策行以
        // 「轨道决策: 流 #N」的形式出现，落选的轨不该有这种行。
        EXPECT_EQ(body.find("轨道决策: 流 #1（video"), std::string::npos)
            << "drop 决策被当成决策行打印，与未保留清单重复";
        EXPECT_GE(CountOccurrences(body, "第二视频轨"), 1u);
    }

    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* fidelity_node = root.Find("fidelity");
    ASSERT_NE(fidelity_node, nullptr);
    const JsonValue* transcode = fidelity_node->Find("transcode");
    ASSERT_NE(transcode, nullptr);
    const JsonValue* decisions = transcode->Find("track_decisions");
    ASSERT_NE(decisions, nullptr);
    EXPECT_TRUE(decisions->IsArray());
    ASSERT_EQ(decisions->Size(), 3u);
    EXPECT_EQ(transcode->Find("track_decision_count")->IntValueOr(-1), 3);
    EXPECT_EQ(transcode->Find("track_decision_input_streams")->IntValueOr(-1), 3);
    bool json_has_drop = false;
    for (const auto& node : decisions->Items()) {
        if (node.Find("reason_code")->StringValueOr("") == "extra_video_single_chain")
            json_has_drop = node.Find("action")->StringValueOr("") == "drop";
    }
    EXPECT_TRUE(json_has_drop) << "JSON 里必须能机读到这条丢轨决策";
}

// 单视频轨 + 音频的常规路径：决策表不得凭空写出「未保留」。
TEST(RepairTranscodeTracks, SingleVideoPathHasNothingDropped) {
    const std::string dir = MakeTempDir("single");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "single");
    ASSERT_TRUE(media.ok) << media.error;

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    ASSERT_EQ(fidelity.track_decisions.size(), 2u);
    EXPECT_EQ(fidelity.track_decisions[0].action, "transcode");
    EXPECT_EQ(fidelity.track_decisions[1].action, "copy");
    EXPECT_EQ(fidelity.track_decisions[1].reason_code, "audio_copy_container_ok")
        << "mp4 + aac 必须由试写头实测判为接受：" << fidelity.track_decisions[1].reason;
    EXPECT_EQ(fidelity.track_decision_summary, "复制 1 轨 / 重编码 1 轨 / 未保留 0 轨");
}

} // namespace
