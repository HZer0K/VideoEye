// MediaInfoAnalyzer 清单入口回归（P0）
//
// 2026-11 用 4,429 个真实文件验证解析链路时发现：本地 HLS/DASH 清单交给
// avformat 的 dash/hls demuxer 后，外层 interrupt_callback 与超时都传不进
// demuxer 为子资源创建的嵌套 AVIO 上下文 —— 一个本地 mpd 超过 45 s 不返回，
// 一个 live 清单直接 0xC0000005。此时取消标志同样失效，后台线程 join 会挂住
// 关闭流程。清单分析本来归「流媒体」页（StreamingManifestScan），媒体信息入口
// 必须在分配 AVFormatContext 之前拒绝。
//
// 这一组用例钉住四条入口：
//   1. 本地 .mpd / .m3u8 —— 扩展名识别
//   2. 无扩展名的本地清单 —— 文本内容嗅探
//   3. 带查询串的网络清单 URL —— FormatDetector 的扩展名提取不剥 "?..."，单独兜底
// 再加一条反向断言：普通媒体文件仍然能正常打开，避免"为了拒绝清单把一切都拒了"。
//
// 断言里的 2 秒上限不是性能指标，而是"有没有掉进 FFmpeg 子资源 IO"的分界线：
// 走入口拒绝是微秒级，走 avformat 就是数十秒或崩溃。

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <string>

#include "core/analysis/orchestration/MediaInfoAnalyzer.h"

namespace {

constexpr long long kGuardBudgetMs = 2000;

class ManifestGuardTest : public ::testing::Test {
protected:
    std::string WriteFile(const char* name, const char* body) {
        const QString path = dir_.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return std::string();
        file.write(body);
        file.close();
        return path.toStdString();
    }

    // 清单入口的公共断言：不开、快速返回、错误说明可读。
    ::testing::AssertionResult RefusedQuickly(const std::string& path) {
        videoeye::MediaInfoAnalyzer analyzer;
        const auto start = std::chrono::steady_clock::now();
        const bool opened = analyzer.Open(path);
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (opened)
            return ::testing::AssertionFailure() << "清单不应交给 avformat 打开: " << path;
        if (elapsed >= kGuardBudgetMs)
            return ::testing::AssertionFailure()
                   << "清单入口未在 " << kGuardBudgetMs << " ms 内返回, 实际 " << elapsed << " ms";
        if (analyzer.GetLastError().find("清单") == std::string::npos)
            return ::testing::AssertionFailure() << "错误说明应指出清单入口, 实际: " << analyzer.GetLastError();
        return ::testing::AssertionSuccess();
    }

    QTemporaryDir dir_;
};

TEST_F(ManifestGuardTest, RejectsDashManifestByExtension) {
    const std::string path =
        WriteFile("manifest.mpd", "<?xml version=\"1.0\"?>\n<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"/>\n");
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(RefusedQuickly(path));
}

TEST_F(ManifestGuardTest, RejectsHlsManifestByExtension) {
    const std::string path = WriteFile("index.m3u8", "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n");
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(RefusedQuickly(path));
}

TEST_F(ManifestGuardTest, RejectsHlsManifestWithoutExtensionByContent) {
    // 没有扩展名：只能靠内容嗅探，FormatDetector 的 HLS 判定（首行 #EXTM3U）。
    const std::string path = WriteFile("playlist", "#EXTM3U\n#EXT-X-VERSION:3\n");
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(RefusedQuickly(path));
}

TEST_F(ManifestGuardTest, RejectsDashManifestWithoutExtensionByContent) {
    const std::string path =
        WriteFile("manifest", "<?xml version=\"1.0\"?>\n<MPD mediaPresentationDuration=\"PT1M\"/>\n");
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(RefusedQuickly(path));
}

TEST_F(ManifestGuardTest, RejectsManifestUrlWithQueryString) {
    // 网络清单不落盘：只验证入口在发起任何网络 IO 之前就拒绝。
    // 查询串是重点 —— FormatDetector::DetectByExtension 看到的是 "m3u8?token=x"。
    EXPECT_TRUE(RefusedQuickly("http://127.0.0.1:9/live/index.m3u8?token=abc&expires=1"));
    EXPECT_TRUE(RefusedQuickly("https://127.0.0.1:9/dash/manifest.mpd?token=abc"));
}

TEST_F(ManifestGuardTest, StillOpensRealMedia) {
    videoeye::MediaInfoAnalyzer analyzer;
    const std::string path = std::string(VIDEOEYE_CORPUS_DIR) + "/audio/real_pcm.wav";
    ASSERT_TRUE(analyzer.Open(path)) << analyzer.GetLastError();
    EXPECT_TRUE(analyzer.IsReady());
    EXPECT_FALSE(analyzer.GetCompleteInfo().empty());
}

} // namespace
