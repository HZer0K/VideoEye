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
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/ffmpeg_io/FfmpegInterrupt.h"
#include "core/player/Decoders.h"
#include "core/player/PlaybackClock.h"
#include "core/player/AudioOutput.h"
#include "core/player/VideoFrameExporter.h"
#include "core/player/AnalysisSession.h"
#include "core/player/ContainerInspectionController.h"
#include "core/player/ExportController.h"
#include "core/player/OpenController.h"
#include "core/player/PlaybackSession.h"
#include "core/player/RealtimeAnalysisController.h"
#include "infrastructure/concurrency/TaskManager.h"
#include "core/qt/QtWorkerOwner.h"
#include "core/exporter/MediaExporter.h"
#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/player/FrameData.h"
#include "core/domain/model/SeekMode.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"
#include "core/domain/model/FrameTimingInfo.h"
// 下面两个只为信号/入口里出现的 videoeye:: 值类型（SceneChangeResult / VisualDefectOptions）
// 提供定义；逐帧分析器实体已随实时分析搬进 RealtimeAnalysisController。
#include "core/analysis/stream/StreamAnalyzer.h"
#include "core/analysis/quality/SceneChangeAnalyzer.h"
#include "core/analysis/quality/VisualDefectAnalyzer.h"
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
    // 播放期分析功能的统一开关。UI 侧只走这一个入口。
    //
    // 以前每个维度一个 SetXxxAnalysisEnabled()，新增一个播放期分析维度要同时改三处：
    // domain 的 AnalysisFeature、MediaPlayer 的 setter、MainWindow 里那个把枚举翻译
    // 成 setter 调用的 switch。现在 MediaPlayer 直接认 model::AnalysisFeature，UI 只做
    // 转发，新增维度只需在枚举里加一项 + 在本函数的实现里加一行。
    //
    // 语义上要留意两点：
    //   * Master 与 StreamStats 都映射到 EnableAnalysis()（总开关）；
    //   * Diagnostics（全文件扫描 + QC）不在播放会话里，这里对它不做任何事 ——
    //     扫描由面板侧的 DiagnosticsPage 单独驱动。
    void SetAnalysisFeature(model::AnalysisFeature feature, bool enable);

    // 画面质量 / 视觉缺陷（黑场 / 冻结 / 马赛克 / 模糊 / 闪烁 / 曝光 / 色偏 / 梳齿 / 黑边）。
    // 开关会顺带启停分析用的工作线程；换采样档位用 SetVisualDefectOptions()。
    // 开关请经 SetAnalysisFeature(AnalysisFeature::VisualDefect, ...) 调用。
    bool IsVisualDefectAnalysisEnabled() const { return analysis_session_.IsVisualDefectAnalysisEnabled(); }
    void SetVisualDefectOptions(const videoeye::VisualDefectOptions& options);
    // 线程安全地取视觉缺陷采样选项副本: 解码线程逐帧读取, UI 线程写入,
    // 普通值类型直接跨线程访问存在数据竞争, 故加锁后返回副本。
    videoeye::VisualDefectOptions GetVisualDefectOptions() const;
    // 播完 / 停止时把还开着的缺陷段闭合（否则最后一段要等下一次播放才显示）
    void FlushVisualDefectSegments(double end_timestamp_seconds);

    // 硬件解码
    void SetHardwareDecodingEnabled(bool enable) { analysis_session_.SetHardwareDecodingEnabled(enable); }
    bool IsHardwareDecoding() const;
    std::string GetHwDeviceName() const;

    // 视频帧导出
    //
    // 若上一次抽帧的线程还没退出来，本次请求**排队**而不是立即启动：两次任务写的是
    // 同一个输出目录、文件名又只由帧序号决定，并行跑会互相覆盖产物。排队请求会在旧
    // 线程真正结束的 finished 回调里自动续跑（用户不必再点一次）。
    void StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality = 90, int frame_interval = 1);
    // 只置取消标志并请求线程退出，**不等待**：调用方在 UI 线程，绝不能被卡住。
    void CancelVideoFrameExport();

    // 音视频导出 (remux / transcode)。排队语义同 StartVideoFrameExport。
    void StartMediaExport(const exporter::ExportOptions& opt);
    // 同 CancelVideoFrameExport: 请求停止但不等待。
    void CancelMediaExport();

    // 取消**全部**导出（抽帧 + 音视频），并作废各自的排队请求与代际号。
    //
    // 谁该调它: 任何"媒体上下文要换了"的时刻 —— 打开新文件（OpenInternal 会自己调）、
    // 关闭/析构。以前这件事散在 UI 里做（MainWindow::OpenMedia 只调了抽帧那一半），
    // 于是"媒体导出进行中打开新文件"会留下一个仍在跑、且终态信号还能串回界面的旧任务。
    // 生命周期保证必须住在播放器里，UI 只负责重置自己的进度框。
    // 代际号一并推进: 旧任务迟到的进度/完成/错误信号会被代际校验丢弃，
    // 不会覆盖新媒体的界面状态。
    void CancelAllExports();

    // 渲染抑制: 播放区被隐藏时跳过画面输出 (sws_scale + FrameReady),
    // 解码线程、实时分析与音频照常运行; 重新展开播放区即恢复画面。
    void SetRenderingSuppressed(bool suppressed) {
        playback_session_.SetRenderingSuppressed(suppressed);
    }
    bool IsRenderingSuppressed() const {
        return playback_session_.IsRenderingSuppressed();
    }
    
    // 获取分析器
    //
    // 不再对外暴露可变的 StreamAnalyzer&: 它的内部状态由解码线程更新,
    // 外部拿可变引用等于开了一条绕锁的读写通道(历史曲线接口尤其明显)。
    // 需要统计就用 GetCurrentStats()/下面这些快照。
    const videoeye::StreamAnalyzer& stream_analyzer() const { return analysis_session_.stream_analyzer(); }
    videoeye::StreamStats GetCurrentStats() const;
    
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
    void StreamStatsReady(const videoeye::StreamStats& stats);
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
    // 结构分析**没出结果**时发出（解析失败 / 任务体抛异常）。
    // 与 Ready 互斥：Ready 出去时本信号一定不会来，反之亦然 —— 页面据此决定
    // 是画结构树还是显示一条错误。
    void ContainerStructureFailed(const QString& message);
    void MacroblockInfoReady(const videoeye::model::MacroblockFrameAnalysis& analysis);
    void SceneChangeReady(const videoeye::SceneChangeResult& result);
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
    // --- SetAnalysisFeature() 的分派目标 ---
    //
    // 各维度开关的实际实现。UI 不要直接调这些，一律走 SetAnalysisFeature()：
    // 那才是"按 AnalysisFeature 设开关"的唯一入口，也是新增维度时唯一要改的地方。
    // 其中几个不是单纯转发 —— VisualDefect / Macroblock / FrameType 还会启停工作
    // 线程或重置分析器状态，所以必须以函数而不是内联赋值的形式存在。
    void SetFrameTypeAnalysisEnabled(bool enable);
    void SetAudioFrameAnalysisEnabled(bool enable);
    void SetPacketAnalysisEnabled(bool enable);
    void SetEventAnalysisEnabled(bool enable);
    void SetSyncAnalysisEnabled(bool enable);
    void SetTimelineAnalysisEnabled(bool enable);
    void SetContainerStructureEnabled(bool enable);
    void SetMacroblockAnalysisEnabled(bool enable);
    void SetSceneChangeAnalysisEnabled(bool enable);
    void SetVisualDefectAnalysisEnabled(bool enable);

    bool OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options);

    // 导出的实际启动 / 排队 / 代际 / 取消全部在 ExportController 里
    // （见 core/player/ExportController.h），本类不保留任何导出状态。

    // 播放会话: demux / 解码 / 音频输出 / 解码线程 / 播放时钟 / 播放状态机
    // 全部住在 PlaybackSession 里; 状态、位置、时长都从它读。
    PlaybackSession playback_session_;

    // 播放信息
    model::StreamInfo stream_info_;
    int volume_ = 100;
    QString current_url_;
    QString last_open_error_;   // 最近一次 Open/OpenRawPcm 失败的详细原因

    // 打开媒体时的 FFmpeg 中断状态（打开/探测阶段带绝对超时）。
    //
    // 为什么是**成员**而不是 OpenInternal 里的局部变量: 装到 AVFormatContext 上的回调
    // 会被它派生出的 AVIOContext / URLContext 各复制一份，而上下文在打开之后归
    // 播放会话所有、解复用阶段仍在用 —— 指向栈上状态的 opaque 一返回就悬垂。
    // 作为成员，它的生命周期天然覆盖上下文；探测结束后 deadline 清零，于是对那些
    // 残留副本而言它只是个只响应取消的空钩子。
    //
    // open_cancel_ 是挂给上面的 cancel 的那个取消标志: 网络源卡在
    // avformat_open_input / av_read_frame 里时，deadline 只会"等到超时"，
    // 只有它能让阻塞的 IO 立刻退出来(见 MediaPlayer::Stop())。
    ffmpeg_io::AvInterruptState open_interrupt_;
    std::atomic<bool> open_cancel_{false};

    // 用户选择的定位方式 (菜单设置)。实际执行在 PlaybackSession::Seek()。
    std::atomic<model::SeekMode> seek_mode_{model::SeekMode::NearestKeyframe};
    
    // 分析器
    // 分析会话: 12 个分析开关 + StreamAnalyzer + 视觉缺陷采样选项 (见 AnalysisSession.h)。
    // MediaPlayer 经它转发开关/统计, 自身不再持有这些散落成员。
    AnalysisSession analysis_session_;

    // 播放期实时分析的全部编排（六个 hook / 19 个逐帧计数器 / 逐帧分析器 / 视觉缺陷
    // 采样与工作线程）住在 RealtimeAnalysisController 里，本类只转发入口与信号。
    // 必须声明在 analysis_session_ 与 playback_session_ **之后**: 控制器借用这两个会话
    // （只持引用），而成员按声明顺序初始化。
    RealtimeAnalysisController realtime_analysis_;

    // 后台任务统一调度: 任务 ID / 取消标志 / 终态 / 过期结果丢弃。
    // 容器结构分析走它的受管线程; 抽帧与媒体导出的 worker 是 QObject(要发进度信号),
    // 仍留在 QThread 上, 但生命周期(Begin/End/Cancel)也登记在这里。
    task::TaskManager task_manager_;

    // 抽帧 / 音视频导出的全部编排（发起、排队、代际、取消、worker 线程生命周期）
    // 住在 ExportController 里，本类只转发入口与信号。
    // 必须声明在 task_manager_ **之后**: 构造时把它的引用交给控制器, 而成员按声明顺序初始化。
    ExportController export_controller_;

    // 容器结构分析的后台编排（派发 / 作废 / 过期结果丢弃）住在
    // ContainerInspectionController 里，本类只负责"该不该分析"和转发两条信号。
    // 同上: 必须声明在 task_manager_ 之后。
    ContainerInspectionController container_inspection_;

    // 打开媒体的全部编排（超时中断 / 探测 / 选流 / 封面图 / 解码器初始化 / 流信息提取）
    // 住在 OpenController 里，本类只做"换媒体前的复位"与"打开成功后派发容器分析"。
    // 中断状态与取消标志仍由本类持有（见上方 open_interrupt_ 的注释），控制器只借引用。
    OpenController open_controller_;
};

} // namespace player
} // namespace videoeye
