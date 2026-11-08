// 解析资源稳定性回归（批量吞吐场景）
//
// 单次"能打开"不代表连续跑几千个文件不会耗光句柄或把内存撑爆。真实使用里
// 打开/关闭发生在同一个进程内成百上千次，任何一次 open 漏一个 FILE* / AVIO /
// AVFormatContext，单测和手工点开一个文件都发现不了，只有批量跑起来才会在
// 某个文件数上突然打不开 —— 典型的"用了半天突然报错"。
//
// 这一组用例把"打开-关闭是否配平"钉在进程级指标上：
//   * 句柄数（Windows GetProcessHandleCount / Linux /proc/self/fd）增量必须为 0±4；
//   * 当前 RSS 增量必须落在阈值内（FFmpeg 内部缓存有波动，不能按 0 断言）。
// 先做 warm-up 再测基线，避免把首次加载的动态库 / 线程池算成泄漏。

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/analysis/orchestration/MediaInfoAnalyzer.h"
#include "infrastructure/logging/PerfMetrics.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr int kOpenIterations = 200;
constexpr int kStructureIterations = 30;

// 句柄增量上限。允许 4 个的抖动：Windows CRT / FFmpeg 在首次打开某些编码
// 时会惰性加载并缓存少量模块句柄，之后不再增长。持续泄漏每次至少 +1。
constexpr long long kMaxHandleGrowth = 4;
// RSS 增量上限（MB）。AVFormatContext 与解码器缓存的释放时机不精确，
// 但一个稳定实现跑 200 次不该留下几十 MB。
constexpr double kMaxRssGrowthMb = 40.0;

long long CurrentHandleCount() {
#ifdef _WIN32
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count))
        return -1;
    return static_cast<long long>(count);
#else
    std::error_code ec;
    long long count = 0;
    std::filesystem::directory_iterator it("/proc/self/fd", ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        ++count;
        it.increment(ec);
    }
    return ec ? -1 : count;
#endif
}

std::string CorpusPath(const char* relative) {
    return std::string(VIDEOEYE_CORPUS_DIR) + "/" + relative;
}

TEST(AnalysisResourceStability, MediaInfoOpenCloseLoopDoesNotLeak) {
    const std::string path = CorpusPath("container/real_h264_aac_faststart.mp4");

    // warm-up：把惰性加载（DLL / 线程池 / 编码器表）提前做掉，避免算进增量。
    for (int i = 0; i < 10; ++i) {
        videoeye::MediaInfoAnalyzer analyzer;
        ASSERT_TRUE(analyzer.Open(path)) << analyzer.GetLastError();
        analyzer.Close();
    }

    const long long handles_before = CurrentHandleCount();
    ASSERT_GE(handles_before, 0) << "无法采集句柄数";
    const double rss_before = videoeye::PerfMetrics::CurrentRssMb();
    ASSERT_GT(rss_before, 0.0) << "无法采集 RSS";

    for (int i = 0; i < kOpenIterations; ++i) {
        videoeye::MediaInfoAnalyzer analyzer;
        ASSERT_TRUE(analyzer.Open(path)) << "第 " << i << " 次打开失败: " << analyzer.GetLastError();
        EXPECT_FALSE(analyzer.GetCompleteInfo().empty());
        analyzer.Close();
    }

    const long long handles_after = CurrentHandleCount();
    const double rss_after = videoeye::PerfMetrics::CurrentRssMb();

    EXPECT_LE(handles_after - handles_before, kMaxHandleGrowth)
        << kOpenIterations << " 次 open/close 后句柄从 " << handles_before << " 涨到 " << handles_after;
    EXPECT_LE(rss_after - rss_before, kMaxRssGrowthMb)
        << kOpenIterations << " 次 open/close 后 RSS 从 " << rss_before << " MB 涨到 " << rss_after << " MB";
}

TEST(AnalysisResourceStability, StructureAnalyzeLoopDoesNotLeak) {
    const std::string path = CorpusPath("container/real_h264_aac_faststart.mp4");

    for (int i = 0; i < 5; ++i) {
        videoeye::ContainerStructureAnalyzer analyzer;
        videoeye::model::ContainerStructureResult result;
        ASSERT_TRUE(analyzer.Analyze(path, result)) << result.error_message;
    }

    const long long handles_before = CurrentHandleCount();
    ASSERT_GE(handles_before, 0) << "无法采集句柄数";
    const double rss_before = videoeye::PerfMetrics::CurrentRssMb();
    ASSERT_GT(rss_before, 0.0) << "无法采集 RSS";

    for (int i = 0; i < kStructureIterations; ++i) {
        videoeye::ContainerStructureAnalyzer analyzer;
        videoeye::model::ContainerStructureResult result;
        ASSERT_TRUE(analyzer.Analyze(path, result)) << "第 " << i << " 次解析失败: " << result.error_message;
        EXPECT_TRUE(result.valid);
        analyzer.Reset();
    }

    const long long handles_after = CurrentHandleCount();
    const double rss_after = videoeye::PerfMetrics::CurrentRssMb();

    EXPECT_LE(handles_after - handles_before, kMaxHandleGrowth)
        << kStructureIterations << " 次结构解析后句柄从 " << handles_before << " 涨到 " << handles_after;
    EXPECT_LE(rss_after - rss_before, kMaxRssGrowthMb)
        << kStructureIterations << " 次结构解析后 RSS 从 " << rss_before << " MB 涨到 " << rss_after << " MB";
}

TEST(AnalysisResourceStability, MixedAudioAndVideoLoopDoesNotLeak) {
    // 交替打开音频与视频：FFmpeg 的 demuxer/decoder 注册表按格式惰性初始化，
    // 只测一种格式会把"每换一种格式就多一份不释放的缓存"漏掉。
    const std::string video = CorpusPath("container/real_h264_aac_faststart.mp4");
    const std::string audio = CorpusPath("audio/real_pcm.wav");

    for (int i = 0; i < 10; ++i) {
        videoeye::MediaInfoAnalyzer analyzer;
        ASSERT_TRUE(analyzer.Open(i % 2 == 0 ? video : audio)) << analyzer.GetLastError();
    }

    const long long handles_before = CurrentHandleCount();
    ASSERT_GE(handles_before, 0) << "无法采集句柄数";

    for (int i = 0; i < kOpenIterations; ++i) {
        videoeye::MediaInfoAnalyzer analyzer;
        ASSERT_TRUE(analyzer.Open(i % 2 == 0 ? audio : video))
            << "第 " << i << " 次打开失败: " << analyzer.GetLastError();
    }

    const long long handles_after = CurrentHandleCount();
    EXPECT_LE(handles_after - handles_before, kMaxHandleGrowth)
        << "音视频交替 " << kOpenIterations << " 次后句柄从 " << handles_before << " 涨到 " << handles_after;
}

} // namespace
