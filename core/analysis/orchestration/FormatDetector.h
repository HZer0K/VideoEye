#pragma once

#include "core/domain/model/ContainerStructureInfo.h"
#include <string>
#include <vector>

namespace videoeye {

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

    // ---- 扩展名清单（单一事实源）----
    //
    // 这三张表都从 FormatDetector.cpp 里那张 `扩展名 -> 格式` 表派生，和
    // DetectByExtension 用的是同一份数据。以前 UI 的「打开文件」过滤器另抄了一份
    // 手写清单（只有 mp4/avi/mkv/flv/ts），于是 mov / m2ts / wmv / ogg / mpg / mxf
    // 这些早就能解析的格式在对话框里根本看不见 —— 功能做了但用户以为不支持。
    // 新增格式请只改那张表，别在 UI 里再写一份。

    /// 全部可专用解析的扩展名（无点、小写，保持表内顺序、无重复）
    static const std::vector<std::string>& SupportedExtensions();

    /// 音视频与裸流扩展名（不含 HLS/DASH 清单）
    static const std::vector<std::string>& MediaExtensions();

    /// 流媒体清单扩展名（m3u8/m3u/mpd）
    static const std::vector<std::string>& ManifestExtensions();
};

} // namespace videoeye
