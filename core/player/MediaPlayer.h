#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/player/Decoders.h"
#include "core/model/FrameData.h"

namespace videoeye {
namespace player {

// 媒体播放器类 - 使用Qt信号槽机制
class MediaPlayer : public QObject {
    Q_OBJECT
    
public:
    explicit MediaPlayer(QObject* parent = nullptr);
    ~MediaPlayer();
    
    // 播放控制
    bool Open(const QString& url);
    void Play();
    void Pause();
    void Stop();
    void Seek(int position_ms);
    
    // 状态查询
    model::PlayerState GetState() const { return state_; }
    model::StreamInfo GetStreamInfo() const { return stream_info_; }
    int GetDuration() const { return duration_ms_; }
    int GetCurrentPosition() const { return current_position_ms_; }
    
    // 音量控制 (0-100)
    void SetVolume(int volume);
    int GetVolume() const { return volume_; }
    
signals:
    // 状态改变信号
    void StateChanged(model::PlayerState state);
    
    // 视频帧就绪信号
    void FrameReady(const QImage& frame);
    
    // 播放进度更新信号
    void PositionChanged(int position_ms, int duration_ms);
    
    // 错误信号
    void Error(const QString& message);
    
    // 播放完成信号
    void PlaybackFinished();
    
private:
    // 解码线程
    void DecodeThread();
    void VideoDecodeThread();
    void AudioDecodeThread();
    
    // 清理资源
    void Cleanup();
    
    // 成员变量
    std::atomic<model::PlayerState> state_ = model::PlayerState::Idle;
    std::atomic<bool> should_stop_ = false;
    
    AVFormatContext* format_ctx_ = nullptr;
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
    
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    
    std::thread decode_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    model::StreamInfo stream_info_;
    int duration_ms_ = 0;
    int current_position_ms_ = 0;
    int volume_ = 100;
    
    QString current_url_;
};

} // namespace player
} // namespace videoeye
