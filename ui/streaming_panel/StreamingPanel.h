#pragma once

#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include "core/domain/model/StreamPackageInfo.h"
#include "ui/analysis_panel/ScanClient.h"

namespace videoeye {
namespace ui {

class StreamingStructureView;
class StreamingLadderView;
class StreamingTimelineView;
class StreamingIssueView;

// ==========================================================================
// StreamingPanel - 流媒体包（HLS / DASH）面板
//
// 数据来源: ContainerStructureResult::streaming_package
//   （由 ContainerStructureAnalyzer 分发到 HlsManifestAnalyzer /
//    DashManifestAnalyzer，再经 SegmentQcAnalyzer 做分片级与 ladder 级校验）
//
// 布局:
//   顶部  一行摘要（清单类型 / 码率层数 / 分片数 / 问题数）
//   中部  左侧 manifest 结构树，右侧「码率阶梯表 + 分片时间轴对齐表」
//   底部  问题列表（级别着色）
//
// 本类只保留摘要、工具栏、扫描态与数据分发；四块展示内容各自拆成子视图
// （结构树 / 码率阶梯 / 分片时间轴 / 问题表），子视图都不持有 ScanClient，
// 也不认识扫描生命周期 —— 数据由本页按快照转发。
// ==========================================================================
class StreamingPanel : public QWidget, public ScanClient {
    Q_OBJECT

public:
    explicit StreamingPanel(QWidget* parent = nullptr);
    ~StreamingPanel() override;

    // 直接吃分析结果
    void SetResult(const model::StreamingPackageResult& result);

    // 清空面板（关闭文件 / 开始新一次扫描时）
    void Clear();

    bool HasResult() const {
        return has_result_;
    }

    // ScanClient：本页共用诊断页发起的那次全文件扫描，扫描中「开始分析」按钮
    // 跟着禁用，终态一起复位。
    void SetScanActive(bool active) override;
    // 侧栏导航标记：问题表的行数
    int IssueCount() const override;
    bool IsScanActive() const {
        return scan_active_;
    }

    // 序列化：复制 JSON / 导出 JSON 文件共用
    QString ExportToJson() const;
    void CopyJsonToClipboard() const;

signals:
    // 用户点了「开始分析」：由外层决定重新跑一次全文件扫描
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnCopyJsonClicked();
    void OnExportJsonClicked();

private:
    void UpdateSummary(const model::StreamingPackageResult& result);

    QLabel* summary_label_ = nullptr;
    StreamingStructureView* structure_view_ = nullptr;
    StreamingLadderView* ladder_view_ = nullptr;
    StreamingTimelineView* timeline_view_ = nullptr;
    StreamingIssueView* issue_view_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* copy_json_button_ = nullptr;
    QPushButton* export_json_button_ = nullptr;

    model::StreamingPackageResult current_result_;
    bool has_result_ = false;
    bool scan_active_ = false;
};

} // namespace ui
} // namespace videoeye
