#pragma once

// 「码流分析」页的底部区：视频帧 / 包 / GOP 摘要 / 音频帧 四张明细表。
//
// 从 AnalysisPanel 拆出来的独立页面组件。它本身不是外部 QStackedWidget 的一页，
// 而是被 SetupBitstreamTab 与 StreamOverviewView 一起拼进「码流分析」页
// （顶部固定、底部可伸展）。四个子页已全部抽成独立组件，本页只负责：
//   - tab 容器：按原顺序 addTab（0 视频帧 / 1 包 / 2 GOP 摘要 / 3 音频帧），tab 序号
//     是帧表↔包表互跳 setCurrentIndex(0/1) 的依据，不能因搬家而变
//   - 采集开关：本页不再有自己的开关。整页（含上方流概览）由页头那一个聚合
//     「启用分析」统一控制，逐项过滤仍在 AnalysisPanel 按 feature 做
//   - 跨表联动：帧表 ↔ 包表按 PTS 互跳要同时碰两张表，协调留在本页
//   - 转发：数据 Reset*/Append*、刷新 HasPending()/FlushPending()（面板 120ms 节拍
//     驱动，避免逐行 insertRow 在主线程上触发 O(n²) 卡顿）、GopSummariesChanged
//
// 子页组件（每个自持记录缓存 / 脏标志 / 增量游标）：
//   - 子页 0（视频帧）+ 子页 2（GOP 摘要）→ VideoFrameTableWidget：GOP 是从解码帧
//     pict_type 推导的派生数据，必须和帧住在一起
//   - 子页 1（包）→ PacketTableWidget
//   - 子页 3（音频帧）→ AudioFrameTableWidget
//
// 数据出去：GopSummariesChanged —— GOP 摘要由 VideoFrameTableWidget 产出，
// 「流概览」区要用它填「最大GOP大小」并画「GOP 帧数分布」曲线。
// 两个视图不互相持有指针，只经面板连线。

#include <QWidget>

#include <QTabWidget>

#include <functional>
#include <vector>

#include "core/domain/model/PacketInfo.h"
#include "ui/analysis_panel/StreamRecords.h"

namespace videoeye {
namespace ui {

class AudioFrameTableWidget; // 前向声明：音频帧子页 (见 AudioFrameTableWidget.{h,cpp})
class PacketTableWidget;     // 前向声明：包子页 (见 PacketTableWidget.{h,cpp})
class VideoFrameTableWidget; // 前向声明：视频帧 + GOP 摘要子页 (见 VideoFrameTableWidget.{h,cpp})

class FramePacketView : public QWidget {
    Q_OBJECT

public:
    explicit FramePacketView(QWidget* parent = nullptr);

    // 本页没有自己的采集开关：整页由「播放统计与帧包」页头那一个聚合「启用分析」
    // 控制（见 StreamOverviewView::SetFeatureHooks）。子页不再挂同名勾选框。

    // GOP 摘要的最新快照（面板在收到 GopSummariesChanged 后转给「流概览」区）
    // 视频帧记录与 GOP 推导都在 VideoFrameTableWidget 里，本页只转发快照。
    // 定义在 .cpp：VideoFrameTableWidget 在这里只有前向声明，内联解引用不完整类型会编译失败。
    const std::vector<GopSummary>& GopSummaries() const;

    bool HasPending() const;
    void FlushPending();

    // 导出入口：本页页头只有 StreamOverviewView 那一个「导出」菜单，四个子页各自的
    // CSV 导出经这里并入同一菜单（子页内部不再各放一个导出按钮）。
    void ExportVideoFrameCsv();
    void ExportGopCsv();
    void ExportAudioFrameCsv();
    void ExportPacketCsv();

signals:
    // gop_summaries 发生变化（新增 GOP 段 / 被裁剪 / 被清空）时发出，由
    // VideoFrameTableWidget 在其 FlushPending 里统一发出，本页只转发，避免每个
    // GOP 边界都拷一次大向量。刻意不带参数：接收方（面板）用 GopSummaries()
    // 取快照再转交「流概览」区，这样信号里不出现自定义容器类型，不必额外注册元类型。
    void GopSummariesChanged();

public slots:
    void ResetVideoFrames();
    void AppendVideoFrame(int index, int frame_type, bool is_key_frame, qint64 pts, double timestamp_seconds);

    void ResetAudioFrames();
    void AppendAudioFrame(int index, qint64 pts, double timestamp_seconds, int sample_count, int sample_rate,
                          int channels, int byte_count);

    void ResetPackets();
    void AppendPacket(const model::PacketInfo& packet_info);

private:
    void SetupUi();

    // 包表选中 → 帧表按 PTS 跳转；帧表选中 → 包表按 PTS 跳转
    void OnPacketTableSelectionChanged();
    void OnVideoFrameTableSelectionChanged();

    QTabWidget* sub_tabs_ = nullptr;
    // 四个子页已全部抽为独立组件，本页只持有指针并转发数据。
    VideoFrameTableWidget* video_page_ = nullptr; // 视频帧 + GOP 摘要（子页 0 / 2）
    PacketTableWidget* packet_page_ = nullptr;    // 包（子页 1）
    AudioFrameTableWidget* audio_page_ = nullptr; // 音频帧（子页 3）

    bool linking_ = false; // 包/帧互跳回调重入保护
};

} // namespace ui
} // namespace videoeye