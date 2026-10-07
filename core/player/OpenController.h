#pragma once

// 打开媒体这条链路的全部编排: 超时中断、探测、选流、封面图、音视频解码器与音频输出
// 初始化、流信息提取。
//
// 为什么从 MediaPlayer 里拆出来: 这是 MediaPlayer 里最长的一个函数（约 300 行），
// 而它和"播放"其实是两件事 —— 它做的是"把一个文件变成一套就绪的解码管线"，
// 之后播/停/定位都不再碰它。它自带的知识也很独立：
//   * 打开/探测必须带绝对超时，且中断回调要在 avformat_open_input **之前**装到自建的
//     AVFormatContext 上；
//   * 探测一结束就要关掉截止时间，否则打开期的超时会误杀播放期的正常长读；
//   * 封面图（AV_DISPOSITION_ATTACHED_PIC）要单独解码一次，解完把视频流降级成 -1；
//   * 硬件解码与宏块分析互斥（HW 不导出运动矢量 side data）。
//
// 为什么拆成 Prepare / Commit 两步（事务式）: 评审指出这条链路以前整段跑在 UI 线程，
// 网络源/异常文件会让界面冻结数秒。现在把"把一个文件变成一套就绪的解码管线"做成一个
// **事务**：
//   * Prepare() 只创建**独立的** AVFormatContext / 解码器 / 音频输出 / 流信息，产出
//     一个自包含的 OpenResult，**绝不**触碰正在使用的 PlaybackSession —— 因此可以安全
//     地放在后台线程上跑；
//   * Commit() 在 UI 线程上把 OpenResult 一次性采用进 PlaybackSession（成功整体采用，
//     失败整体丢弃），并在这里发封面图 / 媒体模式 / 打开失败三条信号。
// MediaPlayer::Open() 仍在调用线程上同步 Prepare + Commit（兼容旧调用与测试）；
// MediaPlayer::OpenAsync() 则在后台线程 Prepare、排回 UI 线程 Commit。
//
// 本类是 QObject: 提交时要发封面图 / 媒体模式 / 打开失败三条信号，而对外契约
// （信号名与参数）必须与拆分前一个字节不差 —— 所以这里声明同名信号，由 MediaPlayer
// 原样转发。
//
// 生命周期: Prepare 用到的取消状态（OpenAttempt）由 MediaPlayer 在换媒体时新建，并通过
// OpenPrepareParams 传进来 —— 本类不拥有它。提交成功后播放会话继续用它的中断状态响应
// 取消（Stop() 靠它把阻塞的 av_read_frame 打断），因此它必须活过整个播放会话。

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string>

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/player/PlaybackSession.h"
#include "core/player/StreamInfoExtractor.h"

extern "C" {
struct AVInputFormat;
struct AVFormatContext;
struct AVStream;
struct AVDictionary;
struct AVCodec;
}

namespace videoeye {
namespace player {

// video_index 指向的流是不是"封面图"（AV_DISPOSITION_ATTACHED_PIC）。
//
// 这个判定必须先做边界检查再取 streams[video_index]：音频-only 文件里
// av_find_best_stream(VIDEO) 返回 -1，此时 streams[-1] 是越界访问（未定义行为）。
// 抽成独立函数是为了让"video_index < 0 / 越界 / 空上下文"这几条路径能被单测直接覆盖，
// 不需要真实媒体样本。
bool IsCoverArtStream(const AVFormatContext* format, int video_index);

// 一次打开尝试的中断/取消状态。
//
// 为什么按"每次尝试"独立分配而不是复用 MediaPlayer 上的一个固定成员: 上一次打开的
// 后台线程可能还卡在 FFmpeg 的阻塞 IO 里，而新一次打开会复位取消标志。若两者共用一颗
// 标志，新尝试一复位就把上一次的"已取消"抹掉，旧线程会继续往下跑。每次尝试各自持有
// 一份，互不干扰；提交后播放会话继续用同一份（Stop() 仍能打断播放期的阻塞读）。
struct OpenAttempt {
    std::atomic<bool> cancel{false};
    ffmpeg_io::AvInterruptState interrupt;

    OpenAttempt() {
        interrupt.cancel = &cancel;
        interrupt.deadline_us = 0;
    }

    OpenAttempt(const OpenAttempt&) = delete;
    OpenAttempt& operator=(const OpenAttempt&) = delete;
};

// Prepare()（可在后台线程执行）所需的全部输入。
struct OpenPrepareParams {
    QString url;
    const AVInputFormat* input_format = nullptr;   // 借用; 通常为 nullptr
    AVDictionary* input_options = nullptr;          // 借用并移交所有权(由 Prepare 释放)
    int volume_percent = 100;
    bool prefer_hw_decoding = false;                // 是否允许硬件解码
    bool macroblock_analysis = false;               // 宏块分析启用时强制软件解码
    std::shared_ptr<OpenAttempt> attempt;           // 取消/超时状态(必须非空)
};

// 一次打开请求的产物: 完全独立于 PlaybackSession。
//
// 后台线程只构造它，UI 线程提交时才把资源交给播放会话。析构时负责关闭还没被提交的
// AVFormatContext —— 失败 / 被丢弃的结果不会泄漏上下文。
struct OpenResult {
    bool ok = false;
    QString error;

    AVFormatContext* format_ctx = nullptr;          // 独立上下文, 由本结构拥有
    std::unique_ptr<VideoDecoder> video_decoder;
    std::unique_ptr<AudioDecoder> audio_decoder;
    std::unique_ptr<AudioOutput> audio_output;

    int video_stream_index = -1;                    // 已按封面图降级后的可播放视频流
    int audio_stream_index = -1;
    bool has_video = false;                          // 文件里有没有视频轨(含封面图)
    model::StreamInfo stream_info;
    // 媒体信息文本: 在后台探测时顺带从同一个上下文格式化（MediaInfoAnalyzer::
    // FormatFromContext），UI 不再为它单独跑一次 avformat 打开/探测。
    std::string media_info_text;
    int duration_ms = 0;
    QImage cover_art;                                // 可能为空

    OpenResult() = default;
    OpenResult(const OpenResult&) = delete;
    OpenResult& operator=(const OpenResult&) = delete;
    // 移动必须手写, 不能用 = default。
    //
    // format_ctx 是裸指针, 默认移动对它做的是**按值拷贝**: 移动之后源对象仍握着同一个
    // AVFormatContext*, 而它一析构就 avformat_close_input —— 被搬到的新对象(UI 线程正在
    // 提交、解码线程随后要 av_read_frame 的那一手)手里立刻变成悬垂指针, 先是段错误,
    // 源对象再析构一次又变成 double free。
    // OpenAsync 正是靠 lambda 捕获 `result = std::move(result)` 把产物从后台线程搬进
    // UI 队列的, 这条路径一走就是"打开必崩"。
    OpenResult(OpenResult&& other) noexcept { MoveFrom(other); }
    OpenResult& operator=(OpenResult&& other) noexcept {
        if (this != &other) {
            Release();
            MoveFrom(other);
        }
        return *this;
    }
    ~OpenResult();

private:
    // 关掉本对象当前持有的上下文并释放解码器/音频输出, 之后回到空态(析构安全)。
    void Release() noexcept;
    // 接管 other 的全部资源; 返回后 other 为空, 其析构不再释放任何东西。
    void MoveFrom(OpenResult& other) noexcept;
};

class OpenController : public QObject {
    Q_OBJECT

public:
    // playback: 借用的播放会话（引用，必须活过本对象），只在 Commit() 里使用。
    explicit OpenController(PlaybackSession& playback, QObject* parent = nullptr);

    OpenController(const OpenController&) = delete;
    OpenController& operator=(const OpenController&) = delete;

    // 后台线程: 建独立上下文 / 探测 / 选流 / 解码器 / 音频输出 / 流信息，产出 OpenResult。
    // **不碰** PlaybackSession，因此可安全地在任意线程调用。失败时 ok=false + error。
    // 做成 static: 任务体在后台线程上不持有任何可能先于它销毁的对象（BlockingIo 约定）。
    static OpenResult Prepare(const OpenPrepareParams& params);

    // UI 线程: 一次性把 Prepare 的成果提交进播放会话。
    // 成功: 采用上下文/解码器/流索引/时长，回填 out_info，发 FrameReady(封面) +
    //       MediaModeChanged；失败: 回填 out_error、发 OpenFailed、释放会话。
    bool Commit(OpenResult&& result, model::StreamInfo& out_info, QString& out_error);

signals:
    // 打开阶段失败 (版本不匹配 / 打开超时 / 探测失败 / 无可用流 / 解码器初始化失败)。
    void OpenFailed(const QString& message);
    // 封面图（AV_DISPOSITION_ATTACHED_PIC）解出后作为一帧画面发出。
    void FrameReady(const QImage& frame);
    void MediaModeChanged(bool has_video);
    // 媒体信息文本（提交成功且非空时发出）。文本在 Prepare 阶段由同一个上下文
    // 格式化完成，这里只负责跨线程回到 UI；顺序保证先于 OpenFinished。
    void MediaInfoTextReady(const QString& text);

private:
    // --- Prepare() 的步骤。纯静态: 只依赖传入参数与 FFmpeg, 不依赖任何成员 ---
    static bool CheckRuntimeVersion(QString& out_error);
    // 打开输入并探测流信息；成功后上下文交由 result 拥有(失败时 result.format_ctx 可能为空)。
    static bool OpenAndProbe(const OpenPrepareParams& params, OpenResult& result);
    static bool FindStreams(AVFormatContext* fmt, int& video_index, int& audio_index,
                            const AVCodec*& best_video_codec, QString& out_error);
    // 封面图: 解出 QImage 返回(可能为空)；是否命中由调用方用 IsCoverArtStream 判定。
    static QImage DecodeCoverArt(AVStream* video_stream);
    static bool PrepareVideoDecoder(const OpenPrepareParams& params, AVFormatContext* fmt,
                                    int video_index, const AVCodec* best_video_codec,
                                    std::unique_ptr<VideoDecoder>& out_decoder, QString& out_error);
    static bool PrepareAudioDecoder(const OpenPrepareParams& params, AVFormatContext* fmt,
                                    int audio_index, std::unique_ptr<AudioDecoder>& out_decoder,
                                    std::unique_ptr<AudioOutput>& out_output, QString& out_error);

    PlaybackSession& playback_;
};

}  // namespace player
}  // namespace videoeye
