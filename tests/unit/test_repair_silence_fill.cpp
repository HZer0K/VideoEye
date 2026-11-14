#include <gtest/gtest.h>

// ============================================================
// 阶段 6 / 切片 W：音频轨时间轴空洞的探测与合成静音填补
//
// 旧行为（实测确认）：S08 的音频链按源 PTS 逐帧定位，源侧中间缺一段时把位置
// 直接跳过去（`next_pts_ = want`）。产物里于是留了一段没有采样覆盖的时间，
// 而报告一个字都不提 —— 播放器是跳过、补静音还是卡住，取决于它的实现。
//
// 这一组用例盯住五件事：
//   1. 探测与填补分开：没启用 / 该轨是码流复制，也必须如实报出「源里有 N 处空洞」；
//   2. 判据可自证：缺失量不到半个编码块不算洞（容器时基取整会抖 ±1 采样）；
//   3. 合成内容不进账本、不进恢复率分子 —— 补静音不能让恢复比例变好看；
//   4. 码流复制轨「做不到」和「用户关掉了」必须能区分：前者改开关也没用；
//   5. 三份报告同源：TXT / HTML / JSON 都能看出补在哪、哪几处没补、为什么。
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/domain/model/RepairRequest.h"
#include "core/repair/RepairEngine.h"
#include "core/repair/RepairSilenceFill.h"
#include "core/repair/RepairTolerantTranscode.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/TestMediaFactory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {

using namespace videoeye;
using namespace videoeye::model;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_silencefill_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 44.1 kHz + AAC：一个编码块 1024 采样。
constexpr int kSampleRate = 44100;
constexpr int64_t kBlock = 1024;

repair::SilenceFillPolicy MakePolicy(bool enabled) {
    repair::SilenceFillPolicy policy;
    policy.sample_rate = kSampleRate;
    policy.block_samples = kBlock;
    policy.fill_enabled = enabled;
    return policy;
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir, bool fill) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = RepairVerificationLevel::V1_FrameSample;
    request.mode = RepairMode::RecoverContent;
    request.fill_audio_gaps_with_silence = fill;
    request.write_report = true;
    return request;
}

// 注入「容器拒绝原编码」：本构建造不出「源容器能装、目标容器不收」的真实语料
// （mp4 写不进它自己拒绝的编码），所以要让音频轨走重编码只能靠这条注入。
// 它只改接受性这一个判定，转码目标仍是真实实测的。
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

// ---- 判据（纯函数）----

// 容器时基取整会让同一帧的 PTS 抖 ±1 采样（TS 的 1/90000 落在 44100 上就是如此）。
// 半个编码块以内一律不算空洞 —— 补它等于往产物里灌噪声。
TEST(RepairSilenceFill, JitterWithinToleranceIsNotAGap) {
    for (const int64_t deficit : {int64_t{1}, int64_t{2}, int64_t{512}}) {
        const repair::SilenceFillPlan plan = repair::PlanSilenceFill(0, deficit, MakePolicy(true), 0);
        EXPECT_EQ(plan.verdict, repair::SilenceFillVerdict::NoGap) << "缺失 " << deficit << " 采样不该算空洞";
        EXPECT_EQ(plan.gap.inserted_samples, 0);
    }
}

TEST(RepairSilenceFill, OneMissingBlockIsFilledWhenEnabled) {
    const repair::SilenceFillPlan plan = repair::PlanSilenceFill(4096, 4096 + kBlock, MakePolicy(true), 0);
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::Fill);
    EXPECT_EQ(plan.gap.missing_samples, kBlock);
    EXPECT_EQ(plan.gap.inserted_samples, kBlock);
    EXPECT_EQ(plan.gap.action, "filled");
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_inserted");
    EXPECT_NEAR(plan.gap.source_position_seconds, 4096.0 / kSampleRate, 1e-9);
    EXPECT_NEAR(plan.gap.gap_seconds, static_cast<double>(kBlock) / kSampleRate, 1e-9);
}

// 反向守护：没启用时**一个采样都不许补**，但仍要说清「这里有个洞」。
TEST(RepairSilenceFill, DisabledFillStillRecordsTheGap) {
    const repair::SilenceFillPlan plan = repair::PlanSilenceFill(0, 12 * kBlock, MakePolicy(false), 0);
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::NotRequested);
    EXPECT_EQ(plan.gap.inserted_samples, 0);
    EXPECT_EQ(plan.gap.missing_samples, 12 * kBlock) << "缺失采样数仍然要算出来，否则读者无法判断洞有多大";
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_not_requested");
    EXPECT_EQ(plan.gap.action, "not_filled");
}

// 码流复制轨没有编码器：这不是「用户没开」，改开关也没用。两种原因必须能区分。
TEST(RepairSilenceFill, CopyTrackCannotSynthesizeEvenWhenEnabled) {
    repair::SilenceFillPolicy policy = MakePolicy(true);
    policy.reencode_available = false;
    const repair::SilenceFillPlan plan = repair::PlanSilenceFill(0, 12 * kBlock, policy, 0);
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::CopyTrackCannotSynthesize);
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_track_copied");
    EXPECT_EQ(plan.gap.inserted_samples, 0);
}

// gap_policy = MergeContiguous 是用户明确要求「塌缩空洞」，补静音会直接违背它。
TEST(RepairSilenceFill, MergePolicyWinsOverFillRequest) {
    repair::SilenceFillPolicy policy = MakePolicy(true);
    policy.merge_contiguous = true;
    const repair::SilenceFillPlan plan = repair::PlanSilenceFill(0, 12 * kBlock, policy, 0);
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::MergeContiguous);
    EXPECT_EQ(plan.gap.inserted_samples, 0);
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_merge_contiguous");
}

// 大段缺失不用静音掩盖：超过单处上限就不补，也不许「只补前 5 秒」。
TEST(RepairSilenceFill, LargeGapIsRefusedNotPartiallyFilled) {
    const repair::SilenceFillPlan plan = repair::PlanSilenceFill(0, 6 * kSampleRate, MakePolicy(true), 0);
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::GapTooLarge);
    EXPECT_EQ(plan.gap.inserted_samples, 0) << "拒绝就是一点都不补";
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_gap_too_large");
}

TEST(RepairSilenceFill, TotalBudgetStopsInsertion) {
    repair::SilenceFillPolicy policy = MakePolicy(true);
    policy.max_total_seconds = 1.0;
    const repair::SilenceFillPlan plan =
        repair::PlanSilenceFill(0, kSampleRate, policy, static_cast<int64_t>(0.8 * kSampleRate));
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::BudgetExhausted);
    EXPECT_EQ(plan.gap.inserted_samples, 0);
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_budget_exhausted");
}

// 时间戳回退不归这里管（有专门的时间轴修复链），绝不能当成「洞」补静音。
TEST(RepairSilenceFill, BackwardsTimestampsAreNotGaps) {
    for (const int64_t actual : {int64_t{0}, int64_t{2048}}) {
        const repair::SilenceFillPlan plan = repair::PlanSilenceFill(8192, actual, MakePolicy(true), 0);
        EXPECT_EQ(plan.verdict, repair::SilenceFillVerdict::NoGap) << "actual=" << actual;
        EXPECT_EQ(plan.gap.inserted_samples, 0);
    }
}

// 采样率不可用时连「补多少秒」都算不出来：不许猜（码流复制轨没有解码器时可能遇到）。
TEST(RepairSilenceFill, UnknownSampleRateCannotBeFilled) {
    repair::SilenceFillPolicy policy = MakePolicy(true);
    policy.sample_rate = 0;
    const repair::SilenceFillPlan plan = repair::PlanSilenceFill(0, 12 * kBlock, policy, 0);
    ASSERT_EQ(plan.verdict, repair::SilenceFillVerdict::NoSampleRate);
    EXPECT_EQ(plan.gap.reason_code, "audio_silence_no_sample_rate");
    EXPECT_EQ(plan.gap.inserted_samples, 0);
}

// 反向守护：所有「不补」的判定必须带自己的理由码，且码不重复 ——
// 否则报告里会出现「未填补（audio_silence_unknown）」这种等于没说的话。
TEST(RepairSilenceFill, EveryRefusalHasItsOwnReasonCode) {
    const std::vector<repair::SilenceFillVerdict> refusals = {repair::SilenceFillVerdict::NotRequested,
                                                              repair::SilenceFillVerdict::MergeContiguous,
                                                              repair::SilenceFillVerdict::CopyTrackCannotSynthesize,
                                                              repair::SilenceFillVerdict::GapTooLarge,
                                                              repair::SilenceFillVerdict::BudgetExhausted,
                                                              repair::SilenceFillVerdict::NoSampleRate,
                                                              repair::SilenceFillVerdict::EncoderRejected};
    std::vector<std::string> codes;
    for (const auto verdict : refusals) {
        const std::string code = repair::SilenceFillReasonCode(verdict, true);
        EXPECT_FALSE(code.empty());
        EXPECT_EQ(code.find("unknown"), std::string::npos) << "理由码不许退化成 unknown";
        codes.push_back(code);
    }
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(std::adjacent_find(codes.begin(), codes.end()), codes.end()) << "理由码重复了";
}

// 反向守护：没补的条目里不许出现「已补入」—— 那是把动作词写成了事实。
TEST(RepairSilenceFill, RefusalTextNeverClaimsInsertion) {
    const std::vector<std::string> refusal_codes = {"audio_silence_not_requested",    "audio_silence_merge_contiguous",
                                                    "audio_silence_track_copied",     "audio_silence_gap_too_large",
                                                    "audio_silence_budget_exhausted", "audio_silence_no_sample_rate"};
    for (const auto& code : refusal_codes) {
        RepairSyntheticAudioGap gap;
        gap.source_position_seconds = 1.0;
        gap.gap_seconds = 0.28;
        gap.missing_samples = 12288;
        gap.after_audio_frames = 25;
        gap.action = "not_filled";
        gap.reason_code = code;
        const std::string text = repair::SilenceFillGapText(gap);
        EXPECT_EQ(text.find("已补入"), std::string::npos) << code << " 的说明里出现了「已补入」";
        EXPECT_NE(text.find("未填补"), std::string::npos) << code << " 没说清为什么没补：" << text;
    }
}

// 没跑这条轨 ≠ 没有空洞。空表不许被读成「没问题」。
TEST(RepairSilenceFill, UninspectedSummaryDoesNotClaimZeroGaps) {
    RepairSyntheticAudioFill fill;
    fill.input_stream_index = 1;
    const std::string summary = repair::SilenceFillSummary(fill);
    EXPECT_NE(summary.find("未做音频空洞探测"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("探测到 0 处"), std::string::npos) << "未探测不许渲染成「0 处空洞」";
    const std::string note = repair::SilenceFillBasisNote(fill);
    EXPECT_NE(note.find("不等于"), std::string::npos) << "必须写明「没探测」不等于「没有空洞」：" << note;
}

TEST(RepairSilenceFill, SummaryCountsFilledAndRefused) {
    RepairSyntheticAudioFill fill;
    fill.inspected = true;
    fill.requested = true;
    fill.input_stream_index = 1;
    fill.coding_mode = "transcode";
    fill.gaps_detected = 3;
    fill.samples_inserted = 4096;
    fill.seconds_inserted = 4096.0 / kSampleRate;
    fill.gaps_refused = 2;
    const std::string summary = repair::SilenceFillSummary(fill);
    EXPECT_NE(summary.find("音频流 #1"), std::string::npos) << summary;
    EXPECT_NE(summary.find("3 处空洞"), std::string::npos) << summary;
    EXPECT_NE(summary.find("4096 采样"), std::string::npos) << summary;
    EXPECT_NE(summary.find("2 处未填补"), std::string::npos) << summary;
}

// 码流复制轨判空洞靠包时长；容器不给时长时探测不完整，这句话不能吞掉。
TEST(RepairSilenceFill, PacketsWithoutDurationAreReported) {
    RepairSyntheticAudioFill fill;
    fill.inspected = true;
    fill.coding_mode = "copy";
    fill.packets_without_duration = 5;
    const std::string note = repair::SilenceFillBasisNote(fill);
    EXPECT_NE(note.find("5 个包没有可用时长"), std::string::npos) << note;
    EXPECT_NE(note.find("不完整"), std::string::npos) << note;
    EXPECT_NE(note.find("码流复制"), std::string::npos) << "复制轨一定要说清「补不了」的原因：" << note;
}

// ---- 端到端：真实 S08 转码链 ----

struct SilenceRun {
    RepairResult result;
    videoeye_test::MediaProbe source;
    videoeye_test::MediaProbe output;
};

// reencode_audio=false 时音频走码流复制（不需要注入）；true 时注入容器拒绝，
// 让这条轨真的走重编码 —— 只有重编码轨手里才有编码器。
SilenceRun RunWithAudioGap(const std::string& tag, bool fill, bool reencode_audio) {
    const std::string dir = MakeTempDir(tag);
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 40;
    spec.fps = 25;
    spec.sample_rate = kSampleRate;
    // 第 20 帧之后丢掉 12 个音频编码帧（12 × 1024 = 12288 采样 ≈ 0.279 s）。
    // 12 个整块是刻意的：样本账本的「只减不增」断言要求合成量与编码块对齐。
    spec.audio_gap_after_frame = 20;
    spec.audio_gap_frames = 12;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "agap", spec);
    SilenceRun run;
    if (!media.ok) {
        ADD_FAILURE() << "语料生成失败: " << media.error;
        return run;
    }
    run.source = videoeye_test::ProbeMediaFile(media.path);
    int audio_index = -1;
    std::unique_ptr<AudioRejectInjection> injection;
    if (reencode_audio) {
        audio_index = FirstAudioStreamIndex(media.path);
        if (audio_index < 0) {
            ADD_FAILURE() << "语料里找不到音频流";
            return run;
        }
        injection = std::make_unique<AudioRejectInjection>(audio_index);
    }
    run.result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", fill), nullptr, nullptr);
    if (run.result.output_published) {
        run.output = videoeye_test::ProbeMediaFile(run.result.output_path);
    }
    return run;
}

const RepairSyntheticAudioFill* FindAudioFill(const RepairResult& result, const std::string& coding_mode) {
    for (const auto& fill : result.fidelity.synthetic_audio) {
        if (fill.coding_mode == coding_mode)
            return &fill;
    }
    return nullptr;
}

// 探测是证据：用户没启用补静音，也必须在报告里看到「源里有 1 处空洞」。
TEST(RepairSilenceFill, GapIsDetectedAndReportedEvenWhenFillIsOff) {
    const SilenceRun run = RunWithAudioGap("off", false, true);
    ASSERT_TRUE(run.result.output_published) << run.result.summary << " | " << run.result.error;
    const RepairSyntheticAudioFill* fill = FindAudioFill(run.result, "transcode");
    ASSERT_NE(fill, nullptr) << "重编码轨必须有一条逐轨账（否则报告里「没条目」会被读成「没有空洞」）";
    EXPECT_TRUE(fill->inspected);
    EXPECT_FALSE(fill->requested);
    ASSERT_EQ(fill->gaps_detected, 1) << "源里那 1 处音频空洞没被发现（summary=" << fill->summary << "）";
    ASSERT_EQ(fill->gaps.size(), 1u);
    EXPECT_EQ(fill->gaps.front().reason_code, "audio_silence_not_requested") << fill->gaps.front().text;
    EXPECT_EQ(fill->gaps.front().missing_samples, 12 * kBlock)
        << "缺失采样数应等于丢掉的 12 个编码块（实际 " << fill->gaps.front().missing_samples << "）";
    EXPECT_EQ(fill->gaps.front().inserted_samples, 0);
    EXPECT_EQ(fill->samples_inserted, 0);

    // 报告必须说明「探到了但没补」，不能让读者以为没有空洞。
    const bool limitation_mentions_gap =
        std::any_of(run.result.fidelity.limitations.begin(), run.result.fidelity.limitations.end(),
                    [](const std::string& item) { return item.find("处时间轴空洞") != std::string::npos; });
    EXPECT_TRUE(limitation_mentions_gap) << "音频空洞探到却没补，限制项里必须写出来";
}

// 启用后：空洞被静音填满，但**样本账本只减不增**（含合成采样的编码块整块排除）。
TEST(RepairSilenceFill, FillInsertsSilenceWithoutInflatingTheLedger) {
    const SilenceRun off = RunWithAudioGap("off2", false, true);
    const SilenceRun on = RunWithAudioGap("on", true, true);
    ASSERT_TRUE(off.result.output_published) << off.result.error;
    ASSERT_TRUE(on.result.output_published) << on.result.error;

    const RepairSyntheticAudioFill* fill = FindAudioFill(on.result, "transcode");
    ASSERT_NE(fill, nullptr);
    ASSERT_EQ(fill->gaps.size(), 1u) << fill->summary;
    EXPECT_EQ(fill->gaps.front().reason_code, "audio_silence_inserted") << fill->gaps.front().text;
    EXPECT_EQ(fill->gaps.front().missing_samples, 12 * kBlock);
    EXPECT_EQ(fill->gaps.front().inserted_samples, 12 * kBlock);
    EXPECT_EQ(fill->samples_inserted, 12 * kBlock);
    EXPECT_GT(fill->seconds_inserted, 0.27);

    // 合成内容不进账本：补过静音的那次样本数必须**不多于**没补那次。
    EXPECT_LE(on.result.sample_ledger.total_samples, off.result.sample_ledger.total_samples)
        << "合成静音被算进了样本账本（off=" << off.result.sample_ledger.total_samples
        << " on=" << on.result.sample_ledger.total_samples << "）";

    // 恢复比例不许因为补静音变好看。
    const auto ratio_off = off.result.fidelity.original_content_recovery_ratio;
    const auto ratio_on = on.result.fidelity.original_content_recovery_ratio;
    if (ratio_off.has_value() && ratio_on.has_value()) {
        EXPECT_LE(*ratio_on, *ratio_off + 0.001)
            << "补静音把恢复比例抬高了（off=" << *ratio_off << " on=" << *ratio_on << "）";
    }

    // 产物必须真的含合成静音（不是只在报告里记了一笔）：音频包数应比没补时多。
    ASSERT_TRUE(off.output.opened) << off.output.error;
    ASSERT_TRUE(on.output.opened) << on.output.error;
    EXPECT_GT(on.output.audio_packet_count, off.output.audio_packet_count)
        << "补了 12 个编码块的静音，产物音频包数应当变多";
}

// 码流复制轨：探测得到，但补不了 —— 而且理由必须是「做不到」而不是「你没开」。
TEST(RepairSilenceFill, CopiedTrackReportsGapButCannotSynthesize) {
    const SilenceRun run = RunWithAudioGap("copy", true, false);
    ASSERT_TRUE(run.result.output_published) << run.result.summary << " | " << run.result.error;
    const RepairSyntheticAudioFill* fill = FindAudioFill(run.result, "copy");
    ASSERT_NE(fill, nullptr) << "复制轨也必须有一条逐轨账";
    EXPECT_TRUE(fill->inspected);
    ASSERT_EQ(fill->gaps_detected, 1) << fill->summary << " | no_dur=" << fill->packets_without_duration
                                      << " | basis=" << fill->basis;
    EXPECT_EQ(fill->gaps.front().reason_code, "audio_silence_track_copied") << fill->gaps.front().text;
    EXPECT_EQ(fill->samples_inserted, 0);
    EXPECT_NE(fill->note.find("码流复制"), std::string::npos) << fill->note;
}

// 三份报告同源：JSON 能机读到逐处空洞，TXT / HTML 能读到同一件事。
TEST(RepairSilenceFill, FillIsVisibleInAllThreeReports) {
    const SilenceRun run = RunWithAudioGap("report", true, true);
    ASSERT_TRUE(run.result.output_published) << run.result.error;
    const std::string dir = fs::path(run.result.output_path).parent_path().string();
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(run.result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;

    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("音频合成填补"), std::string::npos) << "报告缺少音频合成填补行";
        EXPECT_NE(body.find("已补入"), std::string::npos) << "报告没有说明静音补了多少";
    }

    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* fidelity = root.Find("fidelity");
    ASSERT_NE(fidelity, nullptr);
    const JsonValue* synthetic_audio = fidelity->Find("synthetic_audio");
    ASSERT_NE(synthetic_audio, nullptr);
    ASSERT_TRUE(synthetic_audio->IsArray());
    ASSERT_EQ(synthetic_audio->Size(), 1u);
    const JsonValue& track = synthetic_audio->Items().front();
    EXPECT_EQ(track.Find("coding_mode")->StringValueOr(""), "transcode");
    EXPECT_TRUE(track.Find("requested")->BoolValueOr(false));
    // J64 写的是字符串（大整数不丢精度），所以机读时按字符串读再比较。
    EXPECT_EQ(track.Find("samples_inserted")->StringValueOr(""), std::to_string(12 * kBlock));
    const JsonValue* gaps = track.Find("gaps");
    ASSERT_NE(gaps, nullptr);
    ASSERT_TRUE(gaps->IsArray());
    ASSERT_EQ(gaps->Size(), 1u);
    EXPECT_EQ(gaps->Items().front().Find("reason_code")->StringValueOr(""), "audio_silence_inserted");
    EXPECT_EQ(gaps->Items().front().Find("inserted_samples")->StringValueOr(""), std::to_string(12 * kBlock));
}

// 反向守护：没有空洞的源不许凭空写出「空洞」条目（条目本身仍要有 —— 那是「查过」的证据）。
TEST(RepairSilenceFill, CleanSourceHasNoGapRecords) {
    const std::string dir = MakeTempDir("clean");
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 20;
    videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "clean", spec);
    ASSERT_TRUE(media.ok) << media.error;
    const int audio_index = FirstAudioStreamIndex(media.path);
    ASSERT_GE(audio_index, 0);
    AudioRejectInjection injection(audio_index);
    const RepairResult result =
        repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", true), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    const RepairSyntheticAudioFill* fill = FindAudioFill(result, "transcode");
    ASSERT_NE(fill, nullptr) << "没有空洞也要有条目：查过 ≠ 没事";
    EXPECT_TRUE(fill->inspected);
    EXPECT_EQ(fill->gaps_detected, 0);
    EXPECT_TRUE(fill->gaps.empty());
    EXPECT_EQ(fill->samples_inserted, 0);
}

} // namespace
