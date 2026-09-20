#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QTextEdit>
#include "core/analyzer/BitstreamAnalyzer.h"

namespace videoeye {
namespace ui {

// ==========================================================================
// Bitstream Panel - 码流分析面板
// 
// 功能：
//   1. Tree view 展示 SPS/PPS/VPS/OBU 层级结构
//   2. Table view 三列对比（container/bitstream/raw）
//   3. Warning 区域高亮显示不一致项
//   4. JSON 导出按钮
// ==========================================================================
class BitstreamPanel : public QWidget {
    Q_OBJECT

public:
    explicit BitstreamPanel(QWidget* parent = nullptr);
    ~BitstreamPanel();

    // 设置分析结果并更新 UI
    void SetAnalysisResult(const videoeye::analyzer::BitstreamAnalyzer& analyzer);
    
    // 清空面板
    void Clear();
    
    // 导出为 JSON
    QString ExportToJson() const;
    
    // 复制 JSON 到剪贴板
    void CopyJsonToClipboard() const;

signals:
    // 用户点击了"刷新"按钮
    void RefreshRequested();

private slots:
    void OnRefreshClicked();
    void OnCopyJsonClicked();
    void OnTreeWidgetItemDoubleClicked(QTreeWidgetItem* item, int column);

private:
    // 构建 Tree view
    void BuildStructureTreeView(const videoeye::model::BitstreamAnalysisResult& result);
    
    // 构建对比 Table
    void BuildComparisonTable(const videoeye::model::BitstreamAnalysisResult& result);
    
    // 显示不一致警告
    void ShowInconsistencies(const std::vector<videoeye::model::BitstreamAnalysisResult::Inconsistency>& inconsistencies);
    
    // 获取字段显示名
    QString GetFieldName(const std::string& field_name) const;
    
    // 根据 codec 类型获取图标
    QIcon GetCodecIcon(const std::string& codec_name) const;

    // UI 组件
    QTreeWidget* structure_tree_;           // 结构树
    QTableWidget* comparison_table_;       // 对比表
    QTextEdit* warning_text_;              // 警告区
    QPushButton* refresh_btn_;             // 刷新按钮
    QPushButton* copy_json_btn_;           // 复制 JSON 按钮
    
    // 当前分析结果
    videoeye::model::BitstreamAnalysisResult current_result_;
};

} // namespace ui
} // namespace videoeye
