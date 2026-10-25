#pragma once

// 「码流分析 → 包」子页：包明细表 + 汇总行 + 筛选（全部/视频/音频/其他）+ CSV 导出
// + 「启用分析」开关。
//
// 从 FramePacketView 抽出的最后一个子页组件 —— 原 FramePacketView 把视频帧 / 包 /
// GOP / 音频帧四张表连同各自的记录缓存、脏标志、增量游标全塞在一个类里，膨胀到
// 1100+ 行；四个子页依次抽成组件后，本页退化为 tab 容器 + 开关路由 + 跨表联动协调。
// 本组件只管包这一张表，对外接缝：
//   - 数据进来：ResetPackets() / AppendPacket()（面板转发播放期回吐的包信息）
//   - 刷新出去：HasPending() / FlushPending()（由面板 120ms 节拍驱动，避免逐行
//     insertRow 在主线程上触发 O(n²) 卡顿）
//   - 跨表互跳：packetTable() / packetModel() 交回父页做「帧表 ↔ 包表按 PTS 互跳」
//     —— 双向联动要同时碰帧表与包表，协调留在 FramePacketView。
//     注意互跳要的是"包表的第 N 行对应哪条记录"，筛选生效时行号 ≠ 记录索引，
//     所以父页必须走 RecordIndexOfRow() 而不是直接拿 row 当下标。
//   - 开关：toggle() 交回面板接线 —— 开关编号（2 = 数据包）到 AnalysisFeature 的映射、
//     以及「钩子注入后回写真实状态」都由面板做，组件只认自己这个勾选框。
//
// 表是 QTableView + RecordTableModel（见 RecordTableModel.h）：包记录比帧还多
// （一帧往往对应多个包），十万级下 QTableWidget 的"行数 = item 数"同样撑不住。
// 记录上限统一取 kDefaultMaxRecordCount。

#include <QWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableView>

#include <cstddef>
#include <vector>

#include "core/domain/model/PacketInfo.h"
#include "ui/analysis_panel/RecordTableModel.h"
#include "ui/analysis_panel/StreamRecords.h"

namespace videoeye {
namespace ui {

class PacketTableWidget : public QWidget {
    Q_OBJECT

public:
    explicit PacketTableWidget(QWidget* parent = nullptr);
    ~PacketTableWidget() override;

    // === 数据进来（由 FramePacketView 转发）===
    void ResetPackets();
    void AppendPacket(const model::PacketInfo& packet_info);

    // === 增量刷新（由面板节拍驱动）===
    bool HasPending() const;
    void FlushPending();

    // === 跨表互跳（父页同时碰帧表与包表做 PTS 联动）===
    QTableView* packetTable() const {
        return packet_table_;
    }
    RecordTableModel<PacketRecord>* packetModel() const {
        return packet_model_;
    }
    const std::vector<PacketRecord>& PacketRecords() const;

    // 「启用分析」勾选框。必须外露：面板注入 SetFeatureHooks 后要拿它回写真实开关状态，
    // 否则界面与实际行为不一致（评审 P1）。
    QCheckBox* toggle() const {
        return packet_toggle_;
    }

private:
    void SetupUi();

    void ApplyFilter();
    void UpdatePacketSummary();

    QString PacketFlagsToString(int flags) const;
    QString PacketStreamTypeToName(int type) const;
    bool PacketMatchesFilter(const PacketRecord& record) const;

    void FlushPendingPacketTableUpdates();

    void OnExportPacketCsv();

    QLabel* packet_summary_label_ = nullptr;
    QComboBox* packet_filter_combo_ = nullptr;
    QTableView* packet_table_ = nullptr;
    RecordTableModel<PacketRecord>* packet_model_ = nullptr;
    QPushButton* export_packet_csv_button_ = nullptr;
    QCheckBox* packet_toggle_ = nullptr;

    int packet_filter_mode_ = -1; // -1=全部, 0=视频流, 1=音频流, 2=其他流

    // 播放期回调只入队 + 置脏，FlushPending 时一次性交给模型
    std::vector<PacketRecord> pending_packets_;
    bool packet_table_dirty_ = false;
    bool packet_summary_dirty_ = false;
};

} // namespace ui
} // namespace videoeye
