#include <gtest/gtest.h>

// ============================================================
// 阶段 6 / 切片 V：音频轨「转码成目标容器能收的编码」
//
// 切片 T 把音频轨落成 copy / drop 两条路。drop 是诚实的，但「这条音轨还在、只是
// 编码不合适」被变成「这条音轨没有了」—— 用户拿到一份没声音的产物。实测可复现
// 的拒绝组合（mov + flac / mov + opus、mp4 + truehd）都在这一组用例的覆盖范围里。
//
// 这一组盯四件事：
//   1. 目标不是查表来的：编码器存在 → 参数能开 → 试写头接受，三条全过才选它；
//   2. 参数选择按编码器**自己声明的**能力走，被迫改采样率 / 声道时留下说明；
//   3. 决策层是纯函数：能转码 → transcode；不允许 → drop（写明是用户关的）；
//      一条候选都过不了 → drop 并逐条报出被拒原因；
//   4. 端到端真的产出了可解码的音频（不是只在报告里记了一笔）。
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
#include "core/repair/RepairAudioTranscode.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairTolerantTranscode.h"
#include "core/repair/RepairTranscodeTracks.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/TestMediaFactory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace {

using namespace videoeye;
using namespace videoeye::model;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_audiotc_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir, bool reencode) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = RepairVerificationLevel::V1_FrameSample;
    request.mode = RepairMode::RecoverContent;
    request.reencode_audio_to_fit_container = reencode;
    request.write_report = true;
    return request;
}

// 注入开关的 RAII：用例失败提前返回时也要复位，否则会污染同一进程里的其它用例。
class AudioRejectInjection {
public:
    explicit AudioRejectInjection(int input_stream_index) {
        repair::SetTranscodeAudioContainerRejectedForTesting(input_stream_index);
    }
    ~AudioRejectInjection() {
        repair::SetTranscodeAudioContainerRejectedForTesting(-1);
    }
    AudioRejectInjection(const AudioRejectInjection&) = delete;
    AudioRejectInjection& operator=(const AudioRejectInjection&) = delete;
};

// 源文件里第一条音频流的输入下标。不写死 1：语料工厂的流顺序是它的实现细节。
int FirstAudioStreamIndex(const std::string& path) {
    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, path.c_str(), nullptr, nullptr) < 0 || context == nullptr)
        return -1;
    int index = -1;
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        if (context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            index = static_cast<int>(i);
            break;
        }
    }
    avformat_close_input(&context);
    return index;
}

AVCodecParameters* MakeAudioParameters(AVCodecID codec_id, int sample_rate, int channels) {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = codec_id;
    parameters->sample_rate = sample_rate;
    av_channel_layout_default(&parameters->ch_layout, channels);
    parameters->frame_size = 1024;
    parameters->bit_rate = 128000;
    return parameters;
}

bool EncoderPresent(const char* name) {
    return avcodec_find_encoder_by_name(name) != nullptr;
}

// ---- 候选顺序与参数选择（纯函数）----

TEST(RepairAudioTranscode, CandidatesPreferAacAndSkipMissing) {
    // 「本构建有什么」由注入的可用性函数决定 —— 候选表不能假设某个编码器一定存在。
    auto only_aac_ac3 = [](const char* name) { return std::string(name) == "aac" || std::string(name) == "ac3"; };
    const auto candidates = repair::AudioReencodeCandidates("mp4", only_aac_ac3);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].encoder_name, "aac") << "aac 是唯一被全部目标容器接受的编码，必须排第一";
    EXPECT_EQ(candidates[1].encoder_name, "ac3");

    auto nothing = [](const char*) { return false; };
    EXPECT_TRUE(repair::AudioReencodeCandidates("matroska", nothing).empty());
}

TEST(RepairAudioTranscode, SettingsKeepSourceRateAndChannelsWhenSupported) {
    const AVCodec* encoder = avcodec_find_encoder_by_name("aac");
    ASSERT_NE(encoder, nullptr) << "本构建没有原生 aac 编码器，后面的用例都失去前提";
    AVCodecParameters* source = MakeAudioParameters(AV_CODEC_ID_AAC, 44100, 2);
    repair::AudioEncodeSettings settings;
    std::string error;
    ASSERT_TRUE(repair::ChooseAudioEncodeSettings(encoder, source, &settings, &error)) << error;
    EXPECT_EQ(settings.sample_rate, 44100) << "编码器支持源采样率就必须原样保留（不重采样 = 不动音高）";
    EXPECT_EQ(settings.channels, 2);
    EXPECT_FALSE(settings.sample_rate_changed);
    EXPECT_FALSE(settings.channel_layout_changed);
    EXPECT_TRUE(settings.note.empty()) << "没有改动就不该有说明：" << settings.note;
    EXPECT_GE(settings.sample_format, 0);
    avcodec_parameters_free(&source);
}

// 编码器不接受源采样率时必须改，而且必须留下说明 —— 「悄悄重采样」等于悄悄改了音高。
TEST(RepairAudioTranscode, SettingsResampleAndSaySo) {
    if (!EncoderPresent("libopus")) {
        GTEST_SKIP() << "本构建没有 libopus，无法验证「不支持 44100」这条路径";
    }
    const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
    AVCodecParameters* source = MakeAudioParameters(AV_CODEC_ID_AAC, 44100, 2);
    repair::AudioEncodeSettings settings;
    std::string error;
    ASSERT_TRUE(repair::ChooseAudioEncodeSettings(encoder, source, &settings, &error)) << error;
    EXPECT_EQ(settings.sample_rate, 48000) << "libopus 只接受 48 kHz，最近可用的就是 48000";
    EXPECT_TRUE(settings.sample_rate_changed);
    EXPECT_NE(settings.note.find("44100"), std::string::npos) << "必须写清从多少改到多少：" << settings.note;
    EXPECT_NE(repair::AudioEncodeSettingsText(settings).find("48000"), std::string::npos);
    avcodec_parameters_free(&source);
}

TEST(RepairAudioTranscode, SettingsDownmixWhenEncoderRejectsChannels) {
    if (!EncoderPresent("libmp3lame")) {
        GTEST_SKIP() << "本构建没有 libmp3lame，无法验证「不支持 6 声道」这条路径";
    }
    const AVCodec* encoder = avcodec_find_encoder_by_name("libmp3lame");
    AVCodecParameters* source = MakeAudioParameters(AV_CODEC_ID_AC3, 48000, 6);
    repair::AudioEncodeSettings settings;
    std::string error;
    ASSERT_TRUE(repair::ChooseAudioEncodeSettings(encoder, source, &settings, &error)) << error;
    EXPECT_EQ(settings.channels, 2) << "5.1 不被支持时应先保立体声，而不是退成单声道";
    EXPECT_TRUE(settings.channel_layout_changed);
    EXPECT_NE(settings.note.find("声道"), std::string::npos) << settings.note;
    avcodec_parameters_free(&source);
}

TEST(RepairAudioTranscode, AttemptsTextListsEveryCandidateWithReason) {
    std::vector<repair::AudioReencodeAttempt> attempts(3);
    attempts[0].encoder_name = "aac";
    attempts[0].settings_rejected = true;
    attempts[0].detail = "编码器打不开";
    attempts[1].encoder_name = "ac3";
    attempts[1].container_rejected = true;
    attempts[1].detail = "封装器返回 Invalid argument";
    attempts[2].encoder_name = "flac";
    attempts[2].encoder_missing = true;
    const std::string text = repair::AudioReencodeAttemptsText(attempts);
    EXPECT_NE(text.find("aac: 参数开不起来（编码器打不开）"), std::string::npos) << text;
    EXPECT_NE(text.find("ac3: 容器拒绝（封装器返回 Invalid argument）"), std::string::npos) << text;
    EXPECT_NE(text.find("flac: 本构建没有该编码器"), std::string::npos) << text;
}

// ---- 目标实测：编码器真的能开 + 容器真的接受 ----

TEST(RepairAudioTranscode, SelectsAacForMovOpusSource) {
    // mov 拒绝 opus（实测），aac 是第一个能过的候选。
    AVCodecParameters* source = MakeAudioParameters(AV_CODEC_ID_OPUS, 48000, 2);
    repair::AudioReencodeTarget target = repair::SelectAudioReencodeTarget("mov", source);
    avcodec_parameters_free(&source);

    ASSERT_TRUE(target.found) << "逐候选尝试记录：" << repair::AudioReencodeAttemptsText(target.attempts);
    EXPECT_EQ(target.encoder_name, "aac");
    EXPECT_EQ(target.codec_name, "aac");
    EXPECT_NE(target.encoder, nullptr);
    EXPECT_EQ(target.settings.sample_rate, 48000);
    avcodec_free_context(&target.encoder);
}

// 一个候选都过不了时必须返回 found=false，且每个候选都有自己的失败原因 ——
// 「没有可用编码器」不能是空口断言。
TEST(RepairAudioTranscode, NoCandidateFitsReportsEveryAttempt) {
    AVCodecParameters* source = MakeAudioParameters(AV_CODEC_ID_AAC, 44100, 2);
    repair::AudioReencodeTarget target = repair::SelectAudioReencodeTarget("videoeye_no_such_muxer", source);
    avcodec_parameters_free(&source);

    EXPECT_FALSE(target.found);
    EXPECT_EQ(target.encoder, nullptr);
    ASSERT_FALSE(target.attempts.empty());
    const auto candidates = repair::AudioReencodeCandidates("videoeye_no_such_muxer", {});
    EXPECT_EQ(target.attempts.size(), candidates.size()) << "每个候选都要留下记录";
    for (const auto& attempt : target.attempts)
        EXPECT_FALSE(attempt.detail.empty()) << attempt.encoder_name << " 没写原因";
}

// ---- 决策表（纯函数）----

repair::TranscodeTrackProbe AudioProbe(repair::TranscodeContainerFit fit) {
    repair::TranscodeTrackProbe probe;
    probe.input_stream_index = 1;
    probe.kind = "audio";
    probe.source_codec = "opus";
    probe.container_fit = fit;
    probe.container_fit_detail = "封装器返回 Invalid argument";
    return probe;
}

TEST(RepairAudioTranscode, RejectedAudioBecomesTranscodeWhenTargetExists) {
    repair::TranscodeTrackProbe probe = AudioProbe(repair::TranscodeContainerFit::Rejected);
    probe.audio_reencode_encoder = "aac";
    probe.audio_reencode_codec = "aac";
    probe.audio_reencode_settings = "48000 Hz / 2 声道 / fltp";
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks({probe});
    ASSERT_EQ(plan.decisions.size(), 1u);
    EXPECT_EQ(plan.decisions[0].action, "transcode");
    EXPECT_EQ(plan.decisions[0].reason_code, "audio_reencode_container_rejected");
    EXPECT_EQ(plan.decisions[0].target_codec, "aac") << "目标编码要能机读，不能让消费方去猜文本";
    EXPECT_NE(plan.decisions[0].text.find("重编码为 aac"), std::string::npos) << plan.decisions[0].text;
    EXPECT_EQ(plan.transcode_count, 1u);
    EXPECT_EQ(plan.drop_count, 0u);
}

TEST(RepairAudioTranscode, UserDisabledReencodeDropsWithExplicitReason) {
    repair::TranscodeTrackProbe probe = AudioProbe(repair::TranscodeContainerFit::Rejected);
    probe.audio_reencode_allowed = false;
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks({probe});
    ASSERT_EQ(plan.decisions.size(), 1u);
    EXPECT_EQ(plan.decisions[0].action, "drop");
    // 明确区分「用户关掉了」和「机器上没有可用编码器」—— 两种原因的处理方式完全不同。
    EXPECT_EQ(plan.decisions[0].reason_code, "audio_drop_reencode_disabled");
    EXPECT_EQ(plan.decisions[0].target_codec, "");
}

TEST(RepairAudioTranscode, NoFittingEncoderDropsAndNamesTheAttempts) {
    repair::TranscodeTrackProbe probe = AudioProbe(repair::TranscodeContainerFit::Rejected);
    probe.audio_reencode_note = "aac: 容器拒绝（Invalid argument）；ac3: 本构建没有该编码器";
    const repair::TranscodeTrackPlan plan = repair::PlanTranscodeTracks({probe});
    ASSERT_EQ(plan.decisions.size(), 1u);
    EXPECT_EQ(plan.decisions[0].action, "drop");
    EXPECT_EQ(plan.decisions[0].reason_code, "audio_drop_no_fitting_encoder");
    EXPECT_NE(plan.decisions[0].reason.find("ac3: 本构建没有该编码器"), std::string::npos)
        << "逐候选的机会都要报出来：" << plan.decisions[0].reason;
}

TEST(RepairAudioTranscode, AcceptedAudioStillCopies) {
    const repair::TranscodeTrackPlan plan =
        repair::PlanTranscodeTracks({AudioProbe(repair::TranscodeContainerFit::Accepted)});
    ASSERT_EQ(plan.decisions.size(), 1u);
    EXPECT_EQ(plan.decisions[0].action, "copy") << "容器接受原编码时不许无谓重编码 —— 重编码是有损的";
    EXPECT_EQ(plan.decisions[0].target_codec, "");
}

// ---- 端到端：真实 S08 链 ----

struct AudioRun {
    RepairResult result;
    videoeye_test::MediaProbe source;
    videoeye_test::MediaProbe output;
};

AudioRun RunWithRejectedAudio(const std::string& tag, bool reencode) {
    const std::string dir = MakeTempDir(tag);
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "audio");
    AudioRun run;
    if (!media.ok) {
        ADD_FAILURE() << "语料生成失败: " << media.error;
        return run;
    }
    const int audio_index = FirstAudioStreamIndex(media.path);
    if (audio_index < 0) {
        ADD_FAILURE() << "语料里没有音频流，这组用例失去前提";
        return run;
    }
    run.source = videoeye_test::ProbeMediaFile(media.path);
    AudioRejectInjection injection(audio_index);
    run.result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", reencode), nullptr, nullptr);
    if (run.result.output_published)
        run.output = videoeye_test::ProbeMediaFile(run.result.output_path);
    return run;
}

TEST(RepairAudioTranscode, RejectedAudioIsReencodedNotDropped) {
    const AudioRun run = RunWithRejectedAudio("on", true);
    ASSERT_TRUE(run.result.output_published) << run.result.summary << " | " << run.result.error;
    ASSERT_TRUE(run.source.opened) << run.source.error;
    ASSERT_TRUE(run.output.opened) << run.output.error;

    // 产物里音轨还在，而且真的能解码出包 —— 不是「写了一条款式上的空流」。
    EXPECT_EQ(run.output.audio_streams, 1) << "重编码的目的就是别把这条音轨丢掉";
    EXPECT_GT(run.output.audio_packet_count, 0) << "音轨存在但一个包都没有 = 实际仍然丢了";
    EXPECT_EQ(run.output.audio_codec, "aac") << "目标编码必须是实测选出来的那个";
    // 时长必须与源基本一致：解码 → 重采样 → 编码这条链只要有一层没冲干净
    // （解码器 / swr / fifo），产物音频就会短一截，而「能播」是看不出来的。
    // 容差 5 ms：实测正确路径的差值是 0.0000 s，只要少冲一层缓冲（末帧 921 采样
    // ≈ 21 ms）就会露出。用 0.12 s 这种"看起来安全"的容差等于没测。
    EXPECT_NEAR(run.output.audio_duration_seconds, run.source.audio_duration_seconds, 0.005)
        << "源音频 " << run.source.audio_duration_seconds << " s，产物 " << run.output.audio_duration_seconds
        << " s（少一段就是某层缓冲没排空）";
    EXPECT_LT(run.output.audio_start_seconds, 0.1) << "重编码后音频起点被推后了 —— 时间轴没对齐";

    const RepairTranscodeFidelity& fidelity = run.result.fidelity.transcode;
    ASSERT_EQ(fidelity.track_decisions.size(), 2u);
    const auto audio = std::find_if(fidelity.track_decisions.begin(), fidelity.track_decisions.end(),
                                    [](const RepairTranscodeTrackDecision& d) { return d.kind == "audio"; });
    ASSERT_NE(audio, fidelity.track_decisions.end());
    EXPECT_EQ(audio->action, "transcode") << audio->reason;
    EXPECT_EQ(audio->reason_code, "audio_reencode_container_rejected");
    EXPECT_EQ(audio->target_codec, "aac");
    EXPECT_TRUE(run.result.fidelity.audio_reencoded) << "重编码过的产物不能在保真度里写成「音频原样复制」";

    // 结论层必须逐项写清「做了什么」，不能只说「音轨已处理」。
    const std::string& note = fidelity.track_decision_note;
    EXPECT_NE(note.find("实测"), std::string::npos) << note;
}

// 反向守护：用户关掉重编码时，该轨必须被明确剔除，并且原因写的是「用户关的」，
// 不是「没有可用编码器」—— 这两句话对用户意味着完全不同的下一步动作。
TEST(RepairAudioTranscode, DisabledReencodeDropsTheTrackAndSaysWhy) {
    const AudioRun run = RunWithRejectedAudio("off", false);
    ASSERT_TRUE(run.result.output_published) << run.result.summary << " | " << run.result.error;
    ASSERT_TRUE(run.output.opened) << run.output.error;
    EXPECT_EQ(run.output.audio_streams, 0) << "未允许重编码时该轨只能被剔除";

    const RepairTranscodeFidelity& fidelity = run.result.fidelity.transcode;
    ASSERT_EQ(fidelity.track_decisions.size(), 2u);
    const auto audio = std::find_if(fidelity.track_decisions.begin(), fidelity.track_decisions.end(),
                                    [](const RepairTranscodeTrackDecision& d) { return d.kind == "audio"; });
    ASSERT_NE(audio, fidelity.track_decisions.end());
    EXPECT_EQ(audio->action, "drop");
    EXPECT_EQ(audio->reason_code, "audio_drop_reencode_disabled");
    EXPECT_FALSE(run.result.fidelity.audio_reencoded);
}

// 三份报告同源：JSON 里能机读到「转成了什么」，文本里能人读到。
// 报告由表现层导出（引擎不越层写报告），所以这里显式调一次 WriteAll。
TEST(RepairAudioTranscode, DecisionIsVisibleInJsonAndTextReports) {
    const std::string dir = MakeTempDir("report");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "audio");
    ASSERT_TRUE(media.ok) << media.error;
    const int audio_index = FirstAudioStreamIndex(media.path);
    ASSERT_GE(audio_index, 0);

    RepairResult result;
    {
        AudioRejectInjection injection(audio_index);
        result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", true), nullptr, nullptr);
    }
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    const std::string json_path = dir + "/r.json";
    const std::string text_path = dir + "/r.txt";
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(result, json_path, text_path, dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;

    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(json_path), root, &error)) << error;
    const JsonValue* transcode = root.Find("fidelity") != nullptr ? root.Find("fidelity")->Find("transcode") : nullptr;
    ASSERT_NE(transcode, nullptr);
    const JsonValue* decisions = transcode->Find("track_decisions");
    ASSERT_NE(decisions, nullptr);
    bool found_transcode = false;
    for (const auto& node : decisions->Items()) {
        if (node.Find("reason_code")->StringValueOr("") != "audio_reencode_container_rejected")
            continue;
        found_transcode = true;
        EXPECT_EQ(node.Find("action")->StringValueOr(""), "transcode");
        EXPECT_EQ(node.Find("target_codec")->StringValueOr(""), "aac");
    }
    EXPECT_TRUE(found_transcode) << "JSON 里必须能机读到这条重编码决策";

    const std::string text = ReadText(text_path);
    EXPECT_NE(text.find("轨道决策"), std::string::npos);
    EXPECT_NE(text.find("重编码为 aac"), std::string::npos) << "文本报告必须写清转成了什么";
}

} // namespace
