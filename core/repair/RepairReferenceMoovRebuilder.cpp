#include "core/repair/RepairReferenceMoovRebuilder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBoxWriter.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairDisplayOrder.h"
#include "core/repair/RepairMdatSelfCheck.h"
#include "core/repair/RepairSampleScan.h"
#include "core/repair/RepairUtils.h"

namespace videoeye {
namespace repair {
namespace {

using videoeye::model::RecoveredSample;
using videoeye::model::SampleConfidence;
using videoeye::model::SampleTimestampEvidence;

constexpr std::uint32_t kMaxAuBytes = 256u * 1024u * 1024u;
constexpr std::size_t kParsedNalBytes = 64u * 1024u;
constexpr std::uint64_t kMaxBoxBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint32_t kAacSampleRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                               16000, 12000, 11025, 8000,  7350,  0,     0,     0};

std::uint16_t Be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t Be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

bool Stopped(const std::atomic<bool>* cancel, const RepairRunBudget* budget, std::string* error) {
    if (budget != nullptr) {
        if (!budget->Checkpoint())
            return false;
        *error = budget->Canceled() ? "已取消" : "超出本次修复预算";
        return true;
    }
    if (cancel != nullptr && cancel->load()) {
        *error = "已取消";
        return true;
    }
    return false;
}

bool ReadAt(std::ifstream& in, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in)
        return false;
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.gcount() == static_cast<std::streamsize>(size);
}

bool ReadRange(const std::string& path, std::uint64_t offset, std::uint64_t size, std::vector<std::uint8_t>* out,
               std::string* error) {
    if (size == 0 || size > kMaxBoxBytes) {
        *error = "盒大小非法或超过有界读取上限: " + std::to_string(size);
        return false;
    }
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in) {
        *error = "无法读取文件: " + path;
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    if (!ReadAt(in, offset, out->data(), out->size())) {
        *error = "读取盒字节失败（offset=" + std::to_string(offset) + "）";
        return false;
    }
    return true;
}

const IsobmffBox* FindChild(const IsobmffBox& parent, const std::string& type) {
    for (const auto& child : parent.children) {
        if (child.type == type)
            return &child;
    }
    return nullptr;
}

// 从 sample entry 的原始字节里取出某个子盒的载荷（这里只要 avcC）。
// sample entry 是逐字节借来的（不重新序列化），所以不能靠 IsobmffParser 再解析
// 一遍 —— 那个解析器要的是文件。只接受「四字符码前 4 字节就是盒大小」的合法布局，
// 大小越界直接判失败，不做启发式搜索。
bool ExtractBoxPayload(const std::vector<std::uint8_t>& container, const char* type, std::vector<std::uint8_t>* out) {
    if (container.size() < 8)
        return false;
    for (std::size_t i = 4; i + 4 <= container.size(); ++i) {
        if (std::memcmp(container.data() + i, type, 4) != 0)
            continue;
        const std::uint32_t size = Be32(container.data() + i - 4);
        const std::size_t payload_start = i + 4;
        if (size < 8 || size > container.size() - (i - 4))
            return false;
        const std::size_t payload_size = size - 8;
        out->assign(container.begin() + static_cast<std::ptrdiff_t>(payload_start),
                    container.begin() + static_cast<std::ptrdiff_t>(payload_start + payload_size));
        return true;
    }
    return false;
}

// MPEG-4 descriptor 的规整递归：ES_Descriptor(0x03) 前 3 字节是 ES_ID + flags，
// DecoderConfigDescriptor(0x04) 前 13 字节是对象类型 / 流类型 / 码率等固定字段，
// 之后才是嵌套 descriptor。不能用「在线性字节里找 0x05」的启发式 —— 那会把
// 恰好出现的 0x05 当成 AudioSpecificConfig，正是本项目反复拒绝的做法。
bool FindDescriptor(const std::uint8_t* data, std::size_t size, std::uint8_t wanted, const std::uint8_t** body,
                    std::size_t* body_size, int depth = 0) {
    if (depth > 4)
        return false;
    std::size_t pos = 0;
    while (pos < size) {
        const std::uint8_t tag = data[pos++];
        std::uint32_t length = 0;
        bool terminated = false;
        for (int i = 0; i < 4; ++i) {
            if (pos >= size)
                return false;
            const std::uint8_t b = data[pos++];
            length = (length << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0) {
                terminated = true;
                break;
            }
        }
        if (!terminated || pos + length > size)
            return false;
        const std::uint8_t* child = data + pos;
        if (tag == wanted) {
            *body = child;
            *body_size = length;
            return true;
        }
        if (tag == 0x03 && length >= 3 && FindDescriptor(child + 3, length - 3, wanted, body, body_size, depth + 1))
            return true;
        if (tag == 0x04 && length >= 13 && FindDescriptor(child + 13, length - 13, wanted, body, body_size, depth + 1))
            return true;
        pos += length;
    }
    return false;
}

// mp4a sample entry 里的 AudioSpecificConfig（tag 0x05）与声道 / 采样率必须自洽。
// 这是「参考文件自身没被篡改」的独立证据，不依赖损坏文件。
bool ParseAudioConfig(const std::vector<std::uint8_t>& entry, std::uint32_t* rate, std::uint32_t* channels,
                      std::uint8_t* aot, std::uint8_t* sfi, std::string* error) {
    if (entry.size() < 36) {
        *error = "mp4a sample entry 过短";
        return false;
    }
    *channels = Be16(entry.data() + 24);
    *rate = Be32(entry.data() + 32) >> 16;
    // 固定 28 字节之后是子盒（esds）。按盒链找，不假设 esds 一定紧邻。
    std::size_t off = 36;
    const std::uint8_t* asc = nullptr;
    std::size_t asc_size = 0;
    bool found_esds = false;
    while (off + 8 <= entry.size()) {
        const std::uint32_t box_size = Be32(entry.data() + off);
        const std::string type(reinterpret_cast<const char*>(entry.data() + off + 4), 4);
        if (box_size < 8 || off + box_size > entry.size())
            break;
        if (type == "esds" && box_size > 12) {
            found_esds = true;
            found_esds = FindDescriptor(entry.data() + off + 12, box_size - 12, 0x05, &asc, &asc_size) || found_esds;
            break;
        }
        off += box_size;
    }
    if (!found_esds || asc == nullptr || asc_size < 2) {
        *error = "mp4a sample entry 里找不到 AudioSpecificConfig";
        return false;
    }
    *aot = static_cast<std::uint8_t>(asc[0] >> 3);
    *sfi = static_cast<std::uint8_t>(((asc[0] & 0x07u) << 1) | (asc[1] >> 7));
    const std::uint8_t asc_channels = static_cast<std::uint8_t>((asc[1] >> 3) & 0x0F);
    if (*sfi >= 13 || kAacSampleRates[*sfi] != *rate) {
        *error = "AudioSpecificConfig 的采样率索引与 mp4a 声明的采样率不一致（sfi=" + std::to_string(*sfi) +
                 ", rate=" + std::to_string(*rate) + "）";
        return false;
    }
    if (asc_channels != *channels || *channels == 0 || *channels > 8) {
        *error = "AudioSpecificConfig 的声道数与 mp4a 声明不一致";
        return false;
    }
    return true;
}

bool FirstSampleEntry(const std::string& path, const IsobmffBox& stsd, std::vector<std::uint8_t>* out,
                      std::string* error) {
    if (!stsd.children.empty()) {
        const IsobmffBox& first = stsd.children.front();
        return ReadRange(path, first.offset, first.size, out, error);
    }
    // 解析器没下钻到 sample entry 时，按规范手工读 stsd 的第一个 entry。
    const std::uint64_t payload = stsd.offset + stsd.header_size;
    if (stsd.size < stsd.header_size + 12) {
        *error = "stsd 盒过短";
        return false;
    }
    std::array<std::uint8_t, 16> head{};
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in || !ReadAt(in, payload, head.data(), 16)) {
        *error = "读取 stsd 头失败";
        return false;
    }
    const std::uint32_t entry_count = Be32(head.data() + 4);
    if (entry_count == 0) {
        *error = "stsd 没有 sample entry";
        return false;
    }
    std::array<std::uint8_t, 8> box_head{};
    if (!ReadAt(in, payload + 8, box_head.data(), box_head.size())) {
        *error = "读取 sample entry 头失败";
        return false;
    }
    const std::uint32_t entry_size = Be32(box_head.data());
    if (entry_size < 8 || payload + 8 + entry_size > stsd.offset + stsd.size) {
        *error = "stsd sample entry 越界";
        return false;
    }
    return ReadRange(path, payload + 8, entry_size, out, error);
}

std::uint32_t SamplesPerChunk(const IsobmffTrack& track, std::uint32_t one_based_chunk) {
    std::uint32_t value = 0;
    for (const auto& entry : track.stsc) {
        if (entry.first_chunk <= one_based_chunk)
            value = entry.samples_per_chunk;
        else
            break;
    }
    return value;
}

std::vector<std::uint32_t> TrackSizes(const IsobmffTrack& track) {
    if (track.stsz.default_size > 0)
        return std::vector<std::uint32_t>(track.stsz.sample_count, track.stsz.default_size);
    return track.stsz.sizes;
}

struct RawChunk {
    std::uint64_t offset = 0;
    std::uint64_t relative_start = 0;
    int track = -1;
    std::uint32_t samples = 0;
};

// ---- 参考样片：读取 + 自洽核验 ----
RepairReferenceProfile ReadProfileImpl(const std::string& reference_path, const RepairRunBudget* budget) {
    RepairReferenceProfile profile;
    if (const std::string stop = BudgetStopMessage(budget, "参考样片读取阶段"); !stop.empty()) {
        profile.error = stop;
        return profile;
    }
    IsobmffFile parsed;
    if (!IsobmffParser::Parse(reference_path, parsed) || !parsed.ok || !parsed.found_moov) {
        profile.error = "参考文件不是可解析的完整 MP4: " + parsed.error_message;
        return profile;
    }
    if (parsed.fragmented || parsed.tracks.size() != 2) {
        profile.error = "第一版只支持「一条 H.264 视频 + 一条 AAC 音频」的普通 MP4 参考样片（当前 " +
                        std::to_string(parsed.tracks.size()) + " 轨" + (parsed.fragmented ? "，且为分片 MP4" : "") +
                        "）";
        return profile;
    }

    const IsobmffBox* moov = nullptr;
    const IsobmffBox* ftyp = nullptr;
    const IsobmffBox* mdat = nullptr;
    for (const auto& box : parsed.top_level) {
        if (box.type == "moov")
            moov = &box;
        else if (box.type == "ftyp")
            ftyp = &box;
        else if (box.type == "mdat")
            mdat = &box;
    }
    if (moov == nullptr || ftyp == nullptr || mdat == nullptr) {
        profile.error = "参考文件缺少顶层 ftyp / moov / mdat";
        return profile;
    }
    const std::uint64_t reference_payload_start = mdat->offset + mdat->header_size;
    const std::uint64_t reference_payload_end = mdat->offset + mdat->size;
    if (!ReadRange(reference_path, ftyp->offset, ftyp->size, &profile.ftyp, &profile.error))
        return profile;
    profile.movie_timescale = parsed.movie_timescale > 0 ? parsed.movie_timescale : 1000;

    std::vector<const IsobmffBox*> traks;
    for (const auto& child : moov->children) {
        if (child.type == "trak")
            traks.push_back(&child);
    }
    if (traks.size() != 2) {
        profile.error = "参考文件 moov 下的 trak 数量不是 2";
        return profile;
    }

    std::vector<RawChunk> raw_chunks;
    for (std::size_t i = 0; i < traks.size(); ++i) {
        const IsobmffTrack& track = parsed.tracks[i];
        const IsobmffBox* mdia = FindChild(*traks[i], "mdia");
        const IsobmffBox* minf = mdia != nullptr ? FindChild(*mdia, "minf") : nullptr;
        const IsobmffBox* stbl = minf != nullptr ? FindChild(*minf, "stbl") : nullptr;
        const IsobmffBox* stsd = stbl != nullptr ? FindChild(*stbl, "stsd") : nullptr;
        const IsobmffBox* hdlr = mdia != nullptr ? FindChild(*mdia, "hdlr") : nullptr;
        if (stsd == nullptr || hdlr == nullptr || mdia == nullptr) {
            profile.error = "参考文件轨道 " + std::to_string(i + 1) + " 缺少 stsd / hdlr";
            return profile;
        }
        // hdlr: full box(4) + pre_defined(4) + handler_type(4)
        std::array<std::uint8_t, 12> hdlr_head{};
        std::ifstream in(ToNativePath(reference_path), std::ios::binary);
        if (!in || !ReadAt(in, hdlr->offset + hdlr->header_size, hdlr_head.data(), hdlr_head.size())) {
            profile.error = "读取参考文件 hdlr 失败";
            return profile;
        }
        const std::string handler(reinterpret_cast<const char*>(hdlr_head.data() + 8), 4);
        const bool is_video = handler == "vide";
        const bool is_audio = handler == "soun";
        if (handler != track.handler || (!is_video && !is_audio)) {
            profile.error = "参考文件轨道 " + std::to_string(i + 1) + " 的 hdlr 与解析结果不一致";
            return profile;
        }
        if (is_video && track.codec.find("avc") == std::string::npos && track.codec != "h264") {
            profile.error = "参考文件视频轨编码不是 H.264（" + track.codec + "），第一版不支持";
            return profile;
        }
        if (is_audio && track.codec != "mp4a") {
            profile.error = "参考文件音频轨编码不是 AAC/mp4a（" + track.codec + "），第一版不支持";
            return profile;
        }
        if (track.has_ctts) {
            // 含 ctts 说明参考片有 B 帧重排。**不再拒绝**，但绝不借用它的合成偏移：
            // 偏移是「这份文件的帧」的属性，照抄等于把别人的显示顺序按到本片上。
            // 显示顺序由损坏文件自身的码流 POC 实测后写入产物 ctts（切片 H）。
            if (is_video)
                profile.video.has_ctts = true;
            else
                profile.audio.has_ctts = true;
            profile.limitations.push_back("参考样片轨道 " + std::to_string(track.track_id) +
                                          " 含 ctts：不借用其合成偏移，显示顺序改由损坏文件码流实测");
        }
        // 尾帧不是恒定样本时长的例外，两种真实来源：
        //   * AAC 编码器留一个不足 1024 采样的短帧，如 [(57,1024),(1,1023)]；
        //   * 测试工厂 / 部分 muxer 给最后一帧写 delta=0，如 [(39,512),(1,0)]。
        // 只认「单条 stts」会把正常文件全部误拒。这里要求除最后一条的单个
        // 尾帧外全部恒定；重建时尾帧按主 delta 归一化，PTS 序列不变（末条
        // delta 只影响该帧 duration，不影响它自己的 PTS），差异如实记 limitation。
        const std::uint32_t delta = track.stts.front().sample_delta;
        bool constant_delta = delta > 0;
        bool has_short_tail = false;
        for (std::size_t s = 0; s < track.stts.size(); ++s) {
            if (track.stts[s].sample_delta == delta)
                continue;
            if (s + 1 == track.stts.size() && track.stts[s].sample_count == 1 && track.stts[s].sample_delta < delta) {
                has_short_tail = true;
                continue;
            }
            constant_delta = false;
        }
        if (!constant_delta) {
            profile.error = "参考文件轨道 " + std::to_string(track.track_id) +
                            " 的 stts 不是恒定样本时长（第一版只支持 CFR / 1024 采样每帧）";
            return profile;
        }
        const std::uint32_t total_samples = track.SttsSampleCount();
        const std::vector<std::uint32_t> sizes = TrackSizes(track);
        if (sizes.empty() || sizes.size() != total_samples) {
            profile.error = "参考文件轨道 " + std::to_string(track.track_id) + " 的 stsz 与 stts 样本数不一致（" +
                            std::to_string(sizes.size()) + " vs " + std::to_string(total_samples) + "）";
            return profile;
        }

        RepairReferenceTrackProfile& dst = is_video ? profile.video : profile.audio;
        dst.track_id = track.track_id;
        dst.codec = track.codec;
        dst.handler = handler;
        dst.timescale = track.media_timescale;
        dst.sample_delta = delta;
        dst.sample_count = total_samples;
        dst.candidate_sizes = sizes;
        if (has_short_tail)
            profile.limitations.push_back("参考音频轨含不足 1024 采样的尾帧，重建按 1024 归一化");
        if (!FirstSampleEntry(reference_path, *stsd, &dst.sample_entry, &profile.error))
            return profile;
        if (is_video) {
            if (dst.sample_entry.size() < 36) {
                profile.error = "avc1 sample entry 过短";
                return profile;
            }
            dst.width = Be16(dst.sample_entry.data() + 32);
            dst.height = Be16(dst.sample_entry.data() + 34);
            if (dst.width == 0 || dst.height == 0) {
                profile.error = "avc1 sample entry 的宽高为 0";
                return profile;
            }
        } else {
            if (!ParseAudioConfig(dst.sample_entry, &dst.sample_rate, &dst.channels, &dst.aac_object_type, &dst.aac_sfi,
                                  &profile.error))
                return profile;
            if (dst.timescale != dst.sample_rate) {
                profile.error = "参考文件音频轨 mdhd timescale=" + std::to_string(dst.timescale) + " 与 mp4a 采样率 " +
                                std::to_string(dst.sample_rate) + " 不一致；借用该参考会改变播放速度，拒绝";
                return profile;
            }
            if (dst.sample_delta != 1024) {
                profile.error = "参考文件音频轨样本时长 " + std::to_string(dst.sample_delta) +
                                " 不是 AAC 恒定的 1024 采样/帧，第一版不支持";
                return profile;
            }
        }
        if (dst.timescale == 0) {
            profile.error = "参考文件轨道 " + std::to_string(dst.track_id) + " 的 mdhd timescale 为 0";
            return profile;
        }

        std::uint64_t declared = 0;
        for (std::size_t c = 0; c < track.chunk_offsets.size(); ++c) {
            const std::uint32_t spc = SamplesPerChunk(track, static_cast<std::uint32_t>(c + 1));
            if (spc == 0) {
                profile.error =
                    "参考文件轨道 " + std::to_string(dst.track_id) + " 的 stsc 无法解释 chunk " + std::to_string(c + 1);
                return profile;
            }
            const std::uint64_t offset = track.chunk_offsets[c];
            if (offset < reference_payload_start || offset >= reference_payload_end) {
                profile.error = "参考文件轨道 " + std::to_string(dst.track_id) + " 的 chunk " + std::to_string(c + 1) +
                                " 偏移落在 mdat 载荷之外";
                return profile;
            }
            raw_chunks.push_back(RawChunk{offset, offset - reference_payload_start, is_video ? 0 : 1, spc});
            declared += spc;
        }
        if (declared != dst.sample_count || track.chunk_offsets.empty()) {
            profile.error = "参考文件轨道 " + std::to_string(dst.track_id) + " 的 chunk 布局与样本数不守恒（" +
                            std::to_string(declared) + " vs " + std::to_string(dst.sample_count) + "）";
            return profile;
        }
    }
    if (profile.video.sample_count == 0 || profile.audio.sample_count == 0) {
        profile.error = "参考文件缺少视频轨或音频轨";
        return profile;
    }
    std::sort(raw_chunks.begin(), raw_chunks.end(),
              [](const RawChunk& a, const RawChunk& b) { return a.relative_start < b.relative_start; });
    if (raw_chunks.empty() || raw_chunks.front().relative_start != 0) {
        profile.error = "参考文件 mdat 载荷起点没有 chunk 覆盖（第一版要求载荷被布局线索完整解释）";
        return profile;
    }
    for (std::size_t i = 1; i < raw_chunks.size(); ++i) {
        if (raw_chunks[i].relative_start == raw_chunks[i - 1].relative_start) {
            profile.error = "参考文件存在重叠 chunk 偏移，布局线索不唯一";
            return profile;
        }
    }
    for (const auto& chunk : raw_chunks)
        profile.chunks.push_back(RepairReferenceProfile::ChunkHint{chunk.track, chunk.samples, chunk.relative_start});

    std::ostringstream fingerprint;
    fingerprint << "avc1/" << profile.video.width << "x" << profile.video.height << "/ts" << profile.video.timescale
                << "/d" << profile.video.sample_delta << "/n" << profile.video.sample_count << ";mp4a/"
                << profile.audio.sample_rate << "/" << profile.audio.channels << "/n" << profile.audio.sample_count
                << ";chunks=" << profile.chunks.size();
    profile.layout_fingerprint = fingerprint.str();

    profile.compatibility = "verified";
    profile.compatibility_reason =
        "参考样片自身自洽：两轨 stts/stsz/stsc 守恒、AudioSpecificConfig 与 mp4a 采样率/声道一致、"
        "音频 mdhd timescale 等于采样率、样本时长 1024；布局线索还须在损坏文件里逐块字节守恒自证"
        "（参考含 ctts 时不借用其合成偏移，见 limitations）";
    profile.timescale_check = "视频 " + std::to_string(profile.video.sample_delta) + "/" +
                              std::to_string(profile.video.timescale) + " s，音频 1024/" +
                              std::to_string(profile.audio.timescale) +
                              " s；视频时基来自参考 SPS 所属轨道（源侧 moov 已丢失，无独立时基证据）";
    profile.borrowed_parameters.push_back("ftyp 兼容 brands（原样）");
    profile.borrowed_parameters.push_back("视频 stsd/avc1 sample entry（avcC 参数集、宽高）");
    profile.borrowed_parameters.push_back("音频 stsd/mp4a sample entry（esds / AudioSpecificConfig）");
    profile.borrowed_parameters.push_back("逐轨 mdhd timescale 与恒定 stts sample_delta（仅作待核对值）");
    profile.borrowed_parameters.push_back("chunk 布局线索：全局顺序 + 每 chunk 样本数 + 裸 AAC AU 长度候选");
    profile.limitations.push_back("裸 AAC AU 无自描述边界；音频样本长度取自参考 stsz 候选，须在损坏文件逐块守恒验证");
    profile.limitations.push_back("视频时基 / 帧率来自参考，源侧 moov 丢失后没有独立时基证据");
    profile.limitations.push_back("不支持 VFR / 多 mdat / 分片 MP4 / HEVC");
    if (profile.video.has_ctts)
        profile.limitations.push_back("参考视频轨含 ctts：产物 ctts 由损坏文件码流 POC 实测生成，"
                                      "不是从参考复制；POC 不可用时产物不写 ctts 并如实标注");
    profile.ok = true;
    return profile;
}

// ---- 损坏文件顶层结构 ----
struct SourceTopLevel {
    std::uint64_t payload = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t file_size = 0;
};

bool ReadSourceTopLevel(const std::string& path, SourceTopLevel* out, std::string* error) {
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in) {
        *error = "无法读取损坏文件: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    out->file_size = static_cast<std::uint64_t>(in.tellg());
    std::uint64_t offset = 0;
    std::uint32_t mdat_count = 0;
    while (offset + 8 <= out->file_size) {
        std::array<std::uint8_t, 16> head{};
        if (!ReadAt(in, offset, head.data(), 8))
            break;
        std::uint64_t size = Be32(head.data());
        const std::string type(reinterpret_cast<const char*>(head.data() + 4), 4);
        std::uint64_t header = 8;
        if (size == 1) {
            if (!ReadAt(in, offset + 8, head.data(), 8)) {
                *error = "读取 largesize 失败";
                return false;
            }
            size = 0;
            for (int i = 0; i < 8; ++i)
                size = (size << 8) | head[i];
            header = 16;
        } else if (size == 0) {
            size = out->file_size - offset;
        }
        if (size < header || size > out->file_size - offset) {
            *error = "损坏文件顶层盒 " + type + " 大小越界";
            return false;
        }
        if (type == "moov") {
            *error = "损坏文件仍含 moov，不需要重建";
            return false;
        }
        if (type == "moof" || type == "styp") {
            *error = "损坏文件是分片 MP4（含 moof/styp），第一版不支持";
            return false;
        }
        if (type == "mdat") {
            ++mdat_count;
            if (mdat_count == 1) {
                out->payload = offset + header;
                out->payload_size = size - header;
            }
        }
        offset += size;
    }
    if (mdat_count != 1 || out->payload_size == 0) {
        *error = "损坏文件必须恰好含一个非空 mdat（当前 " + std::to_string(mdat_count) + " 个）";
        return false;
    }
    return true;
}

// slice header 的 first_mb_in_slice（ue(v)）是否为 0 —— AU 边界的唯一码流证据。
bool SliceStartsAtZero(const std::uint8_t* nal, std::size_t len) {
    if (len < 2)
        return false;
    std::size_t bit = 8; // 跳过 NAL header
    std::uint32_t zeros = 0;
    while (bit < len * 8 && zeros < 32) {
        const std::uint8_t b = (nal[bit / 8] >> (7 - bit % 8)) & 1u;
        if (b)
            break;
        ++zeros;
        ++bit;
    }
    if (zeros >= 32 || bit + zeros >= len * 8)
        return false;
    std::uint32_t value = 1;
    for (std::uint32_t i = 0; i < zeros; ++i) {
        const std::size_t at = bit + 1 + i;
        if (at >= len * 8)
            return false;
        value = (value << 1) | ((nal[at / 8] >> (7 - at % 8)) & 1u);
    }
    return value - 1 == 0;
}

struct ScannedSample {
    int track = -1;
    std::uint64_t source_offset = 0;
    std::uint32_t size = 0;
    bool sync = false;
    std::uint64_t output_offset = 0;
};

struct SourceScan {
    bool ok = false;
    std::string error;
    std::string layout_evidence;
    std::vector<std::string> limitations;
    std::vector<ScannedSample> samples;
    std::uint64_t total_bytes = 0;
    RepairSampleLedger ledger;
    // 切片 H：视频访问单元边界 + 显示顺序实测结论。AU 范围由本函数的 NAL 链
    // 实测给出，不在显示顺序层重扫一遍（两处各扫迟早会不一致）。
    std::vector<RepairAuRange> video_access_units;
    RepairDisplayOrderPlan display_plan;
};

// end 是布局线索给出的该 chunk 硬边界。视频样本绝不能越界扫到下一个音频
// chunk —— 那会把 AAC 载荷当成 NAL 长度前缀（实测第一个视频 chunk 之后就是
// 音频 chunk，旧实现正是在这里报「NAL 长度非法」）。
bool ScanOneVideoAu(SampleByteSource& src, std::uint64_t pos, std::uint64_t end, std::uint32_t* au_size, bool* sync,
                    std::string* error) {
    std::uint64_t cursor = pos;
    std::uint64_t size = 0;
    bool has_vcl = false;
    *sync = false;
    while (cursor < end) {
        if (end - cursor < 4) {
            *error = "视频 AU 尾部长度前缀不完整（offset=" + std::to_string(cursor) + "）";
            return false;
        }
        std::array<std::uint8_t, 4> length_bytes{};
        if (!src.Read(cursor, length_bytes.data(), length_bytes.size())) {
            *error = src.Error().empty() ? std::string("读取 NAL 长度前缀失败") : src.Error();
            return false;
        }
        const std::uint32_t nal_size = Be32(length_bytes.data());
        if (nal_size == 0 || nal_size > end - cursor - 4 || nal_size > kMaxAuBytes) {
            *error = "NAL 长度非法（offset=" + std::to_string(cursor) + "，chunk 边界=" + std::to_string(end) + "）";
            return false;
        }
        const std::size_t parse_len =
            static_cast<std::size_t>(std::min<std::uint32_t>(nal_size, static_cast<std::uint32_t>(kParsedNalBytes)));
        std::vector<std::uint8_t> nal(parse_len);
        if (!src.Read(cursor + 4, nal.data(), parse_len)) {
            *error = src.Error().empty() ? std::string("读取 NAL 语法前缀失败") : src.Error();
            return false;
        }
        const std::uint8_t nal_type = nal[0] & 0x1Fu;
        if (nal_type == 1 || nal_type == 5) {
            if (has_vcl && SliceStartsAtZero(nal.data(), nal.size()))
                break;
            has_vcl = true;
            if (nal_type == 5)
                *sync = true;
        } else if (has_vcl) {
            break; // AUD / SEI / 参数集出现在 VCL 之后 => 下一个 AU
        }
        size += static_cast<std::uint64_t>(nal_size) + 4;
        cursor += static_cast<std::uint64_t>(nal_size) + 4;
    }
    if (!has_vcl || size == 0 || size > std::numeric_limits<std::uint32_t>::max()) {
        *error = "未能从长度前缀链切出有效视频 AU（offset=" + std::to_string(pos) + "）";
        return false;
    }
    *au_size = static_cast<std::uint32_t>(size);
    return true;
}

SourceScan ScanSource(const std::string& source_path, const RepairReferenceProfile& profile,
                      const std::atomic<bool>* cancel, const RepairRunBudget* budget, std::size_t scan_chunk_bytes) {
    SourceScan scan;
    SourceTopLevel top;
    if (!ReadSourceTopLevel(source_path, &top, &scan.error))
        return scan;

    ScanWindow window;
    window.file_offset = top.payload;
    window.size = top.payload_size;
    window.track_id = profile.video.track_id;
    window.stream_index = 0;
    window.segment_index = 0;
    window.config_id = "reference_assisted_moov_scan";
    ChunkedByteSource src(source_path, window, scan_chunk_bytes, budget, cancel);
    if (!src.Error().empty()) {
        scan.error = src.Error();
        return scan;
    }

    std::uint64_t pos = 0;
    std::size_t video_index = 0;
    std::size_t audio_index = 0;
    std::uint64_t video_chunks = 0;
    std::uint64_t audio_chunks = 0;
    for (std::size_t chunk_index = 0; chunk_index < profile.chunks.size(); ++chunk_index) {
        if (Stopped(cancel, budget, &scan.error))
            return scan;
        const auto& hint = profile.chunks[chunk_index];
        const std::uint64_t end =
            chunk_index + 1 < profile.chunks.size() ? profile.chunks[chunk_index + 1].relative_start : src.Size();
        if (pos != hint.relative_start) {
            scan.error = "布局线索错位：chunk " + std::to_string(chunk_index) + " 期望起点 " +
                         std::to_string(hint.relative_start) + "，源侧已推进到 " + std::to_string(pos);
            return scan;
        }
        if (end <= pos || end > src.Size()) {
            scan.error = "布局线索 chunk 边界非法（" + std::to_string(pos) + ".." + std::to_string(end) + "）";
            return scan;
        }
        if (hint.track == 0) {
            std::uint64_t cursor = pos;
            for (std::uint32_t k = 0; k < hint.samples; ++k) {
                std::uint32_t size = 0;
                bool sync = false;
                if (!ScanOneVideoAu(src, cursor, end, &size, &sync, &scan.error)) {
                    scan.error = "视频 chunk " + std::to_string(video_chunks) + " 第 " + std::to_string(k) +
                                 " 个 AU: " + scan.error;
                    return scan;
                }
                scan.samples.push_back(ScannedSample{0, top.payload + cursor, size, sync, 0});
                scan.video_access_units.push_back(RepairAuRange{cursor, size});
                cursor += size;
                ++video_index;
            }
            if (cursor != end) {
                scan.error = "视频 chunk " + std::to_string(video_chunks) + " 的 NAL 链只覆盖到 " +
                             std::to_string(cursor) + "，未达到源侧 chunk 边界 " + std::to_string(end);
                return scan;
            }
            pos = cursor;
            ++video_chunks;
        } else {
            if (audio_index + hint.samples > profile.audio.candidate_sizes.size()) {
                scan.error = "音频布局线索超出参考 stsz 候选数量";
                return scan;
            }
            std::uint64_t want = 0;
            for (std::uint32_t k = 0; k < hint.samples; ++k)
                want += profile.audio.candidate_sizes[audio_index + k];
            // 候选长度必须与源侧 chunk 边界逐字节守恒，不能只「不越界」。
            if (want == 0 || want != end - pos) {
                scan.error = "音频 chunk " + std::to_string(audio_chunks) + " 的候选长度 " + std::to_string(want) +
                             " 与源侧 chunk 边界 " + std::to_string(end - pos) + " 字节不守恒，布局线索不适用";
                return scan;
            }
            for (std::uint32_t k = 0; k < hint.samples; ++k) {
                const std::uint32_t size = profile.audio.candidate_sizes[audio_index];
                scan.samples.push_back(ScannedSample{1, top.payload + pos, size, true, 0});
                pos += size;
                ++audio_index;
            }
            ++audio_chunks;
        }
    }
    if (pos != src.Size()) {
        scan.error = "布局线索未能完整解释损坏文件 mdat：已消费 " + std::to_string(pos) + " / " +
                     std::to_string(src.Size()) + " 字节；剩余区间标不确定并拒绝";
        return scan;
    }
    if (video_index != profile.video.sample_count || audio_index != profile.audio.sample_count) {
        scan.error = "逐轨样本数与布局线索不守恒：视频 " + std::to_string(video_index) + "/" +
                     std::to_string(profile.video.sample_count) + "，音频 " + std::to_string(audio_index) + "/" +
                     std::to_string(profile.audio.sample_count);
        return scan;
    }

    // 切片 H：显示顺序必须在**损坏文件自身的码流**里实测。参考片的 ctts 一律不借
    // —— 合成偏移是「这份文件的帧」的属性，照抄等于把别人的显示顺序按到本片上。
    // 参数集可以借（那是解码配置，且已在源侧按 chunk 守恒核验过同源）。
    if (scan.video_access_units.size() >= 2) {
        RepairFrameDurationEvidence duration;
        duration.source = "stts_constant";
        duration.timescale = profile.video.timescale;
        duration.sample_delta = profile.video.sample_delta;
        duration.constant = profile.video.timescale > 0 && profile.video.sample_delta > 0;
        duration.detail = "参考 stts 恒定样本时长 " + std::to_string(profile.video.sample_delta) + "/" +
                          std::to_string(profile.video.timescale) + " s（源侧 moov 已丢失，时基无独立证据）";
        std::vector<std::uint8_t> avcc;
        if (ExtractBoxPayload(profile.video.sample_entry, "avcC", &avcc)) {
            scan.display_plan =
                MeasureDisplayOrder(src, scan.video_access_units, avcc, "h264", duration, cancel, budget);
        } else {
            scan.display_plan.unavailable_reason = "参考 sample entry 里没有可解析的 avcC 参数集";
        }
        if (!scan.display_plan.available && !scan.display_plan.unavailable_reason.empty())
            scan.limitations.push_back("显示顺序未实测: " + scan.display_plan.unavailable_reason);
    } else {
        scan.display_plan.unavailable_reason = "视频访问单元不足两个，无法判定显示顺序";
    }

    // 账本：每个样本都落在样本层，上三层计数由它派生。
    // 显示序号只在「实测帧数 == 视频 AU 数」时才敢用；否则退回解码序并保持
    // display_ordinal 未核验的语义（不拿解码序冒充显示序写进 ctts）。
    const bool display_index_usable =
        scan.display_plan.available && scan.display_plan.frames.size() == scan.video_access_units.size();
    const bool offsets_usable =
        display_index_usable && scan.display_plan.composition_offsets.size() == scan.video_access_units.size();
    std::size_t per_track_index[2] = {0, 0};
    for (const auto& sample : scan.samples) {
        RecoveredSample rec;
        rec.source_offset = sample.source_offset;
        rec.size = sample.size;
        rec.track_id = sample.track == 0 ? profile.video.track_id : profile.audio.track_id;
        rec.stream_index = sample.track;
        rec.segment_index = 0;
        rec.config_id = window.config_id;
        rec.decode_ordinal = per_track_index[sample.track];
        rec.display_ordinal = per_track_index[sample.track];
        const std::size_t decode_index = per_track_index[sample.track];
        if (sample.track == 0 && display_index_usable)
            rec.display_ordinal = scan.display_plan.frames[decode_index].display_index;
        rec.is_rap = sample.sync || sample.track == 1;
        rec.rap_evidence = sample.track == 1 ? "adts_equivalent_all_access" : (sample.sync ? "idr_nal" : "");
        const double step = sample.track == 0
                                ? static_cast<double>(profile.video.sample_delta) / profile.video.timescale
                                : 1024.0 / profile.audio.timescale;
        rec.dts_seconds = per_track_index[sample.track] * step;
        rec.pts_seconds = rec.dts_seconds;
        if (sample.track == 0 && offsets_usable) {
            // 合成偏移来自本文件码流 POC，单位是 mdhd timescale。
            const double shift =
                static_cast<double>(scan.display_plan.composition_offsets[decode_index]) / profile.video.timescale;
            rec.pts_seconds = rec.dts_seconds.value_or(0.0) + shift;
        }
        rec.duration_seconds = step;
        rec.timestamp_evidence = SampleTimestampEvidence::Inferred;
        rec.inference_basis = sample.track == 0 ? "参考 stts 恒定样本时长 + 源侧 NAL 边界实测"
                                                : "AAC 恒定 1024 采样/帧 + 参考采样率经源侧 chunk 守恒核对";
        if (sample.track == 0 && offsets_usable)
            rec.inference_basis += " + 显示顺序由源侧码流 POC 实测";
        rec.confidence = SampleConfidence::Inferred;
        rec.decode_status = model::SampleDecodeStatus::NotAttempted;
        scan.ledger.Add(rec);
        ++per_track_index[sample.track];
    }
    scan.total_bytes = scan.ledger.TotalBytes();
    if (scan.total_bytes != pos) {
        scan.error = "样本层总字节数与扫描消费字节数不一致";
        return scan;
    }
    std::ostringstream evidence;
    evidence << "chunk 线索 " << profile.chunks.size() << " 段（视频 " << video_chunks << " / 音频 " << audio_chunks
             << "），在损坏文件 mdat 里逐块推进 " << pos << "/" << src.Size()
             << " 字节无残留；视频 AU 边界由 4 字节 NAL 长度前缀 + first_mb_in_slice 实测，"
             << "音频 chunk 长度由参考 stsz 候选经源侧字节守恒确认且不与视频 NAL 链歧义";
    scan.layout_evidence = evidence.str();
    scan.limitations.push_back("音频样本边界依赖参考 stsz 候选 + 源侧 chunk 守恒，不是自描述恢复");
    scan.limitations.push_back("视频时基来自参考 stts/mdhd，源侧 moov 丢失后无独立时基证据");
    for (const auto& limit : scan.ledger.Limitations())
        scan.limitations.push_back(limit);
    scan.ok = true;
    return scan;
}

// ---- 双轨 moov ----
void WriteMatrix(RepairBoxWriter* writer) {
    writer->U32(0x00010000);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0x00010000);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0x40000000);
}

struct TrackOutput {
    const RepairReferenceTrackProfile* profile = nullptr;
    std::vector<std::uint32_t> sizes;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint32_t> sync_samples;
    // 切片 H：与 sizes 一一对应的有符号合成时间偏移（timescale tick）。
    // 条数不等于样本数时调用方必须留空 —— 宁可不写 ctts，也不写半张表。
    std::vector<std::int64_t> composition_offsets;
};

std::vector<std::uint8_t> BuildMoov(const RepairReferenceProfile& profile, const TrackOutput& video,
                                    const TrackOutput& audio, std::uint64_t payload_start,
                                    std::uint32_t movie_timescale) {
    RepairBoxWriter writer;
    const std::size_t moov = writer.BeginBox("moov");
    const auto to_movie_ticks = [&](double seconds) {
        return static_cast<std::uint32_t>(seconds * movie_timescale + 0.5);
    };
    const double video_seconds =
        static_cast<double>(video.sizes.size()) * video.profile->sample_delta / video.profile->timescale;
    const double audio_seconds =
        static_cast<double>(audio.sizes.size()) * audio.profile->sample_delta / audio.profile->timescale;
    const double movie_seconds = std::max(video_seconds, audio_seconds);
    {
        const std::size_t box = writer.BeginFullBox("mvhd", 0, 0);
        writer.U32(0);
        writer.U32(0);
        writer.U32(movie_timescale);
        writer.U32(to_movie_ticks(movie_seconds));
        writer.U32(0x00010000);
        writer.U16(0x0100);
        writer.Zeros(10);
        WriteMatrix(&writer);
        writer.Zeros(24);
        writer.U32(static_cast<std::uint32_t>(audio.profile->track_id) + 1);
        writer.EndBox(box);
    }
    const TrackOutput* tracks[2] = {&video, &audio};
    for (int index = 0; index < 2; ++index) {
        const TrackOutput& out = *tracks[index];
        const bool is_video = index == 0;
        const std::uint32_t media_duration =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(out.sizes.size()) * out.profile->sample_delta);
        const std::size_t trak = writer.BeginBox("trak");
        {
            const std::size_t box = writer.BeginFullBox("tkhd", 0, 3);
            writer.U32(0);
            writer.U32(0);
            writer.U32(out.profile->track_id);
            writer.U32(0);
            writer.U32(to_movie_ticks(is_video ? video_seconds : audio_seconds));
            writer.Zeros(8);
            writer.U16(0);
            writer.U16(0);
            writer.U16(is_video ? 0 : 0x0100);
            writer.U16(0);
            WriteMatrix(&writer);
            writer.U32(is_video ? out.profile->width << 16 : 0);
            writer.U32(is_video ? out.profile->height << 16 : 0);
            writer.EndBox(box);
        }
        {
            const std::size_t mdia = writer.BeginBox("mdia");
            {
                const std::size_t box = writer.BeginFullBox("mdhd", 0, 0);
                writer.U32(0);
                writer.U32(0);
                writer.U32(out.profile->timescale);
                writer.U32(media_duration);
                writer.U16(0x55C4);
                writer.U16(0);
                writer.EndBox(box);
            }
            {
                const std::size_t box = writer.BeginFullBox("hdlr", 0, 0);
                writer.U32(0);
                writer.FourCC(is_video ? "vide" : "soun");
                writer.Zeros(12);
                const std::string name = is_video ? "VideoHandler" : "SoundHandler";
                writer.Raw(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
                writer.U8(0);
                writer.EndBox(box);
            }
            {
                const std::size_t minf = writer.BeginBox("minf");
                if (is_video) {
                    const std::size_t vmhd = writer.BeginFullBox("vmhd", 0, 1);
                    writer.Zeros(8);
                    writer.EndBox(vmhd);
                } else {
                    const std::size_t smhd = writer.BeginFullBox("smhd", 0, 0);
                    writer.Zeros(4);
                    writer.EndBox(smhd);
                }
                {
                    const std::size_t dinf = writer.BeginBox("dinf");
                    const std::size_t dref = writer.BeginFullBox("dref", 0, 0);
                    writer.U32(1);
                    const std::size_t url = writer.BeginFullBox("url ", 0, 1);
                    writer.EndBox(url);
                    writer.EndBox(dref);
                    writer.EndBox(dinf);
                }
                {
                    const std::size_t stbl = writer.BeginBox("stbl");
                    {
                        const std::size_t stsd = writer.BeginFullBox("stsd", 0, 0);
                        writer.U32(1);
                        writer.Raw(out.profile->sample_entry);
                        writer.EndBox(stsd);
                    }
                    {
                        const std::size_t stts = writer.BeginFullBox("stts", 0, 0);
                        writer.U32(1);
                        writer.U32(static_cast<std::uint32_t>(out.sizes.size()));
                        writer.U32(out.profile->sample_delta);
                        writer.EndBox(stts);
                    }
                    if (!out.composition_offsets.empty() && out.composition_offsets.size() == out.sizes.size()) {
                        // version 1（有符号）：B 帧的第一帧通常需要负偏移，
                        // version 0 的无符号偏移只能靠「整体平移 + elst」表达，
                        // 那会把整条时间轴推后一段。这里做 run-length 压缩。
                        const std::size_t ctts = writer.BeginFullBox("ctts", 1, 0);
                        std::vector<std::pair<std::uint32_t, std::int32_t>> runs;
                        for (const std::int64_t offset : out.composition_offsets) {
                            const auto value = static_cast<std::int32_t>(std::max<std::int64_t>(
                                std::numeric_limits<std::int32_t>::min(),
                                std::min<std::int64_t>(std::numeric_limits<std::int32_t>::max(), offset)));
                            if (!runs.empty() && runs.back().second == value)
                                ++runs.back().first;
                            else
                                runs.emplace_back(1u, value);
                        }
                        writer.U32(static_cast<std::uint32_t>(runs.size()));
                        for (const auto& run : runs) {
                            writer.U32(run.first);
                            writer.U32(static_cast<std::uint32_t>(run.second));
                        }
                        writer.EndBox(ctts);
                    }
                    {
                        // 每个样本一个 chunk：stco 直接给出逐样本绝对偏移，
                        // 不复刻参考的 chunk 分组（那会让偏移再次依赖借来的布局）。
                        const std::size_t stsc = writer.BeginFullBox("stsc", 0, 0);
                        writer.U32(1);
                        writer.U32(1);
                        writer.U32(1);
                        writer.U32(1);
                        writer.EndBox(stsc);
                    }
                    {
                        const std::size_t stsz = writer.BeginFullBox("stsz", 0, 0);
                        writer.U32(0);
                        writer.U32(static_cast<std::uint32_t>(out.sizes.size()));
                        for (std::uint32_t size : out.sizes)
                            writer.U32(size);
                        writer.EndBox(stsz);
                    }
                    const bool use_co64 = !out.offsets.empty() && out.offsets.back() > 0xFFFFFFFFull;
                    if (use_co64) {
                        const std::size_t co64 = writer.BeginFullBox("co64", 0, 0);
                        writer.U32(static_cast<std::uint32_t>(out.offsets.size()));
                        for (std::uint64_t offset : out.offsets)
                            writer.U64(offset);
                        writer.EndBox(co64);
                    } else {
                        const std::size_t stco = writer.BeginFullBox("stco", 0, 0);
                        writer.U32(static_cast<std::uint32_t>(out.offsets.size()));
                        for (std::uint64_t offset : out.offsets)
                            writer.U32(static_cast<std::uint32_t>(offset));
                        writer.EndBox(stco);
                    }
                    if (!out.sync_samples.empty() && out.sync_samples.size() != out.sizes.size()) {
                        const std::size_t stss = writer.BeginFullBox("stss", 0, 0);
                        writer.U32(static_cast<std::uint32_t>(out.sync_samples.size()));
                        for (std::uint32_t sample : out.sync_samples)
                            writer.U32(sample);
                        writer.EndBox(stss);
                    }
                    writer.EndBox(stbl);
                }
                writer.EndBox(minf);
            }
            writer.EndBox(mdia);
        }
        writer.EndBox(trak);
    }
    writer.EndBox(moov);
    (void)payload_start;
    return writer.TakeBytes();
}

model::RepairReferenceAssist MakeAssist(const std::string& reference_path, const RepairReferenceProfile& profile,
                                        const SourceScan& scan) {
    model::RepairReferenceAssist assist;
    assist.used = true;
    assist.reference_name = FileName(reference_path);
    assist.compatibility = profile.compatibility;
    assist.compatibility_reason = profile.compatibility_reason;
    assist.borrowed_parameters = profile.borrowed_parameters;
    assist.layout_evidence = scan.layout_evidence;
    assist.timescale_check = profile.timescale_check;
    assist.limitations = profile.limitations;
    for (const auto& limit : scan.limitations)
        assist.limitations.push_back(limit);
    return assist;
}

} // namespace

RepairReferenceProfile RepairReferenceMoovRebuilder::ReadProfile(const std::string& reference_path,
                                                                 const RepairRunBudget* budget) {
    return ReadProfileImpl(reference_path, budget);
}

RepairReferenceMoovRebuilder::Probe RepairReferenceMoovRebuilder::ProbeWithReference(const std::string& source_path,
                                                                                     const std::string& reference_path,
                                                                                     const std::atomic<bool>* cancel,
                                                                                     const RepairRunBudget* budget,
                                                                                     std::size_t scan_chunk_bytes) {
    Probe probe;
    const RepairReferenceProfile profile = ReadProfileImpl(reference_path, budget);
    if (!profile.ok) {
        probe.reason = "参考样片不可用: " + profile.error;
        return probe;
    }
    const SourceScan scan = ScanSource(source_path, profile, cancel, budget, scan_chunk_bytes);
    if (!scan.ok) {
        probe.reason = "损坏文件无法用参考布局线索完整解释: " + scan.error;
        return probe;
    }
    probe.attemptable = true;
    probe.codec_summary = "H.264 " + std::to_string(profile.video.width) + "x" + std::to_string(profile.video.height) +
                          " + AAC " + std::to_string(profile.audio.sample_rate) + " Hz，视频 " +
                          std::to_string(profile.video.sample_count) + " 样本 / 音频 " +
                          std::to_string(profile.audio.sample_count) + " 样本";
    probe.limitations = profile.limitations;
    for (const auto& limit : scan.limitations)
        probe.limitations.push_back(limit);
    return probe;
}

RepairReferenceRebuildResult
RepairReferenceMoovRebuilder::Rebuild(const std::string& source_path, const std::string& reference_path,
                                      const std::string& output_path, const std::atomic<bool>* cancel,
                                      const RepairRunBudget* budget, std::size_t scan_chunk_bytes) {
    RepairReferenceRebuildResult result;
    result.attempted = true;
    if (Stopped(cancel, budget, &result.error))
        return result;

    result.profile = ReadProfileImpl(reference_path, budget);
    if (!result.profile.ok) {
        result.error = "参考样片不可用: " + result.profile.error;
        return result;
    }
    const SourceScan scan = ScanSource(source_path, result.profile, cancel, budget, scan_chunk_bytes);
    if (!scan.ok) {
        result.error = "损坏文件无法用参考布局线索完整解释: " + scan.error;
        return result;
    }

    TrackOutput video;
    TrackOutput audio;
    video.profile = &result.profile.video;
    audio.profile = &result.profile.audio;
    for (const auto& sample : scan.samples) {
        TrackOutput& out = sample.track == 0 ? video : audio;
        out.sizes.push_back(sample.size);
        if (sample.track == 0 && sample.sync)
            out.sync_samples.push_back(static_cast<std::uint32_t>(out.sizes.size()));
    }
    // 切片 H：合成偏移只有在「实测帧数 == 视频样本数」时才敢写；
    // 否则宁可不写 ctts（播放顺序保持解码序），也不写半张表。
    if (scan.display_plan.available && scan.display_plan.ctts_required &&
        scan.display_plan.composition_offsets.size() == video.sizes.size()) {
        video.composition_offsets = scan.display_plan.composition_offsets;
    }
    result.display_order = SummarizeDisplayOrder(scan.display_plan, !video.composition_offsets.empty());
    if (scan.display_plan.available && scan.display_plan.ctts_required && video.composition_offsets.empty()) {
        result.display_order.limitations.push_back("实测合成偏移条数与视频样本数不符，产物未写 ctts");
        result.display_order.ctts_written = false;
    }

    const std::uint64_t total_payload = scan.total_bytes;
    std::uint8_t mdat_header[16] = {};
    const std::size_t mdat_header_size = MakeMdatHeader(total_payload, mdat_header);
    std::uint64_t payload_start = 0;
    std::vector<std::uint8_t> moov_bytes;
    for (int pass = 0; pass < 4; ++pass) {
        // 先按上一轮的起点填 stco，再让 moov 长度反过来修正起点，迭代到收敛。
        std::uint64_t cursor = payload_start;
        std::size_t v = 0;
        std::size_t a = 0;
        video.offsets.assign(video.sizes.size(), 0);
        audio.offsets.assign(audio.sizes.size(), 0);
        for (const auto& sample : scan.samples) {
            if (sample.track == 0)
                video.offsets[v++] = cursor;
            else
                audio.offsets[a++] = cursor;
            cursor += sample.size;
        }
        moov_bytes = BuildMoov(result.profile, video, audio, payload_start, result.profile.movie_timescale);
        const std::uint64_t next = result.profile.ftyp.size() + moov_bytes.size() + mdat_header_size;
        if (next == payload_start && pass > 0)
            break;
        payload_start = next;
    }
    {
        std::uint64_t cursor = payload_start;
        std::size_t v = 0;
        std::size_t a = 0;
        for (const auto& sample : scan.samples) {
            if (sample.track == 0)
                video.offsets[v++] = cursor;
            else
                audio.offsets[a++] = cursor;
            cursor += sample.size;
        }
    }
    moov_bytes = BuildMoov(result.profile, video, audio, payload_start, result.profile.movie_timescale);
    if (result.profile.ftyp.size() + moov_bytes.size() + mdat_header_size != payload_start) {
        result.error = "重建 moov 尺寸未收敛，拒绝写出可能错位的结果";
        return result;
    }

    std::ofstream out(ToNativePath(output_path), std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "无法创建重建文件: " + output_path;
        return result;
    }
    out.write(reinterpret_cast<const char*>(result.profile.ftyp.data()),
              static_cast<std::streamsize>(result.profile.ftyp.size()));
    out.write(reinterpret_cast<const char*>(moov_bytes.data()), static_cast<std::streamsize>(moov_bytes.size()));
    out.write(reinterpret_cast<const char*>(mdat_header), static_cast<std::streamsize>(mdat_header_size));
    if (!out) {
        result.error = "写入重建文件头失败";
        return result;
    }

    std::ifstream in(ToNativePath(source_path), std::ios::binary);
    if (!in) {
        result.error = "无法读取损坏文件";
        return result;
    }
    std::uint64_t written = 0;
    for (const auto& sample : scan.samples) {
        if (Stopped(cancel, budget, &result.error))
            return result;
        std::vector<std::uint8_t> buffer(sample.size);
        if (!ReadAt(in, sample.source_offset, buffer.data(), buffer.size())) {
            result.error = "读取样本失败：offset=" + std::to_string(sample.source_offset);
            return result;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!out) {
            result.error = "写入重建样本失败";
            return result;
        }
        written += sample.size;
    }
    out.flush();
    if (!out) {
        result.error = "重建文件落盘失败";
        return result;
    }
    out.close();
    if (written != total_payload) {
        result.error = "重建写入字节数与样本表不一致";
        return result;
    }

    IsobmffFile check;
    if (!IsobmffParser::Parse(output_path, check) || !check.ok || !check.found_moov || check.tracks.size() != 2) {
        result.error = "重建结果结构自检失败: " + check.error_message;
        return result;
    }
    if (!VerifyRebuiltMdat(check, payload_start, payload_start + total_payload, total_payload, &result.error))
        return result;

    result.ok = true;
    result.samples_rebuilt = scan.ledger.Total();
    result.sample_ledger = scan.ledger.Summary();
    result.rebuilt_tracks = 2;
    result.output_bytes = result.profile.ftyp.size() + moov_bytes.size() + mdat_header_size + total_payload;
    result.assist = MakeAssist(reference_path, result.profile, scan);
    result.note = "参考样片辅助重建双轨 moov：" + result.profile.layout_fingerprint + "；样本 " +
                  std::to_string(scan.ledger.Total()) + " 个（视频 " + std::to_string(video.sizes.size()) + " / 音频 " +
                  std::to_string(audio.sizes.size()) + "）；" + scan.layout_evidence;
    if (!video.composition_offsets.empty()) {
        result.note += "；显示顺序由源侧码流 POC 实测，已写入 ctts（version 1 有符号，" +
                       std::to_string(result.display_order.reordered_frames) + " 帧重排）";
        result.assist.limitations.push_back("视频 ctts 由损坏文件码流 POC 实测生成，未借用参考片的合成偏移");
    }
    return result;
}

} // namespace repair
} // namespace videoeye
