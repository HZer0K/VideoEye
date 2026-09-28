#pragma once

#include <atomic>
#include <mutex>

#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/VisualDefectAnalyzer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace player {

// 分析会话: 把播放过程中的"分析状态"从 MediaPlayer 里抽出来集中管理。
//
// 为什么存在: 评审指出 MediaPlayer 同时负责播放、分析、容器扫描、两种导出,
// 12 个分析开关 + StreamAnalyzer + 视觉缺陷采样选项散落在几十条成员/调用点里,
// 既难读也难验证. AnalysisSession 把这些状态收口到一个类:
//   - 12 个开关: 全部 std::atomic<bool>, UI 线程写、解码线程读, 单写多读零额外开销;
//   - StreamAnalyzer: 流统计 (码率/帧率/GOP/包), 委托其方法;
//   - VisualDefectOptions: 值类型, 跨线程访问经 GetVisualDefectOptions() 加锁取副本.
//
// 设计约定:
//   - MediaPlayer 的公共 API (EnableAnalysis / SetXxxEnabled / IsXxxEnabled /
//     GetStreamAnalyzer / GetVisualDefectOptions ...) 保持不变, 仅内部转发到本类,
//     因此 UI 层 (AnalysisPanel / PlayerPanel / MainWindow) 无需改动.
//   - 真正的视觉缺陷分析器 (VisualDefectAnalyzer) 仍留在 MediaPlayer: 它要发信号、
//     与解码线程的 Feed/Drain 流程耦合, 不适合塞进本类; 它读开关/选项时走本类的
//     IsVisualDefectAnalysisEnabled() / GetVisualDefectOptions().
class AnalysisSession {
public:
    AnalysisSession() = default;
    ~AnalysisSession() = default;

    // —— 分析开关 (线程安全, 单写多读) ——
    void SetAnalysisEnabled(bool e) {
        analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsAnalysisEnabled() const {
        return analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetFrameTypeAnalysisEnabled(bool e) {
        frame_type_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsFrameTypeAnalysisEnabled() const {
        return frame_type_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetAudioFrameAnalysisEnabled(bool e) {
        audio_frame_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsAudioFrameAnalysisEnabled() const {
        return audio_frame_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetPacketAnalysisEnabled(bool e) {
        packet_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsPacketAnalysisEnabled() const {
        return packet_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetEventAnalysisEnabled(bool e) {
        event_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsEventAnalysisEnabled() const {
        return event_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetSyncAnalysisEnabled(bool e) {
        sync_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsSyncAnalysisEnabled() const {
        return sync_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetTimelineAnalysisEnabled(bool e) {
        timeline_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsTimelineAnalysisEnabled() const {
        return timeline_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetContainerStructureEnabled(bool e) {
        container_structure_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsContainerStructureEnabled() const {
        return container_structure_enabled_.load(std::memory_order_relaxed);
    }

    void SetMacroblockAnalysisEnabled(bool e) {
        macroblock_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsMacroblockAnalysisEnabled() const {
        return macroblock_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetSceneChangeAnalysisEnabled(bool e) {
        scene_change_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsSceneChangeAnalysisEnabled() const {
        return scene_change_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetVisualDefectAnalysisEnabled(bool e) {
        visual_defect_analysis_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsVisualDefectAnalysisEnabled() const {
        return visual_defect_analysis_enabled_.load(std::memory_order_relaxed);
    }

    void SetHardwareDecodingEnabled(bool e) {
        hw_decoding_enabled_.store(e, std::memory_order_relaxed);
    }
    bool IsHardwareDecodingEnabled() const {
        return hw_decoding_enabled_.load(std::memory_order_relaxed);
    }

    // —— 视觉缺陷采样选项 (值类型, 跨线程加锁取副本) ——
    void SetVisualDefectOptions(const analyzer::VisualDefectOptions& options) {
        std::lock_guard<std::mutex> lk(visual_defect_options_mutex_);
        visual_defect_options_ = options;
    }
    analyzer::VisualDefectOptions GetVisualDefectOptions() const {
        std::lock_guard<std::mutex> lk(visual_defect_options_mutex_);
        return visual_defect_options_;
    }

    // —— StreamAnalyzer 委托 ——
    void Start() {
        stream_analyzer_.Start();
    }
    void Stop() {
        stream_analyzer_.Stop();
    }
    void Reset() {
        stream_analyzer_.Reset();
    }
    void AnalyzePacket(const AVPacket* packet, const AVFormatContext* format_ctx) {
        stream_analyzer_.AnalyzePacket(packet, format_ctx);
    }
    void AnalyzeVideoFrame(AVPictureType type) {
        stream_analyzer_.AnalyzeVideoFrame(type);
    }
    void AnalyzeAudioFrame() {
        stream_analyzer_.AnalyzeAudioFrame();
    }
    analyzer::StreamStats GetStats() const {
        return stream_analyzer_.GetStats();
    }

    // 暴露底层引用: 已有的 MediaPlayer 代码通过它访问 StreamAnalyzer 的具体方法
    // (GetRecentPackets / 历史曲线等), 避免再给每个方法都写一层转发.
    analyzer::StreamAnalyzer& stream_analyzer() {
        return stream_analyzer_;
    }
    const analyzer::StreamAnalyzer& stream_analyzer() const {
        return stream_analyzer_;
    }

private:
    std::atomic<bool> analysis_enabled_{false};
    std::atomic<bool> frame_type_analysis_enabled_{false};
    std::atomic<bool> audio_frame_analysis_enabled_{false};
    std::atomic<bool> packet_analysis_enabled_{false};
    std::atomic<bool> event_analysis_enabled_{false};
    std::atomic<bool> sync_analysis_enabled_{false};
    std::atomic<bool> timeline_analysis_enabled_{false};
    std::atomic<bool> container_structure_enabled_{true};
    std::atomic<bool> macroblock_analysis_enabled_{false};
    std::atomic<bool> scene_change_analysis_enabled_{false};
    std::atomic<bool> visual_defect_analysis_enabled_{false};
    std::atomic<bool> hw_decoding_enabled_{false};

    mutable std::mutex visual_defect_options_mutex_;
    analyzer::VisualDefectOptions visual_defect_options_;

    analyzer::StreamAnalyzer stream_analyzer_;
};

} // namespace player
} // namespace videoeye
