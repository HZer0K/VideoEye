#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {

/// OGG 容器结构轻量级解析器
/// 解析 Ogg Page 头, 识别 codec 类型, 统计 logical stream 分布
class OggStructureAnalyzer {
public:
    struct Options {
        bool build_tree = true;
        // 最多扫描多少页（0 = 不限）。结构页 2000 页足够；QC 需要扫到文件尾
        // 才能判断"缺 EOS / 页序号断裂"。
        uint32_t max_pages = 2000;
        // 加入元素树的最多页数（树只是展示，不需要几十万节点）。
        uint32_t max_tree_pages = 200;
    };

    // cancel: 可选的取消标志（nullptr = 不关心取消）。
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr,
                 const Options& options = Options());
};

} // namespace videoeye
