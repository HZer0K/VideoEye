#include "core/repair/RepairFragmentForensics.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>

#include "core/domain/io/Utf8Path.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace repair {
namespace {

// 单个 mdat 一次读进内存的上限。超过就只做结构校验，不做 NAL 取证 ——
// 取证是"尽力而为"，绝不能为了它把 GB 级文件整体装进内存。
constexpr uint64_t kMaxSegmentBytes = 64ull * 1024 * 1024;

uint32_t Rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t Rd64(const uint8_t* p) {
    return (static_cast<uint64_t>(Rd32(p)) << 32) | static_cast<uint64_t>(Rd32(p + 4));
}

uint32_t ReadLength(const uint8_t* p, uint32_t width) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < width; ++i)
        value = (value << 8) | p[i];
    return value;
}

struct BoxRef {
    std::string type;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
};

struct SegmentRef {
    std::vector<BoxRef> moofs;
    std::vector<BoxRef> mdats;
    std::vector<BoxRef> sidxs;
    uint64_t index = 0;
};

struct NalStats {
    bool walked = false;
    uint64_t units = 0;
    uint64_t invalid_h264 = 0;
    uint64_t invalid_hevc = 0;
    uint64_t hevc_vps = 0;
    uint64_t hevc_sps = 0;
    uint64_t hevc_pps = 0;
    uint64_t avc_sps = 0;
    uint64_t avc_pps = 0;
    uint64_t sync = 0;
    uint64_t adts_like = 0;
};

// 从 begin 开始按 lengthSizeMinusOne+1 字节的长度前缀走完整条 NAL 链。
// 必须恰好走到 end：走不通就说明这不是长度前缀布局（例如裸 AAC 访问单元）。
NalStats WalkNalChain(const std::vector<uint8_t>& buffer, uint64_t begin, uint64_t end, uint32_t width) {
    NalStats stats;
    if (width == 0 || width > 4 || begin >= end || end > buffer.size())
        return stats;
    uint64_t pos = begin;
    while (pos + width <= end) {
        const uint32_t length = ReadLength(&buffer[static_cast<std::size_t>(pos)], width);
        const uint64_t nal = pos + width;
        if (length == 0 || nal + length > end)
            return NalStats{};
        if (nal + 2 <= end) {
            const uint8_t b0 = buffer[static_cast<std::size_t>(nal)];
            const uint8_t b1 = buffer[static_cast<std::size_t>(nal + 1)];
            const uint32_t avc_type = b0 & 0x1fu;
            const uint32_t hevc_type = (b0 >> 1) & 0x3fu;
            ++stats.units;
            // AVC: type 0 未定义，24..31 保留；forbidden_zero_bit 必须为 0。
            if ((b0 & 0x80u) != 0 || avc_type == 0 || avc_type >= 24)
                ++stats.invalid_h264;
            // HEVC: forbidden_zero_bit=0，nal_unit_type<=40，nuh_temporal_id_plus1!=0。
            if ((b0 & 0x80u) != 0 || hevc_type > 40 || (b1 & 0x07u) == 0)
                ++stats.invalid_hevc;
            if (hevc_type == 32)
                ++stats.hevc_vps;
            else if (hevc_type == 33)
                ++stats.hevc_sps;
            else if (hevc_type == 34)
                ++stats.hevc_pps;
            else if (hevc_type >= 16 && hevc_type <= 21)
                ++stats.sync;
            if (avc_type == 7)
                ++stats.avc_sps;
            else if (avc_type == 8)
                ++stats.avc_pps;
            else if (avc_type == 5)
                ++stats.sync;
            if (b0 == 0xff && (b1 & 0xf0u) == 0xf0)
                ++stats.adts_like;
        }
        pos = nal + length;
    }
    if (pos != end)
        return NalStats{};
    stats.walked = true;
    return stats;
}

NalStats PickNalLayout(const std::vector<uint8_t>& buffer, uint64_t begin, uint64_t end, uint32_t* width_out) {
    // 4 字节是 CMAF/MP4 的绝对主流；先试它，再退到 3/2/1。
    for (uint32_t width : {4u, 3u, 2u, 1u}) {
        NalStats stats = WalkNalChain(buffer, begin, end, width);
        if (!stats.walked)
            continue;
        // 长度前缀链碰巧"走通"但 NAL 头几乎全非法时，按非 NAL 布局处理。
        if (stats.units >= 4 && stats.invalid_h264 >= stats.units / 2 && stats.invalid_hevc >= stats.units / 2)
            continue;
        *width_out = width;
        return stats;
    }
    *width_out = 0;
    return NalStats{};
}

// sidx (ISO/IEC 14496-12) 只读头部与第一条 reference，不展开全部表项。
struct SidxInfo {
    bool ok = false;
    uint32_t timescale = 0;
    uint64_t earliest_presentation_time = 0;
    uint64_t subsegment_duration = 0;
};

SidxInfo ReadSidx(std::ifstream& in, const BoxRef& box) {
    SidxInfo info;
    if (box.payload_size < 24)
        return info;
    in.clear();
    in.seekg(static_cast<std::streamoff>(box.payload_offset));
    if (!in)
        return info;
    // version 0 的首条 reference 结束在 payload 偏移 36，version 1 在 44。
    uint8_t head[44] = {};
    const std::streamsize want = static_cast<std::streamsize>(std::min<uint64_t>(sizeof(head), box.payload_size));
    in.read(reinterpret_cast<char*>(head), want);
    if (in.gcount() < 24)
        return info;
    const uint8_t version = head[0];
    uint64_t pos = 4; // version + flags
    pos += 4;         // reference_ID
    if (pos + 4 > static_cast<uint64_t>(in.gcount()))
        return info;
    info.timescale = Rd32(head + pos);
    pos += 4;
    if (version == 1) {
        if (pos + 8 > static_cast<uint64_t>(in.gcount()))
            return info;
        info.earliest_presentation_time = Rd64(head + pos);
        pos += 8;
    } else {
        if (pos + 4 > static_cast<uint64_t>(in.gcount()))
            return info;
        info.earliest_presentation_time = Rd32(head + pos);
        pos += 4;
    }
    if (pos + 4 > static_cast<uint64_t>(in.gcount()))
        return info;
    pos += 4; // first_offset
    pos += 4; // reserved(2) + reference_count(2)
    if (pos + 12 > static_cast<uint64_t>(in.gcount()))
        return info;
    pos += 4; // reference_type(1) + referenced_size(31)
    info.subsegment_duration = Rd32(head + pos);
    info.ok = true;
    return info;
}

uint64_t DataStart(const IsobmffFragment& fragment, uint64_t fallback) {
    if (!fragment.trun_data_offset_present)
        return fallback;
    uint64_t base = fragment.offset;
    if (fragment.base_data_offset_present && !fragment.default_base_is_moof)
        base = fragment.base_data_offset;
    const int64_t offset = fragment.trun_data_offset;
    if (offset < 0 && static_cast<uint64_t>(-offset) > base)
        return fallback;
    const uint64_t start = offset >= 0 ? base + static_cast<uint64_t>(offset) : base - static_cast<uint64_t>(-offset);
    return start > 0 ? start : fallback;
}

} // namespace

RepairFragmentEvidence RepairFragmentForensics::Analyze(const std::string& path, const IsobmffFile& parsed,
                                                        const std::atomic<bool>* cancel) {
    RepairFragmentEvidence evidence;
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in) {
        evidence.error = "无法打开源文件做分片取证";
        return evidence;
    }
    evidence.analyzed = true;

    // ---- 1. 顶层盒按 styp 切段；没有 styp 时每个 moof 自成一段 ----
    std::vector<SegmentRef> segments;
    for (const auto& box : parsed.top_level) {
        BoxRef ref;
        ref.type = box.type;
        ref.offset = box.offset;
        ref.size = box.size;
        ref.payload_offset = box.offset + box.header_size;
        ref.payload_size = box.size > box.header_size ? box.size - box.header_size : 0;
        if (box.type == "styp") {
            ++evidence.styp_count;
            if (segments.empty() || !segments.back().moofs.empty() || !segments.back().mdats.empty()) {
                SegmentRef next;
                next.index = segments.size();
                segments.push_back(std::move(next));
            }
        } else if (box.type == "moof") {
            ++evidence.moof_count;
            // 没有 styp 的拼接分片（例如 FFmpeg empty_moov 输出）也要按 moof 边界切段，
            // 否则若干 moof/mdat 会被算成"一段"，分片数与完整性证据都失真。
            if (segments.empty() || !segments.back().moofs.empty()) {
                SegmentRef next;
                next.index = segments.size();
                segments.push_back(std::move(next));
            }
            segments.back().moofs.push_back(ref);
        } else if (box.type == "mdat") {
            ++evidence.mdat_count;
            if (segments.empty()) {
                SegmentRef next;
                next.index = 0;
                segments.push_back(std::move(next));
            }
            segments.back().mdats.push_back(ref);
        } else if (box.type == "sidx") {
            ++evidence.sidx_count;
            if (segments.empty()) {
                SegmentRef next;
                next.index = 0;
                segments.push_back(std::move(next));
            }
            segments.back().sidxs.push_back(ref);
        }
    }
    // 结尾可能出现一个只有 styp 的空段（下载中断），保留它以便报"末段不完整"。
    evidence.segment_count = segments.size();

    // ---- 2. fragment 按 moof 偏移建索引 ----
    std::map<uint64_t, std::vector<const IsobmffFragment*>> by_moof;
    for (const auto& fragment : parsed.fragments)
        by_moof[fragment.offset].push_back(&fragment);

    // 轨道聚合 + 时间轴原始序列
    std::map<uint32_t, RepairFragmentTrackEvidence> tracks;
    std::map<uint32_t, std::vector<uint64_t>> decode_times;
    std::map<uint32_t, uint32_t> locked_widths;
    std::map<uint32_t, bool> layout_decided;
    std::map<uint32_t, bool> raw_tracks;
    std::vector<uint64_t> sidx_times;

    for (const SegmentRef& segment : segments) {
        if (infrastructure::Checkpoint(cancel)) {
            evidence.limitations.push_back("分片取证被取消，后续分段未读取");
            return evidence;
        }

        // sidx：时间基与单段时长只取第一个可读到的值，重复值用于时间轴取证。
        for (const BoxRef& sidx : segment.sidxs) {
            const SidxInfo info = ReadSidx(in, sidx);
            if (!info.ok)
                continue;
            if (evidence.sidx_timescale == 0) {
                evidence.sidx_timescale = info.timescale;
                evidence.subsegment_duration = info.subsegment_duration;
            }
            sidx_times.push_back(info.earliest_presentation_time);
        }

        uint64_t declared = 0;
        uint64_t payload = 0;
        for (const BoxRef& moof : segment.moofs) {
            auto it = by_moof.find(moof.offset);
            if (it == by_moof.end())
                continue;
            for (const IsobmffFragment* fragment : it->second) {
                declared += fragment->total_size;
                RepairFragmentTrackEvidence& track = tracks[fragment->track_id];
                track.track_id = fragment->track_id;
                track.sample_count += fragment->sample_count;
                track.declared_payload_bytes += fragment->total_size;
                if (fragment->has_tfdt)
                    decode_times[fragment->track_id].push_back(fragment->base_media_decode_time);
            }
        }
        for (const BoxRef& mdat : segment.mdats)
            payload += mdat.payload_size;

        RepairFragmentSegmentEvidence seg;
        seg.index = segment.index;
        seg.moof_offset = segment.moofs.empty() ? 0 : segment.moofs.front().offset;
        seg.mdat_offset = segment.mdats.empty() ? 0 : segment.mdats.front().offset;
        seg.declared_sample_bytes = declared;
        seg.mdat_payload_bytes = payload;
        if (segment.moofs.empty())
            seg.issue = "缺少 moof";
        else if (segment.mdats.empty())
            seg.issue = "缺少 mdat";
        else if (declared > payload)
            seg.issue = "trun 声明样本字节 " + std::to_string(declared) + " 超出 mdat 载荷 " + std::to_string(payload);
        else if (payload - declared > 64)
            seg.issue = "mdat 载荷比 trun 声明多 " + std::to_string(payload - declared) + " 字节";
        else
            seg.complete = true;
        if (!seg.complete) {
            ++evidence.incomplete_segments;
            if (evidence.segments.size() < 12)
                evidence.segments.push_back(seg);
        }

        // 只处理"一 moof 对一 mdat"的常规 CMAF 段。多 mdat 段的载荷在文件里
        // 未必连续，按首盒偏移推算 buffer 内位置会越界，宁可少取证也不能读错。
        if (segment.mdats.size() != 1 || payload > kMaxSegmentBytes) {
            if (payload > kMaxSegmentBytes)
                evidence.limitations.push_back("第 " + std::to_string(segment.index) +
                                               " 段 mdat 超过 64 MiB，跳过 NAL 取证");
            else if (segment.mdats.size() > 1)
                evidence.limitations.push_back("第 " + std::to_string(segment.index) +
                                               " 段含多个 mdat，跳过 NAL 载荷取证（结构校验仍有效）");
            continue;
        }

        // ---- 3. 整段 mdat 载荷一次读入，按轨道数据区走 NAL 链 ----
        const BoxRef& first_mdat = segment.mdats.front();
        std::vector<uint8_t> buffer(static_cast<std::size_t>(payload));
        uint64_t read_at = first_mdat.payload_offset;
        for (const BoxRef& mdat : segment.mdats) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(mdat.payload_offset));
            if (!in)
                break;
            in.read(reinterpret_cast<char*>(buffer.data() + static_cast<std::size_t>(mdat.payload_offset - read_at)),
                    static_cast<std::streamsize>(mdat.payload_size));
            if (static_cast<uint64_t>(in.gcount()) != mdat.payload_size)
                break;
        }
        evidence.bytes_scanned += payload;

        uint64_t fallback = first_mdat.payload_offset;
        for (const BoxRef& moof : segment.moofs) {
            auto it = by_moof.find(moof.offset);
            if (it == by_moof.end())
                continue;
            for (const IsobmffFragment* fragment : it->second) {
                const uint64_t start = DataStart(*fragment, fallback);
                fallback = start + fragment->total_size;
                RepairFragmentTrackEvidence& track = tracks[fragment->track_id];
                if (start < first_mdat.payload_offset || start + fragment->total_size > read_at + payload) {
                    if (track.payload_layout.empty())
                        track.payload_layout = "out_of_range";
                    evidence.samples_scanned += fragment->sample_count;
                    continue;
                }
                const uint64_t begin = start - read_at;
                const uint64_t finish = begin + fragment->total_size;

                // 同一条轨道的 NAL 长度前缀宽度必须全程一致：只用首段探测一次，
                // 后续分片沿用，避免个别分片的随机字节碰巧"走通"而污染统计。
                NalStats stats;
                if (!raw_tracks[fragment->track_id] && !layout_decided[fragment->track_id]) {
                    uint32_t width = 0;
                    stats = PickNalLayout(buffer, begin, finish, &width);
                    layout_decided[fragment->track_id] = true;
                    if (stats.walked) {
                        locked_widths[fragment->track_id] = width;
                        track.payload_layout = "nal_length_prefixed";
                        track.nal_length_size = width;
                    } else {
                        raw_tracks[fragment->track_id] = true;
                        track.payload_layout = "raw_access_units";
                    }
                } else if (!raw_tracks[fragment->track_id]) {
                    stats = WalkNalChain(buffer, begin, finish, locked_widths[fragment->track_id]);
                }
                if (!stats.walked) {
                    ++track.raw_fragment_count;
                    evidence.samples_scanned += fragment->sample_count;
                    continue;
                }
                track.nal_units_scanned += stats.units;
                track.invalid_h264_nal_types += stats.invalid_h264;
                track.invalid_hevc_nal_types += stats.invalid_hevc;
                track.hevc_vps_count += stats.hevc_vps;
                track.hevc_sps_count += stats.hevc_sps;
                track.hevc_pps_count += stats.hevc_pps;
                track.avc_sps_count += stats.avc_sps;
                track.avc_pps_count += stats.avc_pps;
                track.sync_nal_count += stats.sync;
                // samples_scanned 记"源侧样本数"，与 expected_tracks 口径一致；
                // NAL 个数单独记，不能把两者混成一个数。
                evidence.samples_scanned += fragment->sample_count;
                evidence.nal_units_scanned += stats.units;
            }
        }
    }

    // ---- 4. 编码判定 / 缺失参数集 ----
    for (auto& entry : tracks) {
        RepairFragmentTrackEvidence& track = entry.second;
        if (track.nal_units_scanned == 0) {
            track.bitstream = "unknown";
            track.bitstream_basis = "该轨道不是 NAL 长度前缀布局，无法从载荷头判定编码";
            continue;
        }
        if (track.invalid_hevc_nal_types < track.invalid_h264_nal_types) {
            track.bitstream = "hevc";
            track.bitstream_basis = "按 HEVC NAL 头解释的非法类型 " + std::to_string(track.invalid_hevc_nal_types) +
                                    " 个，按 H.264 解释有 " + std::to_string(track.invalid_h264_nal_types) + " 个";
            if (track.hevc_vps_count == 0)
                track.missing_parameter_sets.push_back("VPS");
            if (track.hevc_sps_count == 0)
                track.missing_parameter_sets.push_back("SPS");
            if (track.hevc_pps_count == 0)
                track.missing_parameter_sets.push_back("PPS");
        } else if (track.invalid_h264_nal_types < track.invalid_hevc_nal_types) {
            track.bitstream = "h264";
            track.bitstream_basis = "按 H.264 NAL 头解释的非法类型 " + std::to_string(track.invalid_h264_nal_types) +
                                    " 个，按 HEVC 解释有 " + std::to_string(track.invalid_hevc_nal_types) + " 个";
            if (track.avc_sps_count == 0)
                track.missing_parameter_sets.push_back("SPS");
            if (track.avc_pps_count == 0)
                track.missing_parameter_sets.push_back("PPS");
        } else {
            track.bitstream = "unknown";
            track.bitstream_basis =
                "两种 NAL 头解释的非法类型数相同（" + std::to_string(track.invalid_h264_nal_types) + "），证据不足";
        }
    }
    for (const auto& entry : tracks)
        evidence.tracks.push_back(entry.second);

    // ---- 5. 时间轴连续性 ----
    std::vector<uint64_t> timeline;
    if (!sidx_times.empty())
        timeline = sidx_times;
    else if (!decode_times.empty())
        timeline = decode_times.begin()->second;
    if (timeline.size() >= 2) {
        std::map<uint64_t, uint32_t> step_votes;
        for (std::size_t i = 1; i < timeline.size(); ++i) {
            if (timeline[i] > timeline[i - 1])
                ++step_votes[timeline[i] - timeline[i - 1]];
        }
        uint64_t expected_step = 0;
        uint32_t best = 0;
        for (const auto& vote : step_votes) {
            if (vote.second > best) {
                best = vote.second;
                expected_step = vote.first;
            }
        }
        for (std::size_t i = 1; i < timeline.size(); ++i) {
            if (timeline[i] == timeline[i - 1])
                ++evidence.timeline_repeats;
            else if (timeline[i] < timeline[i - 1])
                ++evidence.timeline_regressions;
            else if (expected_step > 0 && timeline[i] - timeline[i - 1] > expected_step + expected_step / 2)
                ++evidence.timeline_gaps;
        }
    }
    // 估算时长只能用**同一条时间轴**：sidx 序列与 sidx_timescale 天然同源。
    // 绝不能把视频 tfdt 和音频 tfdt 混起来取极值 —— 两条轨道时间基不同，
    // 实测样本里音频 tfdt 末值 26196475、视频只有 9124352，混算会把 600 s 报成 1711 s。
    std::vector<uint64_t> primary_timeline;
    if (!sidx_times.empty())
        primary_timeline = sidx_times;
    else if (!decode_times.empty())
        primary_timeline = decode_times.begin()->second;
    if (!primary_timeline.empty()) {
        const auto bounds = std::minmax_element(primary_timeline.begin(), primary_timeline.end());
        evidence.first_decode_time = *bounds.first;
        evidence.last_decode_time = *bounds.second;
    }
    if (evidence.sidx_timescale > 0) {
        uint64_t span = 0;
        if (!primary_timeline.empty())
            span = evidence.last_decode_time - evidence.first_decode_time + evidence.subsegment_duration;
        else
            span = static_cast<uint64_t>(sidx_times.size()) * evidence.subsegment_duration;
        evidence.estimated_duration_seconds = static_cast<double>(span) / static_cast<double>(evidence.sidx_timescale);
    }
    evidence.reached_eof = !infrastructure::Checkpoint(cancel) && !segments.empty();
    evidence.limitations.push_back("只读取 mdat 载荷的 NAL 头与长度前缀，未解码、未校验 Slice 语法");
    evidence.limitations.push_back("编码判定是基于 NAL 头合法性的结构启发式，不是解码器确认");
    evidence.limitations.push_back("载荷若被 DRM/加密保护，NAL 头仍可解析但不能据此断言可解码");
    return evidence;
}

} // namespace repair
} // namespace videoeye
