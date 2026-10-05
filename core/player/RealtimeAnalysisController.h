#pragma once

// 播放期的实时分析：逐包 / 逐帧的计数、判定、发信号，以及画面质量与视觉缺陷的采样编排。
//
// 为什么从 MediaPlayer 里拆出来: 这是 MediaPlayer 里"最容易长、又最不该长"的一块 ——
// 六个 hook（包 / 视频帧 / 音频帧 / 定位完成 / 播完 / 同步采样）各自串着若干个
// "开关判定 -> 计数 -> 填结构 -> 发信号"，再加 19 个逐帧计数器和 4 个分析器。
// 它们和播放状态机没有任何关系，却和播放状态机住在同一个类里：每加一个实时分析维度，
// 就要在 MediaPlayer 上同时改计数器、hook、Reset 与头文件。
//
// 拆出来之后的分工：
//   * 播放机械（demux / 解码 / pacing / 画面输出 / 状态机）—— PlaybackSession；
//   * 该不该分析（12 个开关 + StreamAnalyzer）—— AnalysisSession；
//   * 怎么分析、计到第几帧、发什么信号 —— 本类；
//   * 媒体生命周期（打开 / 定位 / 停止 / 导出 / 容器检查）—— MediaPlayer。
//
// 本类是 QObject: 信号必须跨线程排到 UI 线程，而"该不该发、计数到几"这些状态只有本类
// 知道，所以信号由本类发，MediaPlayer 在构造函数里原样转发 —— 对外契约（信号名与
// 参数）与拆分前一个字节不差。
//
// 线程模型（沿用拆分前，不要改）: 下面所有 OnXxx() 都在**解码线程**上被调用，
// 与 PlaybackSession 的 hook 语义一致；信号由 Qt 自动排队到 UI 线程。
// 因此这里的成员没有加锁：它们只在解码线程写，UI 线程只经信号读副本。
//
// 生命周期: 本类借用调用方的 AnalysisSession 与 PlaybackSession（只持引用），
// 不拥有任何线程 —— 视觉缺陷分析器的工作线程由它自己管（StopWorker 在析构路径上）。

#include <QObject>
#include <QString>

#include <chrono>
#include <map>

#include "core/analysis/quality/MacroblockAnalyzer.h"
#include "core/analysis/quality/SceneChangeAnalyzer.h"
#include "core/analysis/quality/VisualDefectAnalyzer.h"
#include "core/player/AnalysisSession.h"
#include "core/player/AudioVisualizer.h"
#include "core/player/PlaybackSession.h"
// 信号参数里出现的 model 值类型。"谁用谁 include" —— 以前这些是靠 MediaPlayer.h
// 的一堆 include 顺带带进来的，拆出来之后不能继续蹭。
#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/domain/model/FrameTimingInfo.h"   // PacketTiming / FrameTimingInfo / kNoTimestamp
#include "core/domain/model/MacroblockInfo.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/QualityMetric.h"     // FrameQualityMetric / VisualDefect / ActivePictureArea / FrameSample
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"

namespace videoeye {
namespace player {

class RealtimeAnalysisController : public QObject {
    Q_OBJECT

public:
    // analysis: 12 个分析开关 + StreamAnalyzer（引用，必须活过本对象）。
    // playback: 播放会话（引用）。本类只在构造时装 hook、运行时读音/视频流序号与当前位置，
    //   不参与播放状态机的任何决策。
    RealtimeAnalysisController(AnalysisSession& analysis, PlaybackSession& playback,
                               QObject* parent = nullptr);

    RealtimeAnalysisController(const RealtimeAnalysisController&) = delete;
    RealtimeAnalysisController& operator=(const RealtimeAnalysisController&) = delete;

    // --- 媒体生命周期的两个收尾点（由 MediaPlayer 在对应时机调用）---

    // 换媒体 / 重新打开: 19 个逐帧计数器、"已报告过"标志、逐帧分析器全部复位，
    // 并发出清空 UI 列表的信号（六条 ListReset + VisualDefectReset）。
    //
    // 为什么整体放在这里而不是留在 MediaPlayer: 计数器和分析器现在都在本类，
    // 让调用方逐项重置等于把本类的内部状态摊开在别人家；而"哪些列表要清"又取决于
    // 哪些开关开着，这件事同样只有本类知道。
    void ResetForNewMedia();

    // 停止播放: 把还开着的视觉缺陷段闭合，否则最后一条缺陷要等下一次播放才显示。
    void FlushOnStop();

    // --- 画面质量 / 视觉缺陷 ---

    // 开关会顺带启停分析用的工作线程；换采样档位用 SetVisualDefectOptions()。
    void SetVisualDefectAnalysisEnabled(bool enable);
    void SetVisualDefectOptions(const analyzer::VisualDefectOptions& options);
    // 线程安全地取采样选项副本: 解码线程逐帧读取, UI 线程写入,
    // 普通值类型直接跨线程访问存在数据竞争, 故加锁后返回副本。
    analyzer::VisualDefectOptions GetVisualDefectOptions() const;
    // 播完 / 停止时把还开着的缺陷段闭合
    void FlushVisualDefectSegments(double end_timestamp_seconds);

signals:
    // --- 换媒体时清空 UI 列表 ---
    void VideoFrameListReset();
    void AudioFrameListReset();
    void PacketListReset();
    void AnalysisEventListReset();
    void SyncSampleListReset();
    void TimelineEventListReset();

    // --- 实时分析结果 ---
    void StreamStatsReady(const analyzer::StreamStats& stats);
    void VideoFrameInfoReady(int index, int frame_type, bool is_key_frame, qint64 pts,
                             double timestamp_seconds);
    void AudioFrameInfoReady(int index, qint64 pts, double timestamp_seconds,
                             int sample_count, int sample_rate, int channels, int byte_count);
    void PacketInfoReady(const model::PacketInfo& packet_info);
    void AnalysisEventReady(const model::AnalysisEvent& event_info);
    void SyncSampleReady(const model::SyncSample& sample);
    // 时间轴与同步诊断（demux 层 packet 时间 / decode 层 frame 时间）
    void TimelinePacketReady(const model::PacketTiming& timing);
    void FrameTimingReady(const model::FrameTimingInfo& timing);
    void TimelineEventReady(const model::TimelineEvent& event);
    void AudioVisualizationReady(const model::AudioVisualizationFrame& frame);
    void AudioLevelReady(double level, double timestamp_seconds);
    void MacroblockInfoReady(const videoeye::model::MacroblockFrameAnalysis& analysis);
    void SceneChangeReady(const analyzer::SceneChangeResult& result);
    // 画面质量 / 视觉缺陷（实时播放时逐采样帧产出）
    void VisualDefectReset();
    void VisualDefectFrameReady(const model::FrameQualityMetric& metric);
    void VisualDefectReady(const model::VisualDefect& defect);
    // 分析进度: 已分析帧数 / 被丢弃帧数 / 全片有效画面区域（约每秒刷新一次）
    void VisualDefectStatsReady(int analyzed_frames, int dropped_frames,
                                const model::ActivePictureArea& effective_area);

private:
    // --- PlaybackSession 的回调入口（解码线程）---
    void InstallHooks();
    void OnPacket(const PacketContext& ctx);
    void OnVideoFrame(const VideoFrameContext& ctx);
    void OnAudioFrame(const AudioFrameContext& ctx);
    void OnSeekDone(double target_ms, model::SeekMode mode);
    void OnEndOfStream();

    // --- 四类"计数 + 填结构 + 发信号"的统一出口 ---
    void EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                           qint64 pts, double timestamp_seconds,
                           const QString& summary, const QString& detail = QString());
    void EmitSyncSample(double audio_timestamp_seconds, double video_timestamp_seconds,
                        bool audio_anchor);
    void EmitTimelineEvent(const QString& category, double timestamp_seconds,
                           const QString& label, const QString& detail = QString());
    void EmitAudioVisualization(const AudioVisualizationResult& vis_result,
                                int sample_rate, int channels, double timestamp_seconds,
                                double level);

    // --- 画面质量 / 视觉缺陷: 按采样档位抽取解码帧 -> 降采样 -> 投递分析器 ---
    void FeedVisualDefectFrame(const AVFrame* frame, double timestamp_seconds, bool audio_silent);
    // 把分析器已产出的指标 / 缺陷转发成信号（在解码线程调用）
    void DrainVisualDefectResults();
    // 分析进度信号（内部做 1 秒节流，force=true 时立即发）
    void EmitVisualDefectStats(bool force);

    AnalysisSession& analysis_;
    PlaybackSession& playback_;

    // 逐帧分析器
    analyzer::MacroblockAnalyzer macroblock_analyzer_;
    analyzer::SceneChangeAnalyzer scene_change_analyzer_;
    analyzer::VisualDefectAnalyzer visual_defect_analyzer_;
    AudioVisualizer audio_visualizer_;

    // --- 逐帧计数器 ---
    //
    // 全部只在解码线程读写（UI 只经信号拿副本），所以不加锁。
    // 换媒体时由 ResetForNewMedia() 一次性清零 —— 逐个漏掉一个的代价是"索引从
    // 上一份媒体的值继续涨"，界面上表现为序号跳变、定位后统计重复累计。
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
    int audio_timeline_sample_counter_ = 0;

    // 时间戳诊断的逐流状态: 上一包时间（查回退/跳变）与"已报告过"标志
    // （缺失时间戳每流只报一次，否则一个坏文件能刷出上万条事件）
    std::map<int, double> last_packet_ts_by_stream_;
    std::map<int, bool> missing_packet_ts_reported_;
    std::map<int, bool> missing_audio_pts_reported_;
};

}  // namespace player
}  // namespace videoeye
