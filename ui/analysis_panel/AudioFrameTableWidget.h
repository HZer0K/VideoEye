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
// 表是 QTableView + RecordTableModel（见 RecordTableModel.h）而不是 QTableWidget：
// 音频帧在长视频里轻松到十万级，QTableWidget 的行数 = 记录数 = 十万 × 7 个 item，
// 内存与填充耗时都跟着线性涨。换到 model 之后单元格文本是 data() 时现算的，
// 只为可见的几十行生成。记录上限统一取 kDefaultMaxRecordCount（10 万）。

#include <QWidget>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTableView>

#include <cstddef>
#include <vector>

#include "ui/analysis_panel/RecordTableModel.h"
#include "ui/analysis_panel/StreamRecords.h"

namespace videoeye {
namespace ui {

class AudioFrameTableWidget : public QWidget {
    Q_OBJECT

public:
    explicit AudioFrameTableWidget(QWidget* parent = nullptr);
    ~AudioFrameTableWidget() override;

    // === 数据进来（由 FramePacketView 转发）===
    void ResetAudioFrames();
    void AppendAudioFrame(int index, qint64 pts, double timestamp_seconds,
                          int sample_count, int sample_rate, int channels, int byte_count);

    // === 增量刷新（由面板节拍驱动）===
    bool HasPending() const;
    void FlushPending();

    // 视图与模型：父页/测试要读行数与单元格都用 model()（QTableView 本身不存数据）。
    QTableView* audioFrameTable() const { return audio_frame_table_; }
    RecordTableModel<AudioFrameRecord>* audioFrameModel() const { return audio_frame_model_; }
    // 记录缓存的唯一所有者是模型；这里只是转发，避免外部再拿一份副本。
    const std::vector<AudioFrameRecord>& AudioFrameRecords() const;

    // 「启用分析」勾选框。必须外露：面板注入 SetFeatureHooks 后要拿它回写真实开关状态，
    // 否则界面与实际行为不一致（评审 P1）。
    QCheckBox* toggle() const { return audio_toggle_; }

private:
    void SetupUi();
    void UpdateAudioFrameSummary();
    void FlushPendingAudioFrameTableUpdates();
    void OnExportAudioFrameCsv();

    QLabel* audio_frame_summary_label_ = nullptr;
    QPushButton* export_audio_frame_csv_button_ = nullptr;
    QCheckBox* audio_toggle_ = nullptr;
    QTableView* audio_frame_table_ = nullptr;
    RecordTableModel<AudioFrameRecord>* audio_frame_model_ = nullptr;

    // 播放期回调只往这里入队 + 置脏；真正交给模型（一次 rowsInserted）在 FlushPending。
    // 这么分是为了保住 120ms 节流 —— 每帧直接 AppendRecords 会把批量通知拆回逐条。
    std::vector<AudioFrameRecord> pending_audio_frames_;
    bool audio_frame_table_dirty_ = false;
    bool audio_frame_summary_dirty_ = false;
};

}  // namespace ui
}  // namespace videoeye
