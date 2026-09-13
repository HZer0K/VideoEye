#include "FileProbe.h"

#include <fstream>

namespace videoeye {
namespace utils {

namespace {

bool ReadHeaderBytes(const std::string& path, size_t count, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.resize(count);
    f.read(out.data(), static_cast<std::streamsize>(count));
    out.resize(static_cast<size_t>(f.gcount()));
    return !out.empty();
}

// 读取 offset 处的 32 位大端 box 类型；缓冲不足返回空
std::string BoxTypeAt(const std::string& buf, size_t offset) {
    if (offset + 8 > buf.size()) return "";
    // size 字段至少不能为明显非法值 (0 到 7 之间只有 size=0 表示到文件尾，罕见，仍可读类型)
    return buf.substr(offset + 4, 4);
}

} // namespace

FileHeaderInfo ProbeFileHeader(const std::string& file_path) {
    FileHeaderInfo info;
    std::string buf;
    if (!ReadHeaderBytes(file_path, 64, buf) || buf.size() < 8) {
        return info;
    }

    info.first_box_type = BoxTypeAt(buf, 0);
    // fMP4 媒体分片特征: 首盒为 styp (segment type, DASH/Smooth Streaming)，
    // 或直接以 moof/mdat 开头 (无 ftyp/moov)。完整的 fMP4 文件首盒是 ftyp。
    if (info.first_box_type == "styp" ||
        info.first_box_type == "moof" ||
        info.first_box_type == "mdat") {
        info.is_fmp4_segment_without_init = true;
    }
    return info;
}

std::string DiagnoseUnopenableFile(const std::string& file_path) {
    const FileHeaderInfo info = ProbeFileHeader(file_path);
    if (info.is_fmp4_segment_without_init) {
        return "文件首盒为 " + info.first_box_type +
               "，是 fMP4 媒体分片 (DASH/Smooth Streaming 分段)，缺少初始化段 (ftyp+moov/trex)，"
               "无法独立播放。修复：与对应的 init 段 (通常名为 init_*.m4s 或 init.mp4) 二进制拼接，"
               "Windows: copy /b init.m4s+分片文件 合并.mp4；Linux/macOS: cat init.m4s 分片文件 > 合并.mp4；"
               "或从原始 DASH 流重新下载完整文件。";
    }
    return "";
}

} // namespace utils
} // namespace videoeye
