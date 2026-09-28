#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/model/FrameData.h"
#include "core/model/SeekMode.h"
#include "core/player/AudioOutput.h"
#include "core/player/Decoders.h"
#include "core/player/PlaybackClock.h"

namespace videoeye {
namespace player {

// 交给 MediaPlayer 的分析回调上下文。
//
// 切分原则: **播放机械归 PlaybackSession, 分析语义归 MediaPlayer**。
// 解码循环里每一处"要不要发信号 / 要不要计数"都属于分析语义, 因此不搬进 Session,
// 而是由 Session 在对应时机回调 MediaPlayer 装的 hook —— MediaPlayer 侧的代码与
// 各种 Analyzer / 计数器保持在一起, Session 只管 demux、解码、pacing 与画面输出。
struct PacketContext {
    const AVPacket* packet = nullptr;
    AVFormatContext* format_ctx = nullptr;
    double timestamp_seconds = 0.0;              // 已换算到秒; 非有限值表示缺失
    std::int64_t raw_timestamp = AV_NOPTS_VALUE; // pts; 无 pts 时退到 dts
};

struct VideoFrameContext {
    model::FrameData* frame = nullptr; // 可写: Session 已补好 timestamp
    const AVFrame* raw_frame = nullptr;
    const AVPacket* packet = nullptr;
    AVFormatContext* format_ctx = nullptr;
    VideoDecoder* decoder = nullptr;
    int video_stream_index = -1;
    double timestamp_seconds = 0.0;
    // 精确帧定位的追赶阶段: true 时应跳过分析开销(画面仍照常输出以便快速前进)
    bool catching_up = false;
};

struct AudioFrameContext {
    const AVPacket* packet = nullptr;
    AVFormatContext* format_ctx = nullptr;
    AudioDecoder* decoder = nullptr;
    const std::int16_t* samples = nullptr;
    int sample_count = 0;
    int sample_rate = 0;
    int channels = 0;
    int byte_count = 0; // 解码出来的 PCM 字节数
    std::int64_t frame_pts = AV_NOPTS_VALUE;
    double timestamp_seconds = 0.0;
    double level = 0.0; // RMS, 归一到 [0,1]
    int audio_stream_index = -1;
    bool enqueued = false; // 已推送到音频后端(字节数过小时为 false)
};

// 播放会话: 一次"打开媒体 -> 播放/暂停/定位 -> 停止"的完整生命周期。
//
// 持有: AVFormatContext / 解码器 / 音频输出 / 解码线程 / 播放时钟 / 播放状态机。
// 不持有: 任何分析器与分析开关(那些在 AnalysisSession 与 MediaPlayer 里)。
//
// 为什么是 QObject: 画面、位置、状态这些信号必须在工作线程发出、由 Qt 自动排队到
// UI 线程; 用裸回调会让每个信号都多一层转发。
class PlaybackSession : public QObject {
    Q_OBJECT

public:
    // 分析回调集合。全部在解码线程上被调用(与改造前一致), 由 MediaPlayer 在安装前填好。
    struct Hooks {
        std::function<void(const PacketContext&)> on_packet;
        std::function<void(const VideoFrameContext&)> on_video_frame;
        std::function<void(const AudioFrameContext&)> on_audio_frame;
        // 定位成功(seek + flush 之后): 让分析侧重置"当前片段"的统计
        std::function<void(double target_ms, model::SeekMode mode)> on_seek_done;
        // 读到流末尾: 闭合还开着的缺陷段等收尾动作
        std::function<void()> on_end_of_stream;
        // 音视频时间戳对齐采样(audio_ts, video_ts, 以音频为锚)
        std::function<void(double, double, bool)> on_sync_sample;
    };

    explicit PlaybackSession(QObject* parent = nullptr);
    ~PlaybackSession() override;

    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;

    void SetHooks(Hooks hooks);

    // --- 媒体资源 ---
    AVFormatContext* format_ctx() const {
        return format_ctx_;
    }
    void AdoptFormatContext(AVFormatContext* ctx); // 接管所有权(由 Release() 关闭)
    void SetStreamIndices(int video_index, int audio_index);
    int video_stream_index() const {
        return video_stream_index_;
    }
    int audio_stream_index() const {
        return audio_stream_index_;
    }
    void SetVideoDecoder(std::unique_ptr<VideoDecoder> decoder);
    void SetAudioDecoder(std::unique_ptr<AudioDecoder> decoder);
    void SetAudioOutput(std::unique_ptr<AudioOutput> output);
    VideoDecoder* video_decoder() const {
        return video_decoder_.get();
    }
    AudioDecoder* audio_decoder() const {
        return audio_decoder_.get();
    }
    AudioOutput* audio_output() const {
        return audio_output_.get();
    }
    // 释放 demux / 解码 / 音频资源(原 MediaPlayer::Cleanup)
    void Release();

    // --- 播放控制 ---
    bool Play(); // 返回 false 表示没有已打开的媒体
    void Pause();
    void Stop(); // 停解码线程并 join
    void Seek(int position_ms, model::SeekMode mode);

    // --- 状态 ---
    model::PlayerState state() const {
        return state_.load(std::memory_order_acquire);
    }
    // 打开成功后回到 Idle(可播放但未开始)。会发 StateChanged。
    void SetIdle();
    bool IsRenderingSuppressed() const {
        return rendering_suppressed_.load(std::memory_order_relaxed);
    }
    void SetRenderingSuppressed(bool suppressed) {
        rendering_suppressed_.store(suppressed, std::memory_order_relaxed);
    }
    void SetSeekDragging(bool dragging) {
        drag_seeking_.store(dragging);
    }
    void SetDuration(int duration_ms) {
        duration_ms_ = duration_ms;
    }
    int duration_ms() const {
        return duration_ms_;
    }
    int current_position_ms() const {
        return current_position_ms_.load();
    }
    void SetPosition(int position_ms) {
        current_position_ms_.store(position_ms);
    }
    void SetVolume(double volume_0_1);
    // 换文件 / 重新打开时清掉上一次的音视频对齐基准
    void ResetSyncTimestamps();

signals:
    void StateChanged(model::PlayerState state);
    void FrameReady(const QImage& frame);
    void PositionChanged(int position_ms, int duration_ms);
    void Error(const QString& message);
    void PlaybackFinished();

private:
    void DecodeThread();
    void EmitPositionIfNeeded(double timestamp_seconds, int& last_emitted_ms);

    Hooks hooks_;

    // FFmpeg 上下文
    AVFormatContext* format_ctx_ = nullptr;
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
    std::unique_ptr<AudioOutput> audio_output_;
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;

    // 播放状态机
    std::atomic<model::PlayerState> state_{model::PlayerState::Idle};
    std::atomic<bool> should_stop_{false};
    std::atomic<int> current_position_ms_{0};
    int duration_ms_ = 0;
    std::atomic<bool> pending_seek_{false};
    std::atomic<model::SeekMode> pending_seek_mode_{model::SeekMode::NearestKeyframe};
    double seek_request_ms_ = 0.0;             // 受 mutex_ 保护
    std::atomic<double> drop_until_sec_{-1.0}; // 精确帧: 丢弃此秒数之前的帧; <0 表示不丢弃
    std::atomic<bool> drag_seeking_{false};
    std::atomic<bool> rendering_suppressed_{false};
    double last_video_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    double last_audio_sync_ts_ = std::numeric_limits<double>::quiet_NaN();

    // 线程
    std::thread decode_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace player
} // namespace videoeye
