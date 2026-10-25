#pragma once

// 码流页中部左侧：参数集结构树（NAL / OBU / SPS / PPS / VUI / 编码工具）。
//
// 从 BitstreamPanel 拆出的展示型组件，只读 model::BitstreamAnalysisResult，
// 不参与任何扫描或导出。

#include <QTreeWidget>
#include <QWidget>

#include "core/domain/model/BitstreamInfo.h"

namespace videoeye {
namespace ui {

class BitstreamStructureView : public QWidget {
    Q_OBJECT

public:
    explicit BitstreamStructureView(QWidget* parent = nullptr);

    void SetResult(const model::BitstreamAnalysisResult& result);
    void Clear();

private:
    void SetupUi();
    void BuildTree(const model::BitstreamAnalysisResult& result);

    QTreeWidget* structure_tree_ = nullptr;
};

} // namespace ui
} // namespace videoeye
