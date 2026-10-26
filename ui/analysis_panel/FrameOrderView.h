#pragma once

// 「帧顺序」子页：按解码顺序列出每条视频轨（H.264 / HEVC / VP9 / AV1）的逐帧顺序值。
//
// 从 DiagnosticsPage 拆出的展示型组件。与「参数变化」的区别：那条记"配置怎么变"，
// 这条记"帧怎么排" —— 顺序值比上一行小的行标红，重排的存在与深度因此一目了然。
// 列名统一叫「POC / 显示序」：H.264 / HEVC 是 POC，VP9 是"第几次显示"，
// AV1 是解环后的 order_hint（三者由 order_source 区分，别按编码名硬编码）。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QTableWidget>

#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace ui {

class FrameOrderView : public QWidget {
    Q_OBJECT

public:
    explicit FrameOrderView(QWidget* parent = nullptr);

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
