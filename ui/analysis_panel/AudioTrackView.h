#pragma once

// 「音轨对照」子页：全部参与分析的音轨横向对照（多语言 / 评论轨 / 不同混音版本）。
//
// 从 AudioQcPage 拆出的展示型组件。前面几页都是"主音轨"口径，只有这一页逐轨给出
// 覆盖率与时长差 —— "N/N 条轨都分析了"不代表每条轨都跑满了整段。

#include <QWidget>

#include <QLabel>
#include <QTableWidget>

#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace ui {

class AudioTrackView : public QWidget {
    Q_OBJECT

public:
    explicit AudioTrackView(QWidget* parent = nullptr);

    // nullptr = 尚无扫描结果
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
