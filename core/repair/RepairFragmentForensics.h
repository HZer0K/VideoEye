#pragma once

// 缺初始化段的 ISOBMFF 分片序列：只读取证。
//
// 背景：DASH / CMAF / Smooth Streaming 下载器常把若干 (styp, sidx, moof, mdat)
// 直接拼成一个文件，扩展名还可能是 .ts。这类文件 FFmpeg 打不开，而"缺 moov"
// 这一条结论本身并不能回答：有多少段、每段是否完整、里面到底是什么码流、
// 有没有 in-band 参数集（有就可能自行重建初始化段）、时间轴断在哪。
//
// 这里严格只做取证：不写文件、不转码、不推断"可修 / 不可修"，也不把
// "没找到参数集"写成"猜出来的位置"。所有结论都带实测计数与局限说明，
// 由 RepairInspector 翻译成 DamageFinding / RepairCheck / RepairCoverage。
//
// 依赖边界：纯标准库 + 已解析的 IsobmffFile；不碰 Qt / FFmpeg。

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/media/container/IsobmffParser.h"

namespace videoeye {
namespace repair {

// 单条轨道在全部媒体分片上的聚合证据。
struct RepairFragmentTrackEvidence {
    uint32_t track_id = 0;
    uint64_t sample_count = 0;           // moof/trun 声明的样本总数（源侧期望值）
    uint64_t declared_payload_bytes = 0; // trun sample_size 之和
    uint64_t nal_units_scanned = 0;
    uint32_t nal_length_size = 0; // 探测到的 NAL 长度前缀宽度；0 = 不是这种布局
    std::string payload_layout;   // "nal_length_prefixed" / "raw_access_units" / "unknown"
    std::string bitstream;        // "hevc" / "h264" / "unknown"
    std::string bitstream_basis;  // 判定依据（可解释，不能只写一个结论）
    uint64_t invalid_h264_nal_types = 0;
    uint64_t invalid_hevc_nal_types = 0;
    uint64_t hevc_vps_count = 0;
    uint64_t hevc_sps_count = 0;
    uint64_t hevc_pps_count = 0;
    uint64_t avc_sps_count = 0;
    uint64_t avc_pps_count = 0;
    uint64_t sync_nal_count = 0;
    uint64_t raw_fragment_count = 0; // 未走通 NAL 链的分片数（例如裸 AAC 访问单元）
    std::vector<std::string> missing_parameter_sets;
};

// 一个 (styp...mdat) 分段的完整性。
struct RepairFragmentSegmentEvidence {
    uint64_t index = 0;
    uint64_t moof_offset = 0;
    uint64_t mdat_offset = 0;
    uint64_t declared_sample_bytes = 0;
    uint64_t mdat_payload_bytes = 0;
    uint64_t base_media_decode_time = 0;
    bool complete = false;
    std::string issue;
};

struct RepairFragmentEvidence {
    bool analyzed = false;
    std::string error;

    uint64_t segment_count = 0;
    uint64_t moof_count = 0;
    uint64_t mdat_count = 0;
    uint64_t styp_count = 0;
    uint64_t sidx_count = 0;
    uint64_t incomplete_segments = 0;

    uint32_t sidx_timescale = 0;      // 0 = 没读到 sidx
    uint64_t subsegment_duration = 0; // sidx 声明的单段时长（时基单位）
    uint64_t first_decode_time = 0;
    uint64_t last_decode_time = 0;
    double estimated_duration_seconds = 0.0;
    uint64_t timeline_gaps = 0;
    uint64_t timeline_repeats = 0;
    uint64_t timeline_regressions = 0;

    uint64_t bytes_scanned = 0;   // 实际读进内存的 mdat 载荷字节
    uint64_t samples_scanned = 0; // 实际逐个定位到的 NAL / 访问单元
    uint64_t nal_units_scanned = 0;
    bool reached_eof = false;

    std::vector<RepairFragmentTrackEvidence> tracks;
    std::vector<RepairFragmentSegmentEvidence> segments; // 只保留异常段，避免报告膨胀
    std::vector<std::string> limitations;
};

class RepairFragmentForensics {
public:
    // parsed：Inspector 已经解析过的盒树（复用 top_level / fragments，不重复解析）。
    // 每读完一个分段检查一次 cancel；被取消时 analyzed 仍可能为 true，
    // 但 reached_eof=false 且 limitations 会写明停止点。
    static RepairFragmentEvidence Analyze(const std::string& path, const IsobmffFile& parsed,
                                          const std::atomic<bool>* cancel);
};

} // namespace repair
} // namespace videoeye
