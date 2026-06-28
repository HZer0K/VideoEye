#pragma once

#include <QString>
#include "core/model/ContainerStructureInfo.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace analyzer {

/// 统一容器结构分析调度器
/// 根据文件格式自动选择对应的解析器, 输出统一的 ContainerStructureResult
class ContainerStructureAnalyzer {
public:
    ContainerStructureAnalyzer();
    ~ContainerStructureAnalyzer();

    /// 分析文件容器结构
    bool Analyze(const QString& file_path, model::ContainerStructureResult& result);

    /// 重置
    void Reset();

private:
    /// 将 Mp4BoxNode 树映射为 ContainerElement 树
    void ConvertMp4Tree(const QVector<model::Mp4BoxNode>& nodes, int depth,
                        QVector<model::ContainerElement>& out);

    /// 将 EbmlElementNode 树映射为 ContainerElement 树
    void ConvertEbmlTree(const QVector<model::EbmlElementNode>& nodes, int depth,
                         QVector<model::ContainerElement>& out);

    /// FFmpeg 通用元数据回退分析
    bool AnalyzeWithFFmpeg(const QString& file_path, model::ContainerStructureResult& result);
};

} // namespace analyzer
} // namespace videoeye
