#pragma once

// 码流页顶部：摘要行 + 「开始分析 / 复制 JSON / 导出 JSON」工具栏。
//
// 从 BitstreamPanel 拆出的展示型组件。摘要口径（codec / 分辨率 / profile / level /
// 位深 / 参数集数量 / 不一致数 / 能力徽标）在这里；JSON 的复制与落盘仍留在面板，
// 因为那两步要用 current_result_ 与文件对话框。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QPushButton>

#include "core/domain/model/BitstreamInfo.h"

namespace videoeye {
namespace ui {

class BitstreamSummaryView : public QWidget {
    Q_OBJECT

public:
    explicit BitstreamSummaryView(QWidget* parent = nullptr);

    // 喂入一次解析结果（analyzed=false 时按 parse_outcome 给不同引导文案）
    void SetResult(const model::BitstreamAnalysisResult& result);
    void Clear();

    // 全文件扫描进行中禁用「开始分析」，避免重复触发
    void SetRefreshEnabled(bool enabled);

signals:
    void RefreshRequested();
    void CopyJsonRequested();
    void ExportJsonRequested();

private:
    void SetupUi();

    QLabel* summary_label_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* copy_json_button_ = nullptr;
    QPushButton* export_json_button_ = nullptr;
};

} // namespace ui
} // namespace videoeye
