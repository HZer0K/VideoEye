#pragma once

// 「问题」子视图：流媒体包检测出的问题表（严重度着色，高的排前面）。
//
// 从 StreamingPanel 拆出的展示型组件，只吃一份 StreamingPackageResult 快照。
// IssueCount() 给侧栏导航做徽标，口径是"问题表的行数"。

#include <QWidget>

#include "core/domain/model/StreamPackageInfo.h"

class QTableWidget;

namespace videoeye {
namespace ui {

class StreamingIssueView : public QWidget {
    Q_OBJECT

public:
    explicit StreamingIssueView(QWidget* parent = nullptr);

    void SetResult(const model::StreamingPackageResult& result);
    void Clear();

    int IssueCount() const;

private:
    QTableWidget* table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
