#pragma once

#include <atomic>
#include <QString>
#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// OGG 容器结构轻量级解析器
/// 解析 Ogg Page 头, 识别 codec 类型, 统计 logical stream 分布
class OggStructureAnalyzer {
public:
    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr);
};

} // namespace analyzer
} // namespace videoeye
