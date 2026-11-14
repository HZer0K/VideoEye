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
#include "core/repair/RepairTolerantTranscode.h"
#include "core/repair/RepairTranscodeTracks.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/MediaSequenceFingerprint.h"
#include "tests/support/TestMediaFactory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
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
    probes[0].container_fit = repair::TranscodeContainerFit::Accepted;
    probes[2].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    // 切片 AA：封面图现在**会被携带**（按原字节复制），但携带不等于它是内容轨 ——
    // 主视频轨选择必须仍跳过它。容器接受性显式给「实测接受」，这样决策行走
    // cover_art_copy_container_ok；「没测成不许写成接受」另有专门用例守。
    EXPECT_EQ(plan.primary_video_index, 1) << "封面图（attached_pic）不能被选成主视频轨";
    const RepairTranscodeTrackDecision* cover = FindDecision(plan, 0);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->action, "copy");
    EXPECT_EQ(cover->reason_code, "cover_art_copy_container_ok");
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

// ---- 音频：容器接受性决定 copy / transcode / drop ----

// 切片 V 之后：容器拒绝且**一条重编码路都没有**时才 drop。这条用例守的是
// 「拒绝 ≠ 只能丢轨」的另一半 —— 真的没路可走时也必须 drop，并且把封装器的
// 原话和逐候选尝试一起带出来（不能说「没有可用编码器」了事）。
TEST(RepairTranscodeTracks, AudioRejectedWithoutAnyTargetIsDroppedNotCopied) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "audio", "flac")};
    probes[1].container_fit = repair::TranscodeContainerFit::Rejected;
    probes[1].container_fit_detail = "封装器返回 Invalid argument";
    probes[1].audio_reencode_note = "aac: 容器拒绝（Invalid argument）；ac3: 本构建没有该编码器";
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* audio = FindDecision(plan, 1);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->action, "drop") << "没有任何可用目标时不能硬转，只能剔除并报出";
    EXPECT_EQ(audio->reason_code, "audio_drop_no_fitting_encoder");
    // 实测原因要带出来：读者需要知道这是封装器判的，不是我们的猜测。
    EXPECT_NE(audio->reason.find("封装器返回 Invalid argument"), std::string::npos) << audio->reason;
    EXPECT_NE(audio->reason.find("ac3: 本构建没有该编码器"), std::string::npos)
        << "每个候选被拒的原因都要跟着结论走：" << audio->reason;
}

// 决策层不能自己决定「有没有目标」—— 那是执行层实测出来的结果。同一条被拒的轨，
// 有目标就 transcode，没目标就 drop，两者的理由码必须不同（否则报告分不清
// 「我们取舍了」和「我们做不到」）。
TEST(RepairTranscodeTracks, AudioRejectionSplitsByWhetherATargetWasMeasured) {
    repair::TranscodeTrackProbe no_target = Probe(1, "audio", "flac");
    no_target.container_fit = repair::TranscodeContainerFit::Rejected;
    repair::TranscodeTrackProbe with_target = no_target;
    with_target.audio_reencode_encoder = "aac";
    with_target.audio_reencode_codec = "aac";
    with_target.audio_reencode_settings = "48000 Hz / 2 声道 / fltp";

    const repair::TranscodeTrackPlan dropped = repair::PlanTranscodeTracks({Probe(0, "video", "h264"), no_target});
    const repair::TranscodeTrackPlan transcoded = repair::PlanTranscodeTracks({Probe(0, "video", "h264"), with_target});
    ASSERT_EQ(dropped.decisions.size(), 2u);
    ASSERT_EQ(transcoded.decisions.size(), 2u);
    EXPECT_EQ(dropped.decisions[1].reason_code, "audio_drop_no_fitting_encoder");
    EXPECT_EQ(transcoded.decisions[1].reason_code, "audio_reencode_container_rejected");
    EXPECT_EQ(transcoded.decisions[1].action, "transcode");
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

// ---- 切片 Z：非主视频轨按容器实测接受性决定复制还是丢弃 ----

TEST(RepairTranscodeTracks, ExtraVideoCopiedWhenContainerAccepts) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "video", "h264")};
    probes[1].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* extra = FindDecision(plan, 1);
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->action, "copy");
    EXPECT_EQ(extra->reason_code, "extra_video_copy_container_ok");
    // 「复制」不等于「修好了」：这条轨没有任何修复动作，文案必须自己说出来。
    EXPECT_NE(extra->reason.find("不重编码"), std::string::npos) << extra->reason;
    EXPECT_NE(extra->reason.find("不修复"), std::string::npos) << extra->reason;
    EXPECT_EQ(plan.transcode_count, 1u) << "第二视频轨绝不能被当成第二条重编码链";
    EXPECT_EQ(plan.drop_count, 0u);
}

TEST(RepairTranscodeTracks, ExtraVideoDroppedWhenContainerRejects) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "video", "ffv1")};
    probes[1].container_fit = repair::TranscodeContainerFit::Rejected;
    probes[1].container_fit_detail = "封装器返回 Invalid argument";
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* extra = FindDecision(plan, 1);
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->action, "drop");
    EXPECT_EQ(extra->reason_code, "extra_video_copy_container_rejected");
    // 理由必须带上封装器原话，也要点明「没有第二条链」——不是「我们不想」。
    EXPECT_NE(extra->reason.find("封装器返回 Invalid argument"), std::string::npos) << extra->reason;
    EXPECT_NE(extra->reason.find("没有第二条解码-重编码链"), std::string::npos) << extra->reason;
}

TEST(RepairTranscodeTracks, ExtraVideoCopiedWhenFitUnmeasured) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "video", "h264")};
    probes[1].container_fit = repair::TranscodeContainerFit::Unknown;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* extra = FindDecision(plan, 1);
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->action, "copy");
    EXPECT_EQ(extra->reason_code, "extra_video_copy_container_unprobed") << "没实测就不许写成「接受」";
    EXPECT_NE(extra->reason.find("未能实测"), std::string::npos) << extra->reason;
}

// ---- 切片 AA：封面图（attached_pic）按原字节携带 ----
//
// 旧行为的理由是一句因果：「封面不是视频内容」→「产物不含该轨」。前半句对，
// 后半句推不出来：封面的载荷是一张完整图片，原样搬进目标容器不引入任何损失。
// 这三条钉住「搬」与「不搬」的分界 —— 分界只能是容器实测，不是轨的类型。

TEST(RepairTranscodeTracks, CoverArtCopiedWhenContainerAccepts) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "attached_pic", "mjpeg")};
    probes[1].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* cover = FindDecision(plan, 1);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->action, "copy");
    EXPECT_EQ(cover->reason_code, "cover_art_copy_container_ok");
    // 「携带」不等于「重建图片轨」：这句话必须写在理由里，否则读者会以为我们修了图。
    EXPECT_NE(cover->reason.find("原字节"), std::string::npos) << cover->reason;
    EXPECT_NE(cover->reason.find("不重建图片轨"), std::string::npos) << cover->reason;
    EXPECT_EQ(plan.transcode_count, 1u) << "封面绝不能被当成第二条重编码链";
    EXPECT_EQ(plan.drop_count, 0u);
}

TEST(RepairTranscodeTracks, CoverArtDroppedWhenContainerRejects) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "attached_pic", "mjpeg")};
    probes[1].container_fit = repair::TranscodeContainerFit::Rejected;
    probes[1].container_fit_detail = "封装器返回 Invalid argument";
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* cover = FindDecision(plan, 1);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->action, "drop");
    EXPECT_EQ(cover->reason_code, "cover_art_copy_container_rejected");
    EXPECT_NE(cover->reason.find("封装器返回 Invalid argument"), std::string::npos)
        << "容器拒绝的理由必须带实测原话，不能只写「不支持」：" << cover->reason;
}

TEST(RepairTranscodeTracks, CoverArtCopiedWhenFitUnmeasured) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "attached_pic", "mjpeg")};
    probes[1].container_fit = repair::TranscodeContainerFit::Unknown;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* cover = FindDecision(plan, 1);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->action, "copy");
    EXPECT_EQ(cover->reason_code, "cover_art_copy_container_unprobed") << "没实测就不许写成「接受」";
    EXPECT_NE(cover->reason.find("未能实测"), std::string::npos) << cover->reason;
}

// 反向守护：主视频轨**永远**重编码，容器接不接受都不影响它的决策。//
// 把「容器接受就复制」这条规则误用到主轨上，产物会变成「一条修好的视频都没有」， 而报告里每一条理由都写得通顺 ——
// 只有这类反向断言能拦住。
TEST(RepairTranscodeTracks, PrimaryVideoNeverBecomesACopy) {
    std::vector<repair::TranscodeTrackProbe> probes = {Probe(0, "video", "h264"), Probe(1, "audio", "aac")};
    probes[0].container_fit = repair::TranscodeContainerFit::Accepted;
    probes[1].container_fit = repair::TranscodeContainerFit::Accepted;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks(probes);

    const RepairTranscodeTrackDecision* primary = FindDecision(plan, 0);
    ASSERT_NE(primary, nullptr);
    EXPECT_EQ(primary->action, "transcode") << "主视频轨被当成复制轨了：" << primary->text;
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
    // 切片 Z：容器接得下就复制保留 —— 不再是「一律未保留」。
    EXPECT_EQ(extra->action, "copy") << extra->text;
    EXPECT_EQ(extra->reason_code, "extra_video_copy_container_ok") << extra->text;
    EXPECT_EQ(fidelity.track_decisions[0].action, "transcode") << "主视频轨必须被重建";
    EXPECT_EQ(fidelity.track_decision_summary, "复制 2 轨 / 重编码 1 轨 / 未保留 0 轨");

    // 2. 产物里必须真的有两条视频轨 —— 报告说「复制保留」，产物就得有。
    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    EXPECT_EQ(output_probe.video_streams, 2);

    // 3. 「未保留」清单里不能再有它（有它就是谎报丢了东西）。
    const bool omitted_mentions_second_video =
        std::any_of(result.fidelity.omitted_items.begin(), result.fidelity.omitted_items.end(),
                    [](const std::string& item) { return item.find("第二视频轨") != std::string::npos; });
    EXPECT_FALSE(omitted_mentions_second_video) << "第二视频轨已经复制保留，却被列进未保留清单";

    // 4. 三份报告同源：复制决策必须以决策行出现，且不能被写成「未保留」。
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("轨道决策"), std::string::npos) << "报告缺少轨道决策行";
        EXPECT_NE(body.find("第二视频轨"), std::string::npos) << "报告没有交代第二视频轨的去向";
        EXPECT_NE(body.find("轨道决策: 流 #1（video"), std::string::npos)
            << "复制决策必须作为决策行打印（drop 才由「未保留」清单承载）";
        EXPECT_EQ(body.find("未保留: 流 #1（video"), std::string::npos) << "复制保留的轨被写进了未保留清单";
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
    bool json_has_copy = false;
    for (const auto& node : decisions->Items()) {
        if (node.Find("reason_code")->StringValueOr("") == "extra_video_copy_container_ok")
            json_has_copy = node.Find("action")->StringValueOr("") == "copy";
    }
    EXPECT_TRUE(json_has_copy) << "JSON 里必须能机读到这条复制决策";

    // 5. 最硬的一条：复制轨的**逐包载荷序列**必须与源里那条轨完全相同。
    //    只断言「产物有 2 条视频轨」抓不到「拷的是别的东西 / 拷了一半 / 重新编码过」；
    //    三种退化在帧数与流数上都可能照样对得上。
    const auto source_seq = videoeye_test::CaptureMediaSequence(media.path);
    ASSERT_TRUE(source_seq.opened) << source_seq.error;
    const auto output_seq = videoeye_test::CaptureMediaSequence(result.output_path);
    ASSERT_TRUE(output_seq.opened) << output_seq.error;
    const videoeye_test::StreamSequence* src_extra = nullptr;
    for (const auto& stream : source_seq.streams) {
        if (stream.codec_type == "video" && stream.index != 0)
            src_extra = &stream;
    }
    const videoeye_test::StreamSequence* out_extra = nullptr;
    for (const auto& stream : output_seq.streams) {
        if (stream.codec_type == "video" && stream.index != 0)
            out_extra = &stream;
    }
    ASSERT_NE(src_extra, nullptr) << "源里没找到第二视频轨";
    ASSERT_NE(out_extra, nullptr) << "产物里没找到第二视频轨";
    ASSERT_FALSE(src_extra->packets.empty()) << "源第二视频轨一个包都没有，判据无从谈起";
    ASSERT_EQ(out_extra->packets.size(), src_extra->packets.size()) << "复制轨包数变了：不是原码流复制";
    for (std::size_t k = 0; k < src_extra->packets.size(); ++k) {
        EXPECT_EQ(out_extra->packets[k].payload_hash, src_extra->packets[k].payload_hash)
            << "第 " << k << " 个包与源不一致：这条轨不是原样搬过去的";
    }
    // 解码帧数不能按「相等」断言：实测源里这条轨的**最后一个包带 AV_PKT_FLAG_DISCARD**
    // （flag 4，测试语料的老毛病，主视频轨一样会少解一条），avcodec_send_packet 会跳过它，
    // 于是源只解出 19 帧；复制进产物后同一个样本不再带该标记，解出 20 帧。
    // 这是容器元数据的差异，不是码流差异 ——「拷过来的比源多解出一帧」是好事，
    // 抹掉它去凑相等反而是错的。
    EXPECT_GE(out_extra->decoded_frames, src_extra->decoded_frames) << "复制轨解出的帧数比源还少：包虽然在，画面丢了";
    EXPECT_GT(out_extra->decoded_frames, 0u) << "复制轨一帧都解不出来，不能声称保留了内容";

    // 6. 账本 / 守恒 / 逐轨验证三条都要跟着这条轨跑：
    //    * 执行摘要里必须能看到「复制了多少个包」（否则它只是待在产物里、没进账）；
    //    * 守恒问题表必须为空（账本与写入包数必须严格相等）；
    //    * 逐轨验证必须真的验过这条轨 —— 不把它写进「保留轨 → 输出流」契约，
    //      R04 覆盖守护会把它当成「执行器没上报的轨」，整份结论被降级。
    const bool detail_mentions_copy =
        std::any_of(result.executions.begin(), result.executions.end(), [](const RepairActionExecution& execution) {
            return execution.detail.find("第二视频轨按码流复制") != std::string::npos;
        });
    EXPECT_TRUE(detail_mentions_copy) << "执行摘要里没有这条轨的复制包数";
    EXPECT_TRUE(result.sample_conservation_problems.empty())
        << (result.sample_conservation_problems.empty() ? "" : result.sample_conservation_problems.front());
    // 输出侧的流下标要在**产物**里实际枚举出来，不能假设「第二视频轨排第 2」——
    // 建流顺序变了这条断言就会指到别的轨上，而它照样能通过。
    const videoeye_test::MediaSequence layout = videoeye_test::ProbeStreamLayout(result.output_path);
    ASSERT_TRUE(layout.opened) << layout.error;
    int copied_out_index = -1;
    int video_streams_seen = 0;
    for (const auto& stream : layout.streams) {
        if (stream.codec_type != "video")
            continue;
        if (video_streams_seen++ == 1)
            copied_out_index = stream.index;
    }
    ASSERT_GE(copied_out_index, 0) << "产物里没有第二视频轨";
    const RepairTrackVerification* copied_verification = nullptr;
    for (const auto& track : result.verification.per_track) {
        if (track.output_stream_index == copied_out_index && track.kind == "video")
            copied_verification = &track;
    }
    ASSERT_NE(copied_verification, nullptr) << "复制轨没有进逐轨验证（R04 会判成覆盖缺口）";
    EXPECT_TRUE(copied_verification->verified) << "复制轨没被真的解码验证过：" << copied_verification->detail;
}

// 容器拒绝第二视频轨时必须**真的不写它**：只把决策改成 drop、产物里照样留着这条轨，
// 是「说了做不到」的另一种形态（决策是对的，行为没跟上）。
class VideoRejectInjection {
public:
    explicit VideoRejectInjection(int input_stream_index) {
        repair::SetTranscodeVideoContainerRejectedForTesting(input_stream_index);
    }
    ~VideoRejectInjection() {
        repair::SetTranscodeVideoContainerRejectedForTesting(-1);
    }
    VideoRejectInjection(const VideoRejectInjection&) = delete;
    VideoRejectInjection& operator=(const VideoRejectInjection&) = delete;
};

TEST(RepairTranscodeTracks, RejectedSecondVideoTrackIsNotWrittenIntoTheProduct) {
    const std::string dir = MakeTempDir("e2e_reject");
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 20;
    spec.extra_video_tracks = 1;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "two_video_reject", spec);
    ASSERT_TRUE(media.ok) << media.error;

    RepairResult result;
    {
        // 注入只改「容器接受性」这一个判定，丢轨之后的行为全是真的。
        VideoRejectInjection injection(1);
        RepairRequest request = MakeRequest(media.path, dir + "/out");
        request.mode = RepairMode::RecoverContent;
        result = repair::RepairEngine::Run(request, nullptr, nullptr);
    }
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    const RepairTranscodeTrackDecision* extra = nullptr;
    for (const auto& decision : fidelity.track_decisions) {
        if (decision.kind == "video" && decision.input_stream_index != 0)
            extra = &decision;
    }
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->action, "drop");
    EXPECT_EQ(extra->reason_code, "extra_video_copy_container_rejected") << extra->text;
    EXPECT_NE(extra->reason.find("SetTranscodeVideoContainerRejectedForTesting"), std::string::npos)
        << "注入必须留下痕迹，不能让人误以为这是实测结论：" << extra->reason;

    // 决策说 drop，产物就必须真的没有这条轨。
    const auto output_probe = videoeye_test::ProbeMediaFile(result.output_path);
    ASSERT_TRUE(output_probe.opened) << output_probe.error;
    EXPECT_EQ(output_probe.video_streams, 1) << "决策说未保留，产物里却有两条视频轨";

    const bool omitted =
        std::any_of(result.fidelity.omitted_items.begin(), result.fidelity.omitted_items.end(),
                    [](const std::string& item) { return item.find("第二视频轨") != std::string::npos; });
    EXPECT_TRUE(omitted) << "被丢的第二视频轨不在未保留清单里";

    // drop 由「未保留」清单承载，决策表不再重复打印一行。
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string body = ReadText(dir + "/r.txt");
    EXPECT_NE(body.find("未保留: 流 #1（video"), std::string::npos) << "未保留清单缺少这条轨";
    EXPECT_EQ(body.find("轨道决策: 流 #1（video"), std::string::npos) << "drop 决策被当成决策行打印，与未保留清单重复";
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

// ---- 端到端：封面图必须真的到了产物里，且不是靠重建图片轨实现的 ----
//
// 这一组用例把「携带」证到字节级：
//   1. 源里真的有封面（否则后面所有断言都成了空转的假绿）；
//   2. 产物重新打开后仍有一条 attached_pic 轨，图片字节与源**逐字节相同**；
//   3. 产物里 `trak` 的数量不多不少 —— 封面走的是 moov/meta/ilst 的 `covr` 盒，
//      不是第三条图片轨。只断言「能读出封面」是不够的：把封面写成一条真实
//      视频轨也能读出来，但播放器会把它当成一路可解画面。

struct CoverArtProbe {
    bool present = false;
    std::string codec;
    std::vector<uint8_t> bytes;
};

// 读取文件里的封面：优先用 demuxer 挂好的 attached_pic，没有再退到读包。
// FFmpeg 8 的 mov demuxer 两种都会给，这里取到一份即可。
CoverArtProbe ReadCoverArt(const std::string& path) {
    CoverArtProbe cover;
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
        return cover;
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return cover;
    }
    int cover_index = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        AVStream* stream = ctx->streams[i];
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
            continue;
        cover_index = static_cast<int>(i);
        cover.present = true;
        cover.codec = avcodec_get_name(stream->codecpar->codec_id);
        if (stream->attached_pic.data != nullptr && stream->attached_pic.size > 0) {
            cover.bytes.assign(stream->attached_pic.data, stream->attached_pic.data + stream->attached_pic.size);
        }
        break;
    }
    if (cover_index >= 0 && cover.bytes.empty()) {
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(ctx, packet) >= 0) {
            if (packet->stream_index == cover_index && packet->size > 0) {
                cover.bytes.assign(packet->data, packet->data + packet->size);
                av_packet_unref(packet);
                break;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    avformat_close_input(&ctx);
    return cover;
}

// 数文件里某个 4 字节 box 名的出现次数（`trak` / `covr`）。只用于判「封面有没有
// 被写成真实轨道」这一件事，不拿它当通用 box 解析器。
std::size_t CountFourCc(const std::string& path, const std::string& tag) {
    std::ifstream in(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t count = 0;
    for (std::size_t pos = data.find(tag); pos != std::string::npos; pos = data.find(tag, pos + 1))
        ++count;
    return count;
}

} // namespace

TEST(RepairTranscodeTracks, S08CarriesTheCoverArtWithoutRebuildingAnImageTrack) {
    const std::string dir = MakeTempDir("cover");
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 20;
    spec.with_cover_art = true;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "with_cover", spec);
    ASSERT_TRUE(media.ok) << media.error;

    const CoverArtProbe source_cover = ReadCoverArt(media.path);
    ASSERT_TRUE(source_cover.present) << "语料本身没有封面，这条用例就什么都证明不了";
    ASSERT_GT(source_cover.bytes.size(), 0u);

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    // 1. 决策行必须写「复制」，理由码必须是实测接受，且明说「不重建图片轨」。
    const RepairTranscodeFidelity& fidelity = result.fidelity.transcode;
    const RepairTranscodeTrackDecision* cover_decision = nullptr;
    for (const auto& decision : fidelity.track_decisions) {
        if (decision.kind == "attached_pic")
            cover_decision = &decision;
    }
    ASSERT_NE(cover_decision, nullptr) << "封面轨没有决策行";
    EXPECT_EQ(cover_decision->action, "copy") << cover_decision->text;
    EXPECT_EQ(cover_decision->reason_code, "cover_art_copy_container_ok") << cover_decision->text;

    // 2. 产物里必须真的能读回同一张图 —— 逐字节相同。
    const CoverArtProbe output_cover = ReadCoverArt(result.output_path);
    ASSERT_TRUE(output_cover.present) << "报告说携带了封面，产物里却读不到";
    EXPECT_EQ(output_cover.bytes.size(), source_cover.bytes.size()) << "封面字节数变了就不是「原字节」";
    EXPECT_TRUE(output_cover.bytes == source_cover.bytes) << "封面字节与源不一致";
    EXPECT_EQ(output_cover.codec, source_cover.codec);

    // 3. 承载方式：`covr` 盒在，`trak` 数量不变（2 = 视频 + 音频，没有图片轨）。
    EXPECT_EQ(CountFourCc(result.output_path, "covr"), 1u) << "封面没有落在 covr 盒里";
    EXPECT_EQ(CountFourCc(result.output_path, "trak"), 2u)
        << "产物里多了一条轨道 —— 封面被写成真实图片轨，而不是 covr 盒";

    // 4. 「未保留」清单里不能再提它（提了就是谎报丢了东西）；执行摘要要点出携带字节数。
    const bool omitted_mentions_cover =
        std::any_of(result.fidelity.omitted_items.begin(), result.fidelity.omitted_items.end(),
                    [](const std::string& item) { return item.find("attached_pic") != std::string::npos; });
    EXPECT_FALSE(omitted_mentions_cover) << "封面明明携带了，却被列进未保留";
    const bool detail_mentions_cover =
        std::any_of(result.executions.begin(), result.executions.end(), [](const RepairActionExecution& execution) {
            return execution.detail.find("封面图按原字节携带") != std::string::npos &&
                   execution.detail.find("未重建图片轨") != std::string::npos;
        });
    EXPECT_TRUE(detail_mentions_cover) << "执行摘要没交代封面搬了多少字节，也没说清没重建图片轨";

    // 报告同源：复制决策必须作为「轨道决策」行出现，不能落到「未保留」清单里。
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;
    const std::string body = ReadText(dir + "/r.txt");
    EXPECT_NE(body.find("轨道决策"), std::string::npos);
    EXPECT_NE(body.find("（attached_pic"), std::string::npos) << "决策表没有封面这一行";
    EXPECT_EQ(body.find("未保留: 流 #2（attached_pic"), std::string::npos) << "复制保留的封面被写进了未保留清单";

    // 5. 封面也要进「保留轨 → 输出流」契约，否则 R04 覆盖守护会把它当成未上报的轨
    //    （BuildTargets 枚举产物时 attached_pic 也算一条媒体轨，漏报就标 coverage_gap，
    //    整份结论被降级）。它是图片轨，required=false，因此只进记录、不参与解码验证。
    const RepairTrackVerification* cover_verification = nullptr;
    for (const auto& track : result.verification.per_track) {
        if (track.kind == "attached_pic")
            cover_verification = &track;
    }
    ASSERT_NE(cover_verification, nullptr) << "封面轨没有进逐轨验证记录";
    EXPECT_EQ(cover_verification->detail.find("未上报保留映射"), std::string::npos)
        << "封面没进保留映射契约，R04 会把它判成覆盖缺口并降级整份结论";
    EXPECT_FALSE(cover_verification->verified) << "封面是图片轨，把它写成「已解码验证」是伪造结论";
}

TEST(RepairTranscodeTracks, RejectedCoverArtIsNotWrittenIntoTheProduct) {
    const std::string dir = MakeTempDir("cover_rejected");
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 20;
    spec.with_cover_art = true;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "with_cover", spec);
    ASSERT_TRUE(media.ok) << media.error;
    const CoverArtProbe source_cover = ReadCoverArt(media.path);
    ASSERT_TRUE(source_cover.present) << "语料本身没有封面";

    // 找出封面输入流下标（注入按输入流下标生效）。
    int cover_input_index = -1;
    {
        AVFormatContext* ctx = nullptr;
        if (avformat_open_input(&ctx, media.path.c_str(), nullptr, nullptr) >= 0) {
            avformat_find_stream_info(ctx, nullptr);
            for (unsigned i = 0; i < ctx->nb_streams; ++i) {
                if ((ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
                    cover_input_index = static_cast<int>(i);
                    break;
                }
            }
            avformat_close_input(&ctx);
        }
    }
    ASSERT_GE(cover_input_index, 0);
    repair::SetTranscodeCoverArtContainerRejectedForTesting(cover_input_index);

    RepairRequest request = MakeRequest(media.path, dir + "/out");
    request.mode = RepairMode::RecoverContent;
    const RepairResult result = repair::RepairEngine::Run(request, nullptr, nullptr);
    repair::SetTranscodeCoverArtContainerRejectedForTesting(-1);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;

    // 决策必须落成 drop，并且理由里带注入痕迹 —— 这个结论不是实测的，不能装成实测。
    const RepairTranscodeTrackDecision* cover_decision = nullptr;
    for (const auto& decision : result.fidelity.transcode.track_decisions) {
        if (decision.kind == "attached_pic")
            cover_decision = &decision;
    }
    ASSERT_NE(cover_decision, nullptr);
    EXPECT_EQ(cover_decision->action, "drop") << cover_decision->text;
    EXPECT_EQ(cover_decision->reason_code, "cover_art_copy_container_rejected");
    EXPECT_NE(cover_decision->reason.find("测试注入"), std::string::npos) << cover_decision->reason;

    // 产物里不能有封面，不能有图片轨，且必须在「未保留」清单里被点名。
    const CoverArtProbe output_cover = ReadCoverArt(result.output_path);
    EXPECT_FALSE(output_cover.present) << "决策是未携带，产物里却有封面";
    EXPECT_EQ(CountFourCc(result.output_path, "trak"), 2u);
    const bool omitted_mentions_cover =
        std::any_of(result.fidelity.omitted_items.begin(), result.fidelity.omitted_items.end(),
                    [](const std::string& item) { return item.find("attached_pic") != std::string::npos; });
    EXPECT_TRUE(omitted_mentions_cover) << "封面没被携带，却没进未保留清单";
}
