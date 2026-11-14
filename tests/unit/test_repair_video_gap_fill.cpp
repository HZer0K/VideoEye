#include <gtest/gtest.h>

// ============================================================
// 阶段 6 / 切片 U + Y：视频轨时间轴空洞的探测与合成填补
//
// 旧行为（实测确认）：S08 的视频 PTS 是「帧序号 × 1/fps」重新生成的，源侧
// 时间戳只用来算一次统一原点。于是源视频轨中间缺一段时，产物把这段静默压掉
// —— 视频变短、音频照原时长走、越到后面口型差得越多，而报告里一句话都没有。
//
// 这一组用例盯住六件事：
//   1. 探测与插帧分开：用户没启用也要如实报出「源里有 N 处空洞」；
//   2. 判据可自证：只有「间隔是帧时长整数倍」的跳变才算可填空洞，
//      不是整数倍的不猜、超过单处上限的不填（大段缺失该由未恢复区间呈现）；
//   3. 填什么必须逐处可辨：黑帧 / 重复上一帧各有自己的内容类型与理由码，
//      没有可用上一帧时退回黑帧并写明（不许假装重复过画面）；
//   4. 「说要重复、实际写黑帧」这类退化只有重新解码产物才抓得到 —— 见
//      `RepeatFillRepeatsThePreviousPictureNotBlack` 的像素级断言；
//   5. 合成内容不进恢复率分子 —— 填了内容不能让恢复比例变好看；
//   6. 三份报告同源：TXT / HTML / JSON 都能看出填在哪、填了什么、为什么那几处没填。
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
#include "core/repair/RepairVideoGapFill.h"
#include "core/reporting/RepairReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/MediaSequenceFingerprint.h"
#include "tests/support/TestMediaFactory.h"

namespace {

using namespace videoeye;
using namespace videoeye::model;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_gapfill_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 25 fps：一帧 0.04 s。
constexpr double kFrameSeconds = 0.04;

repair::VideoGapFillPolicy MakePolicy(double frame_seconds, bool enabled) {
    repair::VideoGapFillPolicy policy;
    policy.frame_seconds = frame_seconds;
    policy.frame_rate_from_source = true;
    policy.fill_enabled = enabled;
    return policy;
}

// 用户要「重复上一帧」时的边界参数。
repair::VideoGapFillPolicy MakeRepeatPolicy(bool enabled, bool previous_available = true) {
    repair::VideoGapFillPolicy policy = MakePolicy(kFrameSeconds, enabled);
    policy.content = VideoGapFillContent::PreviousFrame;
    policy.previous_frame_available = previous_available;
    return policy;
}

RepairRequest MakeRequest(const std::string& source, const std::string& output_dir, bool fill,
                          VideoGapFillContent content = VideoGapFillContent::BlackFrame) {
    RepairRequest request;
    request.source_path = source;
    request.output_directory = output_dir;
    request.verification_level = RepairVerificationLevel::V1_FrameSample;
    request.mode = RepairMode::RecoverContent;
    request.fill_video_gaps = fill;
    request.video_gap_fill_content = content;
    request.write_report = true;
    return request;
}

// ---- 判据（纯函数）----

TEST(RepairVideoGapFill, IntervalWithinToleranceIsNotAGap) {
    for (const double frames : {1.0, 1.2, 1.4, 1.49}) {
        const repair::VideoGapFillPlan plan =
            repair::PlanVideoGapFill(0.0, frames * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0);
        EXPECT_EQ(plan.verdict, repair::VideoGapFillVerdict::NoGap) << "间隔 " << frames << " 帧不该算空洞";
        EXPECT_EQ(plan.gap.inserted_frames, 0);
    }
}

TEST(RepairVideoGapFill, IntegralGapIsFilledWhenEnabled) {
    // 间隔 6 帧 = 缺 5 帧。
    const repair::VideoGapFillPlan plan =
        repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::Fill);
    EXPECT_EQ(plan.gap.missing_frames, 5);
    EXPECT_EQ(plan.gap.inserted_frames, 5);
    EXPECT_EQ(plan.gap.action, "filled");
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_inserted");
    EXPECT_DOUBLE_EQ(plan.gap.source_position_seconds, 1.0);
}

// 反向守护：没启用时**一帧都不许插**，但仍要说清「这里有个洞」。
TEST(RepairVideoGapFill, DisabledFillStillRecordsTheGap) {
    const repair::VideoGapFillPlan plan =
        repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, MakePolicy(kFrameSeconds, false), 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::NotRequested);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.missing_frames, 5) << "缺失帧数仍然要算出来，否则读者无法判断洞有多大";
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_not_requested");
    EXPECT_EQ(plan.gap.action, "not_filled");
}

// 间隔不是帧时长整数倍：更可能是时间戳本身坏了（stts 被改 / 拼接处时基不一致），
// 按帧数外推补黑场等于用编出来的画面掩盖一个未诊断的缺陷 —— 必须拒绝。
TEST(RepairVideoGapFill, NonIntegralIntervalIsRefusedEvenWhenEnabled) {
    const repair::VideoGapFillPlan plan =
        repair::PlanVideoGapFill(1.0, 1.0 + 3.7 * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::NotIntegral);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_not_integral");
}

// 帧率不是源侧证据（SourceFrameRate 兜底的 25 fps）时不许换算：那是编造。
TEST(RepairVideoGapFill, FallbackFrameRateCannotBeUsed) {
    repair::VideoGapFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.frame_rate_from_source = false;
    const repair::VideoGapFillPlan plan = repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, policy, 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::NoFrameRate);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_no_frame_rate");
}

// gap_policy = MergeContiguous 是用户明确要求「塌缩空洞」，插帧会直接违背它。
TEST(RepairVideoGapFill, MergePolicyWinsOverFillRequest) {
    repair::VideoGapFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.merge_contiguous = true;
    const repair::VideoGapFillPlan plan = repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, policy, 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::MergeContiguous);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
}

// 大段缺失不用黑场掩盖：超过单处上限就不填，让报告如实呈现未恢复区间。
TEST(RepairVideoGapFill, LargeGapIsRefusedNotSilentlyTruncated) {
    const double frame_seconds = 0.04;
    const double gap = 5.2; // > 5.0 s
    const repair::VideoGapFillPlan plan = repair::PlanVideoGapFill(0.0, gap, MakePolicy(frame_seconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::GapTooLarge);
    EXPECT_EQ(plan.gap.inserted_frames, 0) << "拒绝就是一点都不填，不许只填前 5 秒";
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_gap_too_large");
}

TEST(RepairVideoGapFill, TotalBudgetStopsInsertion) {
    repair::VideoGapFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.max_total_frames = 20;
    const repair::VideoGapFillPlan plan = repair::PlanVideoGapFill(0.0, 15 * kFrameSeconds, policy, 10);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::BudgetExhausted);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_budget_exhausted");
}

// 时间戳回退不归这里管（有专门的时间轴修复链），绝不能当成「洞」去补黑场。
TEST(RepairVideoGapFill, BackwardsTimestampsAreNotGaps) {
    const repair::VideoGapFillPlan plan = repair::PlanVideoGapFill(2.0, 1.0, MakePolicy(kFrameSeconds, true), 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::NoGap);
    EXPECT_EQ(plan.gap.inserted_frames, 0);
}

// ---- 切片 Y：填什么（黑帧 / 重复上一帧）----

// 判定结果里的 `text` 由调用方在填好 `after_decoded_frames` 之后渲染 —— 规划器
// 不知道当前是第几个解码帧，提前渲染会写出「第 0 个解码帧之后」这种假位置。
// 所以凡是断言文案的用例都必须走这个入口，和 S08 解码链的动作保持一致。
repair::VideoGapFillPlan WithRenderedText(repair::VideoGapFillPlan plan, std::uint64_t after = 20) {
    plan.gap.after_decoded_frames = after;
    plan.gap.text = repair::VideoGapFillGapText(plan.gap);
    return plan;
}

// 默认内容必须是黑帧：「一眼看出这里没有画面」比「看起来连续」更安全，
// 不能让「更顺眼」成为默认值。
TEST(RepairVideoGapFill, DefaultContentIsBlackFrame) {
    const repair::VideoGapFillPlan plan =
        WithRenderedText(repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, MakePolicy(kFrameSeconds, true), 0));
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::Fill);
    EXPECT_EQ(plan.gap.synthetic_kind, "black_frame");
    EXPECT_NE(plan.gap.text.find("合成黑帧"), std::string::npos) << plan.gap.text;
}

// 用户要求重复画面且该处确实有上一帧：必须记成 repeated_frame，不许写成黑帧。
TEST(RepairVideoGapFill, RepeatContentIsMarkedAsRepeated) {
    const repair::VideoGapFillPlan plan =
        WithRenderedText(repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, MakeRepeatPolicy(true), 0));
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::Fill);
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_inserted");
    EXPECT_EQ(plan.gap.synthetic_kind, "repeated_frame");
    EXPECT_EQ(plan.gap.inserted_frames, 5);
    // 文案必须点明「不是源里的新画面」—— 冻结帧最容易被误读成真实内容。
    EXPECT_NE(plan.gap.text.find("重复画面"), std::string::npos) << plan.gap.text;
    EXPECT_NE(plan.gap.text.find("冻结上一帧"), std::string::npos) << plan.gap.text;
    EXPECT_EQ(plan.gap.text.find("合成黑帧"), std::string::npos) << "重复画面不许写成黑帧：" << plan.gap.text;
}

// 要重复但该处没有上一帧：**仍然要占住位置**（否则时间轴重新错位），但必须
// 退回黑帧并逐处写明 —— 两件事同时成立，缺一条就是撒谎。
TEST(RepairVideoGapFill, MissingPreviousFrameFallsBackToBlackAndSaysSo) {
    const repair::VideoGapFillPlan plan =
        WithRenderedText(repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, MakeRepeatPolicy(true, false), 0));
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::FillWithBlackFallback);
    EXPECT_EQ(plan.gap.action, "filled") << "退回黑帧也是填了，不能记成 not_filled";
    EXPECT_EQ(plan.gap.inserted_frames, 5) << "退回黑帧不是不填：位置照样要占住";
    EXPECT_EQ(plan.gap.synthetic_kind, "black_frame") << "实际填的是黑帧，类型不许跟着用户请求走";
    EXPECT_EQ(plan.gap.reason_code, "video_gap_fill_fallback_black");
    EXPECT_NE(plan.gap.text.find("没有可用的上一帧"), std::string::npos) << plan.gap.text;
    EXPECT_NE(plan.gap.text.find("已插入"), std::string::npos) << plan.gap.text;
}

// 反向守护：「上一帧不可用」只对「要重复」的请求有意义。默认黑帧时这个标志
// 是无关信息，不能因此把可填的空洞拒掉。
TEST(RepairVideoGapFill, MissingPreviousFrameDoesNotAffectBlackFill) {
    repair::VideoGapFillPolicy policy = MakePolicy(kFrameSeconds, true);
    policy.previous_frame_available = false;
    const repair::VideoGapFillPlan plan = repair::PlanVideoGapFill(1.0, 1.0 + 6 * kFrameSeconds, policy, 0);
    ASSERT_EQ(plan.verdict, repair::VideoGapFillVerdict::Fill);
    EXPECT_EQ(plan.gap.synthetic_kind, "black_frame");
}

// 逐处自描述：没填的条目不许带内容类型 —— 空串表示「这里没有填进去任何东西」，
// 否则消费方会把它统计成「填了一处黑帧」。
TEST(RepairVideoGapFill, RefusedGapHasNoSyntheticKind) {
    const std::vector<repair::VideoGapFillVerdict> refusals = {
        repair::VideoGapFillVerdict::NotRequested, repair::VideoGapFillVerdict::MergeContiguous,
        repair::VideoGapFillVerdict::GapTooLarge,  repair::VideoGapFillVerdict::NotIntegral,
        repair::VideoGapFillVerdict::NoFrameRate,  repair::VideoGapFillVerdict::BudgetExhausted};
    for (const auto verdict : refusals) {
        EXPECT_TRUE(std::string(repair::VideoGapFillKind(verdict, VideoGapFillContent::PreviousFrame)).empty())
            << "未填补却给出了内容类型：" << repair::VideoGapFillReasonCode(verdict, true);
    }
    // 反向：真正填了的两种判定必须给出内容类型。
    EXPECT_STREQ(repair::VideoGapFillKind(repair::VideoGapFillVerdict::Fill, VideoGapFillContent::BlackFrame),
                 "black_frame");
    EXPECT_STREQ(repair::VideoGapFillKind(repair::VideoGapFillVerdict::Fill, VideoGapFillContent::PreviousFrame),
                 "repeated_frame");
    EXPECT_STREQ(repair::VideoGapFillKind(repair::VideoGapFillVerdict::FillWithBlackFallback,
                                          VideoGapFillContent::PreviousFrame),
                 "black_frame");
}

// 汇总行必须说出「填的是什么」，且退回黑帧时要报数 —— 笼统写「填补 12 帧」
// 会让读者以为整段都是他选的那种内容。
TEST(RepairVideoGapFill, SummaryNamesTheActualContent) {
    RepairSyntheticFill fill;
    fill.inspected = true;
    fill.requested = true;
    fill.gaps_detected = 1;
    fill.frames_inserted = 12;
    fill.seconds_inserted = 0.48;

    fill.content = "black_frame";
    std::string summary = repair::VideoGapFillSummary(fill);
    EXPECT_NE(summary.find("合成黑帧"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("重复画面"), std::string::npos) << summary;

    fill.content = "repeated_frame";
    summary = repair::VideoGapFillSummary(fill);
    EXPECT_NE(summary.find("重复画面"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("合成黑帧"), std::string::npos) << "全部按请求填了，就不该提黑帧：" << summary;

    // 部分退回：必须同时报出「填了 12 帧」和「其中 3 帧是黑帧」。
    fill.fallback_black_frames = 3;
    summary = repair::VideoGapFillSummary(fill);
    EXPECT_NE(summary.find("其中 3 帧"), std::string::npos) << summary;
    EXPECT_NE(summary.find("黑帧"), std::string::npos) << summary;
    EXPECT_NE(summary.find("重复画面"), std::string::npos) << summary;

    // 全部退回：不许再声称填的是重复画面。
    fill.fallback_black_frames = 12;
    summary = repair::VideoGapFillSummary(fill);
    EXPECT_NE(summary.find("全部退回"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("重复画面"), std::string::npos) << "一帧都没重复成功，不许说重复：" << summary;
}

// 反向守护：所有「不填」的判定必须带上自己的理由码，且码不能重复 ——
// 否则报告里会出现「未填补（video_gap_fill_unknown）」这种等于没说的话。
TEST(RepairVideoGapFill, EveryRefusalHasItsOwnReasonCode) {
    const std::vector<repair::VideoGapFillVerdict> refusals = {
        repair::VideoGapFillVerdict::NotRequested,   repair::VideoGapFillVerdict::MergeContiguous,
        repair::VideoGapFillVerdict::GapTooLarge,    repair::VideoGapFillVerdict::NotIntegral,
        repair::VideoGapFillVerdict::NoFrameRate,    repair::VideoGapFillVerdict::BudgetExhausted,
        repair::VideoGapFillVerdict::EncoderRejected};
    std::vector<std::string> codes;
    for (const auto verdict : refusals) {
        const std::string code = repair::VideoGapFillReasonCode(verdict, true);
        EXPECT_FALSE(code.empty());
        EXPECT_EQ(code.find("unknown"), std::string::npos) << "理由码不许退化成 unknown";
        codes.push_back(code);
    }
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(std::adjacent_find(codes.begin(), codes.end()), codes.end()) << "理由码重复了";
}

// 反向守护：没插帧的条目里不许出现「已插入」—— 那是把动作词写成了事实。
TEST(RepairVideoGapFill, RefusalTextNeverClaimsInsertion) {
    const std::vector<std::string> refusal_codes = {"video_gap_fill_not_requested", "video_gap_fill_merge_contiguous",
                                                    "video_gap_fill_gap_too_large", "video_gap_fill_not_integral",
                                                    "video_gap_fill_no_frame_rate", "video_gap_fill_budget_exhausted"};
    for (const auto& code : refusal_codes) {
        RepairSyntheticGap gap;
        gap.source_position_seconds = 1.0;
        gap.gap_seconds = 0.2;
        gap.missing_frames = 5;
        gap.after_decoded_frames = 25;
        gap.action = "not_filled";
        gap.reason_code = code;
        const std::string text = repair::VideoGapFillGapText(gap);
        EXPECT_EQ(text.find("已插入"), std::string::npos) << code << " 的说明里出现了「已插入」";
        EXPECT_NE(text.find("未填补"), std::string::npos) << code << " 没说清为什么没填补：" << text;
    }
}

// 没跑解码链 ≠ 没有空洞。空表不许被读成「没问题」。
TEST(RepairVideoGapFill, UninspectedSummaryDoesNotClaimZeroGaps) {
    RepairSyntheticFill fill;
    const std::string summary = repair::VideoGapFillSummary(fill);
    EXPECT_NE(summary.find("未做空洞探测"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("探测到 0 处"), std::string::npos) << "未探测不许渲染成「0 处空洞」";
    const std::string note = repair::VideoGapFillBasisNote(fill);
    EXPECT_NE(note.find("不等于"), std::string::npos) << "必须写明「没探测」不等于「没有空洞」：" << note;
}

TEST(RepairVideoGapFill, SummaryCountsFilledAndRefused) {
    RepairSyntheticFill fill;
    fill.inspected = true;
    fill.requested = true;
    fill.gaps_detected = 3;
    fill.frames_inserted = 25;
    fill.seconds_inserted = 1.0;
    fill.gaps_refused = 2;
    const std::string summary = repair::VideoGapFillSummary(fill);
    EXPECT_NE(summary.find("3 处空洞"), std::string::npos) << summary;
    EXPECT_NE(summary.find("25 帧"), std::string::npos) << summary;
    EXPECT_NE(summary.find("2 处未填补"), std::string::npos) << summary;
}

TEST(RepairVideoGapFill, MissingSourceTimestampsAreReported) {
    RepairSyntheticFill fill;
    fill.inspected = true;
    fill.frames_without_source_pts = 7;
    const std::string note = repair::VideoGapFillBasisNote(fill);
    EXPECT_NE(note.find("7 帧没有源时间戳"), std::string::npos) << note;
    EXPECT_NE(note.find("不完整"), std::string::npos) << note;
}

// ---- 端到端：真实 S08 转码链 ----

struct GapRun {
    RepairResult result;
    videoeye_test::MediaProbe source;
    videoeye_test::MediaProbe output;
};

GapRun RunWithGap(const std::string& tag, bool fill, VideoGapFillContent content = VideoGapFillContent::BlackFrame) {
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
    run.result = repair::RepairEngine::Run(MakeRequest(media.path, dir + "/out", fill, content), nullptr, nullptr);
    if (run.result.output_published)
        run.output = videoeye_test::ProbeMediaFile(run.result.output_path);
    return run;
}

// 探测是证据：用户没启用插帧，也必须在报告里看到「源里有 1 处空洞」。
TEST(RepairVideoGapFill, GapIsDetectedAndReportedEvenWhenFillIsOff) {
    const GapRun run = RunWithGap("off", false);
    ASSERT_TRUE(run.result.output_published) << run.result.summary << " | " << run.result.error;

    const RepairSyntheticFill& fill = run.result.fidelity.synthetic;
    EXPECT_TRUE(fill.inspected) << "跑了 S08 解码链就必须做过探测";
    EXPECT_FALSE(fill.requested);
    ASSERT_EQ(fill.gaps_detected, 1) << "源里至少有 1 处空洞没被发现";
    ASSERT_EQ(fill.gaps.size(), 1u);
    EXPECT_EQ(fill.gaps.front().reason_code, "video_gap_fill_not_requested")
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
TEST(RepairVideoGapFill, FillRestoresDurationWithoutInflatingRecovery) {
    const GapRun off = RunWithGap("off2", false);
    const GapRun on = RunWithGap("on", true);
    ASSERT_TRUE(off.result.output_published) << off.result.error;
    ASSERT_TRUE(on.result.output_published) << on.result.error;

    const RepairSyntheticFill& fill = on.result.fidelity.synthetic;
    ASSERT_EQ(fill.gaps.size(), 1u);
    EXPECT_EQ(fill.gaps.front().reason_code, "video_gap_fill_inserted");
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
TEST(RepairVideoGapFill, FillIsVisibleInAllThreeReports) {
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
    // 切片 Y：内容类型必须机读得到 —— 报告读者不能只靠中文文案判断「填的是黑帧
    // 还是重复画面」，那正是最容易被误读的一条。
    EXPECT_EQ(synthetic->Find("content")->StringValueOr(""), "black_frame");
    EXPECT_EQ(synthetic->Find("fallback_black_frames")->StringValueOr(""), "0");
    const JsonValue* gaps = synthetic->Find("gaps");
    ASSERT_NE(gaps, nullptr);
    ASSERT_TRUE(gaps->IsArray());
    ASSERT_EQ(gaps->Size(), 1u);
    EXPECT_EQ(gaps->Items().front().Find("reason_code")->StringValueOr(""), "video_gap_fill_inserted");
    EXPECT_EQ(gaps->Items().front().Find("synthetic_kind")->StringValueOr(""), "black_frame");
    EXPECT_EQ(gaps->Items().front().Find("inserted_frames")->StringValueOr(""), "12");
}

// ---- 逐帧亮度：证明「填进去的到底是什么画面」----
//
// 只比报告字段抓不到「说要重复、实际写黑帧」这种退化：报告是 repair 层自己写的，
// 它的自洽不能当作像素事实。这里把产物重新解码一遍，取显示顺序的逐帧亮度均值。
struct FrameLuma {
    bool ok = false;
    std::string error;
    std::vector<double> values;
};

FrameLuma OutputFrameLuma(const std::string& path) {
    FrameLuma out;
    const videoeye_test::MediaSequence sequence = videoeye_test::CaptureMediaSequence(path);
    out.error = sequence.error;
    if (!sequence.opened) {
        out.error = "打不开产物：" + sequence.error;
        return out;
    }
    for (const auto& stream : sequence.streams) {
        if (stream.codec_type == "video" && !stream.frame_mean_luma.empty()) {
            out.values = stream.frame_mean_luma;
            out.ok = true;
            return out;
        }
    }
    out.error = "产物里没有解出任何视频帧";
    return out;
}

double MeanOfRange(const std::vector<double>& values, std::size_t begin, std::size_t count) {
    if (begin >= values.size() || count == 0)
        return 0.0;
    const std::size_t end = std::min(values.size(), begin + count);
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += values[i];
    return sum / static_cast<double>(end - begin);
}

// 切片 Y：重复帧填补必须**真的**重复上一帧画面，不能换成黑帧糊过去。
//
// 这是本切片唯一的像素级判据。变异「repeat 分支实际送黑帧」在报告层看不出来
// （理由码、内容类型、帧数全都对），只有重新解码产物才能抓到。
TEST(RepairVideoGapFill, RepeatFillRepeatsThePreviousPictureNotBlack) {
    const GapRun black = RunWithGap("blk", true, VideoGapFillContent::BlackFrame);
    const GapRun repeat = RunWithGap("rep", true, VideoGapFillContent::PreviousFrame);
    ASSERT_TRUE(black.result.output_published) << black.result.error;
    ASSERT_TRUE(repeat.result.output_published) << repeat.result.error;

    // ---- 报告层：内容类型与请求一致，且一帧都没退回 ----
    const RepairSyntheticFill& fill = repeat.result.fidelity.synthetic;
    EXPECT_EQ(fill.content, "repeated_frame");
    ASSERT_EQ(fill.gaps.size(), 1u);
    EXPECT_EQ(fill.gaps.front().synthetic_kind, "repeated_frame") << fill.gaps.front().text;
    EXPECT_EQ(fill.fallback_black_frames, 0) << fill.summary;
    EXPECT_EQ(fill.frames_inserted, 12);
    EXPECT_NE(fill.summary.find("重复画面"), std::string::npos) << fill.summary;
    EXPECT_NE(repeat.result.fidelity.timeline_note.find("重复画面"), std::string::npos)
        << repeat.result.fidelity.timeline_note;

    // ---- 像素层：这一段必须真的是上一帧，而不是黑场 ----
    const FrameLuma rep = OutputFrameLuma(repeat.result.output_path);
    const FrameLuma blk = OutputFrameLuma(black.result.output_path);
    ASSERT_TRUE(rep.ok) << rep.error;
    ASSERT_TRUE(blk.ok) << blk.error;

    const std::size_t start = static_cast<std::size_t>(fill.gaps.front().after_decoded_frames);
    const std::size_t count = 12;
    ASSERT_GT(rep.values.size(), start + count) << "产物解出的帧数不足，无法验证填补位置";
    ASSERT_GT(blk.values.size(), start + count) << "黑帧那次解出的帧数不足，无法对照";
    ASSERT_GE(start, 1u) << "空洞之前应当已经有解码帧";

    // 语料画面是确定性线性渐变（FillVideoFrame），亮度均值在 128 附近；黑帧在 16 附近。
    // 容差留足余量以吸收有损重编码的偏差。
    const double rep_mean = MeanOfRange(rep.values, start, count);
    const double blk_mean = MeanOfRange(blk.values, start, count);
    EXPECT_GT(rep_mean, 90.0) << "重复帧填补填进去的是暗画面（近似黑场）：" << rep_mean;
    EXPECT_LT(blk_mean, 32.0) << "黑帧填补填进去的却不是黑场：" << blk_mean;
    EXPECT_GT(rep_mean - blk_mean, 60.0) << "两种填补内容的亮度几乎一样，说明「重复」没有真的发生";

    // 逐帧检查：重复段的每一帧都不许掉成黑场（一处掉下去就说明只补了一部分是重复帧）。
    for (std::size_t i = 0; i < count; ++i)
        EXPECT_GT(rep.values[start + i], 80.0) << "第 " << i << " 帧插入的不是上一帧画面";

    // 被重复的那一帧本身要是真实画面，否则「重复上一帧」无从谈起。
    const double previous_mean = MeanOfRange(rep.values, start - 1, 1);
    EXPECT_GT(previous_mean, 80.0) << "空洞之前那一帧本身就不是有效画面";
    EXPECT_NEAR(rep_mean, previous_mean, 8.0) << "重复段的亮度与上一帧差太多，可能重复的不是上一帧";
}

// 反向守护：没有空洞的源不许凭空写出「合成填补」条目。
TEST(RepairVideoGapFill, CleanSourceHasNoGapRecords) {
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
