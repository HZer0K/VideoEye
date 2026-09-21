#include "IsobmffParser.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace videoeye {
namespace utils {
namespace {

// ---- 大端读取 ----
uint32_t Rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint64_t Rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}
uint16_t Rd16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

std::string FourCC(const uint8_t* p) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const uint8_t c = p[i];
        s[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.';
    }
    return s;
}

// 需要递归下钻的容器 box
bool IsContainer(const std::string& type) {
    static const char* kContainers[] = {
        "moov", "trak", "mdia", "minf", "stbl", "edts", "dinf", "udta",
        "meta", "moof", "traf", "mfra", "mvex", "stsd", "sinf", "schi",
    };
    for (const char* c : kContainers) {
        if (type == c) return true;
    }
    return false;
}

bool IsTableBox(const std::string& type) {
    static const char* kTables[] = {"stts", "ctts", "stss", "stsz",
                                    "stz2", "stsc", "stco", "co64", "elst"};
    for (const char* c : kTables) {
        if (type == c) return true;
    }
    return false;
}

// 简单的顺序读文件封装：解析全程只有前向 + 少量回退，直接用 ifstream 足够
class FileReader {
public:
    explicit FileReader(const std::string& path) : in_(path, std::ios::binary) {}
    bool good() const { return in_.good(); }
    uint64_t size() const { return size_; }
    bool Seek(uint64_t pos) {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
        return !in_.fail();
    }
    bool ReadAt(uint64_t pos, uint8_t* dst, uint64_t n) {
        if (!Seek(pos)) return false;
        in_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<uint64_t>(in_.gcount()) == n;
    }
    void InitSize() {
        in_.seekg(0, std::ios::end);
        size_ = static_cast<uint64_t>(in_.tellg());
        in_.clear();
    }

private:
    std::ifstream in_;
    uint64_t size_ = 0;
};

struct ParseContext {
    IsobmffFile* out = nullptr;
    FileReader* reader = nullptr;
    const IsobmffParser::Options* opt = nullptr;

    // 当前 trak 上下文（进入 trak 时压栈，退出时出栈并归档）
    IsobmffTrack* trak = nullptr;
    // 当前 moof/traf 上下文
    IsobmffFragment* frag = nullptr;
    uint32_t moof_index = 0;
    uint32_t moof_counter = 0;
    uint32_t frag_seq = 0;
    uint64_t moof_offset = 0;
    uint64_t moof_size = 0;
    uint32_t tfhd_default_duration = 0;
    uint32_t tfhd_default_size = 0;
};

// 读取 box 的载荷（限量），返回实际读到的字节数
uint64_t ReadPayload(ParseContext& ctx, const IsobmffBox& box, std::vector<uint8_t>& buf,
                     uint64_t limit) {
    const uint64_t payload_size = (box.size > box.header_size) ? (box.size - box.header_size) : 0;
    const uint64_t want = std::min<uint64_t>(payload_size, limit);
    if (want == 0) return 0;
    buf.resize(static_cast<size_t>(want));
    if (!ctx.reader->ReadAt(box.offset + box.header_size, buf.data(), want)) {
        buf.clear();
        return 0;
    }
    return want;
}

// 样本表循环守卫: count 是「条目数」，不是「样本数」。
// 曾经写成 added += entry.sample_count 再与 count 比较 —— 一条 entry 往往覆盖上千个样本，
// 第一条就把 added 顶到 count 之上，结果正常 MP4 只解析出 1 条 entry（stts/ctts 全残）。
// 一律用「已解析条目数」与 count 比。
// 没能把声明的 count 条全部读出来就置位（载荷不够 or 撞到 max_entries 两种原因都算），
// 便于上层区分「完整解析」和「只读到一部分」。
void MarkTruncated(IsobmffTrack& t, size_t parsed, uint32_t count) {
    if (parsed < count) t.tables_truncated = true;
}

void ParseStts(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8) return;
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    while (pos + 8 <= n && t.stts.size() < count && t.stts.size() < max_entries) {
        SttsEntry e;
        e.sample_count = Rd32(p + pos);
        e.sample_delta = Rd32(p + pos + 4);
        t.stts.push_back(e);
        pos += 8;
    }
    MarkTruncated(t, t.stts.size(), count);
    t.has_stts = true;
}

void ParseCtts(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8) return;
    const uint8_t version = p[0];
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    // v0 与 v1 条目都是 8 字节，区别只在偏移量的符号解释：
    //   v0 = uint32（无符号，只出现在没有负 CTS 的场景）
    //   v1 = int32（B 帧重排序时会出现负值）
    // 统一存进 int32_t，v0 超过 INT32_MAX 的极端值夹到 INT32_MAX（正常码流不会出现）。
    while (pos + 8 <= n && t.ctts.size() < count && t.ctts.size() < max_entries) {
        CttsEntry e;
        e.sample_count = Rd32(p + pos);
        const uint32_t raw = Rd32(p + pos + 4);
        if (version == 1) {
            e.sample_offset = static_cast<int32_t>(raw);
        } else {
            e.sample_offset = static_cast<int32_t>(std::min<uint32_t>(raw, 0x7FFFFFFFu));
        }
        t.ctts.push_back(e);
        pos += 8;
    }
    MarkTruncated(t, t.ctts.size(), count);
    t.has_ctts = true;
}

void ParseStss(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8) return;
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    while (pos + 4 <= n && t.stss.size() < count && t.stss.size() < max_entries) {
        t.stss.push_back(Rd32(p + pos));
        pos += 4;
    }
    MarkTruncated(t, t.stss.size(), count);
    t.has_stss = true;
}

void ParseStsz(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 12) return;
    t.stsz.default_size = Rd32(p + 4);
    const uint32_t count = Rd32(p + 8);
    t.stsz.sample_count = count;
    if (t.stsz.default_size == 0) {
        uint64_t pos = 12;
        while (pos + 4 <= n && t.stsz.sizes.size() < count && t.stsz.sizes.size() < max_entries) {
            t.stsz.sizes.push_back(Rd32(p + pos));
            pos += 4;
        }
        MarkTruncated(t, t.stsz.sizes.size(), count);
    }
    t.has_stsz = true;
}

void ParseStz2(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    // stz2: version/flags(4) reserved(3)+field_size(1) sample_count(4) ...
    if (n < 12) return;
    t.stsz.field_size = p[7] & 0xFF;
    const uint32_t count = Rd32(p + 8);
    t.stsz.sample_count = count;
    uint64_t pos = 12;
    if (t.stsz.field_size == 16) {
        while (pos + 2 <= n && t.stsz.sizes.size() < count && t.stsz.sizes.size() < max_entries) {
            t.stsz.sizes.push_back(Rd16(p + pos));
            pos += 2;
        }
    } else if (t.stsz.field_size == 8) {
        while (pos + 1 <= n && t.stsz.sizes.size() < count && t.stsz.sizes.size() < max_entries) {
            t.stsz.sizes.push_back(p[pos]);
            pos += 1;
        }
    } else if (t.stsz.field_size == 4) {
        // 一字节装两个样本；样本数为奇数时，最后一字节的低 4 位是填充，不能算作样本
        while (pos + 1 <= n && t.stsz.sizes.size() < count && t.stsz.sizes.size() < max_entries) {
            const uint8_t b = p[pos];
            t.stsz.sizes.push_back(static_cast<uint32_t>(b >> 4));
            if (t.stsz.sizes.size() < count && t.stsz.sizes.size() < max_entries) {
                t.stsz.sizes.push_back(static_cast<uint32_t>(b & 0x0F));
            }
            pos += 1;
        }
    }
    if (t.stsz.field_size == 4 || t.stsz.field_size == 8 || t.stsz.field_size == 16) {
        MarkTruncated(t, t.stsz.sizes.size(), count);
    }
    t.has_stz2 = true;
}

void ParseStsc(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8) return;
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    while (pos + 12 <= n && t.stsc.size() < count && t.stsc.size() < max_entries) {
        StscEntry e;
        e.first_chunk = Rd32(p + pos);
        e.samples_per_chunk = Rd32(p + pos + 4);
        e.sample_description_index = Rd32(p + pos + 8);
        t.stsc.push_back(e);
        pos += 12;
    }
    MarkTruncated(t, t.stsc.size(), count);
    t.has_stsc = true;
}

void ParseChunkOffsets(IsobmffTrack& t, const uint8_t* p, uint64_t n, bool co64,
                       uint32_t max_entries) {
    if (n < 8) return;
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    const uint64_t entry = co64 ? 8 : 4;
    while (pos + entry <= n && t.chunk_offsets.size() < count &&
           t.chunk_offsets.size() < max_entries) {
        t.chunk_offsets.push_back(co64 ? Rd64(p + pos) : Rd32(p + pos));
        pos += entry;
    }
    MarkTruncated(t, t.chunk_offsets.size(), count);
    if (co64) t.has_co64 = true; else t.has_stco = true;
}

void ParseElst(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8) return;
    const uint8_t version = p[0];
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    while (pos + (version == 1 ? 20 : 12) <= n && t.elst.size() < count &&
           t.elst.size() < max_entries) {
        ElstEntry e;
        if (version == 1) {
            e.segment_duration = Rd64(p + pos);
            e.media_time = static_cast<int64_t>(Rd64(p + pos + 8));
        } else {
            e.segment_duration = Rd32(p + pos);
            e.media_time = static_cast<int32_t>(Rd32(p + pos + 4));
        }
        e.media_rate_integer = static_cast<int16_t>(Rd16(p + pos + (version == 1 ? 16 : 8)));
        t.elst.push_back(e);
        pos += version == 1 ? 20 : 12;
    }
    MarkTruncated(t, t.elst.size(), count);
    t.has_elst = true;
}

void ParseTkhd(IsobmffTrack& t, const uint8_t* p, uint64_t n) {
    if (n < 8) return;
    const uint8_t version = p[0];
    // version/flags(4) + [creation(4|8) modification(4|8)] + track_ID(4)
    const uint64_t id_off = (version == 1) ? (4 + 8 + 8) : (4 + 4 + 4);
    if (id_off + 4 <= n) t.track_id = Rd32(p + id_off);
}

void ParseHdlr(IsobmffTrack& t, const uint8_t* p, uint64_t n) {
    // version/flags(4) + pre_defined(4) + handler_type(4)
    if (n >= 12) t.handler = FourCC(p + 8);
}

void ParseMdhd(IsobmffTrack& t, const uint8_t* p, uint64_t n) {
    if (n < 8) return;
    const uint8_t version = p[0];
    if (version == 1) {
        if (n >= 32) {
            t.media_timescale = Rd32(p + 20);
            t.media_duration = Rd64(p + 24);
        }
    } else {
        if (n >= 20) {
            t.media_timescale = Rd32(p + 12);
            t.media_duration = Rd32(p + 16);
        }
    }
}

void ParseStsdCodec(IsobmffTrack& t, const uint8_t* p, uint64_t n) {
    // version/flags(4) + entry_count(4) + 首个 entry: size(4) + format(4)
    if (n < 16) return;
    t.codec = FourCC(p + 12);
}

void ParseMvhd(IsobmffFile& out, const uint8_t* p, uint64_t n) {
    if (n < 8) return;
    const uint8_t version = p[0];
    if (version == 1) {
        if (n >= 28) out.movie_timescale = Rd32(p + 20);
    } else {
        if (n >= 16) out.movie_timescale = Rd32(p + 12);
    }
}

void ParseTfhd(ParseContext& ctx, const uint8_t* p, uint64_t n) {
    if (!ctx.frag || n < 8) return;
    const uint32_t flags = Rd32(p) & 0x00FFFFFF;
    IsobmffFragment& f = *ctx.frag;
    f.base_data_offset_present = (flags & 0x000001) != 0;
    f.sample_description_index_present = (flags & 0x000002) != 0;
    f.default_sample_duration_present = (flags & 0x000008) != 0;
    f.default_sample_size_present = (flags & 0x000010) != 0;
    f.default_base_is_moof = (flags & 0x020000) != 0;
    f.duration_is_empty = (flags & 0x010000) != 0;

    uint64_t pos = 8;   // 跳过 version/flags(4) + track_ID(4)
    f.track_id = (n >= 8) ? Rd32(p + 4) : 0;
    if (f.base_data_offset_present && pos + 8 <= n) {
        f.base_data_offset = Rd64(p + pos);
        pos += 8;
    }
    if (f.sample_description_index_present && pos + 4 <= n) pos += 4;
    if (f.default_sample_duration_present && pos + 4 <= n) {
        ctx.tfhd_default_duration = Rd32(p + pos);
        pos += 4;
    }
    if (f.default_sample_size_present && pos + 4 <= n) {
        ctx.tfhd_default_size = Rd32(p + pos);
        pos += 4;
    }
}

void ParseTfdt(ParseContext& ctx, const uint8_t* p, uint64_t n) {
    if (!ctx.frag || n < 8) return;
    const uint8_t version = p[0];
    ctx.frag->has_tfdt = true;
    if (version == 1 && n >= 12) {
        ctx.frag->base_media_decode_time = Rd64(p + 4);
    } else if (n >= 8) {
        ctx.frag->base_media_decode_time = Rd32(p + 4);
    }
}

void ParseTrun(ParseContext& ctx, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (!ctx.frag || n < 8) return;
    const uint8_t version = p[0];
    const uint32_t flags = Rd32(p) & 0x00FFFFFF;
    const uint32_t sample_count = Rd32(p + 4);
    IsobmffFragment& f = *ctx.frag;
    ++f.trun_count;

    const bool data_offset_present = (flags & 0x000001) != 0;
    const bool first_sample_flags_present = (flags & 0x000004) != 0;
    const bool sample_duration_present = (flags & 0x000100) != 0;
    const bool sample_size_present = (flags & 0x000200) != 0;
    const bool sample_flags_present = (flags & 0x000400) != 0;
    const bool sample_cts_present = (flags & 0x000800) != 0;

    if (f.trun_count == 1) {
        f.trun_data_offset_present = data_offset_present;
    }

    uint64_t pos = 8;
    if (data_offset_present && pos + 4 <= n) {
        const int32_t off = static_cast<int32_t>(Rd32(p + pos));
        if (f.trun_count == 1) f.trun_data_offset = off;
        pos += 4;
    }
    if (first_sample_flags_present && pos + 4 <= n) pos += 4;

    for (uint32_t i = 0; i < sample_count && f.sample_count < max_entries; ++i) {
        uint32_t duration = ctx.tfhd_default_duration;
        uint32_t size = ctx.tfhd_default_size;
        if (sample_duration_present && pos + 4 <= n) {
            duration = Rd32(p + pos);
            pos += 4;
        }
        if (sample_size_present && pos + 4 <= n) {
            size = Rd32(p + pos);
            pos += 4;
        }
        if (sample_flags_present && pos + 4 <= n) pos += 4;
        if (sample_cts_present && pos + 4 <= n) {
            if (version == 1) {
                pos += 4;
            } else {
                pos += 4;
            }
        }
        ++f.sample_count;
        f.duration += duration;
        f.total_size += size;
        if (pos > n) break;
    }
}

// 解析一个 box 区间 [start, end) 内的所有 box，挂到 parent->children
void ParseBoxes(ParseContext& ctx, uint64_t start, uint64_t end, int depth,
                std::vector<IsobmffBox>* parent) {
    if (depth > ctx.opt->max_depth || start >= end) return;

    uint64_t pos = start;
    uint8_t hdr[16] = {0};
    int guard = 0;
    while (pos + 8 <= end && ++guard < 1000000) {
        if (!ctx.reader->ReadAt(pos, hdr, 8)) break;
        uint64_t size = Rd32(hdr);
        std::string type = FourCC(hdr + 4);
        uint32_t header_size = 8;

        if (size == 1) {
            if (!ctx.reader->ReadAt(pos + 8, hdr + 8, 8)) break;
            size = Rd64(hdr + 8);
            header_size = 16;
        } else if (size == 0) {
            size = end - pos;   // 最后一个 box 延伸到父容器末尾
        }
        if (size < header_size || pos + size > end) {
            // 截断/损坏: 收缩到父容器末尾，之后停止（避免死循环）
            if (size < header_size) break;
            size = end - pos;
        }

        IsobmffBox box;
        box.type = type;
        box.size = size;
        box.offset = pos;
        box.header_size = header_size;
        box.depth = depth;

        // ---- 语义解析 ----
        if (type == "moov") {
            if (!ctx.out->found_moov) {
                ctx.out->found_moov = true;
                ctx.out->moov_offset = box.offset;
                ctx.out->moov_size = box.size;
                ctx.out->moov_before_mdat = !ctx.out->found_mdat;
            }
        } else if (type == "mdat") {
            if (!ctx.out->found_mdat) {
                ctx.out->found_mdat = true;
                ctx.out->first_mdat_offset = box.offset;
                ctx.out->first_mdat_size = box.size;
            }
        } else if (type == "sidx") {
            ++ctx.out->sidx_count;
        } else if (type == "styp") {
            ++ctx.out->styp_count;
        }

        if (depth == 0) ctx.out->top_level_order.push_back(type);

        // 样本表与元数据只在 trak 上下文里解析
        const bool in_trak = (ctx.trak != nullptr);
        if (in_trak && !ctx.opt->skip_sample_tables && IsTableBox(type)) {
            // 表可能很大: 直接按条目上限算需要的字节数，不整块读
            const uint64_t payload = size - header_size;
            const uint64_t cap = std::min<uint64_t>(payload, 64ull * 1024 * 1024);
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, cap);
            if (!buf.empty()) {
                const uint32_t max_entries = ctx.opt->max_entries_per_table;
                if (type == "stts") ParseStts(*ctx.trak, buf.data(), buf.size(), max_entries);
                else if (type == "ctts") ParseCtts(*ctx.trak, buf.data(), buf.size(), max_entries);
                else if (type == "stss") ParseStss(*ctx.trak, buf.data(), buf.size(), max_entries);
                else if (type == "stsz") ParseStsz(*ctx.trak, buf.data(), buf.size(), max_entries);
                else if (type == "stz2") ParseStz2(*ctx.trak, buf.data(), buf.size(), max_entries);
                else if (type == "stsc") ParseStsc(*ctx.trak, buf.data(), buf.size(), max_entries);
                else if (type == "stco") ParseChunkOffsets(*ctx.trak, buf.data(), buf.size(), false, max_entries);
                else if (type == "co64") ParseChunkOffsets(*ctx.trak, buf.data(), buf.size(), true, max_entries);
                else if (type == "elst") ParseElst(*ctx.trak, buf.data(), buf.size(), max_entries);
            }
        } else if (in_trak && (type == "tkhd" || type == "hdlr" || type == "mdhd" ||
                               type == "stsd")) {
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, 4096);
            if (!buf.empty()) {
                if (type == "tkhd") ParseTkhd(*ctx.trak, buf.data(), buf.size());
                else if (type == "hdlr") ParseHdlr(*ctx.trak, buf.data(), buf.size());
                else if (type == "mdhd") ParseMdhd(*ctx.trak, buf.data(), buf.size());
                else if (type == "stsd") ParseStsdCodec(*ctx.trak, buf.data(), buf.size());
            }
        } else if (type == "mvhd" && depth >= 1) {
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, 4096);
            if (!buf.empty()) ParseMvhd(*ctx.out, buf.data(), buf.size());
        }

        // ---- moof / traf ----
        if (type == "moof") {
            ctx.out->fragmented = true;
            ctx.moof_offset = box.offset;
            ctx.moof_size = box.size;
            ctx.moof_index = ctx.moof_counter++;
            ctx.frag_seq = 0;
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, 4096);
            // mfhd 是 moof 的第一个子 box: version/flags(4) + sequence_number(4)
            if (buf.size() >= 16 && FourCC(buf.data() + 4) == "mfhd") {
                ctx.frag_seq = Rd32(buf.data() + 12);
            }
        }

        if (type == "traf" && ctx.out->fragmented) {
            IsobmffFragment f;
            f.moof_index = ctx.moof_index;
            f.index = static_cast<uint32_t>(ctx.out->fragments.size());
            f.sequence_number = ctx.frag_seq;
            f.offset = ctx.moof_offset;
            f.size = ctx.moof_size;
            ctx.out->fragments.push_back(f);
            ctx.frag = &ctx.out->fragments.back();
            ctx.tfhd_default_duration = 0;
            ctx.tfhd_default_size = 0;
            ParseBoxes(ctx, pos + header_size, pos + size, depth + 1, &box.children);
            ctx.frag = nullptr;
        } else if (IsContainer(type)) {
            IsobmffTrack* saved_trak = ctx.trak;
            if (type == "trak") {
                ctx.out->tracks.emplace_back();
                ctx.trak = &ctx.out->tracks.back();
            }
            ParseBoxes(ctx, pos + header_size, pos + size, depth + 1, &box.children);
            if (type == "trak") {
                // 只保留有媒体时基的轨道（hint/空 trak 直接丢弃）
                if (ctx.trak->media_timescale == 0 && ctx.trak->handler.empty()) {
                    ctx.out->tracks.pop_back();
                }
                ctx.trak = saved_trak;
            }
        } else if (ctx.frag && (type == "tfhd" || type == "tfdt" || type == "trun")) {
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, std::min<uint64_t>(size - header_size, 16ull * 1024 * 1024));
            if (!buf.empty()) {
                if (type == "tfhd") ParseTfhd(ctx, buf.data(), buf.size());
                else if (type == "tfdt") ParseTfdt(ctx, buf.data(), buf.size());
                else if (type == "trun") ParseTrun(ctx, buf.data(), buf.size(),
                                                   ctx.opt->max_entries_per_table);
            }
        }

        pos += size;
        if (parent) parent->push_back(std::move(box));
    }
}

}  // namespace

// ---- 对外接口 ----
std::string IsobmffTrack::TypeName() const {
    if (handler == "vide") return "video";
    if (handler == "soun") return "audio";
    if (handler == "hint") return "hint";
    if (handler == "text") return "text";
    if (handler == "sbtl") return "subtitle";
    if (handler == "subt") return "subtitle";
    if (handler == "meta") return "meta";
    if (!handler.empty()) return handler;
    return "unknown";
}

uint32_t IsobmffTrack::SttsSampleCount() const {
    uint32_t n = 0;
    for (const auto& e : stts) n += e.sample_count;
    return n;
}

uint32_t IsobmffTrack::CttsSampleCount() const {
    uint32_t n = 0;
    for (const auto& e : ctts) n += e.sample_count;
    return n;
}

bool IsobmffParser::Parse(const std::string& file_path, IsobmffFile& out,
                          const Options& options) {
    out = IsobmffFile{};

    FileReader reader(file_path);
    if (!reader.good()) {
        out.error_message = "无法打开文件";
        return false;
    }
    reader.InitSize();
    out.file_size = reader.size();
    if (out.file_size < 8) {
        out.error_message = "文件过小，不是有效的 ISOBMFF 文件";
        return false;
    }

    ParseContext ctx;
    ctx.out = &out;
    ctx.reader = &reader;
    ctx.opt = &options;

    ParseBoxes(ctx, 0, out.file_size, 0, &out.top_level);

    if (out.top_level.empty()) {
        out.error_message = "未解析到任何 box（不是有效的 ISOBMFF 文件？）";
        return false;
    }

    out.ok = true;
    return true;
}

}  // namespace utils
}  // namespace videoeye
