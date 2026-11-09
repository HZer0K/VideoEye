#include "core/repair/RepairIndexRepair.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBoxWriter.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairMdatSelfCheck.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace repair {
namespace {

bool Cancelled(const std::atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load();
}

// 重建可能遍历上百万个 chunk；只靠调用方在「整个重建完成后」补查一次预算，
// 界面上设的 60 秒会被这段 CPU / IO 循环轻松吃穿。
bool Stopped(const std::atomic<bool>* cancel, const RepairRunBudget* budget, std::string* error) {
    if (budget != nullptr) {
        if (!budget->Checkpoint())
            return false;
        *error = budget->Canceled() ? "已取消" : "超出本次修复预算";
        return true;
    }
    if (Cancelled(cancel)) {
        *error = "已取消";
        return true;
    }
    return false;
}

const IsobmffBox* FindChild(const IsobmffBox& box, const char* type) {
    for (const auto& child : box.children) {
        if (child.type == type)
            return &child;
    }
    return nullptr;
}

const IsobmffBox* FindDescendant(const IsobmffBox& box, std::initializer_list<const char*> path) {
    const IsobmffBox* current = &box;
    for (const char* type : path) {
        current = FindChild(*current, type);
        if (current == nullptr)
            return nullptr;
    }
    return current;
}

bool ReadWholeBox(const std::string& path, const IsobmffBox& box, std::vector<uint8_t>* out) {
    if (box.size == 0)
        return false;
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in)
        return false;
    in.seekg(static_cast<std::streamoff>(box.offset), std::ios::beg);
    if (!in)
        return false;
    out->resize(static_cast<std::size_t>(box.size));
    in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(box.size));
    const std::streamsize got = in.gcount();
    out->resize(static_cast<std::size_t>(std::max<std::streamsize>(0, got)));
    return out->size() == box.size;
}

struct TrackRawBoxes {
    std::vector<uint8_t> tkhd;
    std::vector<uint8_t> mdhd;
    std::vector<uint8_t> hdlr;
    std::vector<uint8_t> media_header; // vmhd / smhd
    std::vector<uint8_t> dinf;
    std::vector<uint8_t> stsd;
    std::vector<uint8_t> edts; // 可空
};

struct ChunkRef {
    std::size_t track_index = 0;
    std::uint32_t chunk_index = 0; // 1-based
    std::uint64_t size = 0;
    std::uint64_t original_offset = 0;
};

struct MdatInfo {
    const IsobmffBox* box = nullptr;
    std::uint64_t payload = 0;
    std::uint64_t payload_size = 0;
};

std::uint32_t SamplesPerChunk(const std::vector<StscEntry>& stsc, std::uint32_t chunk_index) {
    std::uint32_t result = 0;
    for (const auto& entry : stsc) {
        if (entry.first_chunk <= chunk_index)
            result = entry.samples_per_chunk;
        else
            break;
    }
    return result;
}

std::uint32_t SampleSizeAt(const StszTable& stsz, std::uint32_t sample_index) {
    if (stsz.default_size != 0)
        return stsz.default_size;
    if (sample_index < stsz.sizes.size())
        return stsz.sizes[sample_index];
    return 0;
}

bool BuildChunkRefs(const IsobmffTrack& track, std::size_t track_index, std::vector<ChunkRef>* refs,
                    std::string* error) {
    if (!track.has_stsc || !track.has_stsz || (!track.has_stco && !track.has_co64)) {
        *error = "轨道 " + std::to_string(track.track_id) + " 缺少 stsc/stsz/stco 之一";
        return false;
    }
    if (track.tables_truncated) {
        *error = "轨道 " + std::to_string(track.track_id) + " 的样本表被截断，无法安全重建";
        return false;
    }
    if (track.stsz.default_size == 0 && track.stsz.sizes.size() != track.stsz.sample_count) {
        *error = "轨道 " + std::to_string(track.track_id) + " 的 stsz 条目数与 sample_count 不一致";
        return false;
    }
    if (track.chunk_offsets.empty()) {
        *error = "轨道 " + std::to_string(track.track_id) + " 没有任何 chunk 偏移";
        return false;
    }

    std::uint32_t sample_index = 0;
    for (std::uint32_t chunk = 1; chunk <= track.chunk_offsets.size(); ++chunk) {
        std::uint32_t per_chunk = SamplesPerChunk(track.stsc, chunk);
        if (per_chunk == 0) {
            if (sample_index >= track.stsz.sample_count)
                break;
            per_chunk = track.stsz.sample_count - sample_index; // stsc 的 0 = 剩余全部样本
        }
        if (sample_index + per_chunk > track.stsz.sample_count) {
            *error = "轨道 " + std::to_string(track.track_id) + " 的 stsc 展开超过 sample_count";
            return false;
        }
        std::uint64_t size = 0;
        for (std::uint32_t i = 0; i < per_chunk; ++i)
            size += SampleSizeAt(track.stsz, sample_index + i);
        refs->push_back(ChunkRef{track_index, chunk, size, track.chunk_offsets[chunk - 1]});
        sample_index += per_chunk;
    }
    if (sample_index != track.stsz.sample_count) {
        *error = "轨道 " + std::to_string(track.track_id) + " 的 stsc/stsz 展开样本数 " + std::to_string(sample_index) +
                 " 与 stsz.sample_count " + std::to_string(track.stsz.sample_count) + " 不一致";
        return false;
    }
    return true;
}

bool WriteStts(RepairBoxWriter* writer, const IsobmffTrack& track, std::string* note, std::string* error) {
    const std::uint32_t sample_count = track.stsz.sample_count;
    const bool consistent = track.has_stts && track.SttsSampleCount() == sample_count;
    std::vector<SttsEntry> entries;
    if (consistent) {
        entries = track.stts;
    } else if (sample_count > 0 && track.media_duration > 0) {
        const std::uint32_t delta = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1, track.media_duration / std::max<std::uint32_t>(1, sample_count)));
        entries.push_back(SttsEntry{sample_count, delta});
        if (note != nullptr && note->empty())
            *note = "stts 条目计数与样本数不一致，已按 mdhd 平均帧时长重建；VFR 时间轴会被抹平";
    } else if (sample_count > 0) {
        // 没有可用的 mdhd 时长就无法证明帧时长，写 1 tick/样本等于静默改写播放速度。
        *error = "轨道 " + std::to_string(track.track_id) +
                 " 的 stts 计数与样本数不一致且 mdhd 时长为 0，无法证明帧时长，拒绝重建";
        return false;
    }
    const std::size_t box = writer->BeginFullBox("stts", 0, 0);
    writer->U32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        writer->U32(entry.sample_count);
        writer->U32(entry.sample_delta);
    }
    writer->EndBox(box);
    return true;
}

void WriteCtts(RepairBoxWriter* writer, const IsobmffTrack& track, std::string* note) {
    if (!track.has_ctts)
        return;
    const std::uint32_t sample_count = track.stsz.sample_count;
    if (track.CttsSampleCount() != sample_count) {
        if (note != nullptr)
            *note = "ctts 计数与样本数不一致，已移除合成偏移；B 帧显示顺序可能改变";
        return;
    }
    const std::size_t box = writer->BeginFullBox("ctts", 1, 0);
    writer->U32(static_cast<std::uint32_t>(track.ctts.size()));
    for (const auto& entry : track.ctts) {
        writer->U32(entry.sample_count);
        writer->U32(static_cast<std::uint32_t>(entry.sample_offset));
    }
    writer->EndBox(box);
}

void WriteStss(RepairBoxWriter* writer, const IsobmffTrack& track, std::string* note) {
    if (!track.has_stss)
        return;
    std::vector<std::uint32_t> valid;
    valid.reserve(track.stss.size());
    for (std::uint32_t index : track.stss) {
        if (index > 0 && index <= track.stsz.sample_count)
            valid.push_back(index);
    }
    if (valid.size() != track.stss.size() && note != nullptr)
        *note = "stss 中有越界同步样本标记，已剔除（只影响可寻址性，不影响可解码性）";
    if (valid.empty())
        return; // 没有有效同步标记 = 所有样本都按同步处理
    const std::size_t box = writer->BeginFullBox("stss", 0, 0);
    writer->U32(static_cast<std::uint32_t>(valid.size()));
    for (std::uint32_t index : valid)
        writer->U32(index);
    writer->EndBox(box);
}

void WriteStsz(RepairBoxWriter* writer, const IsobmffTrack& track) {
    const std::size_t box = writer->BeginFullBox("stsz", 0, 0);
    writer->U32(track.stsz.default_size);
    writer->U32(track.stsz.sample_count);
    if (track.stsz.default_size == 0) {
        for (std::uint32_t size : track.stsz.sizes)
            writer->U32(size);
    }
    writer->EndBox(box);
}

void WriteStsc(RepairBoxWriter* writer, const IsobmffTrack& track) {
    const std::size_t box = writer->BeginFullBox("stsc", 0, 0);
    writer->U32(static_cast<std::uint32_t>(track.stsc.size()));
    for (const auto& entry : track.stsc) {
        writer->U32(entry.first_chunk);
        writer->U32(entry.samples_per_chunk);
        writer->U32(entry.sample_description_index);
    }
    writer->EndBox(box);
}

void WriteChunkOffsets(RepairBoxWriter* writer, const std::vector<std::uint64_t>& offsets) {
    const bool use_co64 =
        std::any_of(offsets.begin(), offsets.end(), [](std::uint64_t offset) { return offset > 0xFFFFFFFFull; });
    const std::size_t box = writer->BeginFullBox(use_co64 ? "co64" : "stco", 0, 0);
    writer->U32(static_cast<std::uint32_t>(offsets.size()));
    for (std::uint64_t offset : offsets) {
        if (use_co64)
            writer->U64(offset);
        else
            writer->U32(static_cast<std::uint32_t>(offset));
    }
    writer->EndBox(box);
}

bool BuildMoov(const IsobmffFile& parsed, const std::vector<uint8_t>& mvhd, const std::vector<TrackRawBoxes>& raw_boxes,
               const std::vector<std::vector<std::uint64_t>>& track_offsets, std::string* note,
               std::vector<uint8_t>* out, std::string* error) {
    RepairBoxWriter writer;
    const std::size_t moov = writer.BeginBox("moov");
    writer.Raw(mvhd);
    for (std::size_t i = 0; i < parsed.tracks.size(); ++i) {
        const IsobmffTrack& track = parsed.tracks[i];
        const TrackRawBoxes& raw = raw_boxes[i];
        const std::size_t trak = writer.BeginBox("trak");
        writer.Raw(raw.tkhd);
        if (!raw.edts.empty())
            writer.Raw(raw.edts);
        const std::size_t mdia = writer.BeginBox("mdia");
        writer.Raw(raw.mdhd);
        writer.Raw(raw.hdlr);
        const std::size_t minf = writer.BeginBox("minf");
        writer.Raw(raw.media_header);
        writer.Raw(raw.dinf);
        const std::size_t stbl = writer.BeginBox("stbl");
        writer.Raw(raw.stsd);
        if (!WriteStts(&writer, track, note, error))
            return false;
        WriteCtts(&writer, track, note);
        WriteStss(&writer, track, note);
        WriteStsc(&writer, track);
        WriteStsz(&writer, track);
        WriteChunkOffsets(&writer, track_offsets[i]);
        writer.EndBox(stbl);
        writer.EndBox(minf);
        writer.EndBox(mdia);
        writer.EndBox(trak);
    }
    writer.EndBox(moov);
    *out = writer.TakeBytes();
    return true;
}

bool CopyRange(std::ifstream& in, std::ofstream& out, std::uint64_t offset, std::uint64_t size, std::uint64_t* written,
               std::string* error) {
    static const std::size_t kBlock = 1024u * 1024u;
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        *error = "读取源 chunk 失败：偏移 " + std::to_string(offset);
        return false;
    }
    std::vector<char> buffer(kBlock);
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kBlock));
        in.read(buffer.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            *error = "源 chunk 提前结束：偏移 " + std::to_string(offset) + "，剩余 " + std::to_string(remaining);
            return false;
        }
        out.write(buffer.data(), got);
        if (!out) {
            *error = "写入重建文件失败";
            return false;
        }
        remaining -= static_cast<std::uint64_t>(got);
        *written += static_cast<std::uint64_t>(got);
    }
    return true;
}

std::vector<uint8_t> MakeFtyp() {
    RepairBoxWriter writer;
    const std::size_t box = writer.BeginBox("ftyp");
    writer.FourCC("isom");
    writer.U32(512);
    writer.FourCC("isom");
    writer.FourCC("iso2");
    writer.FourCC("mp41");
    writer.EndBox(box);
    return writer.TakeBytes();
}

void WriteMdatHeader(std::ofstream& out, std::uint64_t payload_size) {
    std::uint8_t header[16] = {};
    const std::size_t header_size = MakeMdatHeader(payload_size, header);
    out.write(reinterpret_cast<const char*>(header), static_cast<std::streamsize>(header_size));
}

} // namespace

RepairIndexRepairResult RepairIndexRepair::Rebuild(const std::string& source_path, const std::string& output_path,
                                                   const std::atomic<bool>* cancel, const RepairRunBudget* budget) {
    RepairIndexRepairResult result;
    if (Stopped(cancel, budget, &result.error))
        return result;

    IsobmffFile parsed;
    if (!IsobmffParser::Parse(source_path, parsed)) {
        result.error = "解析源文件失败: " + parsed.error_message;
        return result;
    }
    if (!parsed.ok || !parsed.found_moov) {
        result.error = "源文件没有可用的 moov";
        return result;
    }
    if (parsed.fragmented) {
        result.error = "分片 MP4 的索引在 moof/trun 中，不走 moov 重建";
        return result;
    }

    const IsobmffBox* moov = nullptr;
    const IsobmffBox* ftyp = nullptr;
    const IsobmffBox* mdat = nullptr;
    std::uint32_t mdat_count = 0;
    for (const auto& box : parsed.top_level) {
        if (box.type == "moov" && moov == nullptr)
            moov = &box;
        else if (box.type == "ftyp" && ftyp == nullptr)
            ftyp = &box;
        else if (box.type == "mdat") {
            mdat = &box;
            ++mdat_count;
        }
    }
    if (moov == nullptr || mdat == nullptr) {
        result.error = "缺少 moov 或 mdat";
        return result;
    }
    if (mdat_count != 1) {
        result.error = "存在多个 mdat，chunk 交错布局无法在单 mdat 内重建";
        return result;
    }
    if (mdat->size < mdat->header_size) {
        result.error = "mdat 头部损坏";
        return result;
    }
    const MdatInfo mdat_info{mdat, mdat->offset + mdat->header_size, mdat->size - mdat->header_size};

    const IsobmffBox* mvhd_box = FindChild(*moov, "mvhd");
    if (mvhd_box == nullptr) {
        result.error = "moov 缺少 mvhd";
        return result;
    }
    std::vector<uint8_t> mvhd;
    if (!ReadWholeBox(source_path, *mvhd_box, &mvhd)) {
        result.error = "读取 mvhd 失败";
        return result;
    }

    std::vector<const IsobmffBox*> traks;
    for (const auto& child : moov->children) {
        if (child.type == "trak")
            traks.push_back(&child);
    }
    if (traks.size() != parsed.tracks.size()) {
        result.error = "moov 的 trak 数量与解析出的轨道数量不一致";
        return result;
    }

    for (const auto& track : parsed.tracks) {
        if (track.handler != "vide" && track.handler != "soun") {
            result.error = "轨道 " + std::to_string(track.track_id) + " 的 handler=" + track.handler +
                           "，当前索引重建只覆盖音视频轨";
            return result;
        }
    }

    std::vector<TrackRawBoxes> raw_boxes(parsed.tracks.size());
    for (std::size_t i = 0; i < traks.size(); ++i) {
        const IsobmffBox& trak = *traks[i];
        const IsobmffBox* tkhd = FindChild(trak, "tkhd");
        const IsobmffBox* mdia = FindChild(trak, "mdia");
        const IsobmffBox* edts = FindChild(trak, "edts");
        const IsobmffBox* mdhd = mdia != nullptr ? FindChild(*mdia, "mdhd") : nullptr;
        const IsobmffBox* hdlr = mdia != nullptr ? FindChild(*mdia, "hdlr") : nullptr;
        const IsobmffBox* minf = mdia != nullptr ? FindChild(*mdia, "minf") : nullptr;
        const IsobmffBox* vmhd = minf != nullptr ? FindChild(*minf, "vmhd") : nullptr;
        const IsobmffBox* smhd = minf != nullptr ? FindChild(*minf, "smhd") : nullptr;
        const IsobmffBox* dinf = minf != nullptr ? FindChild(*minf, "dinf") : nullptr;
        const IsobmffBox* stsd = FindDescendant(trak, {"mdia", "minf", "stbl", "stsd"});
        if (tkhd == nullptr || mdhd == nullptr || hdlr == nullptr || dinf == nullptr || stsd == nullptr ||
            (vmhd == nullptr && smhd == nullptr)) {
            result.error = "轨道 " + std::to_string(parsed.tracks[i].track_id) + " 缺少必要的 box，无法重建 moov";
            return result;
        }
        if (!ReadWholeBox(source_path, *tkhd, &raw_boxes[i].tkhd) ||
            !ReadWholeBox(source_path, *mdhd, &raw_boxes[i].mdhd) ||
            !ReadWholeBox(source_path, *hdlr, &raw_boxes[i].hdlr) ||
            !ReadWholeBox(source_path, *(vmhd != nullptr ? vmhd : smhd), &raw_boxes[i].media_header) ||
            !ReadWholeBox(source_path, *dinf, &raw_boxes[i].dinf) ||
            !ReadWholeBox(source_path, *stsd, &raw_boxes[i].stsd)) {
            result.error = "读取轨道 " + std::to_string(parsed.tracks[i].track_id) + " 的原始 box 失败";
            return result;
        }
        if (edts != nullptr && !ReadWholeBox(source_path, *edts, &raw_boxes[i].edts)) {
            result.error = "读取 edts 失败";
            return result;
        }
    }

    std::vector<ChunkRef> refs;
    for (std::size_t i = 0; i < parsed.tracks.size(); ++i) {
        if (Stopped(cancel, budget, &result.error))
            return result;
        if (!BuildChunkRefs(parsed.tracks[i], i, &refs, &result.error))
            return result;
    }

    bool chunk_offsets_outside_mdat = false;
    for (const auto& ref : refs) {
        if (ref.original_offset < mdat_info.payload ||
            ref.original_offset + ref.size > mdat_info.payload + mdat_info.payload_size) {
            chunk_offsets_outside_mdat = true;
            break;
        }
    }
    bool stts_mismatch = false;
    bool ctts_mismatch = false;
    bool stss_out_of_range = false;
    for (const auto& track : parsed.tracks) {
        if (track.has_stts && track.SttsSampleCount() != track.stsz.sample_count)
            stts_mismatch = true;
        if (track.has_ctts && track.CttsSampleCount() != track.stsz.sample_count)
            ctts_mismatch = true;
        for (std::uint32_t index : track.stss) {
            if (index > track.stsz.sample_count)
                stss_out_of_range = true;
        }
    }
    if (!chunk_offsets_outside_mdat && !stts_mismatch && !ctts_mismatch && !stss_out_of_range) {
        result.error = "样本表未发现需要重建的问题";
        return result;
    }

    result.attempted = true;

    // 整体位移：chunk 偏移的最小值低于 mdat 载荷起点时，说明文件头被增删过。
    std::uint64_t min_offset = std::numeric_limits<std::uint64_t>::max();
    for (const auto& ref : refs)
        min_offset = std::min(min_offset, ref.original_offset);
    std::int64_t shift = 0;
    if (min_offset < mdat_info.payload)
        shift = static_cast<std::int64_t>(mdat_info.payload - min_offset);

    for (const auto& ref : refs) {
        const std::int64_t moved = static_cast<std::int64_t>(ref.original_offset) + shift;
        if (moved < static_cast<std::int64_t>(mdat_info.payload) || moved < 0 ||
            static_cast<std::uint64_t>(moved) + ref.size > mdat_info.payload + mdat_info.payload_size ||
            static_cast<std::uint64_t>(moved) + ref.size > parsed.file_size) {
            result.error =
                "chunk 偏移即使整体位移 " + std::to_string(shift) + " 字节仍越界；数据布局无法解释，拒绝重建";
            return result;
        }
    }

    std::vector<uint8_t> ftyp_bytes;
    if (ftyp != nullptr && !ReadWholeBox(source_path, *ftyp, &ftyp_bytes))
        ftyp_bytes = MakeFtyp();
    if (ftyp_bytes.empty())
        ftyp_bytes = MakeFtyp();

    std::vector<ChunkRef> sorted_refs = refs;
    std::sort(sorted_refs.begin(), sorted_refs.end(), [](const ChunkRef& a, const ChunkRef& b) {
        if (a.original_offset != b.original_offset)
            return a.original_offset < b.original_offset;
        if (a.track_index != b.track_index)
            return a.track_index < b.track_index;
        return a.chunk_index < b.chunk_index;
    });

    std::string table_note;
    std::vector<std::vector<std::uint64_t>> track_offsets(parsed.tracks.size());
    for (std::size_t i = 0; i < parsed.tracks.size(); ++i)
        track_offsets[i].assign(parsed.tracks[i].chunk_offsets.size(), 0);

    std::uint64_t total_payload = 0;
    for (const auto& ref : refs)
        total_payload += ref.size;
    // mdat 头大小取决于载荷是否超过 4 GiB。
    const std::uint64_t mdat_header_size = (total_payload + 8 > 0xFFFFFFFFull) ? 16 : 8;

    // moov 大小依赖 stco/co64 的选择，后者又依赖新偏移；迭代到尺寸稳定为止。
    std::vector<uint8_t> moov_bytes;
    std::uint64_t payload_start = 0;
    for (int pass = 0; pass < 4; ++pass) {
        if (!BuildMoov(parsed, mvhd, raw_boxes, track_offsets, &table_note, &moov_bytes, &result.error))
            return result;
        const std::uint64_t new_start = ftyp_bytes.size() + moov_bytes.size() + mdat_header_size;
        if (new_start == payload_start && pass > 0)
            break;
        payload_start = new_start;
        std::uint64_t cursor = payload_start;
        for (const auto& ref : sorted_refs) {
            track_offsets[ref.track_index][ref.chunk_index - 1] = cursor;
            cursor += ref.size;
        }
    }
    if (!BuildMoov(parsed, mvhd, raw_boxes, track_offsets, &table_note, &moov_bytes, &result.error))
        return result;
    if (ftyp_bytes.size() + moov_bytes.size() + mdat_header_size != payload_start) {
        result.error = "重建 moov 尺寸未收敛，拒绝写出可能错位的结果";
        return result;
    }

    std::ofstream out(ToNativePath(output_path), std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "无法创建重建文件: " + output_path;
        return result;
    }
    out.write(reinterpret_cast<const char*>(ftyp_bytes.data()), static_cast<std::streamsize>(ftyp_bytes.size()));
    out.write(reinterpret_cast<const char*>(moov_bytes.data()), static_cast<std::streamsize>(moov_bytes.size()));
    WriteMdatHeader(out, total_payload);
    if (!out) {
        result.error = "写入重建文件头失败";
        return result;
    }

    std::ifstream in(ToNativePath(source_path), std::ios::binary);
    if (!in) {
        result.error = "无法读取源文件";
        return result;
    }
    std::uint64_t written = 0;
    for (const auto& ref : sorted_refs) {
        if (Stopped(cancel, budget, &result.error))
            return result;
        if (!CopyRange(in, out, ref.original_offset + static_cast<std::uint64_t>(shift), ref.size, &written,
                       &result.error))
            return result;
        ++result.rebuilt_chunks;
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

    // 结构自检：重建结果必须能被同一解析器读回，且轨道数量不变。
    IsobmffFile check;
    if (!IsobmffParser::Parse(output_path, check) || !check.ok || check.tracks.size() != parsed.tracks.size()) {
        result.error = "重建结果结构自检失败: " + check.error_message;
        return result;
    }
    if (!VerifyRebuiltMdat(check, payload_start, payload_start + total_payload, total_payload, &result.error))
        return result;

    result.ok = true;
    result.applied_shift_bytes = shift;
    result.note = "按 chunk 原相对顺序重建 moov 索引；chunk 整体位移 " + std::to_string(shift) + " 字节";
    if (!table_note.empty())
        result.note += "；" + table_note;
    if (total_payload != mdat_info.payload_size)
        result.note += "；mdat 中未被样本表引用的字节已丢弃";
    result.note += "；重建后的产物仍需重新封装并验证";
    return result;
}

} // namespace repair
} // namespace videoeye
