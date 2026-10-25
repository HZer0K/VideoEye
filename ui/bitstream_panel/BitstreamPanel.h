#pragma once

#include <QString>
#include <QWidget>

#include "core/domain/model/BitstreamInfo.h"
#include "ui/analysis_panel/ScanClient.h"

namespace videoeye {
namespace ui {

class BitstreamSummaryView;
class BitstreamStructureView;
class BitstreamComparisonView;
class BitstreamInconsistencyView;

// ==========================================================================
// BitstreamPanel - 编码参数集解析面板
//
// 数据来源：core/analysis/codec/BitstreamAnalyzer.h（只读 extradata，不解码），
// 结果由 QtAnalysisController 汇总到 AnalysisResult::bitstream_analysis。
//
// 布局已按职责拆成四个视图组件（见 ui/bitstream_panel/Bitstream*View.h）：
//   顶部  摘要行 + 复制/导出 JSON 工具栏（BitstreamSummaryView）
//   中部  左侧参数集结构树 + 右侧「容器值 vs 码流值」对比表（BitstreamStructureView /
//         BitstreamComparisonView，装进同一个 QSplitter）
//   底部  不一致项表格（BitstreamInconsistencyView）
// 本页只保留结果缓存、JSON 序列化/落盘与扫描态。
// ==========================================================================
class BitstreamPanel : public QWidget, public ScanClient {
    Q_OBJECT

public:
    explicit BitstreamPanel(QWidget* parent = nullptr);
    ~BitstreamPanel() override;

    // 直接吃分析结果（AnalysisResult::bitstream_analysis）
    void SetResult(const model::BitstreamAnalysisResult& result);

    // 清空面板（关闭文件 / 开始新一次扫描时）
    void Clear();

    bool HasResult() const {
        return has_result_;
    }

    // ScanClient：本页共用诊断页发起的那次全文件扫描，扫描中「开始分析」按钮
    // 跟着禁用，终态一起复位。
    void SetScanActive(bool active) override;
    // 侧栏导航标记：容器/码流不一致表的行数
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
    void BuildUi();

    BitstreamSummaryView* summary_view_ = nullptr;
    BitstreamStructureView* structure_view_ = nullptr;
    BitstreamComparisonView* comparison_view_ = nullptr;
    BitstreamInconsistencyView* inconsistency_view_ = nullptr;

    model::BitstreamAnalysisResult current_result_;
    bool has_result_ = false;
    bool scan_active_ = false;
};

} // namespace ui
} // namespace videoeye
