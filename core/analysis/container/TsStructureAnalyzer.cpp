#include "core/analysis/container/TsStructureAnalyzer.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/SeqFileReader.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace {

constexpr uint16_t kPatPid = 0x0000;
constexpr uint16_t kSdtPid = 0x0011;
constexpr uint16_t kNullPid = 0x1FFF;

// 探测包长时最多读这么多字节（足够覆盖 6 个包，又不会在小文件上白读）
constexpr int kLayoutProbeBytes = 64 * 1024;
// 失步重同步时一次搜多少字节
constexpr int kResyncWindowBytes = 64 * 1024;
// PSI 单段上限。规范给 1024，这里留余量识别畸形段并丢弃，别让它把内存吃光
constexpr int kMaxSectionBytes = 4096;
// 描述符明细最多记多少个未识别的 tag（防止畸形 PMT 把字符串撑爆）
constexpr int kMaxUnknownDescriptorTags = 8;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 本文件内的拼串：Cat(a, b, c) 等价于 a << b << c。
//
// 为什么不用 detail/AnalysisTextUtil.h 的 StrCat：那个 API 是"QString("%1%2").arg()"
// 的替身，参数必须由格式串里的 %N 逐个认领。TS 解析里绝大多数是"标签 + 数值 + 单位"
// 的表格/日志拼装，写成 StrCat("PID=0x%1 %2", pid, name) 每处都要数字位，改一处就错位。
// 这里用 operator<< 直接串，语义直观且不会有"占位符数错导致静默丢参"的问题
// （StrCat 少写一个 %N 就会把参数静默吞掉，编译期也不报）。只在 TS 解析内部使用。
template <typename... Args>
std::string Cat(const Args&... args) {
    std::ostringstream os;
    (void)std::initializer_list<int>{((os << args), 0)...};
    return os.str();
}

bool IsPrintableAscii(uint8_t c) { return c >= 0x20 && c <= 0x7E; }

std::string FourCC(const uint8_t* p, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        out.push_back(IsPrintableAscii(c) ? static_cast<char>(c) : '?');
    }
    return out;
}

// DVB 文本（SDT 的 service_name / provider_name）：
// 首字节 < 0x20 时是字符集/编码标识（0x15 = UTF-8，0x10..0x14 = 各种 ISO-8859），
// 这里只丢掉这一字节、其余按 UTF-8/ASCII 原样收；控制字符折成空格。
std::string DvbText(const uint8_t* d, int n) {
    if (d == nullptr || n <= 0) return {};
    int start = 0;
    if (d[0] < 0x20) start = 1;
    std::string out;
    for (int i = start; i < n; ++i) {
        const uint8_t c = d[i];
        if (c == 0x00) break;
        if (c < 0x20) {
            out.push_back(' ');
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    return TrimCopy(out);
}

// 33-bit / 90 kHz 时间戳（PES 的 PTS/DTS），5 字节
uint64_t ReadTimestamp5(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0] & 0x0E) << 29) |
           (static_cast<uint64_t>(p[1]) << 22) |
           (static_cast<uint64_t>(p[2] & 0xFE) << 14) |
           (static_cast<uint64_t>(p[3]) << 7) |
           (static_cast<uint64_t>(p[4]) >> 1);
}

std::string TimestampSeconds(uint64_t ts) { return Fixed(ts / 90000.0, 3) + "s"; }

// stream_type → 可读编码名。
//
// 表里 0x20/0x24 以前写成 "H.265"、0x24 写成 "H.266" —— 那是错的：
// 规范里 0x24 = HEVC，VVC 是 0x33。按错表解析会把 HEVC 流标成 H.266。
std::string StreamTypeName(uint8_t stream_type) {
    switch (stream_type) {
    case 0x01: return "MPEG-1 Video";
    case 0x02: return "MPEG-2 Video";
    case 0x03: return "MPEG-1 Audio";
    case 0x04: return "MPEG-2 Audio";
    case 0x05: return "私有段";
    case 0x06: return "PES 私有数据";
    case 0x0F: return "AAC (ADTS)";
    case 0x10: return "MPEG-4 Visual";
    case 0x11: return "AAC (LATM)";
    case 0x12: return "MPEG-4 SL";
    case 0x15: return "元数据 (ID3)";
    case 0x1B: return "H.264/AVC";
    case 0x20: return "MPEG-4 MVC";
    case 0x21: return "JPEG 2000";
    case 0x24: return "H.265/HEVC";
    case 0x25: return "H.265/HEVC (temporal subset)";
    case 0x33: return "H.266/VVC";
    case 0x42: return "AVS2";
    case 0x81: return "AC-3";
    case 0x82: return "DTS";
    case 0x86: return "SCTE-35";
    case 0x87: return "E-AC-3";
    case 0xA1: return "E-AC-3 (ATSC)";
    case 0xD1: return "Dirac";
    default:   return Cat("StreamType 0x", HexFillLow(stream_type, 2));
    }
}

// stream_type 只给出大类时（0x06 私有数据最常见），要靠 registration_descriptor
// 的 4CC 才能确定编码 —— 广播里的 AC-3 / HEVC / AV1 都走这条通道。
std::string RegistrationToCodec(const std::string& registration) {
    if (registration == "AC-3") return "AC-3";
    if (registration == "EAC3") return "E-AC-3";
    if (registration == "HEVC") return "H.265/HEVC";
    if (registration == "AV01") return "AV1";
    if (registration == "Opus") return "Opus";
    if (registration == "VC-1") return "VC-1";
    if (registration == "DTS1" || registration == "DTS2" || registration == "DTS3") return "DTS";
    if (registration == "AC-4") return "AC-4";
    if (registration == "BSSD") return "AAC (ADTS)";
    return std::string();
}

std::string CodecNameOf(uint8_t stream_type, const std::string& registration) {
    const std::string by_registration = RegistrationToCodec(registration);
    if (!by_registration.empty()) return by_registration;
    return StreamTypeName(stream_type);
}

std::string KindOf(uint8_t stream_type, const std::string& registration) {
    switch (stream_type) {
    case 0x01: case 0x02: case 0x10: case 0x1B: case 0x20:
    case 0x21: case 0x24: case 0x25: case 0x33: case 0x42: case 0xD1:
        return "video";
    case 0x03: case 0x04: case 0x0F: case 0x11: case 0x81: case 0x82:
    case 0x87: case 0xA1:
        return "audio";
    case 0x06: {
        // 私有数据靠 registration 定性
        if (registration == "AC-3" || registration == "EAC3" || registration == "Opus" ||
            registration == "DTS1" || registration == "AC-4") return "audio";
        if (registration == "HEVC" || registration == "AV01" || registration == "VC-1") return "video";
        return "data";
    }
    case 0x05: case 0x86: return "data";
    default:   return "data";
    }
}

std::string StreamIdName(uint8_t sid) {
    if (sid >= 0xC0 && sid <= 0xDF) return Cat("Audio (0x", HexFillLow(sid, 2), ")");
    if (sid >= 0xE0 && sid <= 0xEF) return Cat("Video (0x", HexFillLow(sid, 2), ")");
    switch (sid) {
    case 0xBC: return "Program Stream Map";
    case 0xBD: return "Private Stream 1";
    case 0xBE: return "Padding Stream";
    case 0xBF: return "Private Stream 2";
    case 0xF0: return "ECM";
    case 0xF1: return "EMM";
    case 0xFD: return "Extended Stream";
    default:   return Cat("stream_id 0x", HexFillLow(sid, 2));
    }
}

// ---------------------------------------------------------------------------
// PSI section 组装
//
// PMT 里流一多（十几个 ES + 描述符）就会超过 184 字节被拆到多个 TS 包，
// SRDT/SDT 同理。以前只解析"落在单个包里的那一段"，于是流表被截断、
// 后面的 ES 全部消失。这里按 section_length 跨包拼装，一个包里有多段也能全拆出来。
// ---------------------------------------------------------------------------
class SectionAssembler {
public:
    void Push(bool pusi, const uint8_t* data, int len, std::vector<std::vector<uint8_t>>& out) {
        if (data == nullptr || len <= 0) return;
        int pos = 0;
        if (pusi) {
            const int pointer = data[0];
            pos = 1;
            // pointer_field 之前的字节是上一段的尾巴
            if (!pending_.empty() && pointer > 0) {
                const int take = std::min(pointer, len - pos);
                Append(data + pos, take, out);
            }
            pos += pointer;
            // pointer 之后必定是新段的起点：旧状态一律丢弃
            pending_.clear();
            active_ = pos <= len;
            need_ = 0;
        } else if (!active_) {
            return;  // 没有起始的续片，继续等下一段
        }
        if (pos < len) Append(data + pos, len - pos, out);
    }

private:
    void Append(const uint8_t* d, int n, std::vector<std::vector<uint8_t>>& out) {
        int pos = 0;
        while (pos < n && active_) {
            if (pending_.size() < 3) {
                const int take = std::min(n - pos, 3 - static_cast<int>(pending_.size()));
                pending_.insert(pending_.end(), d + pos, d + pos + take);
                pos += take;
                if (pending_.size() < 3) return;
                need_ = 3 + (((pending_[1] & 0x0F) << 8) | pending_[2]);
                if (need_ < 3 || need_ > kMaxSectionBytes) {
                    // 畸形段：丢弃并等下一个 PUSI，别在这里死循环
                    pending_.clear();
                    active_ = false;
                    return;
                }
            }
            const int remain = need_ - static_cast<int>(pending_.size());
            if (remain <= 0) {
                pending_.clear();
                active_ = false;
                return;
            }
            const int take = std::min(n - pos, remain);
            pending_.insert(pending_.end(), d + pos, d + pos + take);
            pos += take;
            if (static_cast<int>(pending_.size()) == need_) {
                out.push_back(pending_);
                pending_.clear();
                need_ = 0;
            } else {
                return;
            }
        }
    }

    std::vector<uint8_t> pending_;
    bool active_ = false;
    int need_ = 0;
};

// ---------------------------------------------------------------------------
// 描述符
// ---------------------------------------------------------------------------

struct EsInfo {
    uint8_t stream_type = 0;
    uint16_t pid = 0;
    std::string registration;
    std::string language;
    int component_tag = -1;
    bool has_ca = false;
    int ca_system_id = 0;
    bool ac3 = false;
    bool eac3 = false;
    bool teletext = false;
    bool subtitling = false;
    std::vector<std::string> unknown_tags;

    std::string Kind() const { return KindOf(stream_type, registration); }
    std::string CodecName() const { return CodecNameOf(stream_type, registration); }
};

std::string LanguageText(const uint8_t* d, int len) {
    // ISO_639_language_descriptor 可以带多条语言，第一条为主
    if (len < 3) return std::string();
    const std::string lang = FourCC(d, 3);
    std::string out = TrimCopy(lang);
    if (len >= 4) {
        const uint8_t audio_type = d[3];
        if (audio_type == 0x03) out += " (听障)";
        else if (audio_type == 0x02) out += " (视觉障碍)";
    }
    return out;
}

void ParseEsDescriptors(const uint8_t* d, int len, EsInfo& es) {
    int pos = 0;
    while (pos + 2 <= len) {
        const uint8_t tag = d[pos];
        const int dlen = d[pos + 1];
        const int body = pos + 2;
        if (body + dlen > len) break;
        switch (tag) {
        case 0x05:  // registration_descriptor
            if (dlen >= 4) es.registration = FourCC(d + body, 4);
            break;
        case 0x0A:  // ISO_639_language_descriptor
            es.language = LanguageText(d + body, dlen);
            break;
        case 0x09:  // CA_descriptor
            es.has_ca = true;
            if (dlen >= 2) es.ca_system_id = (d[body] << 8) | d[body + 1];
            break;
        case 0x52:  // stream_identifier_descriptor
            if (dlen >= 1) es.component_tag = d[body];
            break;
        case 0x6A: es.ac3 = true; break;        // AC-3_descriptor
        case 0x7A: es.eac3 = true; break;       // enhanced_AC-3_descriptor
        case 0x56: es.teletext = true; break;   // teletext_descriptor
        case 0x59: es.subtitling = true; break; // subtitling_descriptor
        default:
            if (static_cast<int>(es.unknown_tags.size()) < kMaxUnknownDescriptorTags) {
                es.unknown_tags.push_back(Cat("0x", HexFillLow(tag, 2)));
            }
            break;
        }
        pos = body + dlen;
    }
}

std::string DescribeEs(const EsInfo& es) {
    std::vector<std::string> parts;
    parts.push_back(Cat("PID 0x", HexFillLow(es.pid, 4)));
    if (!es.registration.empty()) parts.push_back("reg=" + es.registration);
    if (!es.language.empty()) parts.push_back("lang=" + es.language);
    if (es.has_ca) {
        parts.push_back(Cat("加密 CA=0x", HexFillLow(es.ca_system_id, 4)));
    }
    if (es.ac3) parts.push_back("AC-3 描述符");
    if (es.eac3) parts.push_back("E-AC-3 描述符");
    if (es.teletext) parts.push_back("图文电视");
    if (es.subtitling) parts.push_back("字幕");
    if (es.component_tag >= 0) {
        parts.push_back(Cat("component_tag=", es.component_tag));
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += " ";
        out += parts[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// 解析中间态
// ---------------------------------------------------------------------------

struct PesSample {
    uint8_t stream_id = 0;
    bool has_pts = false;
    uint64_t pts = 0;
    bool has_dts = false;
    uint64_t dts = 0;
    int64_t file_offset = 0;
};

struct ProgramInfo {
    uint16_t program_number = 0;
    uint16_t pmt_pid = 0;
    uint16_t pcr_pid = 0x1FFF;
    int version = -1;
    std::vector<EsInfo> streams;
};

struct ServiceInfo {
    uint16_t service_id = 0;
    int service_type = -1;
    std::string provider;
    std::string name;
};

struct PidStat {
    uint64_t packets = 0;
    uint64_t payload_bytes = 0;
    uint64_t scrambled = 0;
    uint64_t cc_errors = 0;
    int last_cc = -1;
    uint64_t pcr_count = 0;
    uint64_t first_pcr_90k = 0;
    uint64_t last_pcr_90k = 0;
    uint64_t last_pcr_27m = 0;
    uint64_t max_pcr_gap_27m = 0;
};

// 按包长与同步偏移量定布局。返回 false 表示这不是一个可识别的 TS。
//
// 192 字节（M2TS/AVCHD）在前面多 4 字节 TP_extra_header，同步字节落在偏移 4；
// 204 字节是 RS(204,188) 纠错，同步字节仍在 0。以前一律按 188 硬编码，
// .m2ts 会在第一步就"未找到同步字节"，然后静默回退到 FFmpeg 通用分析。
bool DetectLayout(const std::string& probe, int& packet_size, int64_t& first_sync,
                  int& matched_syncs) {
    static const int kCandidates[] = {188, 192, 204};
    const int64_t len = static_cast<int64_t>(probe.size());
    int best_size = 0;
    int64_t best_off = -1;
    int best_count = 0;

    for (const int ps : kCandidates) {
        for (int off = 0; off < ps && off < len; ++off) {
            int count = 0;
            for (int k = 0; k < 6; ++k) {
                const int64_t pos = off + static_cast<int64_t>(k) * ps;
                if (pos >= len) break;
                if (static_cast<uint8_t>(probe[static_cast<size_t>(pos)]) != 0x47) break;
                ++count;
            }
            if (count > best_count) {
                best_count = count;
                best_size = ps;
                best_off = off;
            }
        }
    }
    if (best_count < 2 || best_size == 0) return false;
    packet_size = best_size;
    first_sync = best_off;
    matched_syncs = best_count;
    return true;
}

// 从 from 开始往后找一个可信的同步点（该位置与 +ps、+2ps 都是 0x47）。
bool FindSyncOffset(SeqFileReader& file, int64_t from, int packet_size, int64_t max_bytes,
                    int64_t& found) {
    if (!file.Seek(from)) return false;
    int64_t scanned = 0;
    while (scanned <= max_bytes) {
        const std::string chunk = file.Read(kResyncWindowBytes);
        if (chunk.empty()) return false;
        const int64_t len = static_cast<int64_t>(chunk.size());
        for (int64_t p = 0; p + 1 < len; ++p) {
            if (static_cast<uint8_t>(chunk[static_cast<size_t>(p)]) != 0x47) continue;
            // 至少再验一个包，避免把载荷里的 0x47 当成同步字节
            const int64_t next = p + packet_size;
            if (next < len && static_cast<uint8_t>(chunk[static_cast<size_t>(next)]) != 0x47) {
                continue;
            }
            found = from + scanned + p;
            return true;
        }
        scanned += len;
        if (static_cast<int64_t>(chunk.size()) < kResyncWindowBytes) return false;
    }
    return false;
}

}  // namespace

bool TsStructureAnalyzer::Analyze(const std::string& file_path,
                                  model::ContainerStructureResult& result,
                                  const std::atomic<bool>* cancel,
                                  const TsStructureOptions& options) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::MPEG_TS;
    result.format_name = "MPEG-TS";
    result.file_path = file_path;

    const std::string probe = file.Read(kLayoutProbeBytes);
    int packet_size = 0;
    int64_t first_sync = -1;
    int matched_syncs = 0;
    if (!DetectLayout(probe, packet_size, first_sync, matched_syncs)) {
        result.error_message = "未找到 TS 同步字节";
        return false;
    }
    const int64_t file_size = file.Size();
    if (!file.Seek(first_sync)) {
        result.error_message = "无法定位 TS 同步字节";
        return false;
    }

    // ---- 扫描 ----
    std::map<uint16_t, ProgramInfo> programs;           // program_number -> 信息
    std::map<uint16_t, ServiceInfo> services;           // service_id -> SDT 信息
    std::map<uint16_t, PidStat> pid_stats;
    std::map<uint16_t, std::vector<PesSample>> pes_samples;
    std::map<uint16_t, SectionAssembler> section_asm;   // PSI PID -> 组装器
    std::map<uint16_t, uint16_t> pmt_pid_to_program;    // PMT PID -> program_number

    uint64_t total_packets = 0;
    uint64_t null_packets = 0;
    uint64_t total_pes = 0;
    uint64_t sync_loss = 0;
    uint64_t cc_errors = 0;
    uint64_t scrambled = 0;
    int64_t scanned_bytes = 0;
    // PSI 表是否真的出现过。用"解析到一张表"而不是"programs 非空"来判：
    // 一张 program_number 全为 0（只有 NIT）的 PAT 也是 PAT，报"缺 PAT"就错了。
    bool saw_pat = false;
    bool saw_pmt = false;

    const uint64_t packet_limit =
        (options.max_scan_packets > 0) ? static_cast<uint64_t>(options.max_scan_packets) : 0;

    // packet_size 是"步长"（188 / 192 / 204），而一个 TS 包本体恒为 188 字节：
    //   192 = 前面多 4 字节 TP_extra_header（从同步字节开始读，末尾那 4 字节属于下一包）
    //   204 = 188 + 16 字节 RS 校验尾
    // 所以"读多少"用步长、"字段边界算什么"用 188。两者混用会把 TP 头/校验尾
    // 当成载荷，adaptation_field_length 与 PSI 的边界都会算错。
    constexpr int kTsPacketBytes = 188;
    if (packet_size < kTsPacketBytes) {
        result.error_message = "TS 包长异常";
        return false;
    }

    std::vector<char> buf(static_cast<size_t>(packet_size));
    for (;;) {
        if (packet_limit > 0 && total_packets >= packet_limit) break;
        if (infrastructure::Checkpoint(cancel)) {
            result.error_message = "已取消";
            return false;
        }
        if (!file.ReadRaw(buf.data(), packet_size)) break;

        // 失步：不回退整段扫描，重新找同步点继续（广播流里失步是常态）
        if (static_cast<uint8_t>(buf[0]) != 0x47) {
            const int64_t search_from = file.Pos() - packet_size + 1;
            int64_t new_pos = -1;
            if (!FindSyncOffset(file, search_from, packet_size, options.max_resync_bytes,
                                new_pos)) {
                result.metadata["失步位置"] = std::to_string(search_from);
                break;
            }
            ++sync_loss;
            if (!file.Seek(new_pos)) break;
            continue;
        }

        ++total_packets;
        scanned_bytes += packet_size;

        const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data());
        const bool pusi = (p[1] & 0x40) != 0;
        const uint16_t pid = static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
        const uint8_t scrambling = static_cast<uint8_t>((p[3] >> 6) & 0x03);
        const uint8_t afc = static_cast<uint8_t>((p[3] >> 4) & 0x03);
        const uint8_t cc = static_cast<uint8_t>(p[3] & 0x0F);

        PidStat& stat = pid_stats[pid];
        ++stat.packets;
        if (scrambling != 0) {
            ++stat.scrambled;
            ++scrambled;
        }
        if (pid == kNullPid) {
            ++null_packets;
            continue;
        }
        const bool has_payload = (afc == 1 || afc == 3);
        const bool has_af = (afc == 2 || afc == 3);

        int payload_start = 4;
        bool discontinuity = false;
        if (has_af) {
            if (payload_start >= kTsPacketBytes) continue;
            const int af_len = static_cast<uint8_t>(p[payload_start]);
            if (af_len > 0 && payload_start + 1 < kTsPacketBytes) {
                const uint8_t af_flags = static_cast<uint8_t>(p[payload_start + 1]);
                discontinuity = (af_flags & 0x80) != 0;
                if ((af_flags & 0x10) != 0 && payload_start + 7 < kTsPacketBytes) {
                    const uint8_t* q = p + payload_start + 2;
                    const uint64_t base = (static_cast<uint64_t>(q[0]) << 25) |
                                          (static_cast<uint64_t>(q[1]) << 17) |
                                          (static_cast<uint64_t>(q[2]) << 9) |
                                          (static_cast<uint64_t>(q[3]) << 1) |
                                          (static_cast<uint64_t>(q[4]) >> 7);
                    const uint64_t ext =
                        (static_cast<uint64_t>(q[4] & 0x01) << 8) | q[5];
                    const uint64_t pcr27 = base * 300 + ext;
                    if (stat.pcr_count == 0) {
                        stat.first_pcr_90k = base;
                    } else {
                        const uint64_t gap =
                            (pcr27 > stat.last_pcr_27m) ? (pcr27 - stat.last_pcr_27m) : 0;
                        if (gap > stat.max_pcr_gap_27m) stat.max_pcr_gap_27m = gap;
                    }
                    stat.last_pcr_27m = pcr27;
                    stat.last_pcr_90k = base;
                    ++stat.pcr_count;
                }
            }
            payload_start += 1 + af_len;
        }

        // 连续计数（continuity_counter）：只对带载荷的包判。
        // discontinuity_indicator 置位表示"这里本来就该断"（拼接/切换点），不计错误。
        if (has_payload) {
            if (stat.last_cc >= 0 && !discontinuity) {
                const int expected = (stat.last_cc + 1) & 0x0F;
                if (static_cast<int>(cc) != expected) {
                    ++stat.cc_errors;
                    ++cc_errors;
                }
            }
            stat.last_cc = cc;
        }

        if (!has_payload || payload_start >= kTsPacketBytes) continue;
        const uint8_t* payload = p + payload_start;
        const int payload_len = kTsPacketBytes - payload_start;
        stat.payload_bytes += static_cast<uint64_t>(payload_len);

        // ---- PSI：PAT / PMT / SDT ----
        if (pid == kPatPid || pid == kSdtPid || pmt_pid_to_program.count(pid) > 0) {
            std::vector<std::vector<uint8_t>> sections;
            SectionAssembler& asm_ = section_asm[pid];
            asm_.Push(pusi, payload, payload_len, sections);
            for (const auto& sec : sections) {
                if (sec.size() < 3) continue;
                const uint8_t table_id = sec[0];
                if (pid == kPatPid && table_id == 0x00) {
                    saw_pat = true;
                    // transport_stream_id(2) version(1) sec_no(1) last(1) 之后是 4 字节条目
                    if (sec.size() < 12) continue;
                    const int end = static_cast<int>(sec.size()) - 4;  // 去掉 CRC32
                    for (int pos = 8; pos + 4 <= end; pos += 4) {
                        const uint16_t program_number =
                            static_cast<uint16_t>((sec[pos] << 8) | sec[pos + 1]);
                        const uint16_t pmt_pid =
                            static_cast<uint16_t>(((sec[pos + 2] & 0x1F) << 8) | sec[pos + 3]);
                        if (program_number == 0) continue;  // 0 = NIT
                        ProgramInfo& prog = programs[program_number];
                        prog.program_number = program_number;
                        prog.pmt_pid = pmt_pid;
                        pmt_pid_to_program[pmt_pid] = program_number;
                    }
                } else if (pmt_pid_to_program.count(pid) > 0 && table_id == 0x02) {
                    saw_pmt = true;
                    if (sec.size() < 16) continue;
                    const uint16_t program_number =
                        static_cast<uint16_t>((sec[3] << 8) | sec[4]);
                    ProgramInfo& prog = programs[program_number];
                    prog.program_number = program_number;
                    prog.pmt_pid = pid;
                    prog.version = (sec[5] >> 1) & 0x1F;
                    prog.pcr_pid =
                        static_cast<uint16_t>(((sec[8] & 0x1F) << 8) | sec[9]);
                    const int program_info_len =
                        static_cast<int>(((sec[10] & 0x0F) << 8) | sec[11]);
                    int pos = 12 + program_info_len;
                    const int end = static_cast<int>(sec.size()) - 4;  // 去掉 CRC32
                    prog.streams.clear();
                    while (pos + 5 <= end) {
                        EsInfo es;
                        es.stream_type = sec[pos];
                        es.pid = static_cast<uint16_t>(((sec[pos + 1] & 0x1F) << 8) | sec[pos + 2]);
                        const int es_info_len =
                            static_cast<int>(((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4]);
                        const int body = pos + 5;
                        if (body + es_info_len > end) break;
                        ParseEsDescriptors(sec.data() + body, es_info_len, es);
                        prog.streams.push_back(es);
                        pos = body + es_info_len;
                    }
                } else if (pid == kSdtPid && table_id == 0x42) {
                    // transport_stream_id(2) version(1) sec_no(1) last(1)
                    // original_network_id(2) reserved(1) 之后是 service 循环
                    if (sec.size() < 15) continue;
                    int pos = 11;
                    const int end = static_cast<int>(sec.size()) - 4;
                    while (pos + 5 <= end) {
                        ServiceInfo svc;
                        svc.service_id = static_cast<uint16_t>((sec[pos] << 8) | sec[pos + 1]);
                        const uint8_t flags = sec[pos + 2];
                        svc.service_type = static_cast<int>((flags >> 2) & 0x3F);
                        const int desc_len =
                            static_cast<int>(((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4]);
                        const int body = pos + 5;
                        if (body + desc_len > end) break;
                        int dp = body;
                        while (dp + 2 <= body + desc_len) {
                            const uint8_t tag = sec[dp];
                            const int dlen = sec[dp + 1];
                            if (dp + 2 + dlen > body + desc_len) break;
                            if (tag == 0x48) {  // service_descriptor
                                // service_type(1) provider_len(1) provider name_len(1) name
                                // 长度全由段内字节给出，逐段夹在 limit 内，畸形段不会越界读。
                                const int p = dp + 2;
                                const int limit = p + dlen;
                                int at = p;
                                if (at < limit) {
                                    svc.service_type = sec[at];
                                    ++at;
                                }
                                if (at < limit) {
                                    const int plen = sec[at];
                                    ++at;
                                    if (at + plen <= limit) {
                                        svc.provider = DvbText(sec.data() + at, plen);
                                        at += plen;
                                    } else {
                                        at = limit;
                                    }
                                }
                                if (at < limit) {
                                    const int nlen = sec[at];
                                    ++at;
                                    if (at + nlen <= limit) {
                                        svc.name = DvbText(sec.data() + at, nlen);
                                    }
                                }
                            }
                            dp += 2 + dlen;
                        }
                        services[svc.service_id] = svc;
                        pos = body + desc_len;
                    }
                }
            }
            continue;
        }

        // ---- PES 采样 ----
        if (pusi && payload_len >= 9 && payload[0] == 0x00 && payload[1] == 0x00 &&
            payload[2] == 0x01) {
            const uint8_t stream_id = payload[3];
            PesSample sample;
            sample.stream_id = stream_id;
            sample.file_offset = file.Pos() - packet_size;
            // padding / program_stream_map / private_stream_2 / ECM / EMM 没有 PES 扩展头
            const bool has_ext = !(stream_id == 0xBC || stream_id == 0xBE || stream_id == 0xBF ||
                                   stream_id == 0xF0 || stream_id == 0xF1 || stream_id == 0xF2 ||
                                   stream_id == 0xF8 || stream_id == 0xFF);
            if (has_ext && payload_len >= 18) {
                const uint8_t pts_dts_flags =
                    static_cast<uint8_t>((payload[7] >> 6) & 0x03);
                const uint8_t* opt = payload + 9;
                if ((pts_dts_flags & 0x02) != 0) {
                    sample.has_pts = true;
                    sample.pts = ReadTimestamp5(opt);
                }
                if (pts_dts_flags == 0x03 && payload_len >= 23) {
                    sample.has_dts = true;
                    sample.dts = ReadTimestamp5(opt + 5);
                }
            }
            auto& list = pes_samples[pid];
            if (static_cast<int>(list.size()) < options.max_pes_per_pid) {
                list.push_back(sample);
            }
            ++total_pes;
        }
    }

    if (total_packets == 0) {
        result.error_message = "未找到 TS 同步字节";
        return false;
    }

    // ---- 结构树 ----
    model::ContainerElement root;
    root.name = "MPEG-TS Stream";
    root.type = "TS";
    root.size = static_cast<uint64_t>(file_size < 0 ? 0 : file_size);
    root.offset = 0;
    root.depth = 0;
    root.value = Cat("包长=", packet_size, "B | 已扫 ", total_packets, " 包 | 同步偏移=",
                        first_sync);
    root.extra = Cat("文件大小=", file_size, " 字节 | 预估总包数=",
                        (packet_size > 0 ? (file_size / packet_size) : 0),
                        " | 扫描覆盖=",
                        Fixed(packet_size > 0 && file_size > 0
                                  ? (100.0 * static_cast<double>(scanned_bytes) /
                                     static_cast<double>(file_size))
                                  : 0.0,
                              1),
                        "%");

    // 节目 → ES → PES 采样
    for (const auto& kv : programs) {
        const ProgramInfo& prog = kv.second;
        model::ContainerElement prog_elem;
        std::string service_name;
        auto svc_it = services.find(prog.program_number);
        if (svc_it != services.end()) service_name = svc_it->second.name;
        prog_elem.name = service_name.empty()
                             ? Cat("Program ", prog.program_number)
                             : Cat("Program ", prog.program_number, " (", service_name, ")");
        prog_elem.type = "Program";
        prog_elem.depth = 1;
        prog_elem.value = Cat("PMT PID=0x", HexFillLow(prog.pmt_pid, 4),
                                 " PCR PID=0x", HexFillLow(prog.pcr_pid, 4),
                                 " 流=", prog.streams.size());
        if (prog.version >= 0) {
            prog_elem.extra = Cat("PMT version=", prog.version);
        }

        for (const EsInfo& es : prog.streams) {
            model::ContainerElement es_elem;
            es_elem.name = Cat("PID 0x", HexFillLow(es.pid, 4), " - ", es.CodecName());
            es_elem.type = "Elementary Stream";
            es_elem.depth = 2;
            es_elem.value = DescribeEs(es);
            auto stat_it = pid_stats.find(es.pid);
            if (stat_it != pid_stats.end()) {
                es_elem.value += Cat(" 包=", stat_it->second.packets);
                if (total_packets > 0) {
                    es_elem.value += Cat(" (",
                                            Fixed(100.0 * static_cast<double>(
                                                             stat_it->second.packets) /
                                                      static_cast<double>(total_packets),
                                                  1),
                                            "%)");
                }
                if (stat_it->second.cc_errors > 0) {
                    es_elem.value += Cat(" CC错=", stat_it->second.cc_errors);
                }
                if (stat_it->second.pcr_count > 0) {
                    es_elem.value += Cat(" PCR=", stat_it->second.pcr_count);
                }
            }

            const auto pes_it = pes_samples.find(es.pid);
            if (pes_it != pes_samples.end()) {
                for (const PesSample& pe : pes_it->second) {
                    model::ContainerElement pes_elem;
                    pes_elem.name = StreamIdName(pe.stream_id);
                    pes_elem.type = "PES";
                    pes_elem.depth = 3;
                    pes_elem.offset = static_cast<uint64_t>(pe.file_offset < 0 ? 0
                                                                              : pe.file_offset);
                    std::string v;
                    if (pe.has_pts) v += "PTS=" + TimestampSeconds(pe.pts);
                    if (pe.has_dts) {
                        v += (v.empty() ? "" : " ");
                        v += "DTS=" + TimestampSeconds(pe.dts);
                    }
                    if (v.empty()) v = "(无时间戳)";
                    pes_elem.value = v;
                    es_elem.children.push_back(pes_elem);
                }
            }
            prog_elem.children.push_back(es_elem);

            model::ContainerStreamInfo si;
            si.index = static_cast<int>(result.streams.size());
            si.codec = es.CodecName();
            si.type = es.Kind();
            std::string details = Cat("PID 0x", HexFillLow(es.pid, 4));
            if (!es.language.empty()) details += " lang=" + es.language;
            if (es.has_ca) details += " 加密";
            if (!es.registration.empty()) details += " reg=" + es.registration;
            // 码率份额：按包数占比给，比按字节更稳（空包/填充的影响小），
            // 但仍然比"只有 PID 号"有用得多 —— 直接能看出哪条流在吃带宽。
            if (total_packets > 0) {
                auto stat_it = pid_stats.find(es.pid);
                if (stat_it != pid_stats.end()) {
                    details += Cat(" ", Fixed(100.0 * static_cast<double>(stat_it->second.packets) /
                                                  static_cast<double>(total_packets),
                                              1),
                                   "%");
                }
            }
            si.details = details;
            result.streams.push_back(si);
        }
        root.children.push_back(prog_elem);
    }

    // SDT 服务表（节目名/提供商）
    if (!services.empty()) {
        model::ContainerElement sdt;
        sdt.name = Cat("SDT 服务表 (", services.size(), ")");
        sdt.type = "ServiceDescription";
        sdt.depth = 1;
        for (const auto& kv : services) {
            model::ContainerElement svc;
            svc.name = Cat("Service ", kv.second.service_id);
            svc.type = "Service";
            svc.depth = 2;
            std::string v = kv.second.name.empty() ? std::string("(无名称)") : kv.second.name;
            if (!kv.second.provider.empty()) v += " | 提供商: " + kv.second.provider;
            svc.value = v;
            sdt.children.push_back(svc);
        }
        root.children.push_back(sdt);
    }

    // PID 分布（按包数降序，最多 20 行）
    {
        std::vector<std::pair<uint16_t, uint64_t>> ordered;
        ordered.reserve(pid_stats.size());
        for (const auto& kv : pid_stats) ordered.emplace_back(kv.first, kv.second.packets);
        std::sort(ordered.begin(), ordered.end(),
                  [](const std::pair<uint16_t, uint64_t>& a,
                     const std::pair<uint16_t, uint64_t>& b) {
                      return a.second != b.second ? a.second > b.second : a.first < b.first;
                  });

        model::ContainerElement pid_elem;
        pid_elem.name = "PID 分布";
        pid_elem.type = "Statistics";
        pid_elem.depth = 1;
        pid_elem.value = Cat("已扫 ", total_packets, " 包 / ", pid_stats.size(),
                                " 个 PID / 空包 ", null_packets);
        const size_t shown = std::min<size_t>(20, ordered.size());
        for (size_t i = 0; i < shown; ++i) {
            model::ContainerElement row;
            row.name = Cat("PID 0x", HexFillLow(ordered[i].first, 4));
            row.type = "PID";
            row.depth = 2;
            const double pct = (total_packets > 0)
                                   ? 100.0 * static_cast<double>(ordered[i].second) /
                                         static_cast<double>(total_packets)
                                   : 0.0;
            const auto stat_it = pid_stats.find(ordered[i].first);
            const double mbytes =
                (stat_it != pid_stats.end())
                    ? static_cast<double>(stat_it->second.payload_bytes) / (1024.0 * 1024.0)
                    : 0.0;
            row.value = Cat(ordered[i].second, " 包 (", Fixed(pct, 1), "%) ", Fixed(mbytes, 2),
                            " MB 载荷");
            pid_elem.children.push_back(row);
        }
        root.children.push_back(pid_elem);
    }

    // PCR / 时序
    {
        bool any_pcr = false;
        model::ContainerElement pcr_elem;
        pcr_elem.name = "PCR / 时序";
        pcr_elem.type = "Timing";
        pcr_elem.depth = 1;
        for (const auto& kv : pid_stats) {
            if (kv.second.pcr_count == 0) continue;
            any_pcr = true;
            model::ContainerElement row;
            row.name = Cat("PCR PID 0x", HexFillLow(kv.first, 4));
            row.type = "PCR";
            row.depth = 2;
            row.value = Cat("采样 ", kv.second.pcr_count, " 次 | 首 ",
                               TimestampSeconds(kv.second.first_pcr_90k), " | 末 ",
                               TimestampSeconds(kv.second.last_pcr_90k), " | 最大间隔 ",
                               Fixed(static_cast<double>(kv.second.max_pcr_gap_27m) / 27000.0, 1),
                               "ms");
            pcr_elem.children.push_back(row);
            result.metadata["PCR PID"] = Cat("0x", HexFillLow(kv.first, 4));
            result.metadata["PCR 最大间隔(ms)"] =
                Fixed(static_cast<double>(kv.second.max_pcr_gap_27m) / 27000.0, 1);
            result.metadata["PCR 首末(90kHz)"] =
                Cat(kv.second.first_pcr_90k, " -> ", kv.second.last_pcr_90k);
        }
        if (any_pcr) root.children.push_back(pcr_elem);
    }

    // 传输层错误
    if (cc_errors > 0 || scrambled > 0 || sync_loss > 0) {
        model::ContainerElement err;
        err.name = "传输层错误";
        err.type = "Errors";
        err.depth = 1;
        err.value = Cat("CC 错=", cc_errors, " 加扰包=", scrambled, " 失步=", sync_loss);
        for (const auto& kv : pid_stats) {
            if (kv.second.cc_errors == 0 && kv.second.scrambled == 0) continue;
            model::ContainerElement row;
            row.name = Cat("PID 0x", HexFillLow(kv.first, 4));
            row.type = "Error";
            row.depth = 2;
            row.value = Cat("CC 错=", kv.second.cc_errors, " 加扰=", kv.second.scrambled);
            err.children.push_back(row);
        }
        root.children.push_back(err);
    }

    result.element_tree.push_back(root);
    result.valid = true;

    // ---- metadata ----
    result.metadata["包长"] = Cat(packet_size, " 字节");
    result.metadata["同步偏移"] = std::to_string(first_sync);
    result.metadata["已扫描包数"] = std::to_string(total_packets);
    result.metadata["估算总包数"] =
        std::to_string(packet_size > 0 ? (file_size / packet_size) : 0);
    result.metadata["扫描是否完整"] =
        (packet_limit > 0 && total_packets >= packet_limit) ? "否（已到扫描上限）" : "是";
    result.metadata["节目数"] = std::to_string(programs.size());
    result.metadata["空包"] = std::to_string(null_packets);
    if (!services.empty()) result.metadata["SDT 服务数"] = std::to_string(services.size());
    if (cc_errors > 0) result.metadata["连续计数错误"] = std::to_string(cc_errors);
    if (scrambled > 0) result.metadata["加扰包"] = std::to_string(scrambled);
    if (sync_loss > 0) result.metadata["失步重同步次数"] = std::to_string(sync_loss);

    result.summary = Cat("MPEG-TS | ", packet_size, "B 包 | 扫 ", total_packets, "/",
                            (packet_size > 0 ? file_size / packet_size : 0), " 包 | ",
                            programs.size(), " 节目 | ", result.streams.size(), " 流 | ",
                            total_pes, " PES", cc_errors > 0 ? Cat(" | CC 错 ", cc_errors)
                                                             : std::string(),
                            sync_loss > 0 ? Cat(" | 失步 ", sync_loss) : std::string());

    // ---- 传输层摘要（QC 规则消费）----
    //
    // 与上面那棵树共用同一组数字，只是换成一个扁平的值对象：规则层不该为了几个
    // 计数去理解 PSI 树的形状（见 TransportStreamInfo.h 的口径说明）。
    {
        model::TransportStreamInfo& ts = result.transport;
        ts.analyzed = true;
        ts.packet_size = packet_size;
        ts.scanned_packets = static_cast<int64_t>(total_packets);
        ts.estimated_packets = (packet_size > 0) ? (file_size / packet_size) : 0;
        ts.scan_complete = !(packet_limit > 0 && total_packets >= packet_limit);
        ts.programs = static_cast<int>(programs.size());
        int total_streams = 0;
        for (const auto& kv : programs) {
            total_streams += static_cast<int>(kv.second.streams.size());
        }
        ts.streams = total_streams;
        ts.services = static_cast<int>(services.size());
        ts.has_pat = saw_pat;
        ts.has_pmt = saw_pmt;
        ts.has_sdt = !services.empty();
        ts.sync_loss = static_cast<int64_t>(sync_loss);
        ts.continuity_errors = static_cast<int64_t>(cc_errors);
        ts.scrambled_packets = static_cast<int64_t>(scrambled);
        double pcr_gap_ms = 0.0;
        for (const auto& kv : pid_stats) {
            const double gap_ms = static_cast<double>(kv.second.max_pcr_gap_27m) / 27000.0;
            if (gap_ms > pcr_gap_ms) pcr_gap_ms = gap_ms;
        }
        ts.pcr_max_gap_ms = pcr_gap_ms;
    }

    file.Close();
    return true;
}

} // namespace videoeye
