// 视觉缺陷检测（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边）
//
// 说明: VisualDefectAnalyzer 刻意不依赖 FFmpeg —— 它只吃 model::FrameSample 里的
// 降采样像素（AVFrame -> FrameSample 的缩放在 QualityAnalyzer::BuildSample）。
// 所以这里的用例全部用合成图，不需要任何真实媒体文件，跑起来是毫秒级。

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

#include "core/analyzer/VisualDefectAnalyzer.h"

namespace {

using videoeye::analyzer::VisualDefectAnalyzer;
using videoeye::analyzer::VisualDefectOptions;
using videoeye::analyzer::VisualSamplingPreset;
using videoeye::model::FrameQualityMetric;
using videoeye::model::FrameSample;
using videoeye::model::VisualDefectType;
using videoeye::model::VisualDefectSeverity;

constexpr int kW = 64;
constexpr int kH = 36;
constexpr int kRW = 32;
constexpr int kRH = 18;

FrameSample MakeSample(int index, double ts, const std::vector<uint8_t>& gray,
                       const std::vector<uint8_t>* rgb = nullptr, bool audio_silent = false) {
    FrameSample s;
    s.frame_index = index;
    s.timestamp_seconds = ts;
    s.width = kW;
    s.height = kH;
    s.gray = gray;
    if (rgb) {
        s.rgb_width = kRW;
        s.rgb_height = kRH;
        s.rgb = *rgb;
    }
    s.audio_silent = audio_silent;
    return s;
}

std::vector<uint8_t> Solid(uint8_t v) {
    return std::vector<uint8_t>(static_cast<size_t>(kW) * kH, v);
}

// 高频棋盘 + 条纹噪声，模拟有细节的画面
std::vector<uint8_t> Sharp(int phase = 0) {
    std::vector<uint8_t> g(static_cast<size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const int checker = ((x / 2 + y / 2 + phase) % 2) ? 210 : 40;
            const int noise = ((x * 7 + y * 13 + phase * 3) % 17) * 3;
            int v = checker + noise - 24;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            g[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(v);
        }
    }
    return g;
}

std::vector<uint8_t> Blur(const std::vector<uint8_t>& src, int radius, int iterations) {
    std::vector<uint8_t> cur = src;
    for (int it = 0; it < iterations; ++it) {
        std::vector<uint8_t> dst(cur.size());
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                long sum = 0;
                int count = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int yy = y + dy;
                        const int xx = x + dx;
                        if (yy < 0 || yy >= kH || xx < 0 || xx >= kW) continue;
                        sum += cur[static_cast<size_t>(yy) * kW + xx];
                        ++count;
                    }
                }
                dst[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(sum / count);
            }
        }
        cur = dst;
    }
    return cur;
}

// 偶数行 = 图案，奇数行 = 同一图案平移（模拟隔行两场的时间差 -> 梳齿）
std::vector<uint8_t> Combing(bool combed, int shift = 2) {
    std::vector<uint8_t> g(static_cast<size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        const int s = (combed && (y % 2 == 1)) ? shift : 0;
        for (int x = 0; x < kW; ++x) {
            const int v = (((x + s) / 4) % 2) ? 200 : 60;
            g[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(v);
        }
    }
    return g;
}

std::vector<uint8_t> Blocks(bool blocky) {
    std::vector<uint8_t> g(static_cast<size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (blocky) {
                const int v = 30 + (((x / 8) * 37 + (y / 8) * 91) % 200);
                g[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(v);
            } else {
                const int v = 128 + ((x * 3 + y * 2) % 40) - 20;
                g[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(v);
            }
        }
    }
    return g;
}

// 上下各 bar_rows 行纯黑，中间是画面
std::vector<uint8_t> Letterbox(int bar_rows) {
    std::vector<uint8_t> g(static_cast<size_t>(kW) * kH, 0);
    for (int y = bar_rows; y < kH - bar_rows; ++y) {
        for (int x = 0; x < kW; ++x) {
            const int v = 90 + ((x * 5 + y * 3) % 80);
            g[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(v);
        }
    }
    return g;
}

std::vector<uint8_t> Rgb(uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> out(static_cast<size_t>(kRW) * kRH * 3);
    for (size_t i = 0; i < static_cast<size_t>(kRW) * kRH; ++i) {
        out[i * 3 + 0] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
    return out;
}

// 按固定帧率喂一串帧
void FeedSequence(VisualDefectAnalyzer& analyzer, double fps,
                  const std::vector<std::pair<double, std::vector<uint8_t>>>& frames,
                  bool audio_silent = false) {
    int index = 0;
    for (const auto& f : frames) {
        analyzer.Feed(MakeSample(index++, f.first, f.second, nullptr, audio_silent));
    }
    (void)fps;
}

int CountType(const std::vector<videoeye::model::VisualDefect>& defects, VisualDefectType type) {
    int count = 0;
    for (const auto& d : defects) {
        if (d.type == type) ++count;
    }
    return count;
}

}  // namespace

// ---------- 单帧指标 ----------

TEST(VisualDefectMetrics, InvalidSampleProducesInvalidMetric) {
    VisualDefectOptions opt;
    FrameSample empty;   // 宽高为 0
    const auto m = VisualDefectAnalyzer::ComputeFrameMetrics(empty, nullptr, 0.0, opt);
    EXPECT_FALSE(m.valid);
    EXPECT_FALSE(m.error_message.empty());
}

TEST(VisualDefectMetrics, LumaAndBlackRatio) {
    VisualDefectOptions opt;
    const auto black = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(0, 0.0, Solid(0)), nullptr, 0.0, opt);
    const auto white = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(1, 0.5, Solid(250)), nullptr, 0.0, opt);
    EXPECT_TRUE(black.valid);
    EXPECT_NEAR(black.luma_mean, 0.0, 1e-9);
    EXPECT_NEAR(black.black_ratio, 1.0, 1e-9);
    EXPECT_NEAR(white.clip_high_ratio, 1.0, 1e-9);
    EXPECT_NEAR(white.highlight_ratio, 1.0, 1e-9);
}

TEST(VisualDefectMetrics, BlurredFrameHasLowerSharpnessThanSharpFrame) {
    VisualDefectOptions opt;
    const auto sharp = Sharp(3);
    const auto blurred = Blur(sharp, 2, 3);
    const auto m_sharp = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(0, 0.0, sharp), nullptr, 0.0, opt);
    const auto m_blur = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(1, 0.5, blurred), nullptr, 0.0, opt);
    EXPECT_LT(m_blur.blur_score, m_sharp.blur_score);
    // 验收口径: 模糊样本必须低于默认阈值，清晰样本必须高于它
    EXPECT_LT(m_blur.blur_score, opt.blur_threshold);
    EXPECT_GT(m_sharp.blur_score, opt.blur_threshold);
}

TEST(VisualDefectMetrics, FrameDiffDetectsIdenticalFrames) {
    VisualDefectOptions opt;
    const auto a = Sharp(1);
    const auto b = Sharp(2);
    const auto prev = MakeSample(0, 0.0, a);
    const auto first = VisualDefectAnalyzer::ComputeFrameMetrics(prev, nullptr, 0.0, opt);
    const auto same = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(1, 0.5, a), &prev, first.luma_mean, opt);
    const auto diff = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(2, 1.0, b), &prev, first.luma_mean, opt);
    // 首帧没有上一帧可比，frame_diff 应为 NaN 而不是 0
    EXPECT_TRUE(std::isnan(first.frame_diff));
    EXPECT_NEAR(same.frame_diff, 0.0, 1e-12);
    EXPECT_GT(diff.frame_diff, opt.freeze_diff);
}

TEST(VisualDefectMetrics, CombingScoreSeparatesInterlacedAndProgressive) {
    VisualDefectOptions opt;
    const auto combed = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(0, 0.0, Combing(true)), nullptr, 0.0, opt);
    const auto progressive = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(1, 0.5, Combing(false)), nullptr, 0.0, opt);
    EXPECT_GT(combed.combing_score, opt.combing_score);
    EXPECT_LT(progressive.combing_score, opt.combing_score);
}

TEST(VisualDefectMetrics, BlockinessScoreSeparatesMosaicAndNatural) {
    VisualDefectOptions opt;
    const auto mosaic = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(0, 0.0, Blocks(true)), nullptr, 0.0, opt);
    const auto natural = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(1, 0.5, Blocks(false)), nullptr, 0.0, opt);
    EXPECT_GT(mosaic.blockiness_score, opt.blockiness_score);
    EXPECT_LT(natural.blockiness_score, opt.blockiness_score);
}

TEST(VisualDefectMetrics, ColorCastNeedsRgbSample) {
    VisualDefectOptions opt;
    const auto gray = Solid(128);
    const auto neutral = Rgb(128, 128, 128);
    const auto reddish = Rgb(210, 100, 100);
    const auto no_rgb = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(0, 0.0, gray), nullptr, 0.0, opt);
    const auto ok = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(1, 0.5, gray, &neutral), nullptr, 0.0, opt);
    const auto cast = VisualDefectAnalyzer::ComputeFrameMetrics(
        MakeSample(2, 1.0, gray, &reddish), nullptr, 0.0, opt);
    EXPECT_TRUE(std::isnan(no_rgb.color_cast_score));   // 没采集 RGB 就不该瞎猜
    EXPECT_LT(ok.color_cast_score, opt.color_cast_score);
    EXPECT_GT(cast.color_cast_score, opt.color_cast_score);
    EXPECT_GT(cast.color_cast_rb, 0.0);   // 偏红
}

// ---------- 缺陷段落（验收用例）----------

TEST(VisualDefectAnalyzer, AllBlackClipMustBeDetected) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    const auto black = Solid(0);
    const auto content = Sharp(0);
    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 1.0; t += 0.25) frames.emplace_back(t, content);
    for (double t = 1.0; t < 4.0; t += 0.25) frames.emplace_back(t, black);
    for (double t = 4.0; t < 5.0; t += 0.25) frames.emplace_back(t, content);
    FeedSequence(a, 4.0, frames);
    a.Flush(5.0);

    ASSERT_EQ(CountType(a.defects(), VisualDefectType::BlackFrame), 1);
    const auto& d = a.defects().front();
    EXPECT_GE(d.start_seconds, 0.75);
    EXPECT_LE(d.start_seconds, 1.25);
    EXPECT_GE(d.DurationSeconds(), 2.5);
    EXPECT_NEAR(d.score, 1.0, 1e-6);   // 黑像素占比
}

TEST(VisualDefectAnalyzer, FiveSecondFreezeMustBeDetected) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    const auto frozen = Sharp(99);
    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 1.0; t += 0.25) frames.emplace_back(t, Sharp(static_cast<int>(t * 4)));
    for (double t = 1.0; t < 6.0; t += 0.25) frames.emplace_back(t, frozen);
    for (double t = 6.0; t < 7.0; t += 0.25) frames.emplace_back(t, Sharp(static_cast<int>(t * 4)));
    FeedSequence(a, 4.0, frames);   // 音频非静音
    a.Flush(7.0);

    ASSERT_EQ(CountType(a.defects(), VisualDefectType::FreezeFrame), 1);
    const auto& d = a.defects().front();
    EXPECT_GE(d.DurationSeconds(), 4.0);
    EXPECT_EQ(d.severity, VisualDefectSeverity::Error);   // 冻结 3s 以上升级为错误
}

TEST(VisualDefectAnalyzer, StillFrameWithSilentAudioIsNotFreeze) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    const auto frozen = Sharp(99);
    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 1.0; t += 0.25) frames.emplace_back(t, Sharp(0));
    for (double t = 1.0; t < 6.0; t += 0.25) frames.emplace_back(t, frozen);
    FeedSequence(a, 4.0, frames, /*audio_silent=*/true);
    a.Flush(6.0);

    EXPECT_EQ(CountType(a.defects(), VisualDefectType::FreezeFrame), 0);
}

TEST(VisualDefectAnalyzer, BlurSegmentIsReportedForBlurredClip) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    // 3 次 3x3 均值模糊: 视觉上已经糊得没法看，但仍保留一点大尺度结构
    // （完全抹平的极端模糊会被"画面有没有内容"这道闸拦掉，那是黑场/纯色该报的）
    const auto blurred = Blur(Sharp(5), 1, 3);
    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 3.0; t += 0.25) frames.emplace_back(t, blurred);
    FeedSequence(a, 4.0, frames);
    a.Flush(3.0);

    EXPECT_GE(CountType(a.defects(), VisualDefectType::Blur), 1);
}

TEST(VisualDefectAnalyzer, FlatFrameIsNotReportedAsBlur) {
    // 纯色帧的拉普拉斯方差天然是 0，那是"没细节"不是"糊了"，应该只报过曝
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 2.0; t += 0.25) frames.emplace_back(t, Solid(253));
    FeedSequence(a, 4.0, frames);
    a.Flush(2.0);

    EXPECT_EQ(CountType(a.defects(), VisualDefectType::Blur), 0);
    EXPECT_EQ(CountType(a.defects(), VisualDefectType::OverExposure), 1);
}

TEST(VisualDefectAnalyzer, NearBlackFrameIsBlackNotUnderExposure) {
    // 15/255 已经是黑场了，不该同时报一条"欠曝"（同一件事不重复计数）
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 2.0; t += 0.25) frames.emplace_back(t, Solid(15));
    FeedSequence(a, 4.0, frames);
    a.Flush(2.0);

    EXPECT_EQ(CountType(a.defects(), VisualDefectType::BlackFrame), 1);
    EXPECT_EQ(CountType(a.defects(), VisualDefectType::UnderExposure), 0);
}

TEST(VisualDefectAnalyzer, FlickerIsDetectedOnAlternatingLuma) {
    VisualDefectOptions opt;
    opt.sample_fps = 10.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    const auto bright = Solid(230);
    const auto dark = Solid(30);
    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (int i = 0; i < 20; ++i) frames.emplace_back(i * 0.1, (i % 2 == 0) ? bright : dark);
    FeedSequence(a, 10.0, frames);
    a.Flush(2.0);

    EXPECT_GE(CountType(a.defects(), VisualDefectType::Flicker), 1);
}

TEST(VisualDefectAnalyzer, LetterboxReportsActivePictureArea) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    // 36 行里上下各 8 行黑边（合计 44%），中间 20 行是画面
    const auto lb = Letterbox(8);
    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 3.0; t += 0.25) frames.emplace_back(t, lb);
    FeedSequence(a, 4.0, frames);
    a.Flush(3.0);

    const auto area = a.EffectiveArea();
    ASSERT_TRUE(area.valid);
    EXPECT_EQ(area.x, 0);
    EXPECT_EQ(area.y, 8);
    EXPECT_EQ(area.width, kW);
    EXPECT_EQ(area.height, kH - 16);
    EXPECT_LT(area.active_ratio, 1.0);
    EXPECT_TRUE(area.letterbox);
    EXPECT_FALSE(area.pillarbox);

    ASSERT_EQ(CountType(a.defects(), VisualDefectType::Letterbox), 1);
    for (const auto& d : a.defects()) {
        if (d.type != VisualDefectType::Letterbox) continue;
        EXPECT_EQ(d.severity, VisualDefectSeverity::Info);   // 有黑边不是错，只是要告诉用户有效区在哪
        EXPECT_GT(d.score, opt.border_bar_ratio);
        EXPECT_FALSE(d.evidence.valid());   // 没采集 RGB 就没有证据图
    }
}

TEST(VisualDefectAnalyzer, NoBarsOnFullFrameContent) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);

    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 2.0; t += 0.25) frames.emplace_back(t, Sharp(static_cast<int>(t * 4)));
    FeedSequence(a, 4.0, frames);
    a.Flush(2.0);

    const auto area = a.EffectiveArea();
    ASSERT_TRUE(area.valid);
    EXPECT_EQ(area.y, 0);
    EXPECT_EQ(area.height, kH);
    EXPECT_FALSE(area.letterbox);
    EXPECT_EQ(CountType(a.defects(), VisualDefectType::Letterbox), 0);
}

TEST(VisualDefectAnalyzer, UnderAndOverExposure) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;

    VisualDefectAnalyzer dark_a;
    dark_a.Reset(opt);
    std::vector<std::pair<double, std::vector<uint8_t>>> dark_frames;
    // 30/255: 够暗（暗部占比 100%）但还没到黑场（黑像素阈值 24）
    for (double t = 0.0; t < 2.0; t += 0.25) dark_frames.emplace_back(t, Solid(30));
    FeedSequence(dark_a, 4.0, dark_frames);
    dark_a.Flush(2.0);
    EXPECT_GE(CountType(dark_a.defects(), VisualDefectType::UnderExposure), 1);

    VisualDefectAnalyzer bright_a;
    bright_a.Reset(opt);
    std::vector<std::pair<double, std::vector<uint8_t>>> bright_frames;
    for (double t = 0.0; t < 2.0; t += 0.25) bright_frames.emplace_back(t, Solid(253));
    FeedSequence(bright_a, 4.0, bright_frames);
    bright_a.Flush(2.0);
    EXPECT_GE(CountType(bright_a.defects(), VisualDefectType::OverExposure), 1);
}

TEST(VisualDefectAnalyzer, ShortSegmentsBelowMinDurationAreDropped) {
    VisualDefectOptions opt;
    opt.sample_fps = 4.0;
    opt.black_min_seconds = 2.0;   // 故意抬高门槛
    VisualDefectAnalyzer a;
    a.Reset(opt);

    std::vector<std::pair<double, std::vector<uint8_t>>> frames;
    for (double t = 0.0; t < 0.5; t += 0.25) frames.emplace_back(t, Solid(0));
    for (double t = 0.5; t < 2.0; t += 0.25) frames.emplace_back(t, Sharp(1));
    FeedSequence(a, 4.0, frames);
    a.Flush(2.0);

    EXPECT_EQ(CountType(a.defects(), VisualDefectType::BlackFrame), 0);
}

// ---------- 采样档位与异步队列 ----------

TEST(VisualDefectOptions, PresetMapsToSampleRateAndWidth) {
    VisualDefectOptions fast;
    fast.preset = VisualSamplingPreset::Fast;
    VisualDefectOptions standard;
    standard.preset = VisualSamplingPreset::Standard;
    VisualDefectOptions fine;
    fine.preset = VisualSamplingPreset::Fine;
    VisualDefectOptions offline;
    offline.preset = VisualSamplingPreset::OfflineFull;

    EXPECT_NEAR(fast.EffectiveSampleFps(), 1.0, 1e-9);
    EXPECT_NEAR(standard.EffectiveSampleFps(), 2.0, 1e-9);
    EXPECT_NEAR(fine.EffectiveSampleFps(), 5.0, 1e-9);
    EXPECT_NEAR(offline.EffectiveSampleFps(), 0.0, 1e-9);   // 0 = 每帧

    EXPECT_LT(fast.EffectiveAnalysisWidth(), fine.EffectiveAnalysisWidth());
    EXPECT_GT(fast.EffectiveEvidenceWidth(), 0);
}

TEST(VisualDefectAnalyzer, AsyncQueueDropsInsteadOfBlocking) {
    VisualDefectOptions opt;
    opt.sample_fps = 0.0;
    VisualDefectAnalyzer a;
    a.Reset(opt);
    a.StartWorker(2);   // 队列只有 2 格

    // 一口气塞 200 帧: 工作线程来不及消费，其余必须被丢弃而不是堆积
    for (int i = 0; i < 200; ++i) {
        a.Submit(MakeSample(i, i * 0.04, Sharp(i % 8)));
    }
    // 等队列排空（给工作线程一点时间，最多 2 秒）
    for (int i = 0; i < 200 && a.dropped_samples() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(a.dropped_samples(), 0);

    a.Flush(8.0);
    a.StopWorker();
    const auto metrics = a.TakePendingMetrics();
    EXPECT_FALSE(metrics.empty());
}

TEST(VisualDefectAnalyzer, FeedIsSynchronousAndAnalyzesEveryFrame) {
    VisualDefectOptions opt;
    VisualDefectAnalyzer a;
    a.Reset(opt);
    for (int i = 0; i < 50; ++i) {
        a.Feed(MakeSample(i, i * 0.04, Sharp(i % 8)));
    }
    EXPECT_EQ(a.analyzed_samples(), 50);
    EXPECT_EQ(a.dropped_samples(), 0);   // 离线路径一帧不丢
    const auto report = a.Snapshot();
    EXPECT_TRUE(report.analyzed);
    EXPECT_EQ(report.analyzed_frames, 50);
    EXPECT_EQ(report.samples.size(), 50u);
}

TEST(VisualDefectAnalyzer, ResetClearsEverything) {
    VisualDefectOptions opt;
    VisualDefectAnalyzer a;
    a.Reset(opt);
    for (int i = 0; i < 20; ++i) a.Feed(MakeSample(i, i * 0.5, Solid(0)));
    a.Flush(10.0);
    ASSERT_FALSE(a.defects().empty());

    a.Reset(opt);
    EXPECT_TRUE(a.defects().empty());
    EXPECT_TRUE(a.metrics().empty());
    EXPECT_EQ(a.analyzed_samples(), 0);
    EXPECT_FALSE(a.EffectiveArea().valid);
}
