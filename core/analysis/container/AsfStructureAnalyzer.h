#pragma once

#include "core/domain/model/ContainerStructureInfo.h"
#include <atomic>
#include <string>

namespace videoeye {

/// Analyze() 的选项。放在命名空间作用域而不是类内：GCC 不接受「带默认成员
/// 初始化器的嵌套类」作同一个类定义体内的默认实参 —— 会报
/// "default member initializer for ... required before the end of its enclosing
/// class"（MSVC / Clang 放行，所以这个坑只在 Linux 构建上炸）。类内用
/// `using Options = AsfStructureOptions;` 保住 `AsfStructureAnalyzer::Options` 的写法。
struct AsfStructureOptions {
    bool build_tree = true;
};

/// ASF/WMV/WMA 容器结构轻量级解析器
class AsfStructureAnalyzer {
public:
    using Options = AsfStructureOptions;

    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr, const Options& options = Options());
};

} // namespace videoeye
