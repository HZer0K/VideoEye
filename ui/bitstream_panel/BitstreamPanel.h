#pragma once

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#include "core/model/BitstreamInfo.h"

namespace videoeye {
namespace ui {

// ==========================================================================
// BitstreamPanel - 编码参数集解析面板
//
// 数据来源：core/analyzer/BitstreamAnalyzer（只读 extradata，不解码），
// 结果由 AnalysisCoordinator 汇总到 AnalysisResult::bitstream_analysis。
//
// 布局：
//   顶部  一行摘要（codec / 分辨率 / profile / level / 位深 / 参数集 / 不一致数）
//   中部  左侧参数集结构树 + 右侧「容器值 vs 码流值」对比表
//   底部  不一致项表格（级别 / 字段 / 容器值 / 码流值 / 说明）
// ==========================================================================
class BitstreamPanel : public QWidget {
    Q_OBJECT

public:
    explicit BitstreamPanel(QWidget* parent = nullptr);
    ~BitstreamPanel() override;

    // 直接吃分析结果（AnalysisResult::bitstream_analysis）
    void SetResult(const model::BitstreamAnalysisResult& result);

    // 清空面板（关闭文件 / 开始新一次扫描时）
    void Clear();

    bool HasResult() const { return has_result_; }

    // 序列化：复制 JSON / 导出 JSON 文件共用
    QString ExportToJson() const;
    void CopyJsonToClipboard() const;

signals:
    // 用户点了「重新扫描」：由外层决定重新跑一次全文件扫描
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnCopyJsonClicked();
    void OnExportJsonClicked();

private:
    void UpdateSummary(const model::BitstreamAnalysisResult& result);
    void BuildStructureTree(const model::BitstreamAnalysisResult& result);
    void BuildComparisonTable(const model::BitstreamAnalysisResult& result);
    void BuildInconsistencyTable(const model::BitstreamAnalysisResult& result);

    // 摘要里那一行「codec / 分辨率 / profile / level」
    static QString DescribeCodec(const model::BitstreamAnalysisResult& result);

    // CICP（ISO/IEC 23001-8）枚举 -> 可读名，H.264 VUI / HEVC VUI / AV1 通用
    static QString PrimariesName(int value);
    static QString TransferName(int value);
    static QString MatrixName(int value);
    static QString ChromaName(int chroma_format_idc);
    static QString LevelName(const model::BitstreamAnalysisResult& result);

    QLabel* summary_label_ = nullptr;
    QTreeWidget* structure_tree_ = nullptr;
    QTableWidget* comparison_table_ = nullptr;
    QTableWidget* inconsistency_table_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* copy_json_button_ = nullptr;
    QPushButton* export_json_button_ = nullptr;

    model::BitstreamAnalysisResult current_result_;
    bool has_result_ = false;
};

}  // namespace ui
}  // namespace videoeye
