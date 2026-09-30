#pragma once

#include <string>

namespace videoeye {
namespace utils {

// 文件头轻量探测结果（纯 ISOBMFF 盒型特征，不依赖 FFmpeg/Qt，便于单测）
struct FileHeaderInfo {
    std::string first_box_type;                 // 头部第一个 box 的类型（styp/ftyp/moof...），空 = 无法识别
    bool is_fmp4_segment_without_init = false;  // styp/moof 开头：fMP4 媒体分片，缺少 ftyp+moov 初始化段
};

// 只读文件头前 64 字节做探测，开销可忽略
FileHeaderInfo ProbeFileHeader(const std::string& file_path);

// 针对"FFmpeg 打不开的文件"给出定向的失败原因与修复建议。
// 目前覆盖：fMP4 媒体分片缺少初始化段（DASH/Smooth Streaming 分段，
// 首盒为 styp/moof，FFmpeg 报 could not find corresponding trex / error reading header）。
// 无已知特征时返回空串，调用方沿用 FFmpeg 的通用错误描述。
std::string DiagnoseUnopenableFile(const std::string& file_path);

} // namespace utils
} // namespace videoeye
