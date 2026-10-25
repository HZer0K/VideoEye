#include "core/analysis/orchestration/FormatDetector.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/SeqFileReader.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {

namespace {

using model::ContainerFormat;

// ===========================================================================
// 扩展名表 —— 全项目唯一事实源
// ===========================================================================
//
// DetectByExtension() 直接查这张表，SupportedExtensions() / MediaExtensions() /
// ManifestExtensions() 也从它派生。新增格式只改这里，别在别处（尤其是 UI 的
// QFileDialog 过滤器）再抄一份 —— 曾经 UI 手写了只有 mp4/avi/mkv/flv/ts 的清单，
// 结果 mov / m2ts / wmv / ogg / mpg / vob / mxf 这些早就能解析的格式在对话框里
// 根本看不见，用户以为不支持。
//
// 顺带补齐了 matroska 家族的 mka / mk3d 与 mpegts 家族的 tts：QC 侧的扩展名
// 一致性表（QcRuleEngineCheckContainer 的 ExtensionAliasMap）早就把它们归到了
// matroska / mpegts，但解析层不认识 —— 同一个扩展名在两处得出不同结论。
struct ExtensionEntry {
    const char* ext; // 无点、小写
    ContainerFormat format;
};

const std::vector<ExtensionEntry>& ExtensionTable() {
    static const std::vector<ExtensionEntry> kTable = {
        // ISOBMFF 家族。f4v 同时能当 FLV 用，但实际内容是 fMP4，归 MP4 解析器。
        {"mp4", ContainerFormat::MP4},
        {"m4v", ContainerFormat::MP4},
        {"m4a", ContainerFormat::MP4},
        {"f4v", ContainerFormat::MP4},
        {"mov", ContainerFormat::MOV},
        // Matroska 家族（mka 是纯音频 MKV，结构完全一致）
        {"mkv", ContainerFormat::MKV},
        {"mka", ContainerFormat::MKV},
        {"mk3d", ContainerFormat::MKV},
        {"webm", ContainerFormat::WebM},
        {"avi", ContainerFormat::AVI},
        {"flv", ContainerFormat::FLV},
        // MPEG-TS：192 字节包长的 m2ts/mts 与 188 的标准 TS 共用分析器
        {"ts", ContainerFormat::MPEG_TS},
        {"mts", ContainerFormat::MPEG_TS},
        {"m2ts", ContainerFormat::MPEG_TS},
        {"tts", ContainerFormat::MPEG_TS},
        {"asf", ContainerFormat::ASF},
        {"wmv", ContainerFormat::ASF},
        {"wma", ContainerFormat::ASF},
        {"ogg", ContainerFormat::OGG},
        {"oga", ContainerFormat::OGG},
        {"ogv", ContainerFormat::OGG},
        {"opus", ContainerFormat::OGG}, // .opus 就是 Ogg 封装的 Opus
        // MPEG-PS：与 TS 同属 MPEG-2 System，但 pack header + PES 是另一套解析
        {"mpg", ContainerFormat::MPEG_PS},
        {"mpe", ContainerFormat::MPEG_PS},
        {"mpeg", ContainerFormat::MPEG_PS},
        {"m2p", ContainerFormat::MPEG_PS},
        {"vob", ContainerFormat::MPEG_PS},
        {"ps", ContainerFormat::MPEG_PS},
        {"m2v", ContainerFormat::MPEG_PS},
        {"mxf", ContainerFormat::MXF},
        // 裸流：没有容器层，结构页走 FFmpeg 通用分析
        {"h264", ContainerFormat::RAW_ES},
        {"264", ContainerFormat::RAW_ES},
        {"avc", ContainerFormat::RAW_ES},
        {"h265", ContainerFormat::RAW_ES},
        {"265", ContainerFormat::RAW_ES},
        {"hevc", ContainerFormat::RAW_ES},
        {"hvc", ContainerFormat::RAW_ES},
        {"vvc", ContainerFormat::RAW_ES},
        {"266", ContainerFormat::RAW_ES},
        {"h266", ContainerFormat::RAW_ES},
        {"av1", ContainerFormat::RAW_ES},
        {"obu", ContainerFormat::RAW_ES},
        // 流媒体清单：纯文本，魔数嗅探之外还能靠这些扩展名兜底
        {"m3u8", ContainerFormat::HLS},
        {"m3u", ContainerFormat::HLS},
        {"mpd", ContainerFormat::DASH},
    };
    return kTable;
}

// ---- 魔数嗅探辅助 ----

unsigned char ByteAt(const std::string& data, size_t index) {
    return static_cast<unsigned char>(data[index]);
}

// 跳过 UTF-8 BOM 与行首空白，返回第一个"内容"字符的下标。
size_t SkipTextPreamble(const std::string& text) {
    size_t i = 0;
    if (text.size() >= 3 && ByteAt(text, 0) == 0xEF && ByteAt(text, 1) == 0xBB && ByteAt(text, 2) == 0xBF) {
        i = 3;
    }
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            ++i;
        } else {
            break;
        }
    }
    return i;
}

// 文本清单嗅探的前置条件：读进来的这一段里不能有 NUL。
// 没有这道闸，"二进制文件里恰好出现 <MPD 四个字节"就会被判成 DASH。
bool LooksLikeText(const std::string& head) {
    return head.find('\0') == std::string::npos;
}

bool LooksLikeHlsManifest(const std::string& head) {
    const size_t start = SkipTextPreamble(head);
    // 规范要求首行就是 #EXTM3U（大小写敏感），允许前面有 BOM 与空行。
    return head.compare(start, 7, "#EXTM3U") == 0;
}

bool LooksLikeDashManifest(const std::string& head) {
    // MPD 是 XML，根元素前面可能有 <?xml ...?> / 注释 / DOCTYPE，因此不能要求
    // 首字符就是 '<'。只在前 1 KiB 里找根元素名；小写也收（个别工具输出小写，
    // 严格按 XML 是错的，但没必要为此拒识）。
    return head.find("<MPD") != std::string::npos || head.find("<mpd") != std::string::npos;
}

// 从 EBML 头部精确读出 DocType（元素 ID 0x4282）。
//
// 以前是在前 1 KiB 里裸搜子串 "webm" —— 一个 Matroska 文件只要标签或
// muxing_app 里出现 webm 四个字符（比如 "libwebm-0.2.1"），就会被判成 WebM。
// DocType 就在文件开头几十字节内，按元素读是免费的。
std::string ReadEbmlDocType(const std::string& head) {
    for (size_t i = 0; i + 3 < head.size(); ++i) {
        if (ByteAt(head, i) != 0x42 || ByteAt(head, i + 1) != 0x82) {
            continue;
        }
        // 紧跟一个 VINT 长度。DocType 的取值是 4~16 字节的短串，超过就别认了。
        const unsigned char length_byte = ByteAt(head, i + 2);
        int width = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((length_byte & (0x80 >> bit)) != 0) {
                width = bit + 1;
                break;
            }
        }
        if (width < 1 || width > 4 || i + 2 + static_cast<size_t>(width) > head.size()) {
            continue;
        }
        uint64_t length = static_cast<uint64_t>(length_byte & (0xFF >> width));
        for (int k = 1; k < width; ++k) {
            length = (length << 8) | ByteAt(head, i + 2 + static_cast<size_t>(k));
        }
        const size_t value_at = i + 2 + static_cast<size_t>(width);
        if (length == 0 || length > 16 || value_at + static_cast<size_t>(length) > head.size()) {
            continue;
        }
        std::string value = head.substr(value_at, static_cast<size_t>(length));
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (value == "webm" || value == "matroska") {
            return value;
        }
    }
    return std::string();
}

// 读大端 32 位（ISOBMFF box size）
uint32_t ReadU32Be(const std::string& data, size_t offset) {
    return (static_cast<uint32_t>(ByteAt(data, offset)) << 24) |
           (static_cast<uint32_t>(ByteAt(data, offset + 1)) << 16) |
           (static_cast<uint32_t>(ByteAt(data, offset + 2)) << 8) | static_cast<uint32_t>(ByteAt(data, offset + 3));
}

// ftyp 的 major_brand 与 compatible_brands 里有没有 QuickTime 标记。
// major_brand 在偏移 8，compatible_brands 从 16 开始、按 4 字节一项排到 box 末尾。
// 以前只看 major_brand，于是 major_brand=mp42 但 compatible 里带 "qt  " 的文件
// 会被判成 MP4；另外还留了一个 `brand == "mdat"` 的死分支（major_brand 不可能是
// mdat —— mdat 是普通 box 类型），一并清掉。
bool FtypDeclaresQuickTime(const std::string& head) {
    const uint32_t box_size = ReadU32Be(head, 0);
    // size 不合法（0=延伸到文件尾 / 1=64 位 largesize / 越界）时退回读入范围
    size_t limit = head.size();
    if (box_size >= 16 && static_cast<size_t>(box_size) <= head.size()) {
        limit = static_cast<size_t>(box_size);
    }
    for (size_t off = 16; off + 4 <= limit; off += 4) {
        if (head.compare(off, 4, "qt  ") == 0) {
            return true;
        }
    }
    return false;
}

// AV1 低开销码流（low-overhead bitstream format）以 temporal delimiter OBU 起头：
// OBU header 的 forbidden=0 / type=2 / extension=0 / has_size=1 / reserved=0 → 0x12，
// 而这层 OBU 的载荷长度必为 0，所以下一个字节是 leb128 的 0x00。再往后应当还能看到
// 一个合法 OBU 头（sequence header=1 ... padding=8），据此排除"两个随机字节"。
//
// 以前只认 Annex-B，README 声称支持的 AV1 裸流实际只能靠扩展名兜底：文件改名或
// 从管道读进来就落到 Generic 页。
bool LooksLikeAv1ObuStream(const std::string& head) {
    if (head.size() < 4 || ByteAt(head, 0) != 0x12 || ByteAt(head, 1) != 0x00) {
        return false;
    }
    const unsigned char next = ByteAt(head, 2);
    if ((next & 0x80) != 0) { // forbidden_zero 必须为 0
        return false;
    }
    const int obu_type = (next >> 3) & 0x0F;
    return obu_type >= 1 && obu_type <= 8; // 1=sequence header ... 8=padding
}

} // namespace

model::ContainerFormat FormatDetector::DetectByMagic(const std::string& file_path) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        return model::ContainerFormat::Unknown;
    }

    std::string header = file.Read(1024);
    file.Close();

    if (header.size() < 4) {
        return model::ContainerFormat::Unknown;
    }

    // ---------------------------------------------------------------------
    // 判定顺序：强魔数在前，弱魔数在后。
    //
    // TS 只靠单字节 0x47 的周期重复来认，本质是弱魔数；它会在前 1 KiB 的**所有**
    // offset 上试，候选位置约 564 个，误判概率 ~564 × (1/256)² ≈ 0.86%。以前它排在
    // OGG / ASF / PS / MXF 前面，一个 Ogg 或 WMV 文件有近 1% 的概率被 TS 抢走。
    // 所以：带多字节签名的格式一律先判，TS 留到最后。
    // ---------------------------------------------------------------------

    // MP4/MOV: offset 4-7 = "ftyp"
    if (header.size() >= 8 && header.compare(4, 4, "ftyp") == 0) {
        // 区分 MP4 vs MOV: major_brand 或 compatible_brands 里带 "qt  "
        const std::string major_brand = header.substr(8, 4);
        if (major_brand == "qt  " || FtypDeclaresQuickTime(header)) {
            return model::ContainerFormat::MOV;
        }
        return model::ContainerFormat::MP4;
    }

    // fMP4 媒体分片 (DASH/Smooth Streaming): 首盒为 styp，或直接以 moof 开头。
    // 属于 ISOBMFF 家族，交给 MP4 Box 解析器可直接展示 styp/moof/mdat 盒树；
    // 若按扩展名兜底 (如 .ts) 会误分发到 TS 分析器。
    if (header.size() >= 8) {
        const std::string first_box = header.substr(4, 4);
        if (first_box == "styp" || first_box == "moof") {
            return model::ContainerFormat::MP4;
        }
    }

    // EBML (MKV/WebM): \x1A\x45\xDF\xA3
    if (ByteAt(header, 0) == 0x1A && ByteAt(header, 1) == 0x45 && ByteAt(header, 2) == 0xDF &&
        ByteAt(header, 3) == 0xA3) {
        const std::string doc_type = ReadEbmlDocType(header);
        if (doc_type == "webm") {
            return model::ContainerFormat::WebM;
        }
        if (doc_type == "matroska") {
            return model::ContainerFormat::MKV;
        }
        // DocType 没读出来（截断/非常规头部）：保留旧的子串兜底，再退回 MKV。
        if (header.find("webm") != std::string::npos) {
            return model::ContainerFormat::WebM;
        }
        return model::ContainerFormat::MKV;
    }

    // AVI: "RIFF" + 4 bytes size + "AVI "
    if (header.size() >= 12 && header.compare(0, 4, "RIFF") == 0 && header.compare(8, 4, "AVI ") == 0) {
        return model::ContainerFormat::AVI;
    }

    // FLV: "FLV"
    if (header.compare(0, 3, "FLV") == 0) {
        return model::ContainerFormat::FLV;
    }

    // ASF: Header Object GUID = 30 26 B2 75 8E 66 CF 11
    if (header.size() >= 8 && ByteAt(header, 0) == 0x30 && ByteAt(header, 1) == 0x26 && ByteAt(header, 2) == 0xB2 &&
        ByteAt(header, 3) == 0x75 && ByteAt(header, 4) == 0x8E && ByteAt(header, 5) == 0x66 &&
        ByteAt(header, 6) == 0xCF && ByteAt(header, 7) == 0x11) {
        return model::ContainerFormat::ASF;
    }

    // OGG: "OggS"
    if (header.compare(0, 4, "OggS") == 0) {
        return model::ContainerFormat::OGG;
    }

    // MPEG-PS: 以 pack header 起始码 00 00 01 BA 开头（.mpg/.vob/.m2p）。
    // 必须在"裸流"判断之前：PS 里同样全是 00 00 01 起始码。
    if (ByteAt(header, 0) == 0x00 && ByteAt(header, 1) == 0x00 && ByteAt(header, 2) == 0x01 &&
        ByteAt(header, 3) == 0xBA) {
        return model::ContainerFormat::MPEG_PS;
    }

    // MXF: SMPTE 通用键前缀 06 0E 2B 34（UL 的前 4 字节）。
    if (ByteAt(header, 0) == 0x06 && ByteAt(header, 1) == 0x0E && ByteAt(header, 2) == 0x2B &&
        ByteAt(header, 3) == 0x34) {
        return model::ContainerFormat::MXF;
    }

    // 流媒体清单：纯文本，没有魔数可认，只能嗅探内容。
    // 以前只靠扩展名，于是无扩展名的 playlist、CDN 常见的 index / playlist / 带
    // token 的路径全部落到 Generic 页 —— HLS 21 条 + DASH 24 条规则一条都不触发。
    if (LooksLikeText(header)) {
        if (LooksLikeHlsManifest(header)) {
            return model::ContainerFormat::HLS;
        }
        if (LooksLikeDashManifest(header)) {
            return model::ContainerFormat::DASH;
        }
    }

    // 裸流（Annex-B）：00 00 00 01 / 00 00 01 起始码 + 合理的 H.264/HEVC NAL 头。
    // 只认"头 4 字节 + 后面还能再看到一个起始码"的组合，避免被随机 0 字节骗到。
    if (header.size() >= 8) {
        size_t nal = std::string::npos;
        if (header.compare(0, 4, std::string("\x00\x00\x00\x01", 4)) == 0) {
            nal = 4;
        } else if (header.compare(0, 3, std::string("\x00\x00\x01", 3)) == 0) {
            nal = 3;
        }
        if (nal < header.size()) {
            const unsigned char b = ByteAt(header, nal);
            const bool forbidden_zero = (b & 0x80) == 0;
            const bool h264_type = ((b & 0x1F) >= 1 && (b & 0x1F) <= 23);
            const bool hevc_type = (((b >> 1) & 0x3F) <= 40);
            // NAL 头之后还应能找到下一个起始码，否则更像一段随机零字节
            const size_t next = header.find(std::string("\x00\x00\x01", 3), nal + 1);
            if (forbidden_zero && (h264_type || hevc_type) && next != std::string::npos) {
                return model::ContainerFormat::RAW_ES;
            }
        }
    }

    // AV1 裸流（OBU 流），不受 Annex-B 起始码约束。
    if (LooksLikeAv1ObuStream(header)) {
        return model::ContainerFormat::RAW_ES;
    }

    // MPEG-TS: sync byte 0x47 repeats at a fixed stride.
    //
    // 三种包长都要认，不能只试 188：
    //   188 = 标准 TS
    //   192 = M2TS / AVCHD，前面多 4 字节 TP_extra_header（同步字节落在偏移 4）
    //   204 = RS(204,188) 纠错包（同步字节仍在 0）
    // 以前只试 188，于是 .m2ts 的魔数判定必然失败、只能靠扩展名兜底；而扩展名兜底
    // 又会把它分给按 188 步长解析的 TS 分析器，最后报"未找到同步字节"。
    //
    // 这是全函数里唯一的弱魔数（判据只是单字节重复），必须排在所有多字节签名之后。
    {
        static const int kPacketSizes[] = {188, 192, 204};
        for (const int ps : kPacketSizes) {
            const int max_offset = std::min<int>(ps, static_cast<int>(header.size()));
            for (int offset = 0; offset < max_offset; ++offset) {
                int matches = 0;
                bool ran_out_of_header = false;
                for (int k = 0; k < 3; ++k) {
                    const int at = offset + k * ps;
                    if (at >= static_cast<int>(header.size())) {
                        ran_out_of_header = true;
                        break;
                    }
                    if (ByteAt(header, static_cast<size_t>(at)) != 0x47)
                        break;
                    ++matches;
                }
                // 三个同步点是正常文件的判据。只有"文件短到装不下第三个包"时
                // 才用两个点收尾 —— 否则 (1/256)^2 的巧合率乘以 1700 个候选位置
                // 会让百分之几的普通文件被误判成 TS。
                if (matches >= 3 || (matches == 2 && ran_out_of_header)) {
                    return model::ContainerFormat::MPEG_TS;
                }
            }
        }
    }

    return model::ContainerFormat::Unknown;
}

model::ContainerFormat FormatDetector::DetectByExtension(const std::string& file_path) {
    const std::string ext = FileExtension(file_path);
    if (ext.empty()) {
        return model::ContainerFormat::Unknown;
    }
    for (const ExtensionEntry& entry : ExtensionTable()) {
        if (ext == entry.ext) {
            return entry.format;
        }
    }
    return model::ContainerFormat::Unknown;
}

model::ContainerFormat FormatDetector::Detect(const std::string& file_path) {
    // 优先用魔数检测
    auto fmt = DetectByMagic(file_path);
    if (fmt != model::ContainerFormat::Unknown) {
        return fmt;
    }
    // 回退到扩展名
    return DetectByExtension(file_path);
}

std::string FormatDetector::FormatName(model::ContainerFormat fmt) {
    switch (fmt) {
    case model::ContainerFormat::MP4:
        return "MP4";
    case model::ContainerFormat::MOV:
        return "MOV";
    case model::ContainerFormat::MKV:
        return "MKV";
    case model::ContainerFormat::WebM:
        return "WebM";
    case model::ContainerFormat::AVI:
        return "AVI";
    case model::ContainerFormat::FLV:
        return "FLV";
    case model::ContainerFormat::MPEG_TS:
        return "MPEG-TS";
    case model::ContainerFormat::ASF:
        return "ASF";
    case model::ContainerFormat::OGG:
        return "OGG";
    case model::ContainerFormat::MPEG_PS:
        return "MPEG-PS";
    case model::ContainerFormat::MXF:
        return "MXF";
    case model::ContainerFormat::RAW_ES:
        return "Raw ES";
    case model::ContainerFormat::HLS:
        return "HLS";
    case model::ContainerFormat::DASH:
        return "DASH";
    case model::ContainerFormat::FFmpeg_Generic:
        return "Generic";
    default:
        return "Unknown";
    }
}

std::string FormatDetector::FormatTitle(model::ContainerFormat fmt) {
    switch (fmt) {
    case model::ContainerFormat::MP4:
        return "MP4 Box 结构";
    case model::ContainerFormat::MOV:
        return "MOV (QuickTime) 结构";
    case model::ContainerFormat::MKV:
        return "MKV (EBML) 结构";
    case model::ContainerFormat::WebM:
        return "WebM (EBML) 结构";
    case model::ContainerFormat::AVI:
        return "AVI (RIFF) 结构";
    case model::ContainerFormat::FLV:
        return "FLV 结构";
    case model::ContainerFormat::MPEG_TS:
        return "MPEG-TS 结构";
    case model::ContainerFormat::ASF:
        return "ASF/WMV 结构";
    case model::ContainerFormat::OGG:
        return "OGG 结构";
    case model::ContainerFormat::MPEG_PS:
        return "MPEG-PS (Pack/PES) 结构";
    case model::ContainerFormat::MXF:
        return "MXF (KLV) 结构";
    case model::ContainerFormat::RAW_ES:
        return "裸流 (Annex-B) 结构";
    case model::ContainerFormat::HLS:
        return "HLS 清单结构";
    case model::ContainerFormat::DASH:
        return "DASH MPD 结构";
    case model::ContainerFormat::FFmpeg_Generic:
        return "FFmpeg 通用结构";
    default:
        return "文件结构";
    }
}

const std::vector<std::string>& FormatDetector::SupportedExtensions() {
    static const std::vector<std::string> kAll = [] {
        std::vector<std::string> out;
        for (const ExtensionEntry& entry : ExtensionTable()) {
            const std::string ext(entry.ext);
            if (std::find(out.begin(), out.end(), ext) == out.end()) {
                out.push_back(ext);
            }
        }
        return out;
    }();
    return kAll;
}

const std::vector<std::string>& FormatDetector::MediaExtensions() {
    static const std::vector<std::string> kMedia = [] {
        std::vector<std::string> out;
        for (const ExtensionEntry& entry : ExtensionTable()) {
            // 清单文本不是媒体容器：混进"媒体文件"过滤器会让用户以为 .m3u8 是素材
            if (entry.format == model::ContainerFormat::HLS || entry.format == model::ContainerFormat::DASH) {
                continue;
            }
            const std::string ext(entry.ext);
            if (std::find(out.begin(), out.end(), ext) == out.end()) {
                out.push_back(ext);
            }
        }
        return out;
    }();
    return kMedia;
}

const std::vector<std::string>& FormatDetector::ManifestExtensions() {
    static const std::vector<std::string> kManifest = [] {
        std::vector<std::string> out;
        for (const ExtensionEntry& entry : ExtensionTable()) {
            if (entry.format != model::ContainerFormat::HLS && entry.format != model::ContainerFormat::DASH) {
                continue;
            }
            const std::string ext(entry.ext);
            if (std::find(out.begin(), out.end(), ext) == out.end()) {
                out.push_back(ext);
            }
        }
        return out;
    }();
    return kManifest;
}

} // namespace videoeye
