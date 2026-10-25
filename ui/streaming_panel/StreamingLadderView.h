#pragma once

// 「码率阶梯」子视图：把 HLS variant / DASH representation 投影成的统一
// StreamingLadderEntry 列表画成一张表（分辨率 / 码率 / 编码 / 分片统计）。
//
// 从 StreamingPanel 拆出的展示型组件，只吃一份 StreamingPackageResult 快照。

#include <QWidget>

#include "core/domain/model/StreamPackageInfo.h"

class QTableWidget;

namespace videoeye {
namespace ui {

class StreamingLadderView : public QWidget {
    Q_OBJECT

public:
    explicit StreamingLadderView(QWidget* parent = nullptr);

    void SetResult(const model::StreamingPackageResult& result);
    void Clear();

private:
    QTableWidget* table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
