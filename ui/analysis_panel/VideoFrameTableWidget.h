#pragma once

// 「码流分析 → 视频帧」+「GOP 摘要」两个子页：帧明细表 + 汇总行 + CSV 导出 +
// 「启用分析」开关，以及由解码帧 pict_type 推导出的 GOP 分段表。
//
// 从 FramePacketView 抽出的独立子页组件 —— 原 FramePacketView 把视频帧 / 包 / GOP /
// 音频帧四张表连同各自的记录缓存、脏标志、增量游标全塞在一个类里，膨胀到 1100+ 行。
//
// 为什么视频帧与 GOP 放在同一个组件里：GOP 不是独立数据源，它是从帧记录的
// pict_type / is_key_frame 推导出来的派生结果 —— 帧记录被 TrimRecords 裁剪时 GOP
// 要一并清空重建，帧汇总行要显示 GOP 段数。拆成两个组件就得把整套帧记录再暴露一遍
// 让父页做传导，反而更绕。所以这里一个组件持两个子页 widget，由父页按原顺序 addTab。
//
// 对外接缝：
//   - 数据进来：ResetVideoFrames() / AppendVideoFrame()（面板转发播放期回吐的解码帧）
//   - 刷新出去：HasPending() / FlushPending()（由面板 120ms 节拍驱动，避免逐行 insertRow
//     在主线程上触发 O(n²) 卡顿）
//   - 派生数据出去：GopSummariesChanged —— GOP 摘要由本组件产出，「流概览」区要用它填
//     「最大GOP大小」并画「GOP 帧数分布」曲线。刻意不带参数，接收方用 GopSummaries()
//     取快照，信号里不出现自定义容器类型。
//   - 开关：toggle() 交回父页接线 —— 开关编号（0 = 视频帧）到 AnalysisFeature 的映射、
//     以及「钩子注入后回写真实状态」都由父页做，组件只认自己这个勾选框。
//   - 帧表 ↔ 包表按 PTS 互跳联动：这是跨两张表的事，组件只外露 frameTable() /
//     FrameRecords()，连线留在父页。
//
// 帧记录上限 5 万条（GOP 段上限取其 1/10），超限裁剪最早一段并整表重建
// （TrimRecords 见 AnalysisPageSupport.h）。

#include <QWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <cstddef>
#include <vector>

#include "ui/analysis_panel/StreamRecords.h"

namespace videoeye {
namespace ui {

class VideoFrameTableWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoFrameTableWidget(QWidget* parent = nullptr);

    // 两个子页 widget：由 FramePacketView 按原顺序 addTab 到它自己的 QTabWidget 上
    // （视频帧 = 0、包 = 1、GOP 摘要 = 2、音频帧 = 3），这样 tab 序号与既有的
    // setCurrentIndex(0/1) 互跳逻辑保持一致。
    QWidget* videoFramePage() const { return video_sub_; }
    QWidget* gopPage() const { return gop_sub_; }

    // === 数据进来（由 FramePacketView 转发）===
    void ResetVideoFrames();
    void AppendVideoFrame(int index, int frame_type, bool is_key_frame,
                          qint64 pts, double timestamp_seconds);

    // === 派生 / 刷新 ===
    const std::vector<GopSummary>& GopSummaries() const { return gop_summaries_; }
    // 包表 → 帧表按 PTS 互跳要用：父页需要一个"找最近帧"的记录源和一个可操作的表。
    const std::vector<VideoFrameRecord>& FrameRecords() const { return frame_records_; }
    QTableWidget* frameTable() const { return frame_table_; }

    bool HasPending() const;
    void FlushPending();

    // 「启用分析」勾选框。必须外露：父页注入 SetFeatureHooks 后要拿它回写真实开关状态，
    // 否则界面与实际行为不一致（评审 P1）。
    QCheckBox* toggle() const { return video_toggle_; }

signals:
    // gop_summaries_ 发生变化（新增 GOP 段 / 被裁剪 / 被清空）时发出，由 FlushPending
    // 在刷新 GOP 表之后统一发出，避免每个 GOP 边界都拷一次大向量。
    void GopSummariesChanged();

private slots:
    void OnFrameFilterChanged();
    void OnExportFrameCsv();
    void OnExportGopCsv();

private:
    void SetupUi();
    QString FrameTypeToString(int frame_type) const;
    bool MatchesFrameFilter(const VideoFrameRecord& record) const;

    void RebuildFrameTable();
    void UpdateFrameSummary();
    void FlushPendingFrameTableUpdates();
    void FlushPendingGopTableUpdates();
    void AppendFrameRowToTable(const VideoFrameRecord& record);
    void UpdateGopRowInTable(int row, const GopSummary& summary);

    QWidget* video_sub_ = nullptr;   // 子页 0: 视频帧
    QWidget* gop_sub_ = nullptr;     // 子页 2: GOP 摘要

    QComboBox* frame_filter_combo_ = nullptr;
    QLabel* frame_summary_label_ = nullptr;
    QTableWidget* frame_table_ = nullptr;
    QPushButton* export_frame_csv_button_ = nullptr;
    QCheckBox* video_toggle_ = nullptr;
    QTableWidget* gop_table_ = nullptr;

    // 帧记录缓存 + 增量同步游标（表格只在 FlushPending 时按游标补差量）
    std::vector<VideoFrameRecord> frame_records_;
    std::vector<GopSummary> gop_summaries_;
    std::size_t frame_table_synced_record_count_ = 0;
    std::size_t gop_table_synced_count_ = 0;

    bool frame_table_dirty_ = false;
    bool gop_table_dirty_ = false;
    bool frame_summary_dirty_ = false;
};

}  // namespace ui
}  // namespace videoeye
