#pragma once

#include "core/analysis/detail/SeqFileReader.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include <atomic>
#include <string>

namespace videoeye {

/// Analyze() 的选项。放在命名空间作用域的原因见 AsfStructureAnalyzer.h 里
/// 同段注释（GCC 拒绝嵌套类带默认成员初始化器时作默认实参）。
struct AviStructureOptions {
    // 结构页需要整棵树；QC 只要摘要，关掉可避免为百万级 movi chunk 建节点。
    bool build_tree = true;
    // QC 不需要逐个 chunk 走 movi，只要 hdrl + idx1/indx/dmlh。
    bool scan_media_data = true;
};

/// AVI (RIFF) 容器结构轻量级解析器
/// 解析 RIFF 容器结构: RIFF 'AVI ' -> LIST hdrl, LIST movi, idx1 等
class AviStructureAnalyzer {
public:
    using Options = AviStructureOptions;

    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr, const Options& options = Options());

private:
    /// 递归解析 RIFF 子块
    bool ParseChunk(SeqFileReader& file, int64_t end_offset, int depth, model::ContainerElement& parent,
                    model::ContainerStructureResult& result, const std::atomic<bool>* cancel = nullptr);

    bool build_tree_ = true;
    bool scan_media_data_ = true;
};

} // namespace videoeye
