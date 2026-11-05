#include "core/repair/RepairInitSegment.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "core/domain/io/Utf8Path.h"
#include "core/media/container/IsobmffParser.h"
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

} // namespace

RepairInitSegment RepairInitSegmentReader::Read(const std::string& reference_path,
                                                const std::vector<uint32_t>& required_track_ids,
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
    if (!FindChild(*mvex, "trex")) {
        result.error = "参考文件的 moov/mvex 缺少 trex，无法为媒体分片提供默认样本参数";
        return result;
    }

    std::set<uint32_t> reference_tracks;
    std::ostringstream codec_summary;
    for (const auto& track : parsed.tracks) {
        if (track.track_id == 0)
            continue;
        reference_tracks.insert(track.track_id);
        result.track_ids.push_back(track.track_id);
        if (codec_summary.tellp() > 0)
            codec_summary << ", ";
        codec_summary << "轨道 " << track.track_id << " " << track.TypeName();
        if (!track.codec.empty())
            codec_summary << "(" << track.codec << ")";
    }
    result.codec_summary = codec_summary.str();

    for (uint32_t track_id : required_track_ids) {
        if (reference_tracks.count(track_id) == 0) {
            result.error = "参考文件没有源分片需要的轨道 " + std::to_string(track_id) + "（参考轨道数 " +
                           std::to_string(reference_tracks.size()) + "）";
            return result;
        }
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
