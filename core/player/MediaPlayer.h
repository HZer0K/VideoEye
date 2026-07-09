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
#include "core/player/PlaybackClock.h"
#include "core/player/StreamInfoExtractor.h"
#include "core/player/AudioVisualizer.h"
#include "core/player/AudioOutput.h"
#include "core/player/VideoFrameExporter.h"
#include "core/player/VulkanContext.h"
#include "core/player/VulkanRenderer.h"
#include "core/model/AnalysisEvent.h"
#include "core/model/AudioVisualizationFrame.h"
#include "core/model/FrameData.h"
#include "core/model/PacketInfo.h"
#include "core/model/SyncSample.h"
#include "core/model/TimelineEvent.h"
#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/FrameAnalyzer.h"
#include "core/analyzer/ContainerStructureAnalyzer.h"
#include "core/analyzer/MacroblockAnalyzer.h"
#include "core/model/ContainerStructureInfo.h"
#include "core/model/MacroblockInfo.h"

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
    void SetHistogramEnabled(bool enable);
    void SetAudioFrameAnalysisEnabled(bool enable) { audio_frame_analysis_enabled_ = enable; }
    void SetPacketAnalysisEnabled(bool enable) { packet_analysis_enabled_ = enable; }
    void SetEventAnalysisEnabled(bool enable) { event_analysis_enabled_ = enable; }
    void SetSyncAnalysisEnabled(bool enable) { sync_analysis_enabled_ = enable; }
    void SetTimelineAnalysisEnabled(bool enable) { timeline_analysis_enabled_ = enable; }
    void SetContainerStructureEnabled(bool enable) { container_structure_enabled_ = enable; }
    void SetMacroblockAnalysisEnabled(bool enable) { macroblock_analysis_enabled_ = enable; }

    // 硬件解码
    void SetHardwareDecodingEnabled(bool enable) { hw_decoding_enabled_ = enable; }
    bool IsHardwareDecoding() const;
    std::string GetHwDeviceName() const;

    // 视频帧导出
    void StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality = 90, int frame_interval = 1);
    void CancelVideoFrameExport();

    // Vulkan 渲染
    void SetVulkanRenderer(VulkanRenderer* renderer);
    void SetVulkanRenderingEnabled(bool enabled) { vulkan_rendering_enabled_ = enabled; }
    
    // 获取分析器
    analyzer::StreamAnalyzer& GetStreamAnalyzer() { return stream_analyzer_; }
    analyzer::StreamStats GetCurrentStats() const;
    
signals:
    void StateChanged(model::PlayerState state);
    void FrameReady(const QImage& frame);
    void PositionChanged(int position_ms, int duration_ms);
    void Error(const QString& message);
    void PlaybackFinished();
    
    // 分析数据信号
    void StreamStatsReady(const analyzer::StreamStats& stats);
    void HistogramReady(const analyzer::HistogramData& hist);
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
    void AudioVisualizationReady(const model::AudioVisualizationFrame& frame);
    void MediaModeChanged(bool has_video);
    void AudioLevelReady(double level, double timestamp_seconds);
    void ContainerStructureReady(const videoeye::model::ContainerStructureResult& result);
    void MacroblockInfoReady(const videoeye::model::MacroblockFrameAnalysis& analysis);
    void VideoFrameExportStarted(int total_frames);
    void VideoFrameExportProgress(int exported_frames);
    void VideoFrameExportFinished(const QString& output_dir);
    void VideoFrameExportCanceled(int exported_frames, const QString& output_dir);
    void VideoFrameExportError(const QString& message);
    
private:
    // 解码线程
    void DecodeThread();
    bool OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options);
    void EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                           qint64 pts, double timestamp_seconds,
                           const QString& summary, const QString& detail = QString());
    void EmitSyncSample(double audio_timestamp_seconds, double video_timestamp_seconds, bool audio_anchor);
    void EmitTimelineEvent(const QString& category, double timestamp_seconds,
                           const QString& label, const QString& detail = QString());
    void EmitAudioVisualization(const AudioVisualizationResult& vis_result,
                                int sample_rate, int channels, double timestamp_seconds, double level);
    void Cleanup();
    
    // 状态
    std::atomic<model::PlayerState> state_ = model::PlayerState::Idle;
    std::atomic<bool> should_stop_ = false;
    
    // FFmpeg 上下文
    AVFormatContext* format_ctx_ = nullptr;
    std::unique_ptr<VideoDecoder> video_decoder_;
    std::unique_ptr<AudioDecoder> audio_decoder_;
    std::unique_ptr<AudioOutput> audio_output_;  // SDL2 音频输出 (PCM -> 声卡)
    std::unique_ptr<VulkanContext> vulkan_ctx_;  // Vulkan 设备上下文
    std::unique_ptr<VulkanRenderer> vulkan_renderer_;  // Vulkan 渲染器
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    
    // 线程
    std::thread decode_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    // 播放信息
    model::StreamInfo stream_info_;
    int duration_ms_ = 0;
    std::atomic<int> current_position_ms_{0};
    int volume_ = 100;
    QString current_url_;
    
    // 分析器
    analyzer::StreamAnalyzer stream_analyzer_;
    analyzer::FrameAnalyzer frame_analyzer_;
    analyzer::ContainerStructureAnalyzer container_analyzer_;
    analyzer::MacroblockAnalyzer macroblock_analyzer_;
    StreamInfoExtractor stream_info_extractor_;
    AudioVisualizer audio_visualizer_;
    std::unique_ptr<VideoFrameExporter> frame_exporter_;
    
    // 分析开关
    bool analysis_enabled_ = false;
    bool frame_type_analysis_enabled_ = false;
    bool histogram_enabled_ = false;
    bool audio_frame_analysis_enabled_ = false;
    bool packet_analysis_enabled_ = false;
    bool event_analysis_enabled_ = false;
    bool sync_analysis_enabled_ = false;
    bool timeline_analysis_enabled_ = false;
    bool container_structure_enabled_ = true;
    bool macroblock_analysis_enabled_ = false;
    bool hw_decoding_enabled_ = false; // 默认关闭硬件解码: Vulkan/D3D11 等 HW 路径在部分 Windows 驱动下会导致"打开视频即闪退"(FFmpeg 内部段错误, 无法被 C++ 异常捕获, 进程直接终止)。软件解码稳定可靠; 如确需 HW 解码性能, 可显式调用 SetHardwareDecodingEnabled(true), 但仍建议保留下方解码线程的异常兜底。
    bool vulkan_rendering_enabled_ = false;  // Vulkan 渲染开关（阶段2）
    
    // 分析索引/状态
    int analysis_frame_counter_ = 0;
    int video_frame_index_ = 0;
    int macroblock_frame_index_ = 0;
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
