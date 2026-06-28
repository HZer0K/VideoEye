#pragma once

#include <QString>
#include "core/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// MPEG-TS 容器结构轻量级解析器
/// 扫描 TS 包, 解析 PAT/PMT 提取节目和流信息
class TsStructureAnalyzer {
public:
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
