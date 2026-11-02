#pragma once

// 码流页顶部摘要行（codec / 分辨率 / profile / level / 位深 / 参数集数量 /
// 不一致数 / 能力徽标）。
//
// 从 BitstreamPanel 拆出的展示型组件。工具栏（开始分析 / 导出 JSON）
// 已上移到页头：整页只有页头那一行控制，不再由各子块各挂一套按钮。

#include <QString>
#include <QWidget>

#include <QLabel>

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

private:
    void SetupUi();

    QLabel* summary_label_ = nullptr;
};

} // namespace ui
} // namespace videoeye
