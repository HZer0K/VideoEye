#pragma once

#include <QString>
#include <string>

// 前向声明 MediaInfoLib 类型
namespace MediaInfoLib {
class MediaInfo;
}

namespace videoeye {
namespace analyzer {

/// @brief MediaInfo 解析器封装，提供简单的媒体文件信息提取接口
/// 底层使用 MediaInfoLib 源码库 (third_party/MediaInfoLib)，方便客制化。
class MediaInfoAnalyzer {
public:
    MediaInfoAnalyzer();
    ~MediaInfoAnalyzer();

    // 禁止拷贝
    MediaInfoAnalyzer(const MediaInfoAnalyzer&) = delete;
    MediaInfoAnalyzer& operator=(const MediaInfoAnalyzer&) = delete;

    /// 打开媒体文件并解析
    bool Open(const QString& filePath);

    /// 获取完整媒体信息 (类似 mediainfo CLI 输出)
    QString GetCompleteInfo() const;

    /// 获取指定流的某个参数值
    /// @param streamKind 流类型 (General=0, Video=1, Audio=2, Text=3, Other=4, Image=5, Menu=6)
    /// @param streamNumber 流序号 (0-based)
    /// @param parameter 参数名称 (如 "Format", "CodecID", "Width", "Height", "BitRate" 等)
    QString GetParameter(int streamKind, int streamNumber, const QString& parameter) const;

    /// 获取指定流的流数量
    int GetStreamCount(int streamKind) const;

    /// 关闭当前文件
    void Close();

    /// 是否已成功打开文件
    bool IsReady() const;

private:
    MediaInfoLib::MediaInfo* mi_;
    bool opened_;
};

} // namespace analyzer
} // namespace videoeye
