// BitrateGopAnalyzer 单元测试
//
// 覆盖 docs/BITRATE_GOP_ANALYSIS.md 中的 4.6 验收标准:
//   1) 固定 GOP 样本 -> 输出稳定的 GOP 长度
//   2) 超长 GOP 样本 -> 必须产生 warning
//   3) VBR 样本     -> 能显示峰值码率与平均码率差异
//   4) 场景切换附近无关键帧 -> 输出优化建议
//
// 该测试不依赖 FFmpeg / Qt，直接喂合成样本，可独立编译。
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/analysis/quality/BitrateGopAnalyzer.h"

using namespace videoeye;

namespace {

struct GenOptions {
    int frames = 300;
    int gop = 50;
    double fps = 25.0;
    double base_bytes = 20000.0;
    bool vbr = false;
    bool emit_frame_types = true;
};

void Generate(BitrateGopAnalyzer& analyzer, const GenOptions& opt) {
    const double frame_dur = 1.0 / opt.fps;
    for (int i = 0; i < opt.frames; ++i) {
        const double ts = static_cast<double>(i) * frame_dur;
        const bool is_key = (i % opt.gop) == 0;
        double bytes = opt.base_bytes * (is_key ? 5.0 : 1.0);
        if (opt.vbr && (i % 60) < 20) bytes *= 4.0;  // 每 60 帧一段高复杂度
        const int64_t size = static_cast<int64_t>(bytes);
        if (opt.emit_frame_types) {
            model::FrameType type = model::FrameType::P;
            if (is_key) {
                type = model::FrameType::I;
            } else if ((i % 3) != 0) {
                type = model::FrameType::B;
            }
            analyzer.OnFrame(0, ts, size, type, is_key);
        } else {
            analyzer.OnPacket(0, ts, size, is_key);
        }
    }
}

}  // namespace

// ---- 验收 1: 固定 GOP 应输出稳定 GOP 长度 ----
TEST(BitrateGopAnalyzerTest, FixedGopProducesStableLength) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    Generate(analyzer, GenOptions{/*.frames=*/300, /*.gop=*/50});
    const BitrateGopAnalysis& r = analyzer.Finish();

    ASSERT_EQ(r.gops.size(), 6u);
    for (const auto& gop : r.gops) {
        EXPECT_EQ(gop.frame_count, 50);
        EXPECT_TRUE(gop.closed_gop);
    }
    EXPECT_NEAR(r.gop_duration_stddev, 0.0, 1e-6);
    EXPECT_EQ(r.long_gop_count, 0);
    EXPECT_EQ(analyzer.result().i_frame_count, 6);
    EXPECT_GT(r.p_frame_count, 0);
    EXPECT_GT(r.b_frame_count, 0);
    EXPECT_NEAR(r.IFrameRatio(), 6.0 / 300.0, 1e-9);
    EXPECT_EQ(r.CountAnomalies(BitrateAnomalyType::IrregularKeyInterval), 0);
}

// ---- 验收 2: 超长 GOP 必须产生 warning ----
TEST(BitrateGopAnalyzerTest, LongGopTriggersWarning) {
    BitrateGopAnalyzer analyzer;
    BitrateGopOptions options;
    options.max_gop_seconds = 10.0;
    analyzer.Reset(options);
    // 25fps，每 1000 帧一个关键帧 -> 40 秒一个 GOP
    Generate(analyzer, GenOptions{/*.frames=*/2500, /*.gop=*/1000});
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_GE(r.long_gop_count, 2);
    EXPECT_GE(r.CountAnomalies(BitrateAnomalyType::LongGop), 2u);
    EXPECT_GT(r.gop_duration_max, 10.0);
    if (!r.anomalies.empty()) {
        EXPECT_FALSE(r.anomalies.front().detail.empty());
        EXPECT_FALSE(r.anomalies.front().suggestion.empty());
    }
    bool has_suggestion = false;
    for (const auto& s : r.suggestions) {
        if (s.find("超长 GOP") != std::string::npos) has_suggestion = true;
    }
    EXPECT_TRUE(has_suggestion);
}

// ---- 验收 3: VBR 应能显示峰值与平均码率差异 ----
TEST(BitrateGopAnalyzerTest, VbrShowsPeakVsAverage) {
    BitrateGopAnalyzer analyzer;
    BitrateGopOptions options;
    options.target_peak_kbps = 12000.0;
    analyzer.Reset(options);
    GenOptions g;
    g.frames = 600;
    g.gop = 50;
    g.vbr = true;
    Generate(analyzer, g);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_GT(r.peak_bitrate_kbps, r.avg_bitrate_kbps * 1.5);
    EXPECT_GT(r.peak_to_mean_ratio, 1.5);
    EXPECT_NEAR(r.target_peak_kbps, 12000.0, 1e-6);
    EXPECT_GT(r.CountAnomalies(BitrateAnomalyType::PeakOvershoot), 0u);

    // 多窗口曲线: 窗口越短峰值越高
    const model::MetricSeries* w05 = analyzer.WindowCurve(0.5);
    const model::MetricSeries* w5 = analyzer.WindowCurve(5.0);
    ASSERT_NE(w05, nullptr);
    ASSERT_NE(w5, nullptr);
    EXPECT_GE(w05->Max(), w5->Max() - 1e-6);
}

TEST(BitrateGopAnalyzerTest, AutoTargetPeakDoesNotFireWhenBelow) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();  // 自动目标峰值 = 平均 * 2.0
    GenOptions g;
    g.frames = 600;
    g.gop = 50;
    g.vbr = true;
    Generate(analyzer, g);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_NEAR(r.target_peak_kbps, r.avg_bitrate_kbps * 2.0, 1.0);
    EXPECT_EQ(r.CountAnomalies(BitrateAnomalyType::PeakOvershoot), 0u);
}

// ---- 验收 4: 场景切换附近无关键帧应输出优化建议 ----
TEST(BitrateGopAnalyzerTest, SceneChangeWithoutKeyframeProducesSuggestion) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    GenOptions g;
    g.frames = 750;
    g.gop = 250;  // 25fps -> 10 秒一个关键帧
    Generate(analyzer, g);

    std::vector<SceneChangeResult> cuts;
    SceneChangeResult far_cut;  // 4.8s，远离关键帧
    far_cut.frame_index = 120;
    far_cut.timestamp = 120.0 / 25.0;
    far_cut.score = 0.85;
    cuts.push_back(far_cut);
    SceneChangeResult on_key;  // 20.0s，正好是关键帧
    on_key.frame_index = 500;
    on_key.timestamp = 500.0 / 25.0;
    on_key.score = 0.90;
    cuts.push_back(on_key);

    analyzer.AssociateSceneChanges(cuts);
    const BitrateGopAnalysis& r = analyzer.Finish();

    ASSERT_EQ(r.scene_matches.size(), 2u);
    EXPECT_FALSE(r.scene_matches[0].has_nearby_keyframe);
    EXPECT_TRUE(r.scene_matches[1].has_nearby_keyframe);
    EXPECT_EQ(r.CountAnomalies(BitrateAnomalyType::SceneChangeWithoutKeyframe), 1);

    bool has_suggestion = false;
    for (const auto& s : r.suggestions) {
        if (s.find("场景切换") != std::string::npos) has_suggestion = true;
    }
    EXPECT_TRUE(has_suggestion);
}

TEST(BitrateGopAnalyzerTest, ApplySceneChangesIsIdempotent) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    GenOptions g;
    g.frames = 500;
    g.gop = 250;
    Generate(analyzer, g);
    const BitrateGopAnalysis& r = analyzer.Finish();

    std::vector<SceneChangeResult> cuts;
    SceneChangeResult cut;
    cut.frame_index = 120;
    cut.timestamp = 4.8;
    cut.score = 0.8;
    cuts.push_back(cut);

    BitrateGopAnalysis copy = r;
    for (int i = 0; i < 3; ++i) {
        BitrateGopAnalyzer::ApplySceneChanges(copy, cuts, BitrateGopOptions{});
    }
    EXPECT_EQ(copy.CountAnomalies(BitrateAnomalyType::SceneChangeWithoutKeyframe), 1);
}

// ---- 其他行为 ----
TEST(BitrateGopAnalyzerTest, PacketOnlyInputStillCountsIFrames) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    GenOptions g;
    g.frames = 200;
    g.gop = 25;
    g.emit_frame_types = false;
    Generate(analyzer, g);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_FALSE(r.frame_types_known);
    EXPECT_EQ(r.i_frame_count, 8);
    EXPECT_EQ(r.unknown_frame_count, 192);
    EXPECT_EQ(r.gops.size(), 8u);
}

TEST(BitrateGopAnalyzerTest, SingleKeyframeIsFlaggedAsSparse) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    GenOptions g;
    g.frames = 1800;      // 25fps -> 72 秒
    g.gop = 100000;       // 仅第 0 帧为关键帧
    Generate(analyzer, g);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_EQ(r.CountAnomalies(BitrateAnomalyType::SparseKeyframes), 1);
    EXPECT_EQ(r.long_gop_count, 1);
}

TEST(BitrateGopAnalyzerTest, EmptyInputIsSafe) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    const BitrateGopAnalysis& r = analyzer.Finish();
    EXPECT_TRUE(r.gops.empty());
    EXPECT_TRUE(r.anomalies.empty());
    EXPECT_EQ(r.avg_bitrate_kbps, 0.0);
    EXPECT_FALSE(r.suggestions.empty());
}

TEST(BitrateGopAnalyzerTest, FinishIsIdempotent) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    Generate(analyzer, GenOptions{/*.frames=*/100, /*.gop=*/25});
    const size_t n1 = analyzer.Finish().gops.size();
    const size_t n2 = analyzer.Finish().gops.size();
    EXPECT_EQ(n1, n2);
    EXPECT_EQ(n1, 4u);
}

// ---- 解码延迟：字节与帧的归属 ----
//
// 缺陷(已修): 解码有延迟时，包先入账、帧晚几包才出来。老实现把"本次送入包的字节"
// 分给"本次收到的帧"，于是：
//   * GOP 字节之和远小于总字节（早先那些包的字节永远进不了 GOP）；
//   * I 帧大小归属到错误的包（拿最后一个包的大小当 I 帧大小）；
//   * EOF flush 出来的帧一律记 0 字节。
// 修法: 包字节进"待认领池"，帧产出时从池里认领 —— 两套账分开维护、总量守恒。
int64_t SumGopBytes(const BitrateGopAnalysis& r) {
    int64_t total = 0;
    for (const auto& gop : r.gops) total += gop.byte_count;
    return total;
}

TEST(BitrateGopAnalyzerTest, DelayedDecoderFramesConserveBytes) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    // 解码器压着 5 个包才吐帧：每包 100 字节、间隔 0.1 秒
    for (int i = 0; i < 5; ++i) {
        analyzer.OnPacketBytes(0, static_cast<double>(i) * 0.1, 100,
                               /*awaiting_frame=*/true);
    }
    // 5 帧一起出来（第 0 帧是 IDR），时间戳回到各自的位置
    std::vector<BitrateGopAnalyzer::FrameEvent> frames;
    for (int i = 0; i < 5; ++i) {
        frames.push_back({static_cast<double>(i) * 0.1,
                          i == 0 ? model::FrameType::I : model::FrameType::P, i == 0});
    }
    analyzer.OnFramesClaimPending(0, frames);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_EQ(r.total_bytes, 500);   // 包级账：一个字节都不少
    EXPECT_EQ(r.total_frames, 5);
    EXPECT_EQ(r.frame_bytes, 500);   // 帧级账：全部字节都归到了帧上
    EXPECT_EQ(r.unassociated_bytes, 0);
    // 关键断言：GOP 字节之和 == 总字节（老实现只有 100）
    EXPECT_EQ(SumGopBytes(r), r.total_bytes);
    // I 帧拿到的是它自己那一份（100），不是"最后送入的那个包"
    EXPECT_EQ(r.i_frame_count, 1);
    EXPECT_EQ(r.i_frame_bytes_, 100);
    // 5 帧分摊同一池 = 估算，必须明确标出来
    EXPECT_TRUE(r.frame_bytes_estimated);
}

TEST(BitrateGopAnalyzerTest, ImmediateDecoderFramesAreExact) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    // 一包一帧、没有延迟：每帧独占自己的包字节 —— 这是精确值，不该标估算
    for (int i = 0; i < 5; ++i) {
        analyzer.OnPacketBytes(0, static_cast<double>(i) * 0.1, 100,
                               /*awaiting_frame=*/true);
        std::vector<BitrateGopAnalyzer::FrameEvent> one{
            {static_cast<double>(i) * 0.1,
             i == 0 ? model::FrameType::I : model::FrameType::P, i == 0}};
        analyzer.OnFramesClaimPending(0, one);
    }
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_EQ(r.total_bytes, 500);
    EXPECT_EQ(r.frame_bytes, 500);
    EXPECT_EQ(r.unassociated_bytes, 0);
    EXPECT_EQ(SumGopBytes(r), r.total_bytes);
    EXPECT_FALSE(r.frame_bytes_estimated);
}

// 池子已空（字节根本没入账 / 已被更早的帧认领干净）时还得记帧，
// 但必须如实标注"没法关联"。
TEST(BitrateGopAnalyzerTest, ClaimingFromEmptyPoolIsMarkedEstimated) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    std::vector<BitrateGopAnalyzer::FrameEvent> one{{0.0, model::FrameType::I, true}};
    analyzer.OnFramesClaimPending(0, one);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_EQ(r.total_frames, 1);
    EXPECT_EQ(r.frame_bytes, 0);
    EXPECT_EQ(r.unassociated_bytes, 0);
    EXPECT_TRUE(r.frame_bytes_estimated);
}

// 字节入账后始终没有帧来认领（解码器丢帧）: 记进 unassociated，不凭空造帧。
TEST(BitrateGopAnalyzerTest, BytesWithoutFramesStayUnassociated) {
    BitrateGopAnalyzer analyzer;
    analyzer.Reset();
    analyzer.OnPacketBytes(0, 0.0, 300, /*awaiting_frame=*/true);
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_EQ(r.total_bytes, 300);
    EXPECT_EQ(r.total_frames, 0);
    EXPECT_EQ(r.unassociated_bytes, 300);
    // 账务闭合: frame_bytes + unassociated_bytes == total_bytes
    EXPECT_EQ(r.frame_bytes + r.unassociated_bytes, r.total_bytes);
}

// ---- 降采样不得改变统计结果 ----
//
// 缺陷(已修): 超过 max_samples 后直接丢弃奇数下标样本，后续仍用幸存样本求字节前缀和、
// 用幸存样本数估算帧间隔 —— 同一组 20 帧输入，覆盖全输入的窗口字节从 2000 掉到 200、
// 估计时长从 2 秒涨到 3.6 秒。
// 修法: 相邻样本**合并成时间桶**（字节相加），时长与帧间隔单独维护。
TEST(BitrateGopAnalyzerTest, DownsamplingConservesBytesAndDuration) {
    auto feed = [](BitrateGopAnalyzer& analyzer) {
        for (int i = 0; i < 20; ++i) {
            analyzer.OnFrame(0, static_cast<double>(i) * 0.1, 100,
                             i == 0 ? model::FrameType::I : model::FrameType::P, i == 0);
        }
    };

    BitrateGopOptions unlimited;
    unlimited.max_samples = 0;
    BitrateGopAnalyzer full;
    full.Reset(unlimited);
    feed(full);
    const BitrateGopAnalysis& unrestricted = full.Finish();

    BitrateGopOptions limited_opts;
    limited_opts.max_samples = 4;
    BitrateGopAnalyzer limited;
    limited.Reset(limited_opts);
    feed(limited);
    const BitrateGopAnalysis& downsampled = limited.Finish();

    EXPECT_FALSE(unrestricted.downsampled);
    EXPECT_TRUE(downsampled.downsampled);

    // 字节总量：降采样前后必须一致（老实现会掉到 200）
    EXPECT_EQ(unrestricted.total_bytes, 2000);
    EXPECT_EQ(downsampled.total_bytes, 2000);

    // 估计时长：降采样前后必须一致（老实现会涨到 3.6 秒）
    EXPECT_NEAR(unrestricted.duration_seconds, 2.0, 0.05);
    EXPECT_NEAR(downsampled.duration_seconds, 2.0, 0.05);

    // 覆盖全部输入的窗口字节数：5 秒窗口能罩住 0~1.9 秒的全部 20 帧
    const model::MetricSeries* w_full = full.WindowCurve(5.0);
    const model::MetricSeries* w_limited = limited.WindowCurve(5.0);
    ASSERT_TRUE(w_full != nullptr);
    ASSERT_TRUE(w_limited != nullptr);
    const double expected_kbps = 2000.0 * 8.0 / 1000.0 / 5.0;  // 3.2 kbps
    EXPECT_NEAR(w_full->Max(), expected_kbps, 0.01);
    EXPECT_NEAR(w_limited->Max(), expected_kbps, 0.01);
}

// 合并出来的桶代表多个帧，不能被当成"单个大帧"刷出一批假异常。
TEST(BitrateGopAnalyzerTest, DownsampledBucketsAreNotReportedAsOversizedFrames) {
    BitrateGopOptions opts;
    opts.max_samples = 8;
    BitrateGopAnalyzer analyzer;
    analyzer.Reset(opts);
    // 20 帧等大（100 字节）：没有任何一帧超过均值 8 倍，不该有异常
    for (int i = 0; i < 20; ++i) {
        analyzer.OnFrame(0, static_cast<double>(i) * 0.1, 100,
                         i == 0 ? model::FrameType::I : model::FrameType::P, i == 0);
    }
    const BitrateGopAnalysis& r = analyzer.Finish();

    EXPECT_TRUE(r.downsampled);
    EXPECT_EQ(r.CountAnomalies(BitrateAnomalyType::OversizedFrame), 0);
}
