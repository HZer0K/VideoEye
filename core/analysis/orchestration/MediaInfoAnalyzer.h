#pragma once

#include <memory>
#include <QString>

// 媒体信息解析封装（FFmpeg 实现）。
//
// 历史: 这一层原本由 MediaInfoLib/ZenLib 提供完整 mediainfo 文本。
// 为了把硬依赖收敛到「Qt Widgets + FFmpeg」，这里改为直接用 libavformat 读取
// 容器/流/编码/色彩/metadata 信息，输出同样结构的纯文本报告。
//
// 与 MediaInfoLib 的差异（有意为之）:
//   * 不做容器的深度逐字节解析（那是「文件结构」页的职责）；
//   * 不解析编解码器内部比特流（那是「码流分析」页的职责）；
//   * 换来的好处是零额外依赖、解析速度与 avformat 探测一致。
namespace videoeye {
namespace analyzer {

class MediaInfoAnalyzer {
public:
    MediaInfoAnalyzer();
    ~MediaInfoAnalyzer();

    MediaInfoAnalyzer(const MediaInfoAnalyzer&) = delete;
    MediaInfoAnalyzer& operator=(const MediaInfoAnalyzer&) = delete;

    /// 打开媒体文件并解析（阻塞，建议在后台线程调用）
    bool Open(const QString& filePath);

    /// 裸 PCM 打开提示（*.pcm 无法被 avformat 自动探测，需调用方给出参数）
    void SetRawPcmHints(const QString& demuxer, int sample_rate, int channels);

    /// 获取完整媒体信息（类 mediainfo CLI 的纯文本输出）
    QString GetCompleteInfo() const;

    /// 关闭当前文件
    void Close();

    /// 是否已成功打开文件
    bool IsReady() const;

    /// 上一次失败的原因（Open 返回 false 时有意义）
    QString GetLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace analyzer
} // namespace videoeye
