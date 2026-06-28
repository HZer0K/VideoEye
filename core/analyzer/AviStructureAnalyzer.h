#pragma once

#include <QString>
#include <QFile>
#include "core/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// AVI (RIFF) 容器结构轻量级解析器
/// 解析 RIFF 容器结构: RIFF 'AVI ' -> LIST hdrl, LIST movi, idx1 等
class AviStructureAnalyzer {
public:
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result);

private:
    /// 递归解析 RIFF 子块
    bool ParseChunk(QFile& file, qint64 end_offset, int depth,
                    model::ContainerElement& parent,
                    model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
