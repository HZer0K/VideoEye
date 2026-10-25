#pragma once

// 「参数变化」子页：按时间顺序列出 SPS/PPS/VPS/AV1 序列头的配置变化。
//
// 从 DiagnosticsPage 拆出的展示型组件。同一参数集 id 的内容被替换（标红）意味着
// 前后帧不是同一套配置，是拼接或编码器重启的直接证据。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QTableWidget>

#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace ui {

class ParameterTimelineView : public QWidget {
    Q_OBJECT

public:
    explicit ParameterTimelineView(QWidget* parent = nullptr);

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
