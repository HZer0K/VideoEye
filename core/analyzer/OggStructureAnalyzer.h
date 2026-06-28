#pragma once

#include <QString>
#include "core/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// OGG 容器结构轻量级解析器
/// 解析 Ogg Page 头, 识别 codec 类型, 统计 logical stream 分布
class OggStructureAnalyzer {
public:
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
