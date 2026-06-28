#pragma once

#include <QString>
#include "core/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// ASF/WMV/WMA 容器结构轻量级解析器
class AsfStructureAnalyzer {
public:
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
