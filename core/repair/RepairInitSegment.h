#pragma once

// 参考初始化段：从"完整 MP4 / 独立的 ftyp+moov 初始化段"里取出可复用的初始化段。
//
// 背景：DASH / Smooth Streaming 下载下来的媒体分片（styp/sidx/moof/mdat）本身不含
// 解码参数（hvcC/avcC/esds 在 moov 里，而 moov 在单独的初始化段里）。这类文件
// FFmpeg 打不开、也无法从分片自身推导出解码参数，只能借助同源初始化段。
//
// 这里只做"提取 + 结构兼容性校验"，不写临时文件、不重封装；拼接与重封装属于
// RepairExecutor 的职责。
//
// ⚠️ track_id 相同不等于同源（R03）。旧实现只检查 ftyp/moov/mvex/trex + 轨道号，
// 参考段 mdhd 的错误时基会让"全部帧仍可解码、播放时长却变了"。现在必须拿源侧
// sidx 时基、逐 NAL 实测的码流类型来交叉验证；证据不足时标 unverified，
// 绝不能写成"兼容"。

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace repair {

// 单条轨道在源分片里实测的时长（trun/tfhd 累加），单位是该轨自己的 mdhd timescale。
// 与 sidx 估算时长一起，可以逐轨核对「借来的 mdhd timescale 是否被改写」。
struct RepairSourceTrackTiming {
    uint32_t track_id = 0;
    uint64_t declared_duration_ticks = 0; // 0 = 源侧没给时长，无法核对这一轨
};

// 从源媒体分片实测得到的独立证据（不来自参考文件）。
struct RepairSourceCompatibilityEvidence {
    uint32_t sidx_timescale = 0; // 0 = 源侧没有 sidx，拿不到独立时基
    std::string bitstream;       // "h264" / "hevc" / "unknown"（逐 NAL 实测）
    std::vector<uint32_t> track_ids;
    uint64_t sample_count = 0;
    double estimated_duration_seconds = 0.0;            // 按 sidx/tfdt 推算的源侧时长（估计值）
    std::vector<RepairSourceTrackTiming> track_timings; // 逐轨 trun 时长（R03 时基核对）
    std::string provenance;
};

// 参考段里一条被借用参数的轨道。多轨 fMP4 的 mdhd timescale 本来就可能不同
// （实测 H.264+AAC：视频 12800、音频 44100），所以不能只取第一条轨当「参考段时基」。
struct RepairReferenceTrack {
    uint32_t track_id = 0;
    uint32_t timescale = 0;
    std::string codec;
};

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

    // verified / unverified / rejected（结构失败时 ok=false，不用看这个字段）
    std::string compatibility = "unverified";
    std::string compatibility_reason;
    std::vector<std::string> borrowed_parameters;
    uint32_t reference_timescale = 0;
    std::vector<RepairReferenceTrack> reference_tracks; // 逐轨时基 / 编码实测清单
    std::string timescale_check;                        // 逐轨时基核对结论（供报告输出）
};

class RepairInitSegmentReader {
public:
    // required_track_ids：源媒体分片 moof/tfhd 里出现过的轨道 id。
    // 参考文件必须是 ISOBMFF、含 ftyp+moov、moov/mvex 含 trex（分片初始化段），
    // 且轨道号覆盖 required_track_ids；任何一条不满足都返回 ok=false 并写明原因。
    static RepairInitSegment Read(const std::string& reference_path, const std::vector<uint32_t>& required_track_ids,
                                  const std::atomic<bool>* cancel);

    // 带源侧证据的版本：用于引擎主链路。默认重载按"无证据"处理，结果必然是
    // unverified —— 保证任何"只按 track_id 判定兼容"的调用都拿不到 verified。
    static RepairInitSegment Read(const std::string& reference_path, const std::vector<uint32_t>& required_track_ids,
                                  const RepairSourceCompatibilityEvidence& evidence, const std::atomic<bool>* cancel);
};

} // namespace repair
} // namespace videoeye
