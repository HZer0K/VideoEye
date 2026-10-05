#pragma once

#include <string>
#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {
namespace analyzer {

/// 容器格式检测器
/// 通过文件魔数 (magic bytes) 和扩展名检测容器格式
class FormatDetector {
public:
    /// 通过读取文件头部魔数检测容器格式
    static model::ContainerFormat DetectByMagic(const std::string& file_path);

    /// 通过文件扩展名推断容器格式 (回退方案)
    static model::ContainerFormat DetectByExtension(const std::string& file_path);

    /// 综合检测: 先魔数, 后扩展名
    static model::ContainerFormat Detect(const std::string& file_path);

    /// 获取格式的可读名称
    static std::string FormatName(model::ContainerFormat fmt);

    /// 获取格式的标签页标题
    static std::string FormatTitle(model::ContainerFormat fmt);
};

} // namespace analyzer
} // namespace videoeye
