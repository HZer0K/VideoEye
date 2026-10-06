#pragma once

// 「码流分析 → 音频帧」子页：音频帧明细表 + 汇总行 + CSV 导出 + 「启用分析」开关。
//
// 从 FramePacketView 抽出的独立子页组件 —— 原 FramePacketView 把视频帧 / 包 / GOP /
// 音频帧四张表连同各自的记录缓存、脏标志、增量游标全塞在一个类里，膨胀到 1100+ 行。
// 本组件只管音频帧这一张表，对外接缝：
//   - 数据进来：ResetAudioFrames() / AppendAudioFrame()（面板转发播放期回吐的音频帧）
//   - 刷新出去：HasPending() / FlushPending()（由面板 120ms 节拍驱动，避免逐行 insertRow
//     在主线程上触发 O(n²) 卡顿）
//   - 开关：toggle() 交回面板接线 —— 开关编号（1 = 音频帧）到 AnalysisFeature 的映射、
//     以及「钩子注入后回写真实状态」都由面板做，组件只认自己这个勾选框。
//
// 记录缓存上限 3 万条，超限裁剪最早一段并整表重建（TrimRecords 见 AnalysisPageSupport.h）。

#include <QWidget>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <cstddef>
#include <vector>

#include "ui/analysis_panel/StreamRecords.h"

namespace videoeye {
namespace ui {

class AudioFrameTableWidget : public QWidget {
    Q_OBJECT

public:
    explicit AudioFrameTableWidget(QWidget* parent = nullptr);

    // === 数据进来（由 FramePacketView 转发）===
    void ResetAudioFrames();
    void AppendAudioFrame(int index, qint64 pts, double timestamp_seconds,
                          int sample_count, int sample_rate, int channels, int byte_count);

    // === 增量刷新（由面板节拍驱动）===
    bool HasPending() const;
    void FlushPending();

    // 「启用分析」勾选框。必须外露：面板注入 SetFeatureHooks 后要拿它回写真实开关状态，
    // 否则界面与实际行为不一致（评审 P1）。
    QCheckBox* toggle() const { return audio_toggle_; }

private:
    void SetupUi();
    void UpdateAudioFrameSummary();
    void FlushPendingAudioFrameTableUpdates();
    void AppendAudioFrameRowToTable(const AudioFrameRecord& record);
    void OnExportAudioFrameCsv();

    QLabel* audio_frame_summary_label_ = nullptr;
    QPushButton* export_audio_frame_csv_button_ = nullptr;
    QCheckBox* audio_toggle_ = nullptr;
    QTableWidget* audio_frame_table_ = nullptr;

    // 记录缓存 + 增量同步游标（表格只在 FlushPending 时按游标补差量）
    std::vector<AudioFrameRecord> audio_frame_records_;
    bool audio_frame_table_dirty_ = false;
    bool audio_frame_summary_dirty_ = false;
    std::size_t audio_frame_table_synced_record_count_ = 0;
};

}  // namespace ui
}  // namespace videoeye
