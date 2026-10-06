#pragma once

// 「码流分析」页的底部区：视频帧 / 包 / GOP 摘要 / 音频帧 四张明细表。
//
// 从 AnalysisPanel 拆出来的独立页面组件。它本身不是外部 QStackedWidget 的一页，
// 而是被 SetupBitstreamTab 与 StreamOverviewView 一起拼进「码流分析」页
// （顶部固定、底部可伸展）。接缝：
//   - 数据进来：Reset*/Append*（播放期逐次回吐包/帧信息）
//   - 开关出去：面板通过 SetFeatureHooks 注入，视图只认自己的编号
//               0 = 视频帧、1 = 音频帧、2 = 数据包
//   - 数据出去：GopSummariesChanged —— GOP 摘要由本视图从解码帧 pict_type 推导产出，
//               「流概览」区要用它填「最大GOP大小」并画「GOP 帧数分布」曲线。
//               两个视图不互相持有指针，只经面板连线。
//   - 帧表 ↔ 包表按 PTS 互跳联动是这一页内部的事，不外露。
//
// 记录缓存有上限（帧 5 万 / 音频帧 3 万 / 包 1 万），超限裁剪掉最早的一段并重置表格；
// 所有表格刷新统一走 FlushPending()，由面板的 120ms 节拍驱动，避免逐行 insertRow
// 在主线程上触发 O(n²) 卡顿。

#include <QWidget>
#include <QString>

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include <cstddef>
#include <functional>
#include <vector>

#include "core/domain/model/PacketInfo.h"
#include "ui/analysis_panel/StreamRecords.h"

class QCheckBox;

namespace videoeye {
namespace ui {

class AudioFrameTableWidget;  // 前向声明：音频帧子页 (见 AudioFrameTableWidget.{h,cpp})
class VideoFrameTableWidget;  // 前向声明：视频帧 + GOP 摘要子页 (见 VideoFrameTableWidget.{h,cpp})

class FramePacketView : public QWidget {
    Q_OBJECT

public:
    explicit FramePacketView(QWidget* parent = nullptr);

    // 页面只认自己的开关编号（0 = 视频帧、1 = 音频帧、2 = 数据包）。编号到
    // AnalysisFeature 的映射、以及 AnalysisFeatureToggled 的转发都由面板做。
    // 注入时会立即把真实开关状态回写到三个「启用分析」勾选框：控件是在构造函数里
    // 建的，那时钩子还不存在。
    void SetFeatureHooks(std::function<bool(int)> is_enabled,
                         std::function<void(int, bool)> set_enabled);

    // GOP 摘要的最新快照（面板在收到 GopSummariesChanged 后转给「流概览」区）
    // 视频帧记录与 GOP 推导都在 VideoFrameTableWidget 里，本页只转发快照。
    // 定义在 .cpp：VideoFrameTableWidget 在这里只有前向声明，内联解引用不完整类型会编译失败。
    const std::vector<GopSummary>& GopSummaries() const;

    bool HasPending() const;
    void FlushPending();

signals:
    // gop_summaries_ 发生变化（新增 GOP 段 / 被裁剪 / 被清空）时发出，由 FlushPending
    // 在刷新 GOP 表之后统一发出，避免每个 GOP 边界都拷一次大向量。
    // 刻意不带参数：接收方（面板）用 GopSummaries() 取快照再转交「流概览」区，
    // 这样信号里不出现自定义容器类型，不必额外注册元类型。
    void GopSummariesChanged();

public slots:
    void ResetVideoFrames();
    void AppendVideoFrame(int index, int frame_type, bool is_key_frame,
                          qint64 pts, double timestamp_seconds);

    void ResetAudioFrames();
    void AppendAudioFrame(int index, qint64 pts, double timestamp_seconds,
                          int sample_count, int sample_rate, int channels, int byte_count);

    void ResetPackets();
    void AppendPacket(const model::PacketInfo& packet_info);

private:
    void SetupUi();
    void SyncTogglesFromHooks();

    void RebuildPacketTable();
    void UpdatePacketSummary();

    QString PacketFlagsToString(int flags) const;
    QString PacketStreamTypeToName(int type) const;
    bool PacketMatchesFilter(const PacketRecord& record) const;

    void FlushPendingPacketTableUpdates();
    void AppendPacketRowToTable(const PacketRecord& record);

    void OnExportPacketCsv();

    // 包表选中 → 帧表按 PTS 跳转；帧表选中 → 包表按 PTS 跳转
    void OnPacketTableSelectionChanged();
    void OnVideoFrameTableSelectionChanged();

    std::function<bool(int)> is_enabled_;
    std::function<void(int, bool)> set_enabled_;

    QTabWidget* sub_tabs_ = nullptr;
    QWidget* packet_sub_ = nullptr;     // 子页 1: 包
    // 子页 0（视频帧）与子页 2（GOP 摘要）已抽为 VideoFrameTableWidget，
    // 子页 3（音频帧）已抽为 AudioFrameTableWidget，本页只持有指针并转发数据。

    QLabel* packet_summary_label_ = nullptr;
    QComboBox* packet_filter_combo_ = nullptr;
    QTableWidget* packet_table_ = nullptr;
    QPushButton* export_packet_csv_button_ = nullptr;

    // 「启用分析」勾选框（视频帧 / 音频帧那两个在子页组件里，经各自的 toggle() 取）。
    // 必须留成员：SetFeatureHooks 注入钩子后要拿它们把真实开关状态回写到界面，
    // 否则界面与实际行为不一致（评审 P1）。
    QCheckBox* packet_toggle_ = nullptr;

    // 已抽出的子页组件：记录缓存 / 脏标志 / 增量游标 / CSV 导出全在里面，本页只转发。
    VideoFrameTableWidget* video_page_ = nullptr;  // 视频帧 + GOP 摘要
    AudioFrameTableWidget* audio_page_ = nullptr;  // 音频帧

    int packet_filter_mode_ = -1;  // -1=全部, 0=视频流, 1=音频流, 2=其他流
    bool linking_ = false;         // 包/帧互跳回调重入保护

    // 记录缓存 + 增量同步游标（表格只在 FlushPending 时按游标补差量）
    // 视频帧 / GOP / 音频帧的记录缓存已随各自子页抽出，这里只剩包。
    std::vector<PacketRecord> packet_records_;

    bool packet_table_dirty_ = false;
    bool packet_summary_dirty_ = false;

    std::size_t packet_table_synced_record_count_ = 0;
};

}  // namespace ui
}  // namespace videoeye
