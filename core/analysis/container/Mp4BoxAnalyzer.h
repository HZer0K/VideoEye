#pragma once

#include <atomic>

#include <QString>

#include "core/domain/model/Mp4BoxInfo.h"

namespace videoeye {
namespace analyzer {

// MP4 Box 分析器
//
// 底层是自研的 utils::IsobmffParser（原为 Bento4）：
// 遍历 Box 树并展开 stts/stco/co64/stsc/stsz/stss 表，产出
// model::Mp4BoxAnalysisResult 供「文件结构」页展示。
class Mp4BoxAnalyzer {
public:
    Mp4BoxAnalyzer();
    ~Mp4BoxAnalyzer();

    // 分析 MP4 文件
    // 返回 true 表示分析成功，result 中包含 Box 树和表数据
    //
    // cancel: 可选的取消标志（nullptr = 不关心取消）。MP4 解析是 GB 级文件里最重的
    // 一段磁盘扫描，切换媒体/关闭流程置位后必须能当场停手。
    bool AnalyzeFile(const QString& file_path, model::Mp4BoxAnalysisResult& result,
                     const std::atomic<bool>* cancel = nullptr);

    // 重置
    void Reset();
};

} // namespace analyzer
} // namespace videoeye
