#include <gtest/gtest.h>

// ============================================================
// 阶段 6 / 切片 U：视频轨时间轴空洞的探测与合成黑帧填补
//
// 旧行为（实测确认）：S08 的视频 PTS 是「帧序号 × 1/fps」重新生成的，源侧
// 时间戳只用来算一次统一原点。于是源视频轨中间缺一段时，产物把这段静默压掉
// —— 视频变短、音频照原时长走、越到后面口型差得越多，而报告里一句话都没有。
//
// 这一组用例盯住四件事：
//   1. 探测与插帧分开：用户没启用也要如实报出「源里有 N 处空洞」；
//   2. 判据可自证：只有「间隔是帧时长整数倍」的跳变才算可填空洞，
//      不是整数倍的不猜、超过单处上限的不填（大段缺失该由未恢复区间呈现）；
//   3. 合成内容不进恢复率分子 —— 插了黑帧不能让恢复比例变好看；
//   4. 三份报告同源：TXT / HTML / JSON 都能看出插在哪、为什么那几处没插。
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
#include "core/repair/RepairBlackFrameFill.h"
#include "core/repair/RepairEngine.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/TestMediaFactory.h"

namespace {

using namespace videoeye;
using namespace videoeye::model;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_blackfill_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

repair::BlackFillPolicy MakePolicy(double frame_seconds, bool enabled) {
    repair::BlackFillPolicy policy;
    policy.frame_seconds = frame_seconds;
    policy.frame_rate_from_source = true;
    policy.fill_enabled = enabled;
    return policy;
}

// 25 fps：一帧 0.04 s。
constexpr double kFrameSeconds = 0.04;

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir, bool fill) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = RepairVerificationLevel::V1_FrameSample;
    request.mode = RepairMode::RecoverContent;
    request.fill_video_gaps_with_black_frames = fill;
    request.write_report = true;
    return request;
}

// ---- 判据（纯函数）----

TEST(RepairBlackFrameFill, IntervalWithinToleranceIsNotAGap) {
    for (const double frames : {1.0, 1.2, 1.4, 1.49}) {
        const repair::BlackFillPlan plan =
            repair::PlanBlackFill(0.0, frames * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0);
        EXPECT_EQ(plan.verdict, repair::BlackFillVerdict::NoGap) << "间隔 " << frames << " 帧不该算空洞";
        EXPECT_EQ(plan.gap.inserted_frames, 0);
    }
}

TEST(RepairBlackFrameFill, IntegralGapIsFilledWhenEnabled) {
    // 间隔 6 帧 = 缺 5 帧。
    const repair::BlackFillPlan plan =
        repair::PlanBlackFill(1.0, 1.0 + 6 * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::Fill);
    EXPECT_EQ(plan.gap.missing_frames, 5);
    EXPECT_EQ(plan.gap.inserted_frames, 5);
    EXPECT_EQ(plan.gap.action, "filled");
    EXPECT_EQ(plan.gap.reason_code, "black_fill_inserted");
    EXPECT_DOUBLE_EQ(plan.gap.source_position_seconds, 1.0);
}

// 反向守护：没启用时**一帧都不许插**，但仍要说清「这里有个洞」。
TEST(RepairBlackFrameFill, DisabledFillStillRecordsTheGap) {
    const repair::BlackFillPlan plan =
        repair::PlanBlackFill(1.0, 1.0 + 6 * kFrameSeconds, MakePolicy(kFrameSeconds, false), 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::NotRequested);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.missing_frames, 5) << "缺失帧数仍然要算出来，否则读者无法判断洞有多大";
    EXPECT_EQ(plan.gap.reason_code, "black_fill_not_requested");
    EXPECT_EQ(plan.gap.action, "not_filled");
}

// 间隔不是帧时长整数倍：更可能是时间戳本身坏了（stts 被改 / 拼接处时基不一致），
// 按帧数外推补黑场等于用编出来的画面掩盖一个未诊断的缺陷 —— 必须拒绝。
TEST(RepairBlackFrameFill, NonIntegralIntervalIsRefusedEvenWhenEnabled) {
    const repair::BlackFillPlan plan =
        repair::PlanBlackFill(1.0, 1.0 + 3.7 * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::NotIntegral);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.reason_code, "black_fill_not_integral");
}

// 帧率不是源侧证据（SourceFrameRate 兜底的 25 fps）时不许换算：那是编造。
TEST(RepairBlackFrameFill, FallbackFrameRateCannotBeUsed) {
    repair::BlackFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.frame_rate_from_source = false;
    const repair::BlackFillPlan plan = repair::PlanBlackFill(1.0, 1.0 + 6 * kFrameSeconds, policy, 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::NoFrameRate);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.reason_code, "black_fill_no_frame_rate");
}

// gap_policy = MergeContiguous 是用户明确要求「塌缩空洞」，插帧会直接违背它。
TEST(RepairBlackFrameFill, MergePolicyWinsOverFillRequest) {
    repair::BlackFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.merge_contiguous = true;
    const repair::BlackFillPlan plan = repair::PlanBlackFill(1.0, 1.0 + 6 * kFrameSeconds, policy, 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::MergeContiguous);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
}

// 大段缺失不用黑场掩盖：超过单处上限就不填，让报告如实呈现未恢复区间。
TEST(RepairBlackFrameFill, LargeGapIsRefusedNotSilentlyTruncated) {
    const double frame_seconds = 0.04;
    const double gap = 5.2; // > 5.0 s
    const repair::BlackFillPlan plan = repair::PlanBlackFill(0.0, gap, MakePolicy(frame_seconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::GapTooLarge);
    EXPECT_EQ(plan.gap.inserted_frames, 0) << "拒绝就是一点都不填，不许只填前 5 秒";
    EXPECT_EQ(plan.gap.reason_code, "black_fill_gap_too_large");
}

TEST(RepairBlackFrameFill, TotalBudgetStopsInsertion) {
    repair::BlackFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.max_total_frames = 20;
    const repair::BlackFillPlan plan = repair::PlanBlackFill(0.0, 15 * kFrameSeconds, policy, 10);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::BudgetExhausted);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.reason_code, "black_fill_budget_exhausted");
}

// 时间戳回退不归这里管（有专门的时间轴修复链），绝不能当成「洞」去补黑场。
TEST(RepairBlackFrameFill, BackwardsTimestampsAreNotGaps) {
    const repair::BlackFillPlan plan = repair::PlanBlackFill(2.0, 1.0, MakePolicy(kFrameSeconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::BlackFillVerdict::NoGap);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
}

// 反向守护：所有「不填」的判定必须带上自己的理由码，且码不能重复 ——
// 否则报告里会出现「未填补（black_fill_unknown）」这种等于没说的话。
TEST(RepairBlackFrameFill, EveryRefusalHasItsOwnReasonCode) {
    const std::vector<repair::BlackFillVerdict> refusals = {
        repair::BlackFillVerdict::NotRequested,   repair::BlackFillVerdict::MergeContiguous,
        repair::BlackFillVerdict::GapTooLarge,    repair::BlackFillVerdict::NotIntegral,
        repair::BlackFillVerdict::NoFrameRate,    repair::BlackFillVerdict::BudgetExhausted,
        repair::BlackFillVerdict::EncoderRejected};
    std::vector<std::string> codes;
    for (const auto verdict : refusals) {
        const std::string code = repair::BlackFillReasonCode(verdict, true);
        EXPECT_FALSE(code.empty());
        EXPECT_EQ(code.find("unknown"), std::string::npos) << "理由码不许退化成 unknown";
        codes.push_back(code);
    }
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(std::adjacent_find(codes.begin(), codes.end()), codes.end()) << "理由码重复了";
}

// 反向守护：没插帧的条目里不许出现「已插入」—— 那是把动作词写成了事实。
TEST(RepairBlackFrameFill, RefusalTextNeverClaimsInsertion) {
    const std::vector<std::string> refusal_codes = {"black_fill_not_requested", "black_fill_merge_contiguous",
                                                    "black_fill_gap_too_large", "black_fill_not_integral",
                                                    "black_fill_no_frame_rate", "black_fill_budget_exhausted"};
    for (const auto& code : refusal_codes) {
        RepairSyntheticGap gap;
        gap.source_position_seconds = 1.0;
        gap.gap_seconds = 0.2;
        gap.missing_frames = 5;
        gap.after_decoded_frames = 25;
        gap.action = "not_filled";
        gap.reason_code = code;
        const std::string text = repair::BlackFillGapText(gap);
        EXPECT_EQ(text.find("已插入"), std::string::npos) << code << " 的说明里出现了「已插入」";
        EXPECT_NE(text.find("未填补"), std::string::npos) << code << " 没说清为什么没填补：" << text;
    }
}

// 没跑解码链 ≠ 没有空洞。空表不许被读成「没问题」。
TEST(RepairBlackFrameFill, UninspectedSummaryDoesNotClaimZeroGaps) {
    RepairSyntheticFill fill;
    const std::string summary = repair::BlackFillSummary(fill);
    EXPECT_NE(summary.find("未做空洞探测"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("探测到 0 处"), std::string::npos) << "未探测不许渲染成「0 处空洞」";
    const std::string note = repair::BlackFillBasisNote(fill);
    EXPECT_NE(note.find("不等于"), std::string::npos) << "必须写明「没探测」不等于「没有空洞」：" << note;
}

TEST(RepairBlackFrameFill, SummaryCountsFilledAndRefused) {
    RepairSyntheticFill fill;
    fill.inspected = true;
    fill.requested = true;
    fill.gaps_detected = 3;
    fill.frames_inserted = 25;
    fill.seconds_inserted = 1.0;
    fill.gaps_refused = 2;
    const std::string summary = repair::BlackFillSummary(fill);
    EXPECT_NE(summary.find("3 处空洞"), std::string::npos) << summary;
    EXPECT_NE(summary.find("25 帧"), std::string::npos) << summary;
    EXPECT_NE(summary.find("2 处未填补"), std::string::npos) << summary;
}

TEST(RepairBlackFrameFill, MissingSourceTimestampsAreReported) {
    RepairSyntheticFill fill;
    fill.inspected = true;
    fill.frames_without_source_pts = 7;
    const std::string note = repair::BlackFillBasisNote(fill);
    EXPECT_NE(note.find("7 帧没有源时间戳"), std::string::npos) << note;
    EXPECT_NE(note.find("不完整"), std::string::npos) << note;
}

// ---- 端到端：真实 S08 转码链 ----

struct GapRun {
    RepairResult result;
    videoeye_test::MediaProbe source;
    videoeye_test::MediaProbe output;
};

GapRun RunWithGap(const std::string& tag, bool fill) {
    const std::string dir = MakeTempDir(tag);
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 40;
    spec.fps = 25;
    // 第 20 帧之后空 12 帧（0.48 s）：够长，不会被 1.5 帧容差吃掉。
    spec.video_gap_after_frame = 20;
    spec.video_gap_frames = 12;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "gap", spec);
    GapRun run;
    if (!media.ok) {
        ADD_FAILURE() << "语料生成失败: " << media.error;
        return run;
    }
    run.source = videoeye_test::ProbeMediaFile(media.path);
    run.result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", fill), nullptr, nullptr);
    if (run.result.output_published)
        run.output = videoeye_test::ProbeMediaFile(run.result.output_path);
    return run;
}

// 探测是证据：用户没启用插帧，也必须在报告里看到「源里有 1 处空洞」。
TEST(RepairBlackFrameFill, GapIsDetectedAndReportedEvenWhenFillIsOff) {
    const GapRun run = RunWithGap("off", false);
    ASSERT_TRUE(run.result.output_published) << run.result.summary << " | " << run.result.error;

    const RepairSyntheticFill& fill = run.result.fidelity.synthetic;
    EXPECT_TRUE(fill.inspected) << "跑了 S08 解码链就必须做过探测";
    EXPECT_FALSE(fill.requested);
    ASSERT_EQ(fill.gaps_detected, 1) << "源里至少有 1 处空洞没被发现";
    ASSERT_EQ(fill.gaps.size(), 1u);
    EXPECT_EQ(fill.gaps.front().reason_code, "black_fill_not_requested")
        << "实际判定：" << fill.gaps.front().text << "（间隔 " << fill.gaps.front().gap_seconds << " s，缺 "
        << fill.gaps.front().missing_frames << " 帧）";
    EXPECT_EQ(fill.gaps.front().inserted_frames, 0);
    EXPECT_EQ(fill.frames_inserted, 0);
    EXPECT_FALSE(run.result.fidelity.synthetic_frames_inserted);
    EXPECT_EQ(run.result.fidelity.synthetic_frame_count, 0u);

    // 源片 40 个包 + 12 帧空洞。基线一律用**实测解码帧数**，不写常量：
    // 语料工厂的最后一个包解不出帧（既有行为，与空洞无关 —— clean 语料同样
    // 是 20 包 / 19 帧），用常量断言会把这条无关的怪癖算进本切片。
    ASSERT_TRUE(run.source.opened) << run.source.error;
    ASSERT_GE(run.source.frame_count, 30) << "语料至少要能解出大部分帧，否则这个样本没有意义";
    ASSERT_TRUE(run.output.opened) << run.output.error;
    EXPECT_EQ(run.output.frame_count, run.source.frame_count)
        << "未填补时产物帧数应等于源的可解码帧数（汇总=" << run.result.summary << "）";
    // 报告必须说明「探到了但没填」，不能让读者以为没有空洞。
    const bool limitation_mentions_gap =
        std::any_of(run.result.fidelity.limitations.begin(), run.result.fidelity.limitations.end(),
                    [](const std::string& item) { return item.find("处时间轴空洞") != std::string::npos; });
    EXPECT_TRUE(limitation_mentions_gap) << "空洞探到却没填，限制项里必须写出来";
}

// 启用后：空洞被黑帧填满，产物时长恢复，但**恢复比例不许变好看**。
TEST(RepairBlackFrameFill, FillRestoresDurationWithoutInflatingRecovery) {
    const GapRun off = RunWithGap("off2", false);
    const GapRun on = RunWithGap("on", true);
    ASSERT_TRUE(off.result.output_published) << off.result.error;
    ASSERT_TRUE(on.result.output_published) << on.result.error;

    const RepairSyntheticFill& fill = on.result.fidelity.synthetic;
    ASSERT_EQ(fill.gaps.size(), 1u);
    EXPECT_EQ(fill.gaps.front().reason_code, "black_fill_inserted");
    EXPECT_EQ(fill.gaps.front().missing_frames, 12);
    EXPECT_EQ(fill.gaps.front().inserted_frames, 12);
    EXPECT_EQ(fill.frames_inserted, 12);
    EXPECT_TRUE(on.result.fidelity.synthetic_frames_inserted);
    EXPECT_EQ(on.result.fidelity.synthetic_frame_count, 12u);

    // 产物里真的多了 12 帧（不是只在报告里记了一笔）。基线取未填补那次实测帧数，
    // 免得把语料工厂「最后一个包解不出帧」的既有怪癖混进来。
    ASSERT_TRUE(off.output.opened) << off.output.error;
    ASSERT_TRUE(on.output.opened) << on.output.error;
    EXPECT_EQ(on.output.frame_count, off.output.frame_count + 12)
        << "未填补 " << off.output.frame_count << " 帧，填补后应多 12 帧（汇总=" << on.result.summary << "）";
    EXPECT_GT(on.output.video_duration_seconds, off.output.video_duration_seconds + 0.4)
        << "填补后视频时长必须回到源侧声明值";

    // 合成内容不进分子：两种跑法的恢复比例必须基本相同。
    const auto ratio_off = off.result.fidelity.original_content_recovery_ratio;
    const auto ratio_on = on.result.fidelity.original_content_recovery_ratio;
    ASSERT_TRUE(ratio_off.has_value());
    ASSERT_TRUE(ratio_on.has_value());
    EXPECT_NEAR(*ratio_on, *ratio_off, 0.02)
        << "填补把时长补长了，但那段不是恢复出来的内容：比例不许被抬高（off=" << *ratio_off << " on=" << *ratio_on
        << "）";
    EXPECT_LT(*ratio_on, 1.0) << "有空洞却报 100% 恢复，说明合成时长进了分子";

    // 合成帧不进样本账本：账本总数必须与未填补那次完全一致。记进去的话，
    // 逐轨覆盖会把填补的空档算成「恢复出来的内容」。
    EXPECT_EQ(on.result.sample_ledger.total_samples, off.result.sample_ledger.total_samples)
        << "合成黑帧被记进了样本账本（off=" << off.result.sample_ledger.total_samples
        << " on=" << on.result.sample_ledger.total_samples << "）";

    // 帧序不再与源一一对应这件事必须写在时间轴说明里。
    EXPECT_NE(on.result.fidelity.timeline_note.find("合成黑帧"), std::string::npos) << on.result.fidelity.timeline_note;
}

// 三份报告同源：JSON 能机读到逐处空洞，TXT / HTML 能读到同一件事。
TEST(RepairBlackFrameFill, FillIsVisibleInAllThreeReports) {
    const GapRun run = RunWithGap("report", true);
    ASSERT_TRUE(run.result.output_published) << run.result.error;
    const std::string dir = fs::path(run.result.output_path).parent_path().string();
    const reporting::RepairReportWriteResult written =
        reporting::RepairReportExporter::WriteAll(run.result, dir + "/r.json", dir + "/r.txt", dir + "/r.html");
    ASSERT_TRUE(written.AllOk()) << written.error;

    for (const auto& body : {ReadText(dir + "/r.txt"), ReadText(dir + "/r.html")}) {
        EXPECT_NE(body.find("合成填补"), std::string::npos) << "报告缺少合成填补行";
        EXPECT_NE(body.find("已插入"), std::string::npos) << "报告没有说明黑帧插了多少";
    }

    JsonValue root;
    std::string error;
    ASSERT_TRUE(JsonParse(ReadText(dir + "/r.json"), root, &error)) << error;
    const JsonValue* fidelity = root.Find("fidelity");
    ASSERT_NE(fidelity, nullptr);
    EXPECT_TRUE(fidelity->Find("synthetic_frames_inserted")->BoolValueOr(false));
    // J64 写的是字符串（大整数不丢精度），所以机读时按字符串读再比较。
    EXPECT_EQ(fidelity->Find("synthetic_frame_count")->StringValueOr(""), "12");
    const JsonValue* synthetic = fidelity->Find("synthetic");
    ASSERT_NE(synthetic, nullptr);
    EXPECT_TRUE(synthetic->Find("inspected")->BoolValueOr(false));
    EXPECT_TRUE(synthetic->Find("requested")->BoolValueOr(false));
    EXPECT_EQ(synthetic->Find("frames_inserted")->StringValueOr(""), "12");
    const JsonValue* gaps = synthetic->Find("gaps");
    ASSERT_NE(gaps, nullptr);
    ASSERT_TRUE(gaps->IsArray());
    ASSERT_EQ(gaps->Size(), 1u);
    EXPECT_EQ(gaps->Items().front().Find("reason_code")->StringValueOr(""), "black_fill_inserted");
    EXPECT_EQ(gaps->Items().front().Find("inserted_frames")->StringValueOr(""), "12");
}

// 反向守护：没有空洞的源不许凭空写出「合成填补」条目。
TEST(RepairBlackFrameFill, CleanSourceHasNoGapRecords) {
    const std::string dir = MakeTempDir("clean");
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 20;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "clean", spec);
    ASSERT_TRUE(media.ok) << media.error;
    const RepairResult result =
        repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", true), nullptr, nullptr);
    ASSERT_TRUE(result.output_published) << result.summary << " | " << result.error;
    EXPECT_EQ(result.fidelity.synthetic.gaps_detected, 0);
    EXPECT_TRUE(result.fidelity.synthetic.gaps.empty());
    EXPECT_FALSE(result.fidelity.synthetic_frames_inserted);
}

} // namespace
