#include "core/repair/RepairInitSegment.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "core/domain/io/Utf8Path.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace repair {
namespace {

// moov 正常只有几 KB；大于这个值基本可以断定不是初始化段，直接拒绝，避免把
// 一个巨大文件整个读进内存。
constexpr uint64_t kMaxInitSegmentBytes = 64ull * 1024 * 1024;

bool ReadBoxBytes(const std::string& path, const IsobmffBox& box, std::string* out) {
    if (box.size == 0 || box.size > kMaxInitSegmentBytes)
        return false;
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in)
        return false;
    in.seekg(static_cast<std::streamoff>(box.offset));
    if (!in)
        return false;
    out->resize(static_cast<std::size_t>(box.size));
    in.read(&(*out)[0], static_cast<std::streamsize>(box.size));
    return static_cast<uint64_t>(in.gcount()) == box.size;
}

const IsobmffBox* FindChild(const IsobmffBox& parent, const std::string& type) {
    for (const auto& child : parent.children) {
        if (child.type == type)
            return &child;
    }
    return nullptr;
}

struct TimescaleCheckOutcome {
    bool rejected = false;
    std::size_t checked_tracks = 0;
    std::string detail;
};

// 逐轨核对借来的 mdhd timescale（R03 的核心）。
//
// 多轨 fMP4 的 mdhd timescale 本来就可能不同（实测 H.264+AAC 是 12800/44100），
// 所以「第一条轨的 timescale 等于 sidx」既不能证明其余轨正确，也不能用来拒绝。
// 真正独立的证据是源分片自己声明的 trun/tfhd 时长：它的单位就是该轨 mdhd timescale，
// 除以借来的 timescale 得到的秒数必须与 sidx 序列估算时长一致。时基被改写成两倍，
// 换算时长就会减半 ——「全部帧都能解码、播放时长却变了」正是这样发生的。
// 双阈值（相对 >10% 且绝对 >0.2s）避免把容器估算值 / 尾段截断的正常小差异误判。
TimescaleCheckOutcome CheckTrackTimescales(const std::vector<RepairReferenceTrack>& refs,
                                           const RepairSourceCompatibilityEvidence& evidence) {
    TimescaleCheckOutcome out;
    if (evidence.estimated_duration_seconds <= 0.0) {
        out.detail = "源侧没有 sidx 估算时长，逐轨 mdhd timescale 未核对";
        return out;
    }
    std::ostringstream lines;
    lines << "源侧 sidx 估算时长 " << FormatSeconds(evidence.estimated_duration_seconds) << " s; ";
    for (const auto& timing : evidence.track_timings) {
        if (timing.declared_duration_ticks == 0)
            continue;
        const auto it = std::find_if(refs.begin(), refs.end(),
                                     [&](const RepairReferenceTrack& ref) { return ref.track_id == timing.track_id; });
        if (it == refs.end() || it->timescale == 0)
            continue;
        const double seconds = static_cast<double>(timing.declared_duration_ticks) / it->timescale;
        const double delta = std::fabs(seconds - evidence.estimated_duration_seconds);
        ++out.checked_tracks;
        lines << "轨道 " << timing.track_id << " mdhd timescale=" << it->timescale << " -> " << FormatSeconds(seconds)
              << " s (差值 " << FormatSeconds(delta) << " s); ";
        if (delta > 0.2 && delta > 0.10 * evidence.estimated_duration_seconds) {
            out.rejected = true;
            out.detail = lines.str() + "轨道 " + std::to_string(timing.track_id) +
                         " 借用的 mdhd timescale 与源侧 trun/tfdt 实测时长不符，播放时长 / 速度会被改写";
            return out;
        }
    }
    if (out.checked_tracks == 0)
        lines << "源侧未给出逐轨 trun 时长，无法逐轨核对 mdhd timescale; ";
    out.detail = lines.str();
    return out;
}

} // namespace

RepairInitSegment RepairInitSegmentReader::Read(const std::string& reference_path,
                                                const std::vector<uint32_t>& required_track_ids,
                                                const std::atomic<bool>* cancel) {
    return Read(reference_path, required_track_ids, RepairSourceCompatibilityEvidence{}, cancel);
}

RepairInitSegment RepairInitSegmentReader::Read(const std::string& reference_path,
                                                const std::vector<uint32_t>& required_track_ids,
                                                const RepairSourceCompatibilityEvidence& evidence,
                                                const std::atomic<bool>* cancel) {
    RepairInitSegment result;
    result.reference_path = reference_path;

    std::error_code ec;
    if (!std::filesystem::exists(ToNativePath(reference_path), ec) ||
        !std::filesystem::is_regular_file(ToNativePath(reference_path), ec)) {
        result.error = "参考文件不存在或不是普通文件";
        return result;
    }

    IsobmffFile parsed;
    IsobmffOptions options;
    // 只需要 moov 结构与 trak 元数据，样本表对初始化段没有意义。
    options.skip_sample_tables = true;
    options.cancel = cancel;
    if (!IsobmffParser::Parse(reference_path, parsed, options)) {
        result.error = "参考文件不是可解析的 ISOBMFF: " + parsed.error_message;
        return result;
    }
    if (infrastructure::Checkpoint(cancel)) {
        result.error = "读取参考文件被取消";
        return result;
    }
    if (!parsed.found_moov) {
        result.error = "参考文件缺少 moov（不是完整 MP4，也不含初始化段）";
        return result;
    }

    const IsobmffBox* ftyp = nullptr;
    const IsobmffBox* moov = nullptr;
    for (const auto& box : parsed.top_level) {
        if (box.type == "ftyp")
            ftyp = &box;
        else if (box.type == "moov")
            moov = &box;
    }
    if (!moov) {
        result.error = "参考文件里没有顶层 moov";
        return result;
    }
    if (!ftyp) {
        result.error = "参考文件缺少 ftyp，无法组成标准初始化段";
        return result;
    }

    const IsobmffBox* mvex = FindChild(*moov, "mvex");
    if (!mvex) {
        result.error = "参考文件的 moov 不含 mvex，是普通 MP4 而不是分片初始化段，无法只复用 moov";
        return result;
    }
    const IsobmffBox* trex = FindChild(*mvex, "trex");
    if (!trex) {
        result.error = "参考文件的 moov/mvex 缺少 trex，无法为媒体分片提供默认样本参数";
        return result;
    }

    std::set<uint32_t> reference_track_ids;
    std::ostringstream codec_summary;
    std::string reference_codecs;
    for (const auto& track : parsed.tracks) {
        if (track.track_id == 0)
            continue;
        reference_track_ids.insert(track.track_id);
        result.track_ids.push_back(track.track_id);
        RepairReferenceTrack ref;
        ref.track_id = track.track_id;
        ref.timescale = track.media_timescale;
        ref.codec = track.codec;
        result.reference_tracks.push_back(ref);
        if (codec_summary.tellp() > 0)
            codec_summary << ", ";
        codec_summary << "轨道 " << track.track_id << " " << track.TypeName();
        if (!track.codec.empty()) {
            codec_summary << "(" << track.codec << ")";
            if (!reference_codecs.empty())
                reference_codecs += ",";
            reference_codecs += track.codec;
        }
        result.borrowed_parameters.push_back("轨道 " + std::to_string(track.track_id) +
                                             ": mdhd timescale=" + std::to_string(track.media_timescale) +
                                             ", stsd=" + (track.codec.empty() ? "unknown" : track.codec));
    }
    result.codec_summary = codec_summary.str();
    result.borrowed_parameters.push_back("ftyp + moov/mvex/trex（默认样本参数）");

    for (uint32_t track_id : required_track_ids) {
        if (reference_track_ids.count(track_id) == 0) {
            result.error = "参考文件没有源分片需要的轨道 " + std::to_string(track_id) + "（参考轨道数 " +
                           std::to_string(reference_track_ids.size()) + "）";
            return result;
        }
    }

    // reference_timescale 只用于「反向冲突」判定。旧实现恒取第一条轨，于是改掉
    // 第 2 条轨的 mdhd timescale 完全绕过检查；现在优先取与源侧 sidx 一致的轨，
    // 但一条轨永远不能代表整个参考段 —— 逐轨核对在下面做。
    for (const auto& ref : result.reference_tracks) {
        if (result.reference_timescale == 0 && ref.timescale > 0)
            result.reference_timescale = ref.timescale;
        if (evidence.sidx_timescale > 0 && ref.timescale == evidence.sidx_timescale)
            result.reference_timescale = ref.timescale;
    }

    // ---- R03：源侧独立证据交叉校验 ----
    // track_id 只说明编号相同；时基与解码参数必须另有证据，否则播放时长可能被
    // 悄悄改写。有冲突直接拒绝，没有充分证据标 unverified 并在报告里列出风险。
    const TimescaleCheckOutcome timescale_check = CheckTrackTimescales(result.reference_tracks, evidence);
    result.timescale_check = timescale_check.detail;
    result.borrowed_parameters.push_back("逐轨 mdhd 时基核对: " + timescale_check.detail);
    if (timescale_check.rejected) {
        result.compatibility = "rejected";
        result.compatibility_reason = timescale_check.detail;
        result.error = "参考初始化段与源分片不兼容: " + timescale_check.detail;
        return result;
    }

    const RepairCompatibilityVerdict verdict = EvaluateReferenceCompatibility(
        evidence.sidx_timescale, result.reference_timescale, evidence.bitstream, reference_codecs);
    result.compatibility = verdict.status;
    result.compatibility_reason = verdict.reason;
    result.borrowed_parameters.push_back(
        "源侧证据: sidx_timescale=" + std::to_string(evidence.sidx_timescale) +
        ", bitstream=" + (evidence.bitstream.empty() ? "unknown" : evidence.bitstream) + " (" +
        (evidence.provenance.empty() ? "unavailable" : evidence.provenance) + ")");
    if (verdict.status == "rejected") {
        result.error = "参考初始化段与源分片不兼容: " + verdict.reason;
        return result;
    }

    // 多轨参考段的 mdhd timescale 本来就可能不同。sidx 只核对了其中一条轨，
    // 其余轨必须靠源侧 trun 时长逐条验证，否则不能自称「已核验」。
    if (result.compatibility == "verified" && evidence.sidx_timescale > 0 &&
        timescale_check.checked_tracks < required_track_ids.size()) {
        result.compatibility = "unverified";
        result.compatibility_reason = "参考段含 " + std::to_string(result.reference_tracks.size()) +
                                      " 条轨，源侧只逐轨核对了 " + std::to_string(timescale_check.checked_tracks) +
                                      "/" + std::to_string(required_track_ids.size()) +
                                      " 条轨的 mdhd timescale；其余轨时基没有独立证据，不能断言兼容";
    }

    std::string ftyp_bytes;
    if (!ReadBoxBytes(reference_path, *ftyp, &ftyp_bytes)) {
        result.error = "读取参考文件 ftyp 失败";
        return result;
    }
    std::string moov_bytes;
    if (!ReadBoxBytes(reference_path, *moov, &moov_bytes)) {
        result.error = "读取参考文件 moov 失败";
        return result;
    }
    const std::string init_bytes = ftyp_bytes + moov_bytes;

    result.bytes.assign(init_bytes.begin(), init_bytes.end());
    result.init_size = static_cast<uint64_t>(result.bytes.size());
    result.ok = !result.bytes.empty();
    if (!result.ok)
        result.error = "初始化段为空";
    return result;
}

} // namespace repair
} // namespace videoeye
