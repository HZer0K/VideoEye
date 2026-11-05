#pragma once

// 参考初始化段：从"完整 MP4 / 独立的 ftyp+moov 初始化段"里取出可复用的初始化段。
//
// 背景：DASH / Smooth Streaming 下载下来的媒体分片（styp/sidx/moof/mdat）本身不含
// 解码参数（hvcC/avcC/esds 在 moov 里，而 moov 在单独的初始化段里）。这类文件
// FFmpeg 打不开、也无法从分片自身推导出解码参数，只能借助同源初始化段。
//
// 这里只做"提取 + 结构兼容性校验"，不写临时文件、不重封装；拼接与重封装属于
// RepairExecutor 的职责。

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace repair {

struct RepairInitSegment {
    bool ok = false;
    std::string reference_path;
    std::string error;
    // 形如 "轨道 1 video(hvc1), 轨道 2 audio(mp4a)"
    std::string codec_summary;
    std::vector<uint32_t> track_ids;
    uint64_t init_size = 0;
    // ftyp + moov 的原始字节
    std::vector<uint8_t> bytes;
};

class RepairInitSegmentReader {
public:
    // required_track_ids：源媒体分片 moof/tfhd 里出现过的轨道 id。
    // 参考文件必须是 ISOBMFF、含 ftyp+moov、moov/mvex 含 trex（分片初始化段），
    // 且轨道号覆盖 required_track_ids；任何一条不满足都返回 ok=false 并写明原因。
    static RepairInitSegment Read(const std::string& reference_path, const std::vector<uint32_t>& required_track_ids,
                                  const std::atomic<bool>* cancel);
};

} // namespace repair
} // namespace videoeye
