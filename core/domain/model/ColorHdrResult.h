#pragma once

// 色彩 / HDR 分析的**纯结果类型**。
//
// 这里只有数据结构，没有任何解析器、FFmpeg 类型或执行逻辑：
//   * ColorHdrAnalyzer 负责生产它
//   * QcReport / 报告导出 / UI 只消费它
//
// 以前这两个结构定义在 ColorHdrAnalyzer.h 里，于是 model(QcReport.h) 为了拿到
// 结果类型必须反向包含 analyzer —— 把分析器拖进每个报告消费者的编译图。
// 拆到这里之后依赖方向变成 Analyzer -> Result，model 只认结果，不再反过来拖 analyzer。

#include <string>
#include <vector>

#include "core/domain/model/ColorInfo.h"
#include "core/domain/model/HdrMetadataInfo.h"

namespace videoeye {
namespace model {

// 一条视频流的色彩 + HDR 分析结果
struct ColorHdrAnalysis {
    bool analyzed = false;
    int stream_index = -1;

    ColorInfo color;
    HdrMetadataInfo hdr;

    std::vector<std::string> notes;  // 降级/来源说明

    // 静态元数据已齐备 -> 不必再为补元数据去解码帧
    bool StaticMetadataComplete() const;

    std::string ToString() const;
};

// UI 表格 / 报告导出共用的三列结构（项目 / 值 / 说明）
struct ColorKeyValueRow {
    std::string key;
    std::string value;
    std::string note;
};

// 把结果摆成"键值 + 说明"的行 —— UI 表格与报告导出都用它。
//
// 为什么放在这里而不是留在 ColorHdrAnalyzer.h: 这是纯展示逻辑, 只吃 domain 类型。
// 以前它俩挂在分析器头上, UI 页面为了用这几十行, 必须把整个分析器(连同它 FFmpeg 侧的
// 前向声明和具体分析器依赖)拖进自己的编译图。放回来之后 UI 只依赖 domain。
std::vector<ColorKeyValueRow> BuildColorRows(const ColorHdrAnalysis& analysis);
std::vector<ColorKeyValueRow> BuildHdrRows(const ColorHdrAnalysis& analysis);

} // namespace model
} // namespace videoeye
