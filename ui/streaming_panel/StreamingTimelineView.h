#pragma once

// 「分片时间轴对齐」子视图：把各码率层的关键帧时间点按序号并排，标出与参考层
// 偏差超过容差的行（ABR 切档点错位）。
//
// 从 StreamingPanel 拆出的展示型组件，只吃一份 StreamingPackageResult 快照。

#include <QWidget>

#include "core/domain/model/StreamPackageInfo.h"

class QTableWidget;

namespace videoeye {
namespace ui {

class StreamingTimelineView : public QWidget {
    Q_OBJECT

public:
    explicit StreamingTimelineView(QWidget* parent = nullptr);

    void SetResult(const model::StreamingPackageResult& result);
    void Clear();

private:
    QTableWidget* table_ = nullptr;
};

} // namespace ui
} // namespace videoeye
