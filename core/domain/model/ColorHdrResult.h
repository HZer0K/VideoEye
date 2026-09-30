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

} // namespace model
} // namespace videoeye
