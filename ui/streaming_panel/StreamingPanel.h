#pragma once

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#include "core/model/StreamPackageInfo.h"

namespace videoeye {
namespace ui {

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
// ==========================================================================
class StreamingPanel : public QWidget {
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

    // 序列化：复制 JSON / 导出 JSON 文件共用
    QString ExportToJson() const;
    void CopyJsonToClipboard() const;

signals:
    // 用户点了「重新扫描」：由外层决定重新跑一次分析
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnCopyJsonClicked();
    void OnExportJsonClicked();

private:
    void UpdateSummary(const model::StreamingPackageResult& result);
    void BuildStructureTree(const model::StreamingPackageResult& result);
    void BuildLadderTable(const model::StreamingPackageResult& result);
    void BuildTimelineTable(const model::StreamingPackageResult& result);
    void BuildIssueTable(const model::StreamingPackageResult& result);

    static QString SeverityText(model::IssueSeverity severity);
    static QColor SeverityColor(model::IssueSeverity severity);
    static QString DescribeResolution(int width, int height);

    QLabel* summary_label_ = nullptr;
    QTreeWidget* structure_tree_ = nullptr;
    QTableWidget* ladder_table_ = nullptr;
    QTableWidget* timeline_table_ = nullptr;
    QTableWidget* issue_table_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* copy_json_button_ = nullptr;
    QPushButton* export_json_button_ = nullptr;

    model::StreamingPackageResult current_result_;
    bool has_result_ = false;
};

} // namespace ui
} // namespace videoeye
