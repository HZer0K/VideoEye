#pragma once

// 打开媒体这条链路的全部编排: 超时中断、探测、选流、封面图、音视频解码器与音频输出
// 初始化、流信息提取。
//
// 为什么从 MediaPlayer 里拆出来: 这是 MediaPlayer 里最长的一个函数（约 300 行），
// 而它和"播放"其实是两件事 —— 它做的是"把一个文件变成一套就绪的解码管线"，
// 之后播/停/定位都不再碰它。它自带的知识也很独立：
//   * 打开/探测必须带绝对超时（这条路径跑在 UI 线程，阻塞多久界面就冻多久），
//     且中断回调要在 avformat_open_input **之前**装到自建的 AVFormatContext 上；
//   * 探测一结束就要关掉截止时间，否则打开期的超时会误杀播放期的正常长读；
//   * 封面图（AV_DISPOSITION_ATTACHED_PIC）要单独解码一次，解完把视频流降级成 -1；
//   * 硬件解码与宏块分析互斥（HW 不导出运动矢量 side data）。
//
// 失败出口统一: 原来 8 个失败分支各自写着"emit OpenFailed + Release + return false"，
// 现在每个步骤只负责给出原因，由 Open() 在一个出口发信号、释放上下文。
//
// 本类是 QObject: 打开过程中要发封面图 / 媒体模式 / 打开失败三条信号，而对外契约
// （信号名与参数）必须与拆分前一个字节不差 —— 所以这里声明同名信号，由 MediaPlayer
// 原样转发。
//
// 生命周期: 本类**不拥有**中断状态与取消标志，只借引用 —— 它们的生命周期必须覆盖
// AVFormatContext（回调会被 AVIOContext / URLContext 各复制一份，播放期仍在用），
// 因此继续由 MediaPlayer 持有（声明在播放会话之前，保证最后才析构）。
// 本类也不拥有播放会话与分析会话，同样只持引用。

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/player/AnalysisSession.h"
#include "core/player/PlaybackSession.h"
#include "core/player/StreamInfoExtractor.h"

extern "C" {
struct AVInputFormat;
struct AVDictionary;
struct AVCodec;
}

namespace videoeye {
namespace player {

class OpenController : public QObject {
    Q_OBJECT

public:
    // playback / analysis: 借用的会话（引用，必须活过本对象）。
    // interrupt / cancel: 打开期的 FFmpeg 中断状态与取消标志（引用，宿主是 MediaPlayer）。
    OpenController(PlaybackSession& playback, AnalysisSession& analysis,
                   ffmpeg_io::AvInterruptState& interrupt, std::atomic<bool>& cancel,
                   QObject* parent = nullptr);

    OpenController(const OpenController&) = delete;
    OpenController& operator=(const OpenController&) = delete;

    // 打开媒体。成功时回填 out_info 并返回 true；失败时回填 out_error、发 OpenFailed
    // 并返回 false（上下文已释放，调用方不要再碰播放会话里的 format_ctx）。
    //
    // volume_percent: 音频输出设备的初始音量（0-100），由调用方从自己的状态带进来。
    // out_error: 失败原因。成功时**不**由本函数清空 —— 清不清是调用方的事
    //   （MediaPlayer 在成功路径上清 last_open_error_）。
    bool Open(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options,
              int volume_percent, model::StreamInfo& out_info, QString& out_error);

    // 让卡在 avformat_open_input / avformat_find_stream_info 上的阻塞 IO 立刻退出。
    // 打开期之外调用也是安全的: 只置取消标志并把打开期的截止时间清零。
    void RequestCancel();

signals:
    // 打开阶段失败 (版本不匹配 / 打开超时 / 探测失败 / 无可用流 / 解码器初始化失败)。
    void OpenFailed(const QString& message);
    // 封面图（AV_DISPOSITION_ATTACHED_PIC）解出后作为一帧画面发出。
    void FrameReady(const QImage& frame);
    void MediaModeChanged(bool has_video);

private:
    // --- Open() 的五个步骤。每个只负责"做成 + 给出失败原因"，不负责发信号与释放 ---
    // （那两件事统一在 Open() 的单一失败出口做，省掉八处重复）。
    bool CheckRuntimeVersion(QString& out_error) const;
    // 打开输入并探测流信息；上下文探测成功后归播放会话所有。
    bool OpenAndProbe(const QString& url, const AVInputFormat* input_format,
                      AVDictionary* input_options, QString& out_error);
    // 选流。选不到任何可播放的音视频流即失败。
    bool FindStreams(int& video_index, int& audio_index, const AVCodec*& best_video_codec,
                     QString& out_error);
    // 封面图: 解出后发 FrameReady，并把视频流降级为 -1（封面不是可播放的视频轨）。
    // 返回是否命中封面图。**只要**视频轨带 AV_DISPOSITION_ATTACHED_PIC 就降级 ——
    // 解不解得出来都不该把它当可播放的视频流去初始化解码器。
    bool HandleCoverArt(int& video_index, int audio_index);
    void EmitCoverArt(AVStream* video_stream);
    bool InitVideoDecoder(int video_index, const AVCodec* best_video_codec, QString& out_error);
    bool InitAudioDecoder(int audio_index, int volume_percent, QString& out_error);

    PlaybackSession& playback_;
    AnalysisSession& analysis_;
    ffmpeg_io::AvInterruptState& interrupt_;
    std::atomic<bool>& cancel_;
    StreamInfoExtractor stream_info_extractor_;
};

}  // namespace player
}  // namespace videoeye
