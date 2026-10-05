#pragma once

#include <atomic>
#include <string>
#include "core/analysis/detail/SeqFileReader.h"
#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {

/// AVI (RIFF) 容器结构轻量级解析器
/// 解析 RIFF 容器结构: RIFF 'AVI ' -> LIST hdrl, LIST movi, idx1 等
class AviStructureAnalyzer {
public:
    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr);

private:
    /// 递归解析 RIFF 子块
    bool ParseChunk(SeqFileReader& file, int64_t end_offset, int depth,
                    model::ContainerElement& parent,
                    model::ContainerStructureResult& result,
                    const std::atomic<bool>* cancel = nullptr);
};

} // namespace videoeye
