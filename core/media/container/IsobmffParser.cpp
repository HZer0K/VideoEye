#include "core/media/container/IsobmffParser.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/domain/io/Utf8Path.h"

#include "infrastructure/concurrency/Cancellation.h"

#include "core/media/detail/ParserLimits.h"

namespace videoeye {
namespace {

// ---- 大端读取 ----
uint32_t Rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint64_t Rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<uint64_t>(p[i]);
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
        if (type == c)
            return true;
    }
    return false;
}

bool IsTableBox(const std::string& type) {
    static const char* kTables[] = {"stts", "ctts", "stss", "stsz", "stz2", "stsc", "stco", "co64", "elst"};
    for (const char* c : kTables) {
        if (type == c)
            return true;
    }
    return false;
}

// stsd 里的 sample entry（avc1 / hvc1 / mp4a …）本身也是容器：
// 它的子 box 才是 avcC / hvcC / av1C / esds / colr / pasp / btrt。
//
// 以前 stsd 只被当成"读到第一个 entry 的 4CC 就完事"，于是整棵
// 编码配置子树从来没进过 box 树 —— Mp4BoxAnalyzer 的字段表里那些
// profile / level / colour 也就永远是空的。
//
// 子 box 的起点随 entry 种类不同（这是 ISO 14496-12 的 SampleEntry 语法）：
//   视频入口：6+2 保留 + 16 预定义 + 2 width + 2 height + 4+4 分辨率
//             + 4 保留 + 2 frame_count + 32 compressorname + 2 depth + 2 预定义 = 78
//   音频入口：6+2 保留 + 版本(2)+revision(2)+vendor(4) + channels(2)+sample_size(2)
//             + compression_id(2)+packet_size(2)+samplerate(4)  = 28
//             v1 再多 16 字节（每声道 4 字节的 samples_per_packet 等），v2 再多 36。
int SampleEntryChildOffset(const std::string& type, const uint8_t* payload, uint64_t n) {
    static const char* kVideoEntries[] = {
        "avc1", "avc2", "avc3", "avc4", "hvc1", "hev1", "av01", "vp08", "vp09", "mp4v", "encv", "dvh1",
        "dvhe", "dva1", "dvav", "jpeg", "mjpa", "ap4h", "ap4x", "apch", "apcn", "apcs", "apco", "s263",
    };
    static const char* kAudioEntries[] = {
        "mp4a", "ac-3", "ec-3", "ac-4", "opus", "Opus", "fLaC", "alac", "dtsc", "dtsh",
        "dtse", "enca", "mha1", "mhm1", "sowt", "twos", "lpcm", "ipcm", "spex",
    };
    for (const char* c : kVideoEntries) {
        if (type == c)
            return 78;
    }
    for (const char* c : kAudioEntries) {
        if (type == c) {
            // 音频入口的 version 在 payload 偏移 8（6 保留 + 2 data_reference_index 之后）
            if (payload != nullptr && n >= 10) {
                const uint16_t version = Rd16(payload + 8);
                if (version == 1)
                    return 44;
                if (version == 2)
                    return 64;
            }
            return 28;
        }
    }
    return -1; // 文本/字幕等入口的子结构不统一，不往下钻
}

// ---- 字段填充小工具 ----
void AddField(IsobmffBox& box, const std::string& name, const std::string& value) {
    box.fields.emplace_back(name, value);
}
void AddFieldInt(IsobmffBox& box, const std::string& name, uint64_t value) {
    box.fields.emplace_back(name, std::to_string(value));
}
void AddFieldHex(IsobmffBox& box, const std::string& name, uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%0*llx", static_cast<int>(value > 0xFFFF ? 8 : 4),
                  static_cast<unsigned long long>(value));
    box.fields.emplace_back(name, buf);
}
// 16.16 定点（tkhd 的 width/height、音频入口的 samplerate 都用它）
std::string Fixed1616(uint32_t raw) {
    const uint32_t whole = raw >> 16;
    const uint32_t frac = raw & 0xFFFF;
    if (frac == 0)
        return std::to_string(whole);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%05u", whole,
                  static_cast<unsigned>((static_cast<uint64_t>(frac) * 100000) >> 16));
    return buf;
}
// mdhd 的 language：3 个 5-bit 字母，每字母 +0x60
std::string LanguageCode(uint16_t packed) {
    char out[4] = {0, 0, 0, 0};
    out[0] = static_cast<char>(((packed >> 10) & 0x1F) + 0x60);
    out[1] = static_cast<char>(((packed >> 5) & 0x1F) + 0x60);
    out[2] = static_cast<char>((packed & 0x1F) + 0x60);
    for (int i = 0; i < 3; ++i) {
        if (out[i] < 'a' || out[i] > 'z')
            out[i] = '?';
    }
    return std::string(out, 3);
}
// hdlr 的 name 在 payload 偏移 24，既可能是 C 串也可能是 Pascal 串
std::string HandlerName(const uint8_t* p, uint64_t n) {
    if (n <= 24)
        return std::string();
    std::string out;
    uint64_t pos = 24;
    // Pascal 串：首字节是长度，且后续字节数相符
    if (p[pos] > 0 && 25 + p[pos] <= n) {
        const uint8_t len = p[pos];
        ++pos;
        for (uint8_t i = 0; i < len; ++i)
            out.push_back(static_cast<char>(p[pos + i]));
    } else {
        for (; pos < n && p[pos] != 0; ++pos)
            out.push_back(static_cast<char>(p[pos]));
    }
    // 去掉非可打印字符（有些 writer 会写入 BOM/控制字符）
    std::string clean;
    for (char c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u != 0x7F)
            clean.push_back(c);
    }
    return clean;
}

// 简单的顺序读文件封装：解析全程只有前向 + 少量回退，直接用 ifstream 足够
class FileReader {
public:
    explicit FileReader(const std::string& path) : in_(ToNativePath(path), std::ios::binary) {
    }
    bool good() const {
        return in_.good();
    }
    uint64_t size() const {
        return size_;
    }
    bool Seek(uint64_t pos) {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
        return !in_.fail();
    }
    bool ReadAt(uint64_t pos, uint8_t* dst, uint64_t n) {
        if (!Seek(pos))
            return false;
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
    // 来自 IsobmffOptions::cancel 的取消标志。box 循环里每个 box 都查一次。
    const std::atomic<bool>* cancel = nullptr;

    // 当前 trak / moof-traf 上下文，用**下标**而不是裸指针。
    // ⚠️ 旧实现在这里存的是 IsobmffTrack* / IsobmffFragment*：解析过程中
    // tracks / fragments 会不断 push_back，vector 一扩容，先前取到的地址就失效了，
    // 嵌套 trak（trak 里再套 trak）时这条路径必踩 use-after-free。
    // 一律改成下标，取当前对象统一走下面的访问器。
    int trak_index = -1;
    int frag_index = -1;

    // 当前 trak / frag 的访问器：-1 表示不在该上下文里，调用前先判下标
    IsobmffTrack& trak() {
        return out->tracks[static_cast<size_t>(trak_index)];
    }
    IsobmffFragment& frag() {
        return out->fragments[static_cast<size_t>(frag_index)];
    }

    uint32_t moof_index = 0;
    uint32_t moof_counter = 0;
    uint32_t frag_seq = 0;
    uint64_t moof_offset = 0;
    uint64_t moof_size = 0;
    uint32_t tfhd_default_duration = 0;
    uint32_t tfhd_default_size = 0;
    // tfhd 的 default_sample_flags（flags & 0x000020）。trun 既没给 first_sample_flags
    // 也没给逐样本 flags 时，首样本的 sync 判定只能靠它。
    uint32_t tfhd_default_flags = 0;
    bool tfhd_default_flags_present = false;
};

// 读取 box 的载荷（限量），返回实际读到的字节数
uint64_t ReadPayload(ParseContext& ctx, const IsobmffBox& box, std::vector<uint8_t>& buf, uint64_t limit) {
    const uint64_t payload_size = (box.size > box.header_size) ? (box.size - box.header_size) : 0;
    const uint64_t want = std::min<uint64_t>(payload_size, limit);
    if (want == 0)
        return 0;
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
    if (parsed < count)
        t.tables_truncated = true;
}

void ParseStts(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8)
        return;
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
    if (n < 8)
        return;
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
    if (n < 8)
        return;
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
    if (n < 12)
        return;
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
    if (n < 12)
        return;
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
    if (n < 8)
        return;
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

void ParseChunkOffsets(IsobmffTrack& t, const uint8_t* p, uint64_t n, bool co64, uint32_t max_entries) {
    if (n < 8)
        return;
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    const uint64_t entry = co64 ? 8 : 4;
    while (pos + entry <= n && t.chunk_offsets.size() < count && t.chunk_offsets.size() < max_entries) {
        t.chunk_offsets.push_back(co64 ? Rd64(p + pos) : Rd32(p + pos));
        pos += entry;
    }
    MarkTruncated(t, t.chunk_offsets.size(), count);
    if (co64)
        t.has_co64 = true;
    else
        t.has_stco = true;
}

void ParseElst(IsobmffTrack& t, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (n < 8)
        return;
    const uint8_t version = p[0];
    const uint32_t count = Rd32(p + 4);
    uint64_t pos = 8;
    while (pos + (version == 1 ? 20 : 12) <= n && t.elst.size() < count && t.elst.size() < max_entries) {
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
    if (n < 8)
        return;
    const uint8_t version = p[0];
    // version/flags(4) + [creation(4|8) modification(4|8)] + track_ID(4)
    const uint64_t id_off = (version == 1) ? (4 + 8 + 8) : (4 + 4 + 4);
    if (id_off + 4 <= n)
        t.track_id = Rd32(p + id_off);
}

void ParseHdlr(IsobmffTrack& t, const uint8_t* p, uint64_t n) {
    // version/flags(4) + pre_defined(4) + handler_type(4)
    if (n >= 12)
        t.handler = FourCC(p + 8);
}

void ParseMdhd(IsobmffTrack& t, const uint8_t* p, uint64_t n) {
    if (n < 8)
        return;
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
    // version/flags(4) + entry_count(4) + entry* (size(4) + format(4) + ...)
    if (n < 8)
        return;
    const uint32_t entry_count = Rd32(p + 4);
    uint64_t pos = 8;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (pos + 8 > n)
            break;
        const uint32_t entry_size = Rd32(p + pos);
        t.sample_descriptions.push_back(FourCC(p + pos + 4));
        if (entry_size < 8 || pos + entry_size > n) {
            // size 不可信：已经拿到 fourcc，停止即可（继续按 size 跳会越界）
            break;
        }
        pos += entry_size;
    }
    if (!t.sample_descriptions.empty())
        t.codec = t.sample_descriptions.front();
}

void ParseMvhd(IsobmffFile& out, const uint8_t* p, uint64_t n) {
    if (n < 8)
        return;
    const uint8_t version = p[0];
    if (version == 1) {
        if (n >= 28)
            out.movie_timescale = Rd32(p + 20);
    } else {
        if (n >= 16)
            out.movie_timescale = Rd32(p + 12);
    }
}

void ParseTfhd(ParseContext& ctx, const uint8_t* p, uint64_t n) {
    if (ctx.frag_index < 0 || n < 8)
        return;
    const uint32_t flags = Rd32(p) & 0x00FFFFFF;
    IsobmffFragment& f = ctx.frag();
    f.base_data_offset_present = (flags & 0x000001) != 0;
    f.sample_description_index_present = (flags & 0x000002) != 0;
    f.default_sample_duration_present = (flags & 0x000008) != 0;
    f.default_sample_size_present = (flags & 0x000010) != 0;
    f.default_base_is_moof = (flags & 0x020000) != 0;
    f.duration_is_empty = (flags & 0x010000) != 0;

    // ⚠️ 字段顺序就是规范顺序，**不能跳**：以前漏了 0x000020（default_sample_flags），
    // 带这个标志的 tfhd 会让 pos 停在 default_sample_size 之后，后面的 box 解析
    // 全部错位。CMAF 里 default_sample_flags 很常见（给整段统一标 non-sync）。
    const bool default_flags_present = (flags & 0x000020) != 0;

    uint64_t pos = 8; // 跳过 version/flags(4) + track_ID(4)
    f.track_id = (n >= 8) ? Rd32(p + 4) : 0;
    if (f.base_data_offset_present && pos + 8 <= n) {
        f.base_data_offset = Rd64(p + pos);
        pos += 8;
    }
    if (f.sample_description_index_present && pos + 4 <= n)
        pos += 4;
    if (f.default_sample_duration_present && pos + 4 <= n) {
        ctx.tfhd_default_duration = Rd32(p + pos);
        pos += 4;
    }
    if (f.default_sample_size_present && pos + 4 <= n) {
        ctx.tfhd_default_size = Rd32(p + pos);
        pos += 4;
    }
    if (default_flags_present && pos + 4 <= n) {
        ctx.tfhd_default_flags = Rd32(p + pos);
        ctx.tfhd_default_flags_present = true;
        f.tfhd_default_sample_flags_present = true;
        f.tfhd_default_sample_flags = ctx.tfhd_default_flags;
        pos += 4;
    }
}

void ParseTfdt(ParseContext& ctx, const uint8_t* p, uint64_t n) {
    if (ctx.frag_index < 0 || n < 8)
        return;
    const uint8_t version = p[0];
    ctx.frag().has_tfdt = true;
    if (version == 1 && n >= 12) {
        ctx.frag().base_media_decode_time = Rd64(p + 4);
    } else if (n >= 8) {
        ctx.frag().base_media_decode_time = Rd32(p + 4);
    }
}

void ParseTrun(ParseContext& ctx, const uint8_t* p, uint64_t n, uint32_t max_entries) {
    if (ctx.frag_index < 0 || n < 8)
        return;
    const uint8_t version = p[0];
    const uint32_t flags = Rd32(p) & 0x00FFFFFF;
    const uint32_t sample_count = Rd32(p + 4);
    IsobmffFragment& f = ctx.frag();
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
        if (f.trun_count == 1)
            f.trun_data_offset = off;
        pos += 4;
    }
    // first_sample_flags 只作用于**本 trun 的首样本**，且只可能来自第一个 trun
    // （分片的首样本就是第一个 trun 的首样本）。以前这 4 字节读完即丢，
    // 于是"分片首样本是否 random access point"这个事实在解析层就没了。
    bool trun_first_flags_ok = false;
    uint32_t trun_first_flags = 0;
    if (first_sample_flags_present && pos + 4 <= n) {
        trun_first_flags = Rd32(p + pos);
        trun_first_flags_ok = (f.trun_count == 1);
        pos += 4;
    }

    bool sample0_flags_ok = false;
    uint32_t sample0_flags = 0;
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
        if (sample_flags_present && pos + 4 <= n) {
            const uint32_t sf = Rd32(p + pos);
            // 分片首样本自己的 flags：优先级低于 trun.first_sample_flags，
            // 高于 tfhd.default_sample_flags。
            if (f.sample_count == 0) {
                sample0_flags = sf;
                sample0_flags_ok = true;
            }
            pos += 4;
        }
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
        if (pos > n)
            break;
    }

    if (f.has_first_sample_flags)
        return;
    // 优先级：trun.first_sample_flags > 首样本自己的 sample_flags > tfhd.default_sample_flags
    if (trun_first_flags_ok) {
        f.first_sample_flags = trun_first_flags;
        f.has_first_sample_flags = true;
    } else if (sample0_flags_ok) {
        f.first_sample_flags = sample0_flags;
        f.has_first_sample_flags = true;
    } else if (ctx.tfhd_default_flags_present) {
        f.first_sample_flags = ctx.tfhd_default_flags;
        f.has_first_sample_flags = true;
    } else {
        // 三个来源都没有：CMAF 的缺省值在 init 段的 trex 里，本解析器（单文件视角）
        // 拿不到。这里显式记账，让上层报"无法证实"而不是假定为真。
        f.first_sample_flags_incomplete = true;
        return;
    }
    // ISO/IEC 14496-12 §8.8.8.1 sample_flags 布局：
    //   bit 16     sample_is_non_sync_sample
    //   bit 24-25  sample_depends_on（0=unknown 1=depends on others 2=independent）
    f.first_sample_is_sync = (f.first_sample_flags & 0x00010000u) == 0;
    f.first_sample_depends_on = static_cast<int>((f.first_sample_flags >> 24) & 0x3);
    if (f.first_sample_depends_on == 0)
        f.first_sample_depends_on = -1; // 0 = unknown，不要当成"不独立"
}

// 读 MPEG-4 描述符的可变长 length（每字节 7 位，最高位 = 还有后续）
bool ReadDescLength(const uint8_t* p, uint64_t n, uint64_t& pos, uint32_t& len) {
    len = 0;
    for (int i = 0; i < 4 && pos < n; ++i) {
        const uint8_t b = p[pos++];
        len = (len << 7) | static_cast<uint32_t>(b & 0x7F);
        if ((b & 0x80) == 0)
            return true;
    }
    return false;
}

// sample entry（stsd 的子 box）自己的字段：宽高 / 声道 / 采样率 / data_reference_index。
// 这些值在 avcC/colr 之前，属于 entry 头本身，不填的话树里那一层也是空的。
void FillSampleEntryFields(IsobmffBox& box, const uint8_t* p, uint64_t n) {
    static const char* kVideoEntries[] = {
        "avc1", "avc2", "avc3", "avc4", "hvc1", "hev1", "av01", "vp08", "vp09", "mp4v", "encv", "dvh1",
        "dvhe", "dva1", "dvav", "jpeg", "mjpa", "ap4h", "ap4x", "apch", "apcn", "apcs", "apco", "s263",
    };
    static const char* kAudioEntries[] = {
        "mp4a", "ac-3", "ec-3", "ac-4", "opus", "Opus", "fLaC", "alac", "dtsc", "dtsh",
        "dtse", "enca", "mha1", "mhm1", "sowt", "twos", "lpcm", "ipcm", "spex",
    };
    if (n < 8)
        return;
    AddFieldInt(box, "data_reference_index", Rd16(p + 6));

    for (const char* c : kVideoEntries) {
        if (box.type != c)
            continue;
        if (n >= 28) {
            AddFieldInt(box, "width", Rd16(p + 24));
            AddFieldInt(box, "height", Rd16(p + 26));
        }
        if (n >= 42)
            AddFieldInt(box, "frame_count", Rd16(p + 40));
        if (n >= 76)
            AddFieldInt(box, "depth", Rd16(p + 74));
        return;
    }
    for (const char* c : kAudioEntries) {
        if (box.type != c)
            continue;
        // sampling_rate 是 32 位（16.16），从偏移 24 读到 27，所以门槛是 28 不是 26
        if (n >= 28) {
            AddFieldInt(box, "channel_count", Rd16(p + 16));
            AddFieldInt(box, "sample_size", Rd16(p + 18));
            AddFieldInt(box, "sampling_rate", Rd32(p + 24) >> 16);
        }
        return;
    }
}

// 按 box 类型填语义字段。
//
// 只读载荷前缀、任何长度不足都直接放弃：字段解析失败不能让 box 树本身失败
// （损坏文件的树仍然要能看，只是没有字段）。
void FillBoxFields(ParseContext& ctx, IsobmffBox& box) {
    const std::string& type = box.type;
    static const char* kFieldBoxes[] = {
        "ftyp", "styp", "mvhd", "tkhd", "mdhd", "hdlr", "sidx", "mfhd", "tfhd",
        "tfdt", "trun", "avcC", "hvcC", "av1C", "esds", "colr", "pasp", "btrt",
    };
    bool known = false;
    for (const char* c : kFieldBoxes) {
        if (type == c) {
            known = true;
            break;
        }
    }
    if (!known)
        return;

    const uint64_t cap = (type == "esds") ? 1024 : 256;
    std::vector<uint8_t> buf;
    if (ReadPayload(ctx, box, buf, cap) == 0)
        return;
    const uint8_t* p = buf.data();
    const uint64_t n = buf.size();

    if (type == "ftyp" || type == "styp") {
        if (n < 8)
            return;
        AddField(box, "major_brand", FourCC(p));
        AddFieldInt(box, "minor_version", Rd32(p + 4));
        std::string brands;
        for (uint64_t pos = 8; pos + 4 <= n && pos < 8 + 4 * 8; pos += 4) {
            if (!brands.empty())
                brands += ",";
            brands += FourCC(p + pos);
        }
        if (!brands.empty())
            AddField(box, "compatible_brands", brands);
        return;
    }

    if (type == "mvhd") {
        if (n < 8)
            return;
        const uint8_t version = p[0];
        AddFieldInt(box, "version", version);
        const uint64_t ts_off = (version == 1) ? 20 : 12;
        const uint64_t dur_off = (version == 1) ? 24 : 16;
        const uint64_t next_off = (version == 1) ? 108 : 96;
        uint32_t timescale = 0;
        if (ts_off + 4 <= n) {
            timescale = Rd32(p + ts_off);
            AddFieldInt(box, "timescale", timescale);
        }
        const bool dur_ok = (version == 1) ? (dur_off + 8 <= n) : (dur_off + 4 <= n);
        if (dur_ok) {
            const uint64_t duration = (version == 1) ? Rd64(p + dur_off) : Rd32(p + dur_off);
            AddFieldInt(box, "duration", duration);
            if (timescale > 0) {
                char sec[32];
                std::snprintf(sec, sizeof(sec), "%.3f", static_cast<double>(duration) / static_cast<double>(timescale));
                AddField(box, "duration_seconds", sec);
            }
        }
        if (next_off + 4 <= n)
            AddFieldInt(box, "next_track_id", Rd32(p + next_off));
        return;
    }

    if (type == "tkhd") {
        if (n < 8)
            return;
        const uint8_t version = p[0];
        const uint32_t flags = Rd32(p) & 0x00FFFFFF;
        AddFieldInt(box, "version", version);
        static const char* kFlagNames[] = {"enabled", "in_movie", "in_preview", "in_poster"};
        std::string flag_text;
        for (int bit = 0; bit < 4; ++bit) {
            if ((flags & (1u << bit)) == 0)
                continue;
            if (!flag_text.empty())
                flag_text += ",";
            flag_text += kFlagNames[bit];
        }
        if (!flag_text.empty())
            AddField(box, "flags", flag_text);
        const uint64_t id_off = (version == 1) ? 20 : 12;
        const uint64_t dur_off = (version == 1) ? 28 : 20;
        const uint64_t wh_off = (version == 1) ? 88 : 76;
        if (id_off + 4 <= n)
            AddFieldInt(box, "track_id", Rd32(p + id_off));
        const bool dur_ok = (version == 1) ? (dur_off + 8 <= n) : (dur_off + 4 <= n);
        if (dur_ok) {
            AddFieldInt(box, "duration(movie_timescale)", (version == 1) ? Rd64(p + dur_off) : Rd32(p + dur_off));
        }
        if (wh_off + 8 <= n) {
            AddField(box, "width", Fixed1616(Rd32(p + wh_off)));
            AddField(box, "height", Fixed1616(Rd32(p + wh_off + 4)));
        }
        return;
    }

    if (type == "mdhd") {
        if (n < 8)
            return;
        const uint8_t version = p[0];
        AddFieldInt(box, "version", version);
        const uint64_t ts_off = (version == 1) ? 20 : 12;
        const uint64_t dur_off = (version == 1) ? 24 : 16;
        const uint64_t lang_off = (version == 1) ? 32 : 20;
        if (ts_off + 4 <= n)
            AddFieldInt(box, "timescale", Rd32(p + ts_off));
        const bool dur_ok = (version == 1) ? (dur_off + 8 <= n) : (dur_off + 4 <= n);
        if (dur_ok) {
            AddFieldInt(box, "duration", (version == 1) ? Rd64(p + dur_off) : Rd32(p + dur_off));
        }
        if (lang_off + 2 <= n)
            AddField(box, "language", LanguageCode(Rd16(p + lang_off)));
        return;
    }

    if (type == "hdlr") {
        if (n >= 12)
            AddField(box, "handler_type", FourCC(p + 8));
        const std::string name = HandlerName(p, n);
        if (!name.empty())
            AddField(box, "name", name);
        return;
    }

    if (type == "sidx") {
        if (n < 8)
            return;
        const uint8_t version = p[0];
        AddFieldInt(box, "version", version);
        AddFieldInt(box, "reference_id", Rd32(p + 4));
        if (8 + 4 <= n)
            AddFieldInt(box, "timescale", Rd32(p + 8));
        if (version == 1) {
            if (20 <= n)
                AddFieldInt(box, "earliest_presentation_time", Rd64(p + 12));
            if (28 <= n)
                AddFieldInt(box, "first_offset", Rd64(p + 20));
            if (30 <= n)
                AddFieldInt(box, "reference_count", Rd16(p + 28));
        } else {
            if (16 <= n)
                AddFieldInt(box, "earliest_presentation_time", Rd32(p + 12));
            if (20 <= n)
                AddFieldInt(box, "first_offset", Rd32(p + 16));
            if (22 <= n)
                AddFieldInt(box, "reference_count", Rd16(p + 20));
        }
        return;
    }

    if (type == "mfhd") {
        if (n >= 8)
            AddFieldInt(box, "sequence_number", Rd32(p + 4));
        return;
    }

    if (type == "tfhd") {
        if (n < 8)
            return;
        AddFieldHex(box, "flags", Rd32(p) & 0x00FFFFFF);
        AddFieldInt(box, "track_id", Rd32(p + 4));
        return;
    }

    if (type == "tfdt") {
        if (n < 8)
            return;
        const uint8_t version = p[0];
        AddFieldInt(box, "version", version);
        AddFieldInt(box, "base_media_decode_time", (version == 1 && n >= 12) ? Rd64(p + 4) : Rd32(p + 4));
        return;
    }

    if (type == "trun") {
        if (n < 8)
            return;
        AddFieldHex(box, "flags", Rd32(p) & 0x00FFFFFF);
        AddFieldInt(box, "sample_count", Rd32(p + 4));
        if ((Rd32(p) & 0x000001) != 0 && n >= 12) {
            AddFieldInt(box, "data_offset", static_cast<int32_t>(Rd32(p + 8)));
        }
        return;
    }

    if (type == "avcC") {
        if (n < 6)
            return;
        AddFieldInt(box, "configuration_version", p[0]);
        AddFieldInt(box, "profile_idc", p[1]);
        AddFieldInt(box, "profile_compatibility", p[2]);
        AddFieldInt(box, "level_idc", p[3]);
        const int num_sps = p[5] & 0x1F;
        AddFieldInt(box, "num_of_sps", static_cast<uint64_t>(num_sps));
        AddFieldInt(box, "length_size_minus_one", static_cast<uint64_t>(p[4] & 0x03));
        // 顺着 SPS/PPS 长度走一遍，把 PPS 个数也报出来（找不到就只留 SPS 数）
        uint64_t pos = 6;
        for (int i = 0; i < num_sps && pos + 2 <= n; ++i) {
            const uint16_t len = Rd16(p + pos);
            pos += 2 + len;
        }
        if (pos + 1 <= n)
            AddFieldInt(box, "num_of_pps", static_cast<uint64_t>(p[pos]));
        return;
    }

    if (type == "hvcC") {
        if (n < 23)
            return;
        AddFieldInt(box, "configuration_version", p[0]);
        AddFieldInt(box, "general_profile_space", static_cast<uint64_t>(p[1] >> 6));
        AddFieldInt(box, "general_tier_flag", static_cast<uint64_t>((p[1] >> 5) & 0x01));
        AddFieldInt(box, "general_profile_idc", static_cast<uint64_t>(p[1] & 0x1F));
        AddFieldInt(box, "general_level_idc", p[12]);
        AddFieldInt(box, "min_spatial_segmentation_idc", static_cast<uint64_t>(Rd16(p + 13) & 0x0FFF));
        AddFieldInt(box, "parallelism_type", static_cast<uint64_t>(p[15] & 0x03));
        AddFieldInt(box, "chroma_format_idc", static_cast<uint64_t>(p[16] & 0x03));
        AddFieldInt(box, "bit_depth_luma_minus8", static_cast<uint64_t>(p[17] & 0x07));
        AddFieldInt(box, "bit_depth_chroma_minus8", static_cast<uint64_t>(p[18] & 0x07));
        AddFieldInt(box, "num_temporal_layers", static_cast<uint64_t>((p[21] >> 3) & 0x07) + 1);
        AddFieldInt(box, "length_size_minus_one", static_cast<uint64_t>(p[21] & 0x03));
        AddFieldInt(box, "num_of_arrays", p[22]);
        return;
    }

    if (type == "av1C") {
        if (n < 4)
            return;
        // p[0]: marker(1) + version(7)；marker 位写在地 1 位，别整字节当版本
        AddFieldInt(box, "seq_profile", static_cast<uint64_t>((p[1] >> 5) & 0x07));
        AddFieldInt(box, "seq_level_idx_0", static_cast<uint64_t>(p[1] & 0x1F));
        AddFieldInt(box, "seq_tier_0", static_cast<uint64_t>((p[2] >> 7) & 0x01));
        const int bit_depth = ((p[2] >> 6) & 0x01) != 0 ? (((p[2] >> 5) & 0x01) != 0 ? 12 : 10) : 8;
        AddFieldInt(box, "bit_depth", static_cast<uint64_t>(bit_depth));
        AddFieldInt(box, "monochrome", static_cast<uint64_t>((p[2] >> 4) & 0x01));
        AddFieldInt(box, "chroma_subsampling_x", static_cast<uint64_t>((p[2] >> 3) & 0x01));
        AddFieldInt(box, "chroma_subsampling_y", static_cast<uint64_t>((p[2] >> 2) & 0x01));
        AddFieldInt(box, "chroma_sample_position", static_cast<uint64_t>(p[2] & 0x03));
        return;
    }

    if (type == "esds") {
        if (n < 6)
            return;
        uint64_t pos = 4; // FullBox(4)
        for (int guard = 0; guard < 8 && pos + 2 <= n; ++guard) {
            const uint8_t tag = p[pos++];
            uint32_t len = 0;
            if (!ReadDescLength(p, n, pos, len))
                return;
            const uint64_t body = pos;
            if (body + len > n)
                return;
            if (tag == 0x03) { // ES_Descriptor：跳过头部，进入子描述符
                if (len < 3)
                    return;
                const uint8_t flags = p[body + 2];
                uint64_t next = body + 3;
                if ((flags & 0x80) != 0)
                    next += 2;                                  // dependsOn_ES_ID
                if ((flags & 0x40) != 0 && next < body + len) { // URL
                    next += 1 + p[next];
                }
                if ((flags & 0x20) != 0)
                    next += 2; // OCR_ES_Id
                if (next > body + len)
                    return;
                pos = next;
                continue;
            }
            if (tag == 0x04) { // DecoderConfigDescriptor
                if (len >= 13) {
                    AddFieldInt(box, "object_type_indication", p[body]);
                    AddFieldInt(box, "stream_type", static_cast<uint64_t>(p[body + 1] >> 2));
                    AddFieldHex(box, "buffer_size_db",
                                (static_cast<uint32_t>(p[body + 2]) << 16) | (static_cast<uint32_t>(p[body + 3]) << 8) |
                                    p[body + 4]);
                    AddFieldInt(box, "max_bitrate", Rd32(p + body + 5));
                    AddFieldInt(box, "avg_bitrate", Rd32(p + body + 9));
                }
                return;
            }
            pos = body + len;
        }
        return;
    }

    if (type == "colr") {
        if (n < 4)
            return;
        const std::string colour_type = FourCC(p);
        AddField(box, "colour_type", colour_type);
        // nclx / nclc 都是 primaries(2) transfer(2) matrix(2)；nclx 末尾还有 1 位 range
        if ((colour_type == "nclx" || colour_type == "nclc") && n >= 10) {
            AddFieldInt(box, "colour_primaries", Rd16(p + 4));
            AddFieldInt(box, "transfer_characteristics", Rd16(p + 6));
            AddFieldInt(box, "matrix_coefficients", Rd16(p + 8));
            if (colour_type == "nclx" && n >= 11) {
                AddFieldInt(box, "full_range_flag", static_cast<uint64_t>((p[10] >> 7) & 0x01));
            }
        }
        return;
    }

    if (type == "pasp") {
        if (n < 8)
            return;
        AddFieldInt(box, "hSpacing", Rd32(p));
        AddFieldInt(box, "vSpacing", Rd32(p + 4));
        return;
    }

    if (type == "btrt") {
        if (n < 12)
            return;
        AddFieldInt(box, "bufferSizeDB", Rd32(p));
        AddFieldInt(box, "maxBitrate", Rd32(p + 4));
        AddFieldInt(box, "avgBitrate", Rd32(p + 8));
        return;
    }
}

// 解析一个 box 区间 [start, end) 内的所有 box，挂到 parent->children
//
// parent_type：父 box 的 4CC。stsd 的子 box 是 sample entry（avc1/hvc1/mp4a…），
// 它们自己也是容器，但子 box 的起点由 entry 种类决定（见 SampleEntryChildOffset），
// 不能走 IsContainer 那条通用路径。
void ParseBoxes(ParseContext& ctx, uint64_t start, uint64_t end, int depth, std::vector<IsobmffBox>* parent,
                const std::string* parent_type = nullptr) {
    if (depth > ctx.opt->max_depth || start >= end)
        return;

    uint64_t pos = start;
    uint8_t hdr[16] = {0};
    int guard = 0;
    while (pos + 8 <= end && ++guard < 1000000) {
        // 逐个 box 检查取消：只有入口查一次的话，取消后仍会把整个文件扫完。
        if (infrastructure::Checkpoint(ctx.cancel))
            return;
        if (!ctx.reader->ReadAt(pos, hdr, 8))
            break;
        uint64_t size = Rd32(hdr);
        std::string type = FourCC(hdr + 4);
        uint32_t header_size = 8;

        if (size == 1) {
            if (!ctx.reader->ReadAt(pos + 8, hdr + 8, 8))
                break;
            size = Rd64(hdr + 8);
            header_size = 16;
        } else if (size == 0) {
            size = end - pos; // 最后一个 box 延伸到父容器末尾
        }
        // largesize 接近 UINT64_MAX 时 pos + size 会在 uint64 上回绕成小值：
        // "pos + size > end" 的朴素比较会把越界 box 当合法（阶段 3 语料
        // mp4_size_overflow.mp4 的 largesize=0xFFFFFFFFFFFFFFF8 正是这种情况）。
        // 边界判断一律走 CheckedAdd，回绕不出安全结论。
        uint64_t box_end = 0;
        if (size < header_size || !CheckedAdd(pos, size, box_end) || box_end > end) {
            // 截断/损坏: 收缩到父容器末尾，之后停止（避免死循环）
            if (size < header_size)
                break;
            size = end - pos;
            // 父容器剩余字节连一个 box 头都装不下：继续按"延伸到末尾"处理
            // 会让下面的 size - header_size 无符号下溢，直接停。
            if (size < header_size)
                break;
        }

        IsobmffBox box;
        box.type = type;
        box.size = size;
        box.offset = pos;
        box.header_size = header_size;
        box.depth = depth;

        // 字段级解析（ftyp / mvhd / tkhd / avcC / colr / sidx …）。
        // 不认识的类型一个字节都不读。
        FillBoxFields(ctx, box);

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

        if (depth == 0)
            ctx.out->top_level_order.push_back(type);

        // 样本表与元数据只在 trak 上下文里解析
        const bool in_trak = (ctx.trak_index >= 0);
        if (in_trak && !ctx.opt->skip_sample_tables && IsTableBox(type)) {
            // 表可能很大: 直接按条目上限算需要的字节数，不整块读
            const uint64_t payload = size - header_size;
            const uint64_t cap = std::min<uint64_t>(payload, 64ull * 1024 * 1024);
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, cap);
            if (!buf.empty()) {
                const uint32_t max_entries = ctx.opt->max_entries_per_table;
                if (type == "stts")
                    ParseStts(ctx.trak(), buf.data(), buf.size(), max_entries);
                else if (type == "ctts")
                    ParseCtts(ctx.trak(), buf.data(), buf.size(), max_entries);
                else if (type == "stss")
                    ParseStss(ctx.trak(), buf.data(), buf.size(), max_entries);
                else if (type == "stsz")
                    ParseStsz(ctx.trak(), buf.data(), buf.size(), max_entries);
                else if (type == "stz2")
                    ParseStz2(ctx.trak(), buf.data(), buf.size(), max_entries);
                else if (type == "stsc")
                    ParseStsc(ctx.trak(), buf.data(), buf.size(), max_entries);
                else if (type == "stco")
                    ParseChunkOffsets(ctx.trak(), buf.data(), buf.size(), false, max_entries);
                else if (type == "co64")
                    ParseChunkOffsets(ctx.trak(), buf.data(), buf.size(), true, max_entries);
                else if (type == "elst")
                    ParseElst(ctx.trak(), buf.data(), buf.size(), max_entries);
            }
        } else if (in_trak && (type == "tkhd" || type == "hdlr" || type == "mdhd" || type == "stsd")) {
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, 4096);
            if (!buf.empty()) {
                if (type == "tkhd")
                    ParseTkhd(ctx.trak(), buf.data(), buf.size());
                else if (type == "hdlr")
                    ParseHdlr(ctx.trak(), buf.data(), buf.size());
                else if (type == "mdhd")
                    ParseMdhd(ctx.trak(), buf.data(), buf.size());
                else if (type == "stsd")
                    ParseStsdCodec(ctx.trak(), buf.data(), buf.size());
            }
        } else if (type == "mvhd" && depth >= 1) {
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, 4096);
            if (!buf.empty())
                ParseMvhd(*ctx.out, buf.data(), buf.size());
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
            // push_back 之后 vector 可能已重分配，这里只记住下标，
            // 后续一切访问都走 ctx.frag()，不再拿 push_back 的返回值
            ctx.out->fragments.push_back(f);
            ctx.frag_index = static_cast<int>(ctx.out->fragments.size()) - 1;
            ctx.tfhd_default_duration = 0;
            ctx.tfhd_default_size = 0;
            // tfhd 的默认值随 traf 生效、随 traf 结束：漏清会让下一个 traf
            // 在"自己没写 default flags"时继承上一个 traf 的标志，从而把
            // 一个非独立帧误判成随机访问点。
            ctx.tfhd_default_flags = 0;
            ctx.tfhd_default_flags_present = false;
            ParseBoxes(ctx, pos + header_size, pos + size, depth + 1, &box.children, &type);
            ctx.frag_index = -1;
        } else if (parent_type != nullptr && *parent_type == "stsd") {
            // sample entry：自己也是容器，子 box 是 avcC / hvcC / av1C / esds /
            // colr / pasp / btrt。子 box 起点由 entry 种类决定（视频 78、音频 28/44/64）。
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, 128);
            const uint64_t payload = (box.size > box.header_size) ? (box.size - box.header_size) : 0;
            const int child_off = SampleEntryChildOffset(type, buf.empty() ? nullptr : buf.data(), buf.size());
            if (!buf.empty())
                FillSampleEntryFields(box, buf.data(), buf.size());
            if (child_off >= 0 && static_cast<uint64_t>(child_off) < payload) {
                ParseBoxes(ctx, pos + header_size + static_cast<uint64_t>(child_off), pos + size, depth + 1,
                           &box.children, &type);
            }
        } else if (IsContainer(type)) {
            // stsd / meta 都是 FullBox，子 box 要从 FullBox 头之后开始：
            //   stsd: version/flags(4) + entry_count(4) = 8 字节
            //   meta: version/flags(4) = 4 字节
            // 不跳这些字节，meta 里每个子 box 的 header（size/type）都会错位，
            // 解析出来的 type 全是垃圾。
            uint64_t child_start = pos + header_size;
            if (type == "stsd") {
                child_start += 8;
            } else if (type == "meta") {
                child_start += 4;
            }
            const int saved_trak = ctx.trak_index;
            if (type == "trak") {
                // 同理：只记住新 trak 的下标，别把 tracks.back() 的地址留下来
                ctx.out->tracks.emplace_back();
                ctx.trak_index = static_cast<int>(ctx.out->tracks.size()) - 1;
            }
            ParseBoxes(ctx, child_start, pos + size, depth + 1, &box.children, &type);
            if (type == "trak") {
                // 只保留有媒体时基的轨道（hint/空 trak 直接丢弃）
                if (ctx.trak().media_timescale == 0 && ctx.trak().handler.empty()) {
                    ctx.out->tracks.pop_back();
                }
                ctx.trak_index = saved_trak;
            }
        } else if (ctx.frag_index >= 0 && (type == "tfhd" || type == "tfdt" || type == "trun")) {
            std::vector<uint8_t> buf;
            ReadPayload(ctx, box, buf, std::min<uint64_t>(size - header_size, 16ull * 1024 * 1024));
            if (!buf.empty()) {
                if (type == "tfhd")
                    ParseTfhd(ctx, buf.data(), buf.size());
                else if (type == "tfdt")
                    ParseTfdt(ctx, buf.data(), buf.size());
                else if (type == "trun")
                    ParseTrun(ctx, buf.data(), buf.size(), ctx.opt->max_entries_per_table);
            }
        }

        pos += size;
        if (parent)
            parent->push_back(std::move(box));
    }
}

} // namespace

// ---- 对外接口 ----
std::string IsobmffTrack::TypeName() const {
    if (handler == "vide")
        return "video";
    if (handler == "soun")
        return "audio";
    if (handler == "hint")
        return "hint";
    if (handler == "text")
        return "text";
    if (handler == "sbtl")
        return "subtitle";
    if (handler == "subt")
        return "subtitle";
    if (handler == "meta")
        return "meta";
    if (!handler.empty())
        return handler;
    return "unknown";
}

// stts / ctts 的 sample_count 直接相加容易溢出回绕：stts 一条 entry 常常覆盖几万个
// 样本，几个轨道叠起来就能把 uint32 顶回去，上层拿到的「样本总数」变成个小得离谱的数。
// 用 64 位累加并饱和截断（接口仍是 uint32），宁可报上限也不报一个错误的数。
namespace {
constexpr uint64_t kSampleCountSaturation = 0xFFFFFFFFull;

// stts / ctts 的条目类型不同，抽成模板只在这里用一次，不额外开公共接口
template <typename EntryT>
uint32_t SaturateSampleCount(const std::vector<EntryT>& entries) {
    uint64_t n = 0;
    for (const auto& e : entries) {
        n += e.sample_count;
        if (n > kSampleCountSaturation) {
            n = kSampleCountSaturation;
            break;
        }
    }
    return static_cast<uint32_t>(n);
}
} // namespace

uint32_t IsobmffTrack::SttsSampleCount() const {
    return SaturateSampleCount(stts);
}

uint32_t IsobmffTrack::CttsSampleCount() const {
    return SaturateSampleCount(ctts);
}

bool IsobmffParser::Parse(const std::string& file_path, IsobmffFile& out, const Options& options) {
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
    ctx.cancel = options.cancel;

    ParseBoxes(ctx, 0, out.file_size, 0, &out.top_level);

    if (infrastructure::IsCanceled(ctx.cancel)) {
        // 明确区分"被取消"和"解析失败"：上层据此把任务标成已取消，而不是报失败。
        out.error_message = "已取消";
        return false;
    }

    if (out.top_level.empty()) {
        out.error_message = "未解析到任何 box（不是有效的 ISOBMFF 文件？）";
        return false;
    }

    out.ok = true;
    return true;
}

} // namespace videoeye
