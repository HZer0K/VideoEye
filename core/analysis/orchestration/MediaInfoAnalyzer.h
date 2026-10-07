#pragma once

#include <atomic>
#include <memory>
#include <string>

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
extern "C" {
struct AVFormatContext;  // FFmpeg C 类型前置声明（仅用于静态格式化函数的签名）
}

namespace videoeye {

class MediaInfoAnalyzer {
public:
    MediaInfoAnalyzer();
    ~MediaInfoAnalyzer();

    MediaInfoAnalyzer(const MediaInfoAnalyzer&) = delete;
    MediaInfoAnalyzer& operator=(const MediaInfoAnalyzer&) = delete;

    /// 打开媒体文件并解析（阻塞，建议在后台线程调用）
    /// @param cancel 可选取消标志: 非空时交给 FFmpeg 的 AVIO 中断回调,
    ///               使关闭流程（CancelAll）能及时中止阻塞 IO。传空则不做取消。
    bool Open(const std::string& filePath, std::shared_ptr<std::atomic<bool>> cancel = {});

    /// 裸 PCM 打开提示（*.pcm 无法被 avformat 自动探测，需调用方给出参数）
    void SetRawPcmHints(const std::string& demuxer, int sample_rate, int channels);

    /// 从"已打开并完成探测"的 AVFormatContext 直接生成媒体信息文本。
    ///
    /// 已经打开过文件的调用方（OpenController::Prepare）复用它，就不必为媒体信息
    /// 再做一次 avformat_open_input + find_stream_info —— 打开一个文件只读一遍。
    /// @param format    借用，必须已完成 avformat_find_stream_info；为空返回空串
    /// @param file_path 仅用于报告里的 Complete name 与文件大小（不会再打开它）
    static std::string FormatFromContext(const AVFormatContext* format,
                                         const std::string& file_path);

    /// 获取完整媒体信息（类 mediainfo CLI 的纯文本输出）
    std::string GetCompleteInfo() const;

    /// 关闭当前文件
    void Close();

    /// 是否已成功打开文件
    bool IsReady() const;

    /// 上一次失败的原因（Open 返回 false 时有意义）
    std::string GetLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace videoeye
