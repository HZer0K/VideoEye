#pragma once

#include <QString>
#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// FLV 容器结构轻量级解析器
class FlvStructureAnalyzer {
public:
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
