#pragma once

// 「证据与覆盖」子页：每个分析维度一行，回答"报告里的结论覆盖了多少内容"。
//
// 从 DiagnosticsPage 拆出的展示型组件。列全部维度（含未执行的），不做过滤 ——
// 只列异常的表格没法证明"没漏"，而那正是这张表的用途。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QTableWidget>

#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace ui {

class EvidenceCoverageView : public QWidget {
    Q_OBJECT

public:
    explicit EvidenceCoverageView(QWidget* parent = nullptr);

    // nullptr = 尚无扫描结果（回到引导文案 + 空表）
    void SetResult(const model::AnalysisResult* result);

private:
    void SetupUi();
    void RebuildTable();

    QLabel* hint_ = nullptr;
    QTableWidget* table_ = nullptr;
    // 页面保证引用在下次 SetResult 前有效（facade 的结果快照）
    const model::AnalysisResult* result_ = nullptr;
};

} // namespace ui
} // namespace videoeye
