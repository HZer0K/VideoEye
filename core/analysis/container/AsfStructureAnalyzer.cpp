#include "infrastructure/concurrency/Cancellation.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/container/AsfStructureAnalyzer.h"
#include "core/analysis/detail/SeqFileReader.h"
#include <string>
#include <string>

namespace videoeye {

// ASF GUIDs (以文件中的字节序存储: 前3字段小端, 后8字节原序)
static const std::string kHeaderObjectGuid =
    HexToBytes("3026B2758E66CF11A6D900AA0062CE6C");
static const std::string kFilePropertiesGuid =
    HexToBytes("A1DCAB8C47A9CF118EE400C00C205365");
static const std::string kStreamPropertiesGuid =
    HexToBytes("9107DCB7B7A9CF118EE600C00C205365");
static const std::string kContentDescriptionGuid =
    HexToBytes("3326B2758E66CF11A6D900AA0062CE6C");
static const std::string kExtContentDescGuid =
    HexToBytes("40A4D0D207E3D21197F000A0C95EA850");
static const std::string kDataObjectGuid =
    HexToBytes("3626B2758E66CF11A6D900AA0062CE6C");
static const std::string kIndexObjectGuid =
    HexToBytes("90080033B1E5CF1189F400A0C90349CB");
static const std::string kVideoStreamGuid =
    HexToBytes("C0EF19BC4D5BCF11A8FD00805F5C442B");
static const std::string kAudioStreamGuid =
    HexToBytes("409E69F84D5BCF11A8FD00805F5C442B");

namespace {
uint16_t asfLE16(const std::string& d, int off) {
    if (off + 2 > d.size()) return 0;
    return static_cast<uint16_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(d[off + 1])) << 8);
}
uint32_t asfLE32(const std::string& d, int off) {
    if (off + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3])) << 24);
}
uint64_t asfLE64(const std::string& d, int off) {
    uint64_t v = 0;
    for (int i = 0; i < 8 && off + i < d.size(); ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(d[off + i])) << (i * 8);
    return v;
}
// UTF-16LE 定长字符串 (含结尾 NUL)
std::string asfUtf16(const std::string& d, int off, int bytes) {
    if (bytes <= 0 || off + bytes > d.size()) return std::string();
    // 逐字节拷出 UTF-16LE 的裸字节：跳过 char16_t* → wchar_t* 的隐式转换，
    // 也免得用逗号运算符把构造调用的实参列表吃掉。
    std::string s;
    s.reserve(static_cast<size_t>(bytes) / 2);
    for (int i = 0; i + 1 < bytes; i += 2) {
        s.push_back(static_cast<char>(static_cast<unsigned char>(d[off + i])));
        s.push_back(static_cast<char>(static_cast<unsigned char>(d[off + i + 1])));
    }
    return TrimCopy(RemoveAllCopy(s, '\0'));
}
std::string waveFormatName(uint16_t tag) {
    switch (tag) {
        case 0x0001: return "PCM";
        case 0x0002: return "ADPCM";
        case 0x0055: return "MP3";
        case 0x0161: return "WMA v2";
        case 0x0162: return "WMA Pro";
        case 0x0163: return "WMA Lossless";
        case 0x00FF: return "AAC";
        case 0x2000: return "AC-3";
        default: return HexFill(tag, 4);
    }
}
} // namespace

static std::string GuidToName(const std::string& guid) {
    if (guid == kHeaderObjectGuid) return "Header Object";
    if (guid == kFilePropertiesGuid) return "File Properties";
    if (guid == kStreamPropertiesGuid) return "Stream Properties";
    if (guid == kContentDescriptionGuid) return "Content Description";
    if (guid == kExtContentDescGuid) return "Extended Content Description";
    if (guid == kDataObjectGuid) return "Data Object";
    if (guid == kIndexObjectGuid) return "Index Object";
    return "Unknown Object";
}

bool AsfStructureAnalyzer::Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                                   const std::atomic<bool>* cancel) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::ASF;
    result.format_name = "ASF";
    result.file_path = file_path;

    // Read top-level Header Object
    std::string guid = file.Read(16);
    if (guid.size() < 16 || guid != kHeaderObjectGuid) {
        result.error_message = "不是有效的 ASF 文件";
        return false;
    }

    // Size (8 bytes, little-endian)
    std::string size_buf = file.Read(8);
    if (size_buf.size() < 8) return false;
    uint64_t header_size = asfLE64(size_buf, 0);

    // Number of header objects (4 bytes LE)
    std::string count_buf = file.Read(4);
    uint32_t num_objects = count_buf.size() >= 4 ? asfLE32(count_buf, 0) : 0;

    // Skip 2 reserved bytes
    file.Read(2);

    model::ContainerElement root;
    root.name = "ASF Header";
    root.type = "Header";
    root.size = header_size;
    root.offset = 0;
    root.depth = 0;
    root.value = StrCat("objects=%1", num_objects);

    // Parse child objects
    int64_t header_end = static_cast<int64_t>(header_size);
    if (header_end > file.Size()) header_end = file.Size();

    for (uint32_t i = 0; i < num_objects && file.Pos() < header_end - 24; ++i) {
        std::string obj_guid = file.Read(16);
        std::string obj_size_buf = file.Read(8);
        if (obj_guid.size() < 16 || obj_size_buf.size() < 8) break;

        uint64_t obj_size = asfLE64(obj_size_buf, 0);
        int64_t obj_start = file.Pos() - 24;
        std::string obj_name = GuidToName(obj_guid);

        model::ContainerElement elem;
        elem.name = obj_name;
        elem.type = "ASF Object";
        elem.size = obj_size;
        elem.offset = obj_start;
        elem.depth = 1;

        // Parse specific objects
        if (obj_guid == kFilePropertiesGuid) {
            std::string data = file.Read(std::min(static_cast<int64_t>(obj_size - 24), static_cast<int64_t>(104)));
            if (data.size() >= 64) {
                // Play Duration @40 (8, 100ns), Preroll @56 (8, ms)
                uint64_t play_100ns = asfLE64(data, 40);
                uint64_t preroll_ms = asfLE64(data, 56);
                double duration_sec = play_100ns / 10000000.0 - preroll_ms / 1000.0;
                if (duration_sec < 0) duration_sec = play_100ns / 10000000.0;
                elem.value = StrCat("duration=%1s", Fixed(duration_sec, 2));
                result.metadata["duration"] =
                    (Fixed(duration_sec, 2) + "s");
            }
        } else if (obj_guid == kStreamPropertiesGuid) {
            std::string data = file.Read(std::min(static_cast<int64_t>(obj_size - 24), static_cast<int64_t>(256)));
            if (data.size() >= 54) {
                std::string stream_type_guid = data.substr(0, 16);
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                if (stream_type_guid == kVideoStreamGuid) {
                    si.type = "video";
                    // 视频 type-specific 从 offset 54: width(4),height(4),flags(1),fmtsize(2),BITMAPINFOHEADER
                    uint32_t w = asfLE32(data, 54);
                    uint32_t h = asfLE32(data, 58);
                    // BITMAPINFOHEADER biCompression @ 54+11+16 = 81
                    std::string fourcc = TrimCopy(data.substr(81, 4));
                    si.codec = fourcc.empty() ? "Video" : fourcc;
                    si.details = StrCat("%1x%2", w, h);
                    elem.value = StrCat("Video %1x%2 %3", w, h, (si.codec));
                } else if (stream_type_guid == kAudioStreamGuid) {
                    si.type = "audio";
                    // WAVEFORMATEX 从 offset 54
                    uint16_t tag = asfLE16(data, 54);
                    uint16_t ch = asfLE16(data, 56);
                    uint32_t sr = asfLE32(data, 58);
                    uint16_t bits = asfLE16(data, 68);
                    si.codec = waveFormatName(tag);
                    si.details = StrCat("%1 Hz, %2 ch, %3-bit", sr, ch, bits);
                    elem.value = StrCat("Audio %1 %2Hz %3ch", (si.codec), sr, ch);
                } else {
                    si.type = "data";
                    si.codec = "ASF Data";
                    elem.value = "Data Stream";
                }
                result.streams.push_back(si);
            }
        } else if (obj_guid == kContentDescriptionGuid) {
            std::string data = file.Read(std::min(static_cast<int64_t>(obj_size - 24), static_cast<int64_t>(64 * 1024)));
            if (data.size() >= 10) {
                int tl = asfLE16(data, 0), al = asfLE16(data, 2), cl = asfLE16(data, 4);
                int dl = asfLE16(data, 6), rl = asfLE16(data, 8);
                int p = 10;
                std::string title = asfUtf16(data, p, tl); p += tl;
                std::string author = asfUtf16(data, p, al); p += al;
                std::string copyright = asfUtf16(data, p, cl); p += cl;
                std::string desc = asfUtf16(data, p, dl); p += dl;
                std::string rating = asfUtf16(data, p, rl); p += rl;
                if (!title.empty()) result.metadata["title"] = title;
                if (!author.empty()) result.metadata["author"] = author;
                if (!copyright.empty()) result.metadata["copyright"] = copyright;
                if (!desc.empty()) result.metadata["description"] = desc;
                if (!rating.empty()) result.metadata["rating"] = rating;
                elem.value = title.empty() ? "Metadata" : ("Title: " + title);
            } else {
                elem.value = "Metadata";
            }
        } else {
            elem.value = StrCat("size=%1", obj_size);
        }

        // Ensure we're at the right position for the next object
        int64_t next_pos = obj_start + static_cast<int64_t>(obj_size);
        if (obj_size >= 24 && next_pos <= file.Size()) {
            file.Seek(next_pos);
        } else {
            file.Seek(file.Size());
            root.children.push_back(elem);
            break;
        }

        root.children.push_back(elem);
    }

    // Scan for Data Object and Index Object after header
    file.Seek(header_end);
    while (file.Pos() < file.Size() - 24) {
        if (infrastructure::Checkpoint(cancel)) { result.error_message = "已取消"; return false; }
        std::string obj_guid = file.Read(16);
        std::string obj_size_buf = file.Read(8);
        if (obj_guid.size() < 16 || obj_size_buf.size() < 8) break;

        uint64_t obj_size = asfLE64(obj_size_buf, 0);

        model::ContainerElement elem;
        elem.name = GuidToName(obj_guid);
        elem.type = "ASF Object";
        elem.size = obj_size;
        elem.offset = file.Pos() - 24;
        elem.depth = 1;
        elem.value = StrCat("size=%1", obj_size);
        root.children.push_back(elem);

        if (obj_size < 24) break;
        file.Seek(file.Pos() - 24 + static_cast<int64_t>(obj_size));
    }

    result.element_tree.push_back(root);
    result.valid = true;
    result.summary = StrCat("ASF | 文件大小: %1 字节 | 流: %2 个", file.Size(), result.streams.size());

    file.Close();
    return true;
}

} // namespace videoeye
