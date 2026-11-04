// 「打开媒体文件」对话框过滤器 (ui/main_window/media_file_filter.h) 的守护测试。
//
// 回归点: flac / aiff / ac3 / eac3 / dts / caf / wv / ape 这些格式早就能打开、
// 探测、解码，但过滤器只从「容器扩展名表」派生，而它们走 FFmpeg 通用路径、不在
// 那张表里 —— 用户在对话框里看不到自己的文件，只能切到「所有文件」，
// 效果等同于「不支持」。
//
// 这类漏项不会崩、不会报错，只能靠断言把它钉住。

#include <QString>
#include <QStringList>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/analysis/orchestration/FormatDetector.h"
#include "ui/main_window/media_file_filter.h"

namespace {

// 取过滤器的第 index 段（QFileDialog 的过滤器串以 ";;" 分段）。
QString FilterSection(int index) {
    const QStringList sections = videoeye::ui::BuildMediaFileFilter().split(QStringLiteral(";;"));
    return (index >= 0 && index < sections.size()) ? sections.at(index) : QString();
}

bool HasPattern(const QString& section, const QString& ext) {
    return section.contains(QStringLiteral("*.") + ext);
}

} // namespace

// 报告里点名的 8 个扩展名必须出现在「媒体文件」这一段的模式列表里。
// 判据用 "模式串整体包含" 而不是 contains(ext)：后者会被 *.mp4 里的 "mp" 之类误命中。
TEST(MediaFileFilter, includesReportedAudioExtensions) {
    const QString media = FilterSection(0);
    ASSERT_FALSE(media.isEmpty()) << "过滤器第 0 段本该是「媒体文件」";
    for (const char* ext : {"flac", "aif", "aiff", "ac3", "eac3", "dts", "caf", "wv", "ape"}) {
        EXPECT_TRUE(HasPattern(media, QString::fromLatin1(ext))) << "媒体文件过滤器缺少 *." << ext;
    }
}

TEST(MediaFileFilter, includesRealMediaExtensions) {
    const QString media = FilterSection(0);
    for (const char* ext : {"rm", "rmvb"}) {
        const QString pattern = QStringLiteral("*.") + QString::fromLatin1(ext);
        EXPECT_TRUE(media.contains(pattern + QLatin1Char(' ')) || media.endsWith(pattern + QLatin1Char(')')))
            << "媒体文件过滤器缺少完整模式 " << pattern.toStdString();
    }
}

// 过滤器必须**真的从探测器派生**，而不是又手写一份清单。
//
// ⚠️ 这条用例的判别力有明确边界：它的期望值取自同一张扩展名表，所以"表里漏了某个
// 格式"它抓不住 —— 那件事由 FormatDetectorExtensionsTest.coversEveryDocumentedFormat
// 的必列清单负责。它抓的是另一类回归：有人把过滤器改回硬编码（本文件测的正是这个
// 历史形态），或派生时漏掉了某一张表。
TEST(MediaFileFilter, mediaSectionDerivesFromDetectorTables) {
    const QString media = FilterSection(0);
    const std::vector<std::string>& manifests = videoeye::FormatDetector::ManifestExtensions();
    int covered = 0;
    for (const std::string& ext : videoeye::FormatDetector::SupportedExtensions()) {
        if (std::find(manifests.begin(), manifests.end(), ext) != manifests.end()) {
            continue;
        }
        EXPECT_TRUE(HasPattern(media, QString::fromStdString(ext))) << "媒体文件过滤器缺少 *." << ext;
        ++covered;
    }
    EXPECT_GT(covered, 20) << "容器扩展名表几乎没剩下东西, 断言等于空转";
}

// 清单文本 (m3u8/mpd) 不能混进「媒体文件」—— 它是播放列表，不是素材。
// 反过来「流媒体清单」段必须恰好收着它们。
TEST(MediaFileFilter, manifestsStayInTheirOwnSection) {
    const QString media = FilterSection(0);
    const QString manifest = FilterSection(1);
    for (const std::string& ext : videoeye::FormatDetector::ManifestExtensions()) {
        const QString pattern = QStringLiteral("*.") + QString::fromStdString(ext);
        EXPECT_FALSE(media.contains(pattern)) << "清单扩展名混进了媒体文件过滤: " << ext;
        EXPECT_TRUE(manifest.contains(pattern)) << "流媒体清单过滤器缺少 " << ext;
    }
}

// 原始像素序列由播放器按宽高直接打开，只有 UI 知道，必须自己列进来。
TEST(MediaFileFilter, keepsRawPixelPatterns) {
    const QString media = FilterSection(0);
    for (const char* ext : {"pcm", "yuv", "nv12", "rgb", "bgr", "yuy2", "raw"}) {
        EXPECT_TRUE(HasPattern(media, QString::fromLatin1(ext))) << "媒体文件过滤器缺少 *." << ext;
    }
}

// 段数与「所有文件」兜底：少一段 Qt 会把整串当单个名字显示。
TEST(MediaFileFilter, hasThreeSectionsEndingWithAllFiles) {
    const QStringList sections = videoeye::ui::BuildMediaFileFilter().split(QStringLiteral(";;"));
    ASSERT_EQ(3, sections.size());
    EXPECT_TRUE(sections.at(2).startsWith(QStringLiteral("所有文件"))) << sections.at(2).toStdString();
    EXPECT_TRUE(sections.at(0).startsWith(QStringLiteral("媒体文件")));
    EXPECT_TRUE(sections.at(1).startsWith(QStringLiteral("流媒体清单")));
}
