// SceneChangeAnalyzer 的单元测试。
//
// 检测器本身没有页面/FFmpeg 依赖，却是整条链路的判定核心：
//   - 首帧必须只建基准、不报切点（否则每个文件开头都有一个幽灵切换点）；
//   - Reset 必须丢掉比较基准（定位、重新开启分析都靠它，否则落点首帧会和
//     很久以前的画面比较，实测两块无关画面 score≈0.72）；
//   - 最小间隔必须抑制同一次转场的连续多帧命中（实测广告素材在相邻两帧
//     各报一次，会让下游"切换处是否有关键帧"的统计翻倍）。
// 这些行为写错都不会崩，只会静默多报/漏报，因此在这里钉住。

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/analysis/quality/SceneChangeAnalyzer.h"

using videoeye::SceneChangeAnalyzer;

namespace {

// 单峰直方图：把全部权重压在一个 bin 上，便于构造"完全不同 / 近似"两帧。
std::vector<float> PeakHist(int bin, float weight = 100.0f) {
    std::vector<float> hist(256, 0.0f);
    hist[static_cast<std::size_t>(bin)] = weight;
    return hist;
}

// 两峰直方图：与 PeakHist(bin) 的巴氏距离约 0.106，低于默认阈值。
std::vector<float> SplitHist(int bin, float self_weight, float other_weight) {
    std::vector<float> hist(256, 0.0f);
    hist[static_cast<std::size_t>(bin)] = self_weight;
    hist[255] = other_weight;
    return hist;
}

} // namespace

// 首帧没有比较对象：只建基准，不产生切换点；第二帧才开始比较。
TEST(SceneChangeAnalyzerTests, FirstFrameBuildsBaselineWithoutDetection) {
    SceneChangeAnalyzer analyzer;
    EXPECT_FALSE(analyzer.Feed(0, 0.0, PeakHist(0)).has_value());
    EXPECT_DOUBLE_EQ(analyzer.last_score(), 0.0);

    const auto cut = analyzer.Feed(1, 0.04, PeakHist(255));
    ASSERT_TRUE(cut.has_value());
    EXPECT_EQ(cut->frame_index, 1);
    EXPECT_DOUBLE_EQ(cut->timestamp, 0.04);
    EXPECT_NEAR(cut->score, 1.0, 1e-6); // 全黑 -> 全白
}

// 变化不足阈值时不报切点，但 last_score 仍要更新（UI 要能显示当前差异）。
TEST(SceneChangeAnalyzerTests, BelowThresholdDoesNotReportButUpdatesScore) {
    SceneChangeAnalyzer analyzer;
    analyzer.Feed(0, 0.0, PeakHist(0));

    EXPECT_FALSE(analyzer.Feed(1, 0.04, SplitHist(0, 80.0f, 20.0f)).has_value());
    EXPECT_GT(analyzer.last_score(), 0.0);
    EXPECT_LT(analyzer.last_score(), 0.45);
}

// Reset 丢弃比较基准：重置后的首帧必须重新当基准，不能与重置前的画面比较。
// 定位（OnSeekDone）与重新开启分析都依赖这一条。
TEST(SceneChangeAnalyzerTests, ResetDiscardsBaselineSoNextFrameCannotBePhantomCut) {
    SceneChangeAnalyzer analyzer;
    analyzer.Feed(0, 10.0, PeakHist(0));
    ASSERT_TRUE(analyzer.Feed(1, 10.04, PeakHist(255)).has_value()); // 正常切点

    analyzer.Reset();
    // 重置后第一帧：以前一帧（全白）为基准的话这里必然报切点
    EXPECT_FALSE(analyzer.Feed(2, 30.0, PeakHist(0)).has_value());
    EXPECT_DOUBLE_EQ(analyzer.last_score(), 0.0);

    // 重置不该影响后续正常检测：下一帧仍然可以报
    EXPECT_TRUE(analyzer.Feed(3, 30.04, PeakHist(255)).has_value());
}

// 最小间隔抑制：同一次转场在相邻帧再次越过阈值时只保留第一条。
TEST(SceneChangeAnalyzerTests, MinIntervalSuppressesDuplicateDetectionWithinWindow) {
    SceneChangeAnalyzer analyzer;
    analyzer.SetMinInterval(0.1);

    analyzer.Feed(0, 20.0, PeakHist(0));
    ASSERT_TRUE(analyzer.Feed(1, 20.033, PeakHist(255)).has_value()); // 第一次命中

    // 33ms 后再次命中（典型的转场多帧命中）-> 丢弃
    EXPECT_FALSE(analyzer.Feed(2, 20.066, PeakHist(0)).has_value());
    // 越过 100ms 窗口后的真实切点 -> 保留
    EXPECT_TRUE(analyzer.Feed(3, 20.2, PeakHist(255)).has_value());
}

// 间隔设为 0 表示不做抑制：每一帧越过阈值都算切点。
TEST(SceneChangeAnalyzerTests, ZeroMinIntervalKeepsEveryDetection) {
    SceneChangeAnalyzer analyzer;
    analyzer.SetMinInterval(0.0);

    analyzer.Feed(0, 20.0, PeakHist(0));
    EXPECT_TRUE(analyzer.Feed(1, 20.033, PeakHist(255)).has_value());
    EXPECT_TRUE(analyzer.Feed(2, 20.066, PeakHist(0)).has_value());
}

// 时间戳不可用时无法判断间隔，不能把真实切点一并吞掉。
TEST(SceneChangeAnalyzerTests, NonFiniteTimestampBypassesIntervalSuppression) {
    SceneChangeAnalyzer analyzer;
    analyzer.SetMinInterval(0.1);

    analyzer.Feed(0, 20.0, PeakHist(0));
    ASSERT_TRUE(analyzer.Feed(1, 20.033, PeakHist(255)).has_value());
    EXPECT_TRUE(analyzer.Feed(2, std::nan(""), PeakHist(0)).has_value());
}
