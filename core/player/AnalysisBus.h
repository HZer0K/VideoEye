#pragma once

// 实时分析信号的对外契约：23 条（换媒体清空 6 + 分析结果 13 + 画面质量 4）。
//
// 为什么从 MediaPlayer 门面上摘下来（与 ExportBus / ContainerBus 同理）:
//   * 这是门面上最大的一块：一半以上的信号属于"播放期实时分析"，跟播放机械无关，
//     却要让门面替它们背 23 条声明 + 23 条转发连接；
//   * UI 里关心它的只有 MainWindow（转给 AnalysisPanel）和 PlayerPanel（电平 /
//     波形 / 实时统计）。连总线之后，门面只剩"播放 + 打开"这两件真正属于它的事。
//
// 由谁持有: RealtimeAnalysisController（"该不该发、计数到几"只有它知道），直接在
// 本对象上发射；MediaPlayer 经 analysis_bus() 把引用转给 UI。
// 总线与控制器同线程（UI 线程），连接投递方式与改之前一致（跨线程的仍自动排队）。
//
// ⚠️ 信号名与参数列表一个字节都不许改 —— 它们是 UI 的既有契约。
// ⚠️ 信号在解码线程发射、在 UI 线程落地，参数类型必须是已注册的元类型（值拷贝）。

#include <QObject>
#include <QString>

#include "core/analysis/stream/StreamAnalyzer.h" // videoeye::StreamStats
#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/domain/model/FrameTimingInfo.h" // PacketTiming / FrameTimingInfo
#include "core/domain/model/MacroblockInfo.h"  // MacroblockFrameAnalysis
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/QualityMetric.h"     // FrameQualityMetric / ActivePictureArea
#include "core/domain/model/SceneChangeResult.h" // model::SceneChangeResult
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"
#include "core/domain/model/VisualDefect.h" // model::VisualDefect

namespace videoeye {
namespace player {

class AnalysisBus : public QObject {
    Q_OBJECT

public:
    explicit AnalysisBus(QObject* parent = nullptr);

signals:
    // --- 换媒体时清空 UI 列表 ---
    void VideoFrameListReset();
    void AudioFrameListReset();
    void PacketListReset();
    void AnalysisEventListReset();
    void SyncSampleListReset();
    void TimelineEventListReset();

    // --- 实时分析结果 ---
    void StreamStatsReady(const videoeye::StreamStats& stats);
    void VideoFrameInfoReady(int index, int frame_type, bool is_key_frame, qint64 pts, double timestamp_seconds);
    void AudioFrameInfoReady(int index, qint64 pts, double timestamp_seconds, int sample_count, int sample_rate,
                             int channels, int byte_count);
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
    void SceneChangeReady(const model::SceneChangeResult& result);

    // --- 画面质量 / 视觉缺陷（实时播放时逐采样帧产出）---
    void VisualDefectReset();
    void VisualDefectFrameReady(const model::FrameQualityMetric& metric);
    void VisualDefectReady(const model::VisualDefect& defect);
    // 分析进度: 已分析帧数 / 被丢弃帧数 / 全片有效画面区域（约每秒刷新一次）
    void VisualDefectStatsReady(int analyzed_frames, int dropped_frames,
                                const model::ActivePictureArea& effective_area);
};

} // namespace player
} // namespace videoeye
