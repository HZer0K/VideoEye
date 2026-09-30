#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/player/Decoders.h"
#include "core/player/PlaybackClock.h"
#include "core/player/StreamInfoExtractor.h"
#include "core/player/AudioVisualizer.h"
#include "core/player/AudioOutput.h"
#include "core/player/VideoFrameExporter.h"
#include "core/player/AnalysisSession.h"
#include "core/player/PlaybackSession.h"
#include "infrastructure/concurrency/TaskManager.h"
#include "core/exporter/MediaExporter.h"
#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/player/FrameData.h"
#include "core/domain/model/SeekMode.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"
#include "core/domain/model/FrameTimingInfo.h"
#include "core/analysis/stream/StreamAnalyzer.h"
#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/analysis/quality/MacroblockAnalyzer.h"
#include "core/analysis/quality/SceneChangeAnalyzer.h"
#include "core/analysis/quality/VisualDefectAnalyzer.h"
#include "core/analysis/quality/QualityAnalyzer.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/MacroblockInfo.h"

class QThread;

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
    void Seek(int position_ms, model::SeekMode mode = model::SeekMode::NearestKeyframe);

    // 定位方式 (进度条拖动策略)
    void SetSeekMode(model::SeekMode mode) { seek_mode_.store(mode); }
    model::SeekMode GetSeekMode() const { return seek_mode_.load(); }
    // 拖动进度条期间调用: 抑制音频输出, 避免关键帧预览时从多个位置传出杂音
    void SetSeekDragging(bool dragging) { playback_session_.SetSeekDragging(dragging); }

    // 状态查询
    model::PlayerState GetState() const { return playback_session_.state(); }
    model::StreamInfo GetStreamInfo() const { return stream_info_; }
    int GetDuration() const { return playback_session_.duration_ms(); }
    int GetCurrentPosition() const { return playback_session_.current_position_ms(); }

    // 最近一次打开失败的详细原因 (成功打开后清空)。用于"打开异常文件仍进入分析模式"的
    // 场景: MainWindow 据此在状态栏/分析模块展示失败原因, 而不是弹模态框阻断。
    QString GetLastError() const { return last_open_error_; }
    // 播放器 Open 失败后手动触发容器结构分析 (后台线程, 结果经 ContainerStructureReady 发出)。
    // 正常 Open 成功路径内部已自动触发, 无需调用此方法。
    void RequestContainerStructureAnalysis(const QString& url);
    
    // 音量控制 (0-100)
    void SetVolume(int volume);
    int GetVolume() const { return volume_; }
    
    // 分析控制
    void EnableAnalysis(bool enable);
    bool IsAnalysisEnabled() const { return analysis_session_.IsAnalysisEnabled(); }
    void SetFrameTypeAnalysisEnabled(bool enable);
    void SetAudioFrameAnalysisEnabled(bool enable) { analysis_session_.SetAudioFrameAnalysisEnabled(enable); }
    void SetPacketAnalysisEnabled(bool enable) { analysis_session_.SetPacketAnalysisEnabled(enable); }
    void SetEventAnalysisEnabled(bool enable) { analysis_session_.SetEventAnalysisEnabled(enable); }
    void SetSyncAnalysisEnabled(bool enable) { analysis_session_.SetSyncAnalysisEnabled(enable); }
    void SetTimelineAnalysisEnabled(bool enable) { analysis_session_.SetTimelineAnalysisEnabled(enable); }
    void SetContainerStructureEnabled(bool enable) { analysis_session_.SetContainerStructureEnabled(enable); }
    void SetMacroblockAnalysisEnabled(bool enable);
    void SetSceneChangeAnalysisEnabled(bool enable) { analysis_session_.SetSceneChangeAnalysisEnabled(enable); }

    // 画面质量 / 视觉缺陷（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边）。
    // 开关会顺带启停分析用的工作线程；换采样档位用 SetVisualDefectOptions()。
    void SetVisualDefectAnalysisEnabled(bool enable);
    bool IsVisualDefectAnalysisEnabled() const { return analysis_session_.IsVisualDefectAnalysisEnabled(); }
    void SetVisualDefectOptions(const analyzer::VisualDefectOptions& options);
    // 线程安全地取视觉缺陷采样选项副本: 解码线程逐帧读取, UI 线程写入,
    // 普通值类型直接跨线程访问存在数据竞争, 故加锁后返回副本。
    analyzer::VisualDefectOptions GetVisualDefectOptions() const;
    // 播完 / 停止时把还开着的缺陷段闭合（否则最后一段要等下一次播放才显示）
    void FlushVisualDefectSegments(double end_timestamp_seconds);

    // 硬件解码
    void SetHardwareDecodingEnabled(bool enable) { analysis_session_.SetHardwareDecodingEnabled(enable); }
    bool IsHardwareDecoding() const;
    std::string GetHwDeviceName() const;

    // 视频帧导出
    void StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality = 90, int frame_interval = 1);
    void CancelVideoFrameExport();

    // 音视频导出 (remux / transcode)
    void StartMediaExport(const exporter::ExportOptions& opt);
    void CancelMediaExport();

    // 渲染抑制: 播放区被隐藏时跳过画面输出 (sws_scale + FrameReady),
    // 解码线程、实时分析与音频照常运行; 重新展开播放区即恢复画面。
    void SetRenderingSuppressed(bool suppressed) {
        playback_session_.SetRenderingSuppressed(suppressed);
    }
    bool IsRenderingSuppressed() const {
        return playback_session_.IsRenderingSuppressed();
    }
    
    // 获取分析器
    analyzer::StreamAnalyzer& GetStreamAnalyzer() { return analysis_session_.stream_analyzer(); }
    analyzer::StreamStats GetCurrentStats() const;
    
signals:
    void StateChanged(model::PlayerState state);
    void FrameReady(const QImage& frame);
    void PositionChanged(int position_ms, int duration_ms);
    void Error(const QString& message);
    // 打开阶段失败 (avformat_open_input / find_stream_info / 无可播放流 / 解码器初始化失败)。
    // 与 Error 的区别: 不弹模态框, 只在状态栏提示, 文件仍会加载到分析模块。
    void OpenFailed(const QString& message);
    void PlaybackFinished();
    
    // 分析数据信号
    void StreamStatsReady(const analyzer::StreamStats& stats);
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
    // 时间轴与同步诊断（demux 层 packet 时间 / decode 层 frame 时间）
    void TimelinePacketReady(const model::PacketTiming& timing);
    void FrameTimingReady(const model::FrameTimingInfo& timing);
    void TimelineEventListReset();
    void TimelineEventReady(const model::TimelineEvent& event);
    void AudioVisualizationReady(const model::AudioVisualizationFrame& frame);
    void MediaModeChanged(bool has_video);
    void AudioLevelReady(double level, double timestamp_seconds);
    void ContainerStructureReady(const videoeye::model::ContainerStructureResult& result);
    void MacroblockInfoReady(const videoeye::model::MacroblockFrameAnalysis& analysis);
    void SceneChangeReady(const analyzer::SceneChangeResult& result);
    // 画面质量 / 视觉缺陷（实时播放时逐采样帧产出）
    void VisualDefectReset();
    void VisualDefectFrameReady(const model::FrameQualityMetric& metric);
    void VisualDefectReady(const model::VisualDefect& defect);
    // 分析进度: 已分析帧数 / 被丢弃帧数 / 全片有效画面区域（约每秒刷新一次）
    void VisualDefectStatsReady(int analyzed_frames, int dropped_frames,
                                const model::ActivePictureArea& effective_area);
    void VideoFrameExportStarted(int total_frames);
    void VideoFrameExportProgress(int exported_frames);
    void VideoFrameExportFinished(const QString& output_dir);
    void VideoFrameExportCanceled(int exported_frames, const QString& output_dir);
    void VideoFrameExportError(const QString& message);

    // 音视频导出信号
    void MediaExportStarted(qint64 duration_ms);
    void MediaExportProgress(int percent);
    void MediaExportFinished(const QString& output_path);
    void MediaExportCanceled(const QString& output_path);
    void MediaExportError(const QString& message);

private:
    bool OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options);
    void EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                           qint64 pts, double timestamp_seconds,
                           const QString& summary, const QString& detail = QString());
    void EmitSyncSample(double audio_timestamp_seconds, double video_timestamp_seconds, bool audio_anchor);
    void EmitTimelineEvent(const QString& category, double timestamp_seconds,
                           const QString& label, const QString& detail = QString());
    void EmitAudioVisualization(const AudioVisualizationResult& vis_result,
                                int sample_rate, int channels, double timestamp_seconds, double level);
    void StartContainerStructureAnalysis(const QString& url);

    // PlaybackSession 的回调入口: 解码线程在 demux / 解码 / 定位 / 播完的时机会调进来,
    // 由 MediaPlayer 决定"要不要发分析信号、要不要计数"。详见 core/player/PlaybackSession.h。
    void InstallPlaybackHooks();
    void OnPlaybackPacket(const PacketContext& ctx);
    void OnPlaybackVideoFrame(const VideoFrameContext& ctx);
    void OnPlaybackAudioFrame(const AudioFrameContext& ctx);
    void OnPlaybackSeekDone(double target_ms, model::SeekMode mode);
    void OnPlaybackEndOfStream();

    // 后台任务 slot 名: 容器结构分析 / 抽帧 / 媒体导出。
    // 同一 slot 上永远只有一个任务在跑(见 core/task/TaskManager.h)。
    static constexpr const char* kSlotContainerStructure = "container-structure";
    static constexpr const char* kSlotFrameExport = "frame-export";
    static constexpr const char* kSlotMediaExport = "media-export";

    // 画面质量 / 视觉缺陷: 按采样档位抽取解码帧 -> 降采样 -> 投递分析器
    void FeedVisualDefectFrame(const AVFrame* frame, double timestamp_seconds, bool audio_silent);
    // 把分析器已产出的指标 / 缺陷转发成信号（在解码线程调用）
    void DrainVisualDefectResults();
    // 分析进度信号（内部做 1 秒节流，force=true 时立即发）
    void EmitVisualDefectStats(bool force);
    
    // 播放会话: demux / 解码 / 音频输出 / 解码线程 / 播放时钟 / 播放状态机
    // 全部住在 PlaybackSession 里; 状态、位置、时长都从它读。
    PlaybackSession playback_session_;

    // 播放信息
    model::StreamInfo stream_info_;
    int volume_ = 100;
    QString current_url_;
    QString last_open_error_;   // 最近一次 Open/OpenRawPcm 失败的详细原因

    // 用户选择的定位方式 (菜单设置)。实际执行在 PlaybackSession::Seek()。
    std::atomic<model::SeekMode> seek_mode_{model::SeekMode::NearestKeyframe};
    
    // 分析器
    // 分析会话: 12 个分析开关 + StreamAnalyzer + 视觉缺陷采样选项 (见 AnalysisSession.h)。
    // MediaPlayer 经它转发开关/统计, 自身不再持有这些散落成员。
    AnalysisSession analysis_session_;
    analyzer::MacroblockAnalyzer macroblock_analyzer_;
    analyzer::SceneChangeAnalyzer scene_change_analyzer_;
    analyzer::VisualDefectAnalyzer visual_defect_analyzer_;
    StreamInfoExtractor stream_info_extractor_;
    AudioVisualizer audio_visualizer_;
    QThread* frame_export_thread_ = nullptr;
    VideoFrameExporter* frame_exporter_ = nullptr;

    // 后台任务统一调度: 任务 ID / 取消标志 / 终态 / 过期结果丢弃。
    // 容器结构分析走它的受管线程; 抽帧与媒体导出的 worker 是 QObject(要发进度信号),
    // 仍留在 QThread 上, 但生命周期(Begin/End/Cancel)也登记在这里。
    task::TaskManager task_manager_;

    // 音视频导出 (后台线程)
    QThread* media_export_thread_ = nullptr;
    exporter::MediaExporter* media_exporter_ = nullptr;

    // 分析索引/状态
    int analysis_frame_counter_ = 0;
    int video_frame_index_ = 0;
    int macroblock_frame_index_ = 0;
    int scene_change_frame_index_ = 0;
    int visual_defect_frame_index_ = 0;
    double visual_defect_last_sample_ts_ = -1.0;
    // 统计信号节流: 有效画面区域要取中位数，每秒算一次就够
    std::chrono::steady_clock::time_point visual_defect_last_stats_emit_{};
    double last_audio_level_ = 0.0;   // 最近一帧音频的 RMS（冻结帧判定要排除静音段）
    int audio_frame_index_ = 0;
    int packet_index_ = 0;
    int timeline_packet_index_ = 0;
    int analysis_event_index_ = 0;
    int sync_sample_index_ = 0;
    int timeline_event_index_ = 0;
    int audio_visualization_index_ = 0;
    std::map<int, double> last_packet_ts_by_stream_;
    std::map<int, bool> missing_packet_ts_reported_;
    std::map<int, bool> missing_audio_pts_reported_;
    int audio_timeline_sample_counter_ = 0;
};

} // namespace player
} // namespace videoeye
