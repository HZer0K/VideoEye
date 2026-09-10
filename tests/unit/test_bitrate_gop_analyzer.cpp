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

#include "core/analyzer/BitrateGopAnalyzer.h"

using namespace videoeye;
using namespace videoeye::analyzer;

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
