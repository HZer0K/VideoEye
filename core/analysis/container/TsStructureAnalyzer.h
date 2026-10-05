#pragma once

#include <atomic>
#include <string>
#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// MPEG-TS 容器结构轻量级解析器
/// 扫描 TS 包, 解析 PAT/PMT 提取节目和流信息
class TsStructureAnalyzer {
public:
    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr);
};

} // namespace analyzer
} // namespace videoeye
