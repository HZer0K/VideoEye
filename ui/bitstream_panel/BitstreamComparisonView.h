#pragma once

// 码流页中部右侧：「容器值 vs 码流值」对比表（字段 / 容器值 / 码流值 / 判定）。
//
// 从 BitstreamPanel 拆出的展示型组件。判定口径：只有能力为 Supported、容器有值、
// 码流也解析出值时才算"可比较"，否则一律显示「—」—— 否则 10bit 文件会出现
// "容器 0bit / 码流 10bit"这种看不懂的行。

#include <QTableWidget>
#include <QWidget>

#include "core/domain/model/BitstreamInfo.h"

namespace videoeye {
namespace ui {

class BitstreamComparisonView : public QWidget {
    Q_OBJECT

public:
    explicit BitstreamComparisonView(QWidget* parent = nullptr);

    void SetResult(const model::BitstreamAnalysisResult& result);
    void Clear();

private:
    void SetupUi();
    void BuildTable(const model::BitstreamAnalysisResult& result);

    QTableWidget* comparison_table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
