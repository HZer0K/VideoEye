#pragma once

// 码流页底部：「容器与码流不一致项」标题 + 表格（级别 / 字段 / 容器值 / 码流值 / 说明）。
//
// 从 BitstreamPanel 拆出的展示型组件。行数同时是侧栏导航的异常计数
// （BitstreamPanel::IssueCount），所以对外暴露 RowCount()。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QTableWidget>

#include "core/domain/model/BitstreamInfo.h"

namespace videoeye {
namespace ui {

class BitstreamInconsistencyView : public QWidget {
    Q_OBJECT

public:
    explicit BitstreamInconsistencyView(QWidget* parent = nullptr);

    void SetResult(const model::BitstreamAnalysisResult& result);
    void Clear();
    int RowCount() const;

private:
    void SetupUi();
    void BuildTable(const model::BitstreamAnalysisResult& result);

    QLabel* title_ = nullptr;
    QTableWidget* inconsistency_table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
