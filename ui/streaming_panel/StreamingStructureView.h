#pragma once

// 「清单结构树」子视图：HLS 的 master/media playlist、DASH 的
// Period - AdaptationSet - Representation 层级展开。
//
// 从 StreamingPanel 拆出的展示型组件。只吃一份 StreamingPackageResult 快照，
// 不认识扫描生命周期，也不持有 ScanClient —— 结构树里展示什么全部来自模型。

#include <QWidget>

#include "core/domain/model/StreamPackageInfo.h"

class QTreeWidget;

namespace videoeye {
namespace ui {

class StreamingStructureView : public QWidget {
    Q_OBJECT

public:
    explicit StreamingStructureView(QWidget* parent = nullptr);

    // 未成功解析（valid=false）只清空
    void SetResult(const model::StreamingPackageResult& result);
    void Clear();

private:
    void SetupUi();
    void BuildTree(const model::StreamingPackageResult& result);

    QTreeWidget* tree_ = nullptr;
};

} // namespace ui
} // namespace videoeye
