#include "core/analysis/container/FlvStructureAnalyzer.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/BytesOrder.h"
#include "core/analysis/detail/SeqFileReader.h"
#include "infrastructure/concurrency/Cancellation.h"
#include <cstring>
#include <map>
#include <string>

namespace videoeye {

namespace {
// FLV 视频 CodecID → 名称
std::string flvVideoCodec(int id) {
    switch (id) {
    case 1:
        return "JPEG";
    case 2:
        return "Sorenson H.263";
    case 3:
        return "Screen Video";
    case 4:
        return "On2 VP6";
    case 5:
        return "On2 VP6 Alpha";
    case 6:
        return "Screen Video v2";
    case 7:
        return "H.264 (AVC)";
    case 12:
        return "H.265 (HEVC)";
    default:
        return StrCat("CodecID %1", id);
    }
}
// Enhanced-FLV (Enhanced-RTMP / spec-2020) 视频 fourcc → 名称。
// 视频头首字节 bit7=1 (isExVideoHeader) 时, 后跟 3 字节 fourcc,
// AV1/VP9/HEVC 均通过该通道承载, 传统的低 4 位 CodecID 已废弃。
std::string flvEnhancedVideoFourcc(const std::string& fcc) {
    if (fcc == "av01")
        return "AV1";
    if (fcc == "vp09")
        return "VP9";
    if (fcc == "hvc1" || fcc == "hev1")
        return "H.265 (HEVC)";
    if (fcc == "avc1")
        return "H.264 (AVC)";
    return StrCat("FourCC %1", (fcc));
}
// FLV 音频 SoundFormat → 名称
std::string flvAudioCodec(int fmt) {
    switch (fmt) {
    case 0:
        return "Linear PCM";
    case 1:
        return "ADPCM";
    case 2:
        return "MP3";
    case 3:
        return "PCM LE";
    case 4:
        return "Nellymoser 16kHz";
    case 5:
        return "Nellymoser 8kHz";
    case 6:
        return "Nellymoser";
    case 7:
        return "G.711 A-law";
    case 8:
        return "G.711 mu-law";
    case 10:
        return "AAC";
    case 11:
        return "Speex";
    case 14:
        return "MP3 8kHz";
    default:
        return StrCat("SoundFormat %1", fmt);
    }
}
int flvSoundRate(int r) {
    switch (r) {
    case 0:
        return 5500;
    case 1:
        return 11025;
    case 2:
        return 22050;
    case 3:
        return 44100;
    }
    return 0;
}

// 替代 file.Read(1).at(0)：读到 0 字节时 std::string::at(0) 会抛
// std::out_of_range —— 畸形 FLV（tag 头恰好落在文件尾、data_size 却 >= 1）
// 能把解析线程直接带走（阶段 3：解析器不允许崩溃）。读到才返回 true。
bool ReadOneByte(SeqFileReader& file, uint8_t& out) {
    const std::string b = file.Read(1);
    if (b.empty())
        return false;
    out = static_cast<uint8_t>(b[0]);
    return true;
}

// ---- 最小 AMF0 解析器：从 Script Tag(onMetadata) 提取 metadata 键值 ----
double readBeDouble(const uint8_t* p) {
    // 真 Qt 有 qFromBigEndian(const void*)，此处本可直接调用；
    // 这里逐字节拼大端，语义等价且避免对 uint8_t* 做 8 字节对齐假设。
    uint64_t be = 0;
    for (int i = 0; i < 8; ++i)
        be = (be << 8) | static_cast<uint64_t>(p[i]);
    double d;
    std::memcpy(&d, &be, 8);
    return d;
}
std::string amfNumToStr(double d) {
    if (d == static_cast<double>(static_cast<long long>(d)))
        return std::to_string(static_cast<long long>(d));
    return std::to_string(d);
}
// 解析一个 AMF0 值，pos 前进；仅把标量写入 out(key 非空时)
void parseAmf0Value(const std::string& b, int& pos, const std::string& key, std::map<std::string, std::string>& out,
                    int depth);
void parseAmf0Properties(const std::string& b, int& pos, std::map<std::string, std::string>& out, int depth) {
    while (pos + 2 <= b.size()) {
        uint16_t klen = LoadBE16(reinterpret_cast<const uint8_t*>(b.data() + pos));
        pos += 2;
        if (klen == 0) { // 可能是对象结束标记 00 00 09
            if (pos < b.size() && static_cast<uint8_t>(b[pos]) == 0x09)
                pos++;
            break;
        }
        if (pos + klen > b.size())
            break;
        std::string k = b.substr(pos, klen);
        pos += klen;
        parseAmf0Value(b, pos, k, out, depth);
    }
}
void parseAmf0Value(const std::string& b, int& pos, const std::string& key, std::map<std::string, std::string>& out,
                    int depth) {
    if (pos >= b.size() || depth > 6)
        return;
    uint8_t type = static_cast<uint8_t>(b[pos++]);
    switch (type) {
    case 0x00: { // Number
        if (pos + 8 > b.size()) {
            pos = b.size();
            return;
        }
        double d = readBeDouble(reinterpret_cast<const uint8_t*>(b.data() + pos));
        pos += 8;
        if (!key.empty())
            out[key] = amfNumToStr(d);
        break;
    }
    case 0x01: { // Boolean
        if (pos >= b.size())
            return;
        bool v = b[pos++] != 0;
        if (!key.empty())
            out[key] = v ? "true" : "false";
        break;
    }
    case 0x02: { // String
        if (pos + 2 > b.size()) {
            pos = b.size();
            return;
        }
        uint16_t sl = LoadBE16(reinterpret_cast<const uint8_t*>(b.data() + pos));
        pos += 2;
        if (pos + sl > b.size()) {
            pos = b.size();
            return;
        }
        std::string s = b.substr(pos, sl);
        pos += sl;
        if (!key.empty())
            out[key] = s;
        break;
    }
    case 0x03: // Object
        parseAmf0Properties(b, pos, out, depth + 1);
        break;
    case 0x08: { // ECMA Array (4字节计数 + 属性)
        if (pos + 4 > b.size()) {
            pos = b.size();
            return;
        }
        pos += 4; // 忽略计数，按属性对读到结束标记
        parseAmf0Properties(b, pos, out, depth + 1);
        break;
    }
    case 0x0A: { // Strict Array
        if (pos + 4 > b.size()) {
            pos = b.size();
            return;
        }
        uint32_t n = LoadBE32(reinterpret_cast<const uint8_t*>(b.data() + pos));
        pos += 4;
        for (uint32_t i = 0; i < n && pos < b.size(); ++i)
            parseAmf0Value(b, pos, std::string(), out, depth + 1);
        break;
    }
    case 0x0B: // Date: double + int16 tz
        pos += 10;
        break;
    case 0x05:
    case 0x06: // Null / Undefined
        break;
    case 0x09: // Object end
        break;
    default: // 未知类型，无法安全跳过
        pos = b.size();
        break;
    }
}
} // namespace

bool FlvStructureAnalyzer::Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                                   const std::atomic<bool>* cancel) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::FLV;
    result.format_name = "FLV";
    result.file_path = file_path;

    // FLV Header: "FLV" + version(1) + flags(1) + header_size(4)
    std::string header = file.Read(9);
    if (header.size() < 9 || header.substr(0, 3) != "FLV") {
        result.error_message = "不是有效的 FLV 文件";
        return false;
    }

    uint8_t version = static_cast<uint8_t>(header[3]);
    uint8_t flags = static_cast<uint8_t>(header[4]);
    uint32_t header_size = LoadBE32(reinterpret_cast<const unsigned char*>(header.data()) + 5);

    bool has_video = (flags & 0x01) != 0;
    bool has_audio = (flags & 0x04) != 0;

    // 创建根元素
    model::ContainerElement root;
    root.name = "FLV Header";
    root.type = "Header";
    root.size = header_size;
    root.offset = 0;
    root.depth = 0;
    root.value = StrCat("version=%1 video=%2 audio=%3", version, has_video, has_audio);

    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    if (has_video) {
        model::ContainerStreamInfo vs;
        vs.index = result.streams.size();
        vs.type = "video";
        vs.codec = "FLV Video";
        video_stream_idx = vs.index;
        result.streams.push_back(vs);
    }
    if (has_audio) {
        model::ContainerStreamInfo as;
        as.index = result.streams.size();
        as.type = "audio";
        as.codec = "FLV Audio";
        audio_stream_idx = as.index;
        result.streams.push_back(as);
    }

    // 跳过 PreviousTagSize0
    file.Seek(header_size);

    // 遍历 Tag 序列 (最多收集前 500 个作为结构概览)
    int tag_count = 0;
    int video_tags = 0, audio_tags = 0, script_tags = 0;
    bool video_codec_found = false, audio_codec_found = false, metadata_found = false;
    const int max_tags = 500;

    while (file.Pos() < file.Size() - 11 && tag_count < max_tags) {
        if (infrastructure::Checkpoint(cancel)) {
            result.error_message = "已取消";
            return false;
        }
        std::string tag_header = file.Read(11);
        if (tag_header.size() < 11)
            break;

        uint8_t tag_type = static_cast<uint8_t>(tag_header[0]);
        uint32_t data_size = (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[1])) << 16) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[2])) << 8) |
                             static_cast<uint32_t>(static_cast<uint8_t>(tag_header[3]));
        uint32_t timestamp = (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[4])) << 16) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[5])) << 8) |
                             static_cast<uint32_t>(static_cast<uint8_t>(tag_header[6]));
        uint8_t ts_ext = static_cast<uint8_t>(tag_header[7]);
        timestamp |= (static_cast<uint32_t>(ts_ext) << 24);

        int64_t data_start = file.Pos();

        model::ContainerElement tag;
        tag.offset = data_start - 11;
        tag.size = data_size + 11;
        tag.depth = 1;

        switch (tag_type) {
        case 8: {
            tag.name = "Audio Tag";
            tag.type = "Audio";
            tag.value = StrCat("ts=%1 size=%2", timestamp, data_size);
            audio_tags++;
            if (!audio_codec_found && data_size >= 1) {
                uint8_t sound = 0;
                if (!ReadOneByte(file, sound))
                    break;
                int fmt = (sound >> 4) & 0x0F;
                int rate = (sound >> 2) & 0x03;
                int size = (sound >> 1) & 0x01;
                int chan = sound & 0x01;
                std::string codec = flvAudioCodec(fmt);
                tag.value += StrCat(" | %1 %2Hz %3 %4bit", codec, flvSoundRate(rate), (chan ? "stereo" : "mono"),
                                    (size ? 16 : 8));
                if (audio_stream_idx >= 0) {
                    auto& s = result.streams[audio_stream_idx];
                    s.codec = codec;
                    s.details =
                        StrCat("%1 Hz, %2, %3-bit", flvSoundRate(rate), (chan ? "stereo" : "mono"), (size ? 16 : 8));
                }
                audio_codec_found = true;
            }
            break;
        }
        case 9: {
            tag.name = "Video Tag";
            tag.type = "Video";
            tag.value = StrCat("ts=%1 size=%2", timestamp, data_size);
            video_tags++;
            if (!video_codec_found && data_size >= 1) {
                uint8_t vh = 0;
                if (!ReadOneByte(file, vh))
                    break;
                std::string codec;
                if (vh & 0x80) {
                    // Enhanced-FLV: 首字节 bit7=1 (isExVideoHeader), 后跟 3 字节 fourcc
                    // (av01/vp09/hvc1...)。此处不能回退到低 4 位 CodecID——
                    // 增强头里低 4 位是 PacketType, 会被误判成完全无关的编码器。
                    if (data_size >= 4) {
                        codec = flvEnhancedVideoFourcc(file.Read(3));
                    } else {
                        codec = "Enhanced FLV (fourcc 缺失)";
                    }
                } else {
                    int codec_id = vh & 0x0F;
                    codec = flvVideoCodec(codec_id);
                }
                tag.value += StrCat(" | %1", codec);
                if (video_stream_idx >= 0)
                    result.streams[video_stream_idx].codec = codec;
                video_codec_found = true;
            }
            break;
        }
        case 18: {
            tag.name = "Script Tag";
            tag.type = "Script";
            tag.value = StrCat("ts=%1 size=%2", timestamp, data_size);
            script_tags++;
            if (!metadata_found && data_size > 0 && data_size < 1024 * 1024) {
                std::string sdata = file.Read(data_size);
                int pos = 0;
                // 第一个 AMF0 值通常是字符串 "onMetadata"
                std::map<std::string, std::string> ignore;
                parseAmf0Value(sdata, pos, std::string(), ignore, 0);
                // 第二个值为 metadata 对象/ECMA 数组
                std::map<std::string, std::string> meta;
                parseAmf0Value(sdata, pos, std::string(), meta, 0);
                for (auto it = meta.begin(); it != meta.end(); ++it) {
                    // meta 仍是 analysis 层内部的 std::map（Qt 不进 domain）；
                    // 落到 result.metadata 时统一转成 std::string。
                    const std::string k = it->first;
                    if (!result.metadata.count(k))
                        result.metadata[k] = it->second;
                }
                if (!meta.empty()) {
                    tag.value += " | onMetadata";
                    tag.extra = StrCat("%1 项元数据", meta.size());
                }
                metadata_found = true;
            }
            break;
        }
        default:
            tag.name = StrCat("Tag type=%1", tag_type);
            tag.type = "Unknown";
            tag.value = StrCat("ts=%1 size=%2", timestamp, data_size);
            break;
        }

        root.children.push_back(tag);

        // 跳过 tag data + PreviousTagSize (4 bytes)，始终基于 data_start 定位以避免上面读了部分数据
        file.Seek(data_start + data_size + 4);
        tag_count++;
    }

    result.element_tree.push_back(root);
    result.valid = true;
    std::string summary = "FLV | Video Tags: " + std::to_string(video_tags) +
                          " | Audio Tags: " + std::to_string(audio_tags) +
                          " | Script Tags: " + std::to_string(script_tags);
    if (result.metadata.count("width") && result.metadata.count("height"))
        summary += " | " + result.metadata["width"] + "x" + result.metadata["height"];
    if (result.metadata.count("duration"))
        summary += " | " + result.metadata["duration"] + "s";
    result.summary = summary;

    file.Close();
    return true;
}

} // namespace videoeye
