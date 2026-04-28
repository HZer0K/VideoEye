#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <memory>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/player/Decoders.h"
#include "core/model/AnalysisEvent.h"
#include "core/model/AudioVisualizationFrame.h"
#include "core/model/FrameData.h"
#include "core/model/PacketInfo.h"
#include "core/model/SyncSample.h"
#include "core/model/TimelineEvent.h"
#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/FrameAnalyzer.h"
#include "core/analyzer/FaceDetector.h"

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
    bool OpenRawPcm(const QString& url, const QString& demuxer_name, int sample_rate, int channels);
    void Play();
    void Pause();
    void Stop();
    void Seek(int position_ms);
    
    // 状态查询
    model::PlayerState GetState() const { return state_; }
    model::StreamInfo GetStreamInfo() const { return stream_info_; }
    int GetDuration() const { return duration_ms_; }
    int GetCurrentPosition() const { return current_position_ms_.load(); }
    
    // 音量控制 (0-100)
    void SetVolume(int volume);
    int GetVolume() const { return volume_; }
    
    // 分析控制
    void EnableAnalysis(bool enable);
    bool IsAnalysisEnabled() const { return analysis_enabled_; }
    void SetFrameTypeAnalysisEnabled(bool enable);
    void SetFaceDetectionEnabled(bool enable);
    void SetHistogramEnabled(bool enable);

    // 视频帧导出
    void StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality = 90, int frame_interval = 1);
    void CancelVideoFrameExport();
    
    // 获取分析器
    analyzer::StreamAnalyzer& GetStreamAnalyzer() { return stream_analyzer_; }
    analyzer::StreamStats GetCurrentStats() const;
    
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
    
    // 分析数据信号
    void StreamStatsReady(const analyzer::StreamStats& stats);
    void HistogramReady(const analyzer::HistogramData& hist);
    void FaceDetectionReady(const std::vector<analyzer::FaceInfo>& faces);
    void VideoFrameListReset();
    void VideoFrameInfoReady(int index, int frame_type, bool is_key_frame, qint64 pts, double timestamp_seconds);
    void AudioFrameListReset();
    void AudioFrameInfoReady(int index, qint64 pts, double timestamp_seconds,
                             int sample_count, int sample_rate, int channels, int byte_count);
    void PacketListReset();
    void PacketInfoReady(const model::PacketInfo& packet_info);
    void AnalysisEventListReset();
    void AnalysisEventReady(const model::AnalysisEvent& event_info);
    void SyncSampleListReset();
    void SyncSampleReady(const model::SyncSample& sample);
    void TimelineEventListReset();
    void TimelineEventReady(const model::TimelineEvent& event);
    void AudioVisualizationReset();
    void AudioVisualizationReady(const model::AudioVisualizationFrame& frame);
    void MediaModeChanged(bool has_video);
    void AudioLevelReady(double level, double timestamp_seconds);
    void VideoFrameExportStarted(int total_frames);
    void VideoFrameExportProgress(int exported_frames);
    void VideoFrameExportFinished(const QString& output_dir);
    void VideoFrameExportCanceled(int exported_frames, const QString& output_dir);
    void VideoFrameExportError(const QString& message);
    
private:
    // 解码线程
    void DecodeThread();
    void VideoDecodeThread();
    void AudioDecodeThread();
    void ExportVideoFramesWorker(QString url, QString output_dir, QString format, int jpg_quality, int frame_interval);
    bool OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options);
    void EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                           qint64 pts, double timestamp_seconds,
                           const QString& summary, const QString& detail = QString());
    void EmitSyncSample(double audio_timestamp_seconds, double video_timestamp_seconds, bool audio_anchor);
    void EmitTimelineEvent(const QString& category, double timestamp_seconds,
                           const QString& label, const QString& detail = QString());
    void EmitAudioVisualizationFrame(const int16_t* samples, int sample_count,
                                     int sample_rate, int channels,
                                     double timestamp_seconds, double level);
    
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
    std::thread export_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    model::StreamInfo stream_info_;
    int duration_ms_ = 0;
    std::atomic<int> current_position_ms_{0};
    int volume_ = 100;
    
    QString current_url_;
    std::atomic<bool> export_cancel_ = false;
    
    // 分析器
    analyzer::StreamAnalyzer stream_analyzer_;
    analyzer::FrameAnalyzer frame_analyzer_;
    analyzer::FaceDetector face_detector_;
    bool analysis_enabled_ = false;
    bool frame_type_analysis_enabled_ = false;
    bool face_detection_enabled_ = false;
    bool histogram_enabled_ = false;
    int analysis_frame_counter_ = 0;  // 用于控制分析频率
    int video_frame_index_ = 0;
    int audio_frame_index_ = 0;
    int packet_index_ = 0;
    int analysis_event_index_ = 0;
    int sync_sample_index_ = 0;
    int timeline_event_index_ = 0;
    int audio_visualization_index_ = 0;
    std::map<int, double> last_packet_ts_by_stream_;
    std::map<int, bool> missing_packet_ts_reported_;
    std::map<int, bool> missing_audio_pts_reported_;
    double last_video_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    double last_audio_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    int audio_timeline_sample_counter_ = 0;
};

} // namespace player
} // namespace videoeye
