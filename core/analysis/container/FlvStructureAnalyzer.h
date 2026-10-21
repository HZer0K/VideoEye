#pragma once

#include "core/domain/model/ContainerStructureInfo.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace videoeye {

/// Analyze() 的选项。放在命名空间作用域的原因见 AsfStructureAnalyzer.h 里
/// 同段注释（GCC 拒绝嵌套类带默认成员初始化器时作默认实参）。
struct FlvStructureOptions {
    bool build_tree = true;
    // 最多遍历多少个 tag（0 = 不限）。
    // 结构页取前 500 个就够看；QC 全文件扫描需要不限，否则"时间戳回退/截断"
    // 只能看到开头一小段。
    uint32_t max_tags = 500;
};

/// FLV 容器结构轻量级解析器
class FlvStructureAnalyzer {
public:
    using Options = FlvStructureOptions;

    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr, const Options& options = Options());
};

} // namespace videoeye
