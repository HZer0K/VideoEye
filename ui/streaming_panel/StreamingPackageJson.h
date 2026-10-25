#pragma once

// 流媒体包结果 -> 报告级 JSON。
//
// 「复制 JSON / 导出 JSON」两处共用同一份序列化，以前它长在 StreamingPanel 里
// （约 150 行），和控件、扫描态混在同一个文件。抽成自由函数后：不依赖任何
// 控件，可以直接对输出做断言；面板只负责把结果递进来。

#include <QString>

#include "core/domain/model/StreamPackageInfo.h"

namespace videoeye {
namespace ui {

QString BuildStreamingPackageJson(const model::StreamingPackageResult& result);

} // namespace ui
} // namespace videoeye
