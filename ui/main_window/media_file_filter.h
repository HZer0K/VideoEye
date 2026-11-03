#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "core/analysis/orchestration/FormatDetector.h"

namespace videoeye {
namespace ui {

/// 「打开媒体文件」对话框的过滤器字符串。
///
/// 为什么单独抽成头文件：这段逻辑以前是 MainWindow.cpp 匿名命名空间里的一个
/// 局部函数，谁也调不到，于是"过滤器漏了一批能打开的扩展名"这种回归只能靠用户
/// 投诉发现 —— 用户看到的现象不是"少个后缀"，而是"我的文件打不开"（得手动切到
/// 「所有文件」才选得中）。
///
/// 扩展名一律从 FormatDetector 派生，不在这里手写清单：
///   * 有容器解析器的格式 → MediaExtensions()（已排除 m3u8/mpd 这类清单文本）；
///   * 走 FFmpeg 通用路径的纯音频 → AudioExtensions()
///     （flac / aiff / ac3 / eac3 / dts / caf / wv / ape ...，它们不在容器表里，
///      光看 MediaExtensions() 会漏）；
///   * 原始像素序列只有 UI 知道（播放器按宽高直接打开，与容器无关）。
inline QString BuildMediaFileFilter() {
    const auto join_patterns = [](const std::vector<std::string>& extensions) {
        QStringList patterns;
        patterns.reserve(static_cast<int>(extensions.size()));
        for (const std::string& ext : extensions) {
            patterns << QStringLiteral("*.") + QString::fromStdString(ext);
        }
        return patterns.join(QLatin1Char(' '));
    };

    const QString media = join_patterns(FormatDetector::MediaExtensions());
    const QString audio = join_patterns(FormatDetector::AudioExtensions());
    const QString raw_pixels = QStringLiteral("*.pcm *.yuv *.nv12 *.rgb *.bgr *.yuy2 *.raw");
    const QString extra = audio + QLatin1Char(' ') + raw_pixels;
    const QString manifest = join_patterns(FormatDetector::ManifestExtensions());

    return QCoreApplication::translate("MainWindow", "媒体文件 (%1 %2);;流媒体清单 (%3);;所有文件 (*)")
        .arg(media, extra, manifest);
}

} // namespace ui
} // namespace videoeye
