#pragma once

#include <QString>
#include <QVector>
#include <QStack>
#include <memory>

#include "core/model/Mp4BoxInfo.h"

// Bento4 forward declarations
class AP4_AtomInspector;

namespace videoeye {
namespace analyzer {

// MP4 Box 分析器
// 使用 Bento4 库解析 MP4 文件的 Box 结构，提取 Box 树和 stts/stco/stsc/stsz 表数据
class Mp4BoxAnalyzer {
public:
    Mp4BoxAnalyzer();
    ~Mp4BoxAnalyzer();

    // 分析 MP4 文件
    // 返回 true 表示分析成功，result 中包含 Box 树和表数据
    bool AnalyzeFile(const QString& file_path, model::Mp4BoxAnalysisResult& result);

    // 重置
    void Reset();

private:
    // 使用 Bento4 的 AP4_AtomInspector 自定义实现来收集 Box 树数据
    class VideoEyeInspector;
};

} // namespace analyzer
} // namespace videoeye
