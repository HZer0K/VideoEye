#include "EbmlAnalyzer.h"
#include "utils/Logger.h"
#include <QFile>
#include <QDataStream>
#include <QStack>
#include <QSet>
#include <QBuffer>

namespace videoeye {
namespace analyzer {

// ============================================================
// 已知 EBML 元素名称映射 (Matroska / WebM)
// ============================================================
QMap<uint64_t, QString>& EbmlAnalyzer::ElementNames() {
    static QMap<uint64_t, QString> map;
    if (map.isEmpty()) {
        // EBML Header
        map[0x1A45DFA3] = "EBML";
        map[0x4286]    = "EBMLVersion";
        map[0x42F7]    = "EBMLReadVersion";
        map[0x42F2]    = "EBMLMaxIDLength";
        map[0x42F3]    = "EBMLMaxSizeLength";
        map[0x4282]    = "DocType";
        map[0x4287]    = "DocTypeVersion";
        map[0x4285]    = "DocTypeReadVersion";
        map[0x42BF]    = "CRC-32";
        map[0xEC]      = "Void";
        // Segment
        map[0x18538067] = "Segment";
        // SeekHead
        map[0x114D9B74] = "SeekHead";
        map[0x4DBB]     = "Seek";
        map[0x53AB]     = "SeekID";
        map[0x53AC]     = "SeekPosition";
        // Info
        map[0x1549A966] = "Info";
        map[0x2AD7B1]   = "TimestampScale";
        map[0x4489]     = "Duration";
        map[0x4D80]     = "MuxingApp";
        map[0x5741]     = "WritingApp";
        map[0x7384]     = "SegmentFilename";
        map[0x73A4]     = "SegmentUID";
        map[0x7BA9]     = "Title";
        map[0x3CB923]   = "PrevUID";
        map[0x3C83AB]   = "NextUID";
        map[0x4461]     = "DateUTC";
        // Cluster
        map[0x1F43B675] = "Cluster";
        map[0xE7]       = "Timecode";
        map[0xA7]       = "Position";
        map[0xAB]       = "PrevSize";
        map[0xA0]       = "BlockGroup";
        map[0xA1]       = "Block";
        map[0xA3]       = "SimpleBlock";
        map[0xA2]       = "BlockVirtual";
        map[0x9B]       = "BlockDuration";
        map[0xFA]       = "ReferenceBlock";
        map[0xFB]       = "ReferencePriority";
        map[0xFD]       = "ReferenceVirtual";
        map[0x75A1]     = "BlockAdditions";
        map[0x75A2]     = "DiscardPadding";
        map[0x41A4]     = "CodecState";
        map[0xCB]       = "BlockAdditionID";
        map[0xC8]       = "BlockAdditionMapping";
        map[0x41E4]     = "BlockMore";
        map[0xC9]       = "BlockAdditional";
        map[0x41E7]     = "BlockAddID";
        // Tracks
        map[0x1654AE6B] = "Tracks";
        map[0xAE]       = "TrackEntry";
        map[0xD7]       = "TrackNumber";
        map[0x73C5]     = "TrackUID";
        map[0x83]       = "TrackType";
        map[0xB9]       = "FlagEnabled";
        map[0x88]       = "FlagDefault";
        map[0x55AA]     = "FlagForced";
        map[0x9C]       = "FlagLacing";
        map[0x6DE7]     = "MinCache";
        map[0x6DF8]     = "MaxCache";
        map[0x23E383]   = "DefaultDuration";
        map[0x23314F]   = "DefaultDecodedFieldDuration";
        map[0x536E]     = "Name";
        map[0x22B59C]   = "Language";           // deprecated, use IETF
        map[0x7D7B]     = "LanguageIETF";
        map[0x86]       = "CodecID";
        map[0x63A2]     = "CodecPrivate";
        map[0x258688]   = "CodecName";
        map[0x7446]     = "AttachmentLink";
        map[0x3A9697]   = "CodecSettings";
        map[0x3B4040]   = "CodecInfoURL";
        map[0x26B240]   = "CodecDownloadURL";
        map[0xAA]       = "CodecDecodeAll";
        map[0x6FAB]     = "TrackOverlay";
        map[0x56AA]     = "CodecDelay";
        map[0x56BB]     = "SeekPreRoll";
        // Video
        map[0xE0]       = "Video";
        map[0x9A]       = "FlagInterlaced";
        map[0x9D]       = "FieldOrder";
        map[0x53B8]     = "StereoMode";
        map[0x53C0]     = "AlphaMode";
        map[0xB0]       = "PixelWidth";
        map[0xBA]       = "PixelHeight";
        map[0x54AA]     = "PixelCropBottom";
        map[0x54BB]     = "PixelCropTop";
        map[0x54CC]     = "PixelCropLeft";
        map[0x54DD]     = "PixelCropRight";
        map[0x54B0]     = "DisplayWidth";
        map[0x54BA]     = "DisplayHeight";
        map[0x54B2]     = "DisplayUnit";
        map[0x54B3]     = "AspectRatioType";
        map[0x2EB524]   = "UncompressedFourCC";
        map[0x2FB523]   = "GammaValue";
        map[0x2383E3]   = "FrameRate";
        map[0x55B0]     = "Colour";
        map[0x55B1]     = "MatrixCoefficients";
        map[0x55B2]     = "BitsPerChannel";
        map[0x55B3]     = "ChromaSubsamplingHorz";
        map[0x55B4]     = "ChromaSubsamplingVert";
        map[0x55B5]     = "CbSubsamplingHorz";
        map[0x55B6]     = "CbSubsamplingVert";
        map[0x55B7]     = "ChromaSitingHorz";
        map[0x55B8]     = "ChromaSitingVert";
        map[0x55B9]     = "Range";
        map[0x55BA]     = "TransferCharacteristics";
        map[0x55BB]     = "Primaries";
        map[0x55BC]     = "MaxCLL";
        map[0x55BD]     = "MaxFALL";
        map[0x55D0]     = "MasteringMetadata";
        map[0x55D1]     = "PrimaryRChromaticityX";
        map[0x55D2]     = "PrimaryRChromaticityY";
        map[0x55D3]     = "PrimaryGChromaticityX";
        map[0x55D4]     = "PrimaryGChromaticityY";
        map[0x55D5]     = "PrimaryBChromaticityX";
        map[0x55D6]     = "PrimaryBChromaticityY";
        map[0x55D7]     = "WhitePointChromaticityX";
        map[0x55D8]     = "WhitePointChromaticityY";
        map[0x55D9]     = "LuminanceMax";
        map[0x55DA]     = "LuminanceMin";
        // Audio
        map[0xE1]       = "Audio";
        map[0xB5]       = "SamplingFrequency";
        map[0x78B5]     = "OutputSamplingFrequency";
        map[0x9F]       = "Channels";
        map[0x7D7B]     = "ChannelPositions";
        map[0x6264]     = "BitDepth";
        map[0x52F1]     = "Emphasis";
        // ContentEncodings
        map[0x6D80]     = "ContentEncodings";
        map[0x6240]     = "ContentEncoding";
        map[0x5031]     = "ContentEncodingOrder";
        map[0x5032]     = "ContentEncodingScope";
        map[0x5033]     = "ContentEncodingType";
        map[0x5034]     = "ContentCompression";
        map[0x5035]     = "ContentCompAlgo";
        // Cues
        map[0x1C53BB6B] = "Cues";
        map[0xBB]       = "CuePoint";
        map[0xB3]       = "CueTime";
        map[0xB7]       = "CueTrackPositions";
        map[0xF7]       = "CueTrack";
        map[0xF1]       = "CueClusterPosition";
        map[0x5387]     = "CueRelativePosition";
        map[0xF0]       = "CueRelativePosition";
        map[0x5378]     = "CueBlockNumber";
        map[0xEA]       = "CueCodecState";
        // Chapters
        map[0x1043A770] = "Chapters";
        map[0x45B9]     = "EditionEntry";
        map[0x45BC]     = "EditionUID";
        map[0x45BD]     = "EditionFlagHidden";
        map[0x45DB]     = "EditionFlagDefault";
        map[0x45DD]     = "EditionFlagOrdered";
        map[0xB6]       = "ChapterAtom";
        map[0x73C4]     = "ChapterUID";
        map[0x6E67]     = "ChapterStringUID";
        map[0x91]       = "ChapterTimeStart";
        map[0x92]       = "ChapterTimeEnd";
        map[0x98]       = "ChapterFlagHidden";
        map[0x4598]     = "ChapterFlagEnabled";
        map[0x6EBC]     = "ChapterSegmentUID";
        map[0x6E80]     = "ChapterSegmentEditionUID";
        map[0x8F]       = "ChapterTrack";
        map[0x89]       = "ChapterTrackUID";
        map[0x63C3]     = "ChapterDisplay";
        map[0x437C]     = "ChapLanguage";
        map[0x437E]     = "ChapLanguageIETF";
        map[0x437D]     = "ChapCountry";
        map[0x80]       = "ChapProcess";
        map[0x6944]     = "ChapProcessCommand";
        map[0x6911]     = "ChapProcessTime";
        map[0x6922]     = "ChapProcessData";
        // Tags
        map[0x1254C367] = "Tags";
        map[0x7373]     = "Tag";
        map[0x63C0]     = "Targets";
        map[0x68CA]     = "TargetTypeValue";
        map[0x63CA]     = "TargetType";
        map[0x63C5]     = "TagTrackUID";
        map[0x63C9]     = "TagEditionUID";
        map[0x63C4]     = "TagChapterUID";
        map[0x63C6]     = "TagAttachmentUID";
        map[0x67C8]     = "SimpleTag";
        map[0x45A3]     = "TagName";
        map[0x447A]     = "TagLanguage";
        map[0x447B]     = "TagLanguageIETF";
        map[0x4484]     = "TagDefault";
        map[0x4487]     = "TagString";
        map[0x4485]     = "TagBinary";
        // Attachments
        map[0x1941A469] = "Attachments";
        map[0x61A7]     = "AttachedFile";
        map[0x467E]     = "FileDescription";
        map[0x466E]     = "FileName";
        map[0x4660]     = "FileMimeType";
        map[0x465C]     = "FileData";
        map[0x46AE]     = "FileUID";
    }
    return map;
}

// ============================================================
// 容器元素判定
// ============================================================
bool EbmlAnalyzer::IsContainerElement(uint64_t id) {
    switch (id) {
        case 0x1A45DFA3: // EBML
        case 0x18538067: // Segment
        case 0x114D9B74: // SeekHead
        case 0x4DBB:     // Seek
        case 0x1549A966: // Info
        case 0x1F43B675: // Cluster
        case 0xA0:       // BlockGroup
        case 0x75A1:     // BlockAdditions
        case 0x41E4:     // BlockMore
        case 0x1654AE6B: // Tracks
        case 0xAE:       // TrackEntry
        case 0xE0:       // Video
        case 0xE1:       // Audio
        case 0x55B0:     // Colour
        case 0x55D0:     // MasteringMetadata
        case 0x6D80:     // ContentEncodings
        case 0x6240:     // ContentEncoding
        case 0x5034:     // ContentCompression
        case 0x1C53BB6B: // Cues
        case 0xBB:       // CuePoint
        case 0xB7:       // CueTrackPositions
        case 0x1043A770: // Chapters
        case 0x45B9:     // EditionEntry
        case 0xB6:       // ChapterAtom
        case 0x63C3:     // ChapterDisplay
        case 0x8F:       // ChapterTrack
        case 0x80:       // ChapProcess
        case 0x6944:     // ChapProcessCommand
        case 0x1254C367: // Tags
        case 0x7373:     // Tag
        case 0x63C0:     // Targets
        case 0x67C8:     // SimpleTag
        case 0x1941A469: // Attachments
        case 0x61A7:     // AttachedFile
            return true;
        default:
            return false;
    }
}

// ============================================================
// CodecID 可读名称
// ============================================================
QString EbmlAnalyzer::CodecIdToName(const QString& codec_id) {
    if (codec_id == "V_VP8")            return "VP8";
    if (codec_id == "V_VP9")            return "VP9";
    if (codec_id == "V_AV1")            return "AV1";
    if (codec_id == "V_MPEG4/ISO/AVC")  return "H.264 (AVC)";
    if (codec_id == "V_MPEGH/ISO/HEVC") return "H.265 (HEVC)";
    if (codec_id == "V_MPEGI/ISO/VVC")  return "H.266 (VVC)";
    if (codec_id == "V_THEORA")         return "Theora";
    if (codec_id == "V_MS/VFW/FOURCC")  return "MS VFW (FOURCC)";
    if (codec_id == "V_UNCOMPRESSED")    return "Uncompressed";
    if (codec_id == "A_OPUS")           return "Opus";
    if (codec_id == "A_VORBIS")         return "Vorbis";
    if (codec_id == "A_AAC")            return "AAC";
    if (codec_id == "A_MPEG/L3")        return "MP3";
    if (codec_id == "A_MPEG/L2")        return "MP2";
    if (codec_id == "A_MPEG/L1")        return "MP1";
    if (codec_id == "A_PCM/INT/LIT")    return "PCM (int)";
    if (codec_id == "A_PCM/FLOAT/IEEE") return "PCM (float)";
    if (codec_id == "A_AC3")            return "AC-3";
    if (codec_id == "A_EAC3")           return "E-AC-3";
    if (codec_id == "A_TRUEHD")         return "TrueHD";
    if (codec_id == "A_DTS")            return "DTS";
    if (codec_id == "A_FLAC")           return "FLAC";
    if (codec_id == "A_MLP")            return "MLP";
    if (codec_id == "S_TEXT/UTF8")      return "SubRip/SRT";
    if (codec_id == "S_TEXT/ASS")       return "ASS/SSA";
    if (codec_id == "S_VOBSUB")         return "VobSub";
    if (codec_id == "S_HDMV/PGS")       return "PGS";
    if (codec_id == "S_DVBSUB")         return "DVB Subtitle";
    if (codec_id == "S_KATE")           return "Kate";
    if (codec_id == "S_TEXT/WEBVTT")    return "WebVTT";
    return codec_id;
}

QString EbmlAnalyzer::TrackTypeName(int type) {
    switch (type) {
        case 1:  return QString("视频 (Video)");
        case 2:  return QString("音频 (Audio)");
        case 3:  return QString("复合 (Complex)");
        case 0x10: return QString("Logo");
        case 0x11: return QString("字幕 (Subtitle)");
        case 0x12: return QString("按钮 (Buttons)");
        case 0x20: return QString("控制 (Control)");
        default:  return QString("类型%1").arg(type);
    }
}

QString EbmlAnalyzer::ElementName(uint64_t id) {
    auto& names = ElementNames();
    auto it = names.find(id);
    if (it != names.end()) return it.value();
    if (id <= 0xFF) return QString("0x%1").arg(id, 2, 16, QChar('0'));
    if (id <= 0xFFFF) return QString("0x%1").arg(id, 4, 16, QChar('0'));
    return QString("0x%1").arg(id, 0, 16);
}

// ============================================================
// 二进制 IO 辅助
// ============================================================
static uint64_t readBeUInt(const QByteArray& data, int size) {
    uint64_t val = 0;
    for (int i = 0; i < size && i < data.size(); ++i)
        val = (val << 8) | static_cast<uint8_t>(data[i]);
    return val;
}

static double readBeFloat(const QByteArray& data, int size) {
    if (size == 4 && data.size() >= 4) {
        union { uint32_t u; float f; } uf;
        uf.u = static_cast<uint32_t>(readBeUInt(data, 4));
        return uf.f;
    }
    if (size == 8 && data.size() >= 8) {
        union { uint64_t u; double d; } ud;
        ud.u = readBeUInt(data, 8);
        return ud.d;
    }
    return 0.0;
}

// 从 QByteArray 流式读取 VINT
static uint64_t readVIntFromBytes(const QByteArray& data, int& offset, int& size_out) {
    if (offset >= data.size()) { size_out = 0; return 0; }
    uint8_t first = static_cast<uint8_t>(data[offset++]);
    int width = 0;
    uint8_t mask = 0x80;
    while (mask && !(first & mask)) { width++; mask >>= 1; }
    width++;
    size_out = width;
    uint64_t value = first & (0xFF >> width);
    for (int i = 1; i < width; ++i) {
        if (offset >= data.size()) return 0;
        value = (value << 8) | static_cast<uint8_t>(data[offset++]);
    }
    return value;
}

// ============================================================
// VINT 读取 (QDataStream 版) — 用于 Size 字段，去除标记位
// ============================================================
uint64_t EbmlAnalyzer::ReadVInt(QDataStream& ds, int& size_out) const {
    uint8_t first;
    ds >> first;
    if (ds.status() != QDataStream::Ok) { size_out = 0; return 0; }
    int width = 0;
    uint8_t mask = 0x80;
    while (mask && !(first & mask)) { width++; mask >>= 1; }
    width++;
    size_out = width;
    uint64_t value = first & (0xFF >> width);
    for (int i = 1; i < width; ++i) {
        uint8_t b;
        ds >> b;
        if (ds.status() != QDataStream::Ok) return 0;
        value = (value << 8) | b;
    }
    return value;
}

// ============================================================
// EBML ID 读取 — 保留标记位，ID 的原始字节值
// e.g. 0x1A45DFA3 保留为 0x1A45DFA3，不被 VINT 解码为 0x0A45DFA3
// ============================================================
static uint64_t readIdVInt(const QByteArray& data, int& offset, int& size_out) {
    if (offset >= data.size()) { size_out = 0; return 0; }
    uint8_t first = static_cast<uint8_t>(data[offset]);
    int width = 0;
    uint8_t mask = 0x80;
    while (mask && !(first & mask)) { width++; mask >>= 1; }
    width++;
    size_out = width;
    // ID 保留全部原始字节，不去除标记位
    uint64_t value = first;
    for (int i = 1; i < width; ++i) {
        if (offset + i >= data.size()) return 0;
        value = (value << 8) | static_cast<uint8_t>(data[offset + i]);
    }
    offset += width;
    return value;
}

// 从 QDataStream 读取 ID
static uint64_t readIdVInt(QDataStream& ds, int& size_out) {
    uint8_t first;
    ds >> first;
    if (ds.status() != QDataStream::Ok) { size_out = 0; return 0; }
    int width = 0;
    uint8_t mask = 0x80;
    while (mask && !(first & mask)) { width++; mask >>= 1; }
    width++;
    size_out = width;
    // ID 保留全部原始字节
    uint64_t value = first;
    for (int i = 1; i < width; ++i) {
        uint8_t b;
        ds >> b;
        if (ds.status() != QDataStream::Ok) return 0;
        value = (value << 8) | b;
    }
    return value;
}

// ============================================================
// Block 解析 (BlockGroup 的子元素 Block)
// Block 格式: TrackNumber(VINT) Timecode(int16) Flags(u8) [Lacing data]
// ============================================================
QString EbmlAnalyzer::ParseBlockData(const QByteArray& data, model::EbmlBlockSummary& summary) {
    if (data.size() < 3) return "数据过短";

    int off = 0;
    int tn_size = 0;
    summary.track_number = static_cast<int>(readVIntFromBytes(data, off, tn_size));
    if (tn_size == 0 || off + 3 > data.size()) return "解析失败";

    // Timecode int16 (signed)
    int16_t tc;
    {
        uint16_t raw = static_cast<uint16_t>(readBeUInt(data.mid(off, 2), 2));
        memcpy(&tc, &raw, 2);
    }
    off += 2;
    summary.timecode = tc;

    // Flags
    uint8_t flags = static_cast<uint8_t>(data[off++]);
    summary.keyframe = !(flags & 0x80);   // bit7=0 表示关键帧
    summary.discardable = (flags & 0x01);
    summary.lacing = (flags & 0x06) != 0; // bit2-1: 00=无 lacing, 01=Xiph, 11=EBML, 10=fixed

    summary.data_size = data.size() - off;

    QString lacetype;
    if (flags & 0x06) {
        int lt = (flags >> 1) & 0x03;
        lacetype = (lt == 1) ? " Xiph-lacing" : (lt == 2) ? " fixed-lacing" : " EBML-lacing";
    }

    return QString("Track=%1 Timecode=%2 Flags=0x%3%4%5%6 [%7 bytes]")
        .arg(summary.track_number)
        .arg(summary.timecode)
        .arg(flags, 2, 16, QChar('0'))
        .arg(summary.keyframe ? " KEY" : "")
        .arg(summary.discardable ? " DISCARD" : "")
        .arg(lacetype)
        .arg(summary.data_size);
}

// ============================================================
// SimpleBlock 解析 (Cluster 直接子元素)
// SimpleBlock 数据的解析与 Block 相同 (都包含 TrackNumber+Timecode+Flags 头部)
// ============================================================
QString EbmlAnalyzer::ParseSimpleBlockData(const QByteArray& data, model::EbmlBlockSummary& summary) {
    return "S-" + ParseBlockData(data, summary);
}

// ============================================================
// 叶子元素值解析 + 提取关键数据
// ============================================================
void EbmlAnalyzer::ParseLeafValue(model::EbmlElementNode& node,
                                   const QByteArray& data,
                                   model::EbmlAnalysisResult& result) {
    if (data.isEmpty()) return;

    auto tryString = [&]() -> QString {
        QString s = QString::fromUtf8(data);
        if (s.isEmpty()) return {};
        for (int i = 0; i < s.size(); ++i) {
            ushort ch = s[i].unicode();
            if (ch == 0xFFFD) return {};
            if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') return {};
        }
        return s;
    };

    // --- 根据元素 ID 提取数值 + 关键数据 ---
    switch (node.id) {
        // 1-byte unsigned
        case 0x4286: { uint8_t v = readBeUInt(data, 1); node.value = QString::number(v); result.ebml_version = v; return; }
        case 0x42F7: { uint8_t v = readBeUInt(data, 1); node.value = QString::number(v); result.ebml_read_version = v; return; }
        case 0x42F2: { uint8_t v = readBeUInt(data, 1); node.value = QString::number(v); result.ebml_max_id_length = v; return; }
        case 0x42F3: { uint8_t v = readBeUInt(data, 1); node.value = QString::number(v); result.ebml_max_size_length = v; return; }
        case 0x4287: { uint8_t v = readBeUInt(data, 1); node.value = QString::number(v); result.doc_type_version = v; return; }
        case 0x4285: { uint8_t v = readBeUInt(data, 1); node.value = QString::number(v); result.doc_type_read_version = v; return; }
        case 0x83: { uint8_t v = readBeUInt(data, 1); node.value = TrackTypeName(v); return; }
        case 0xB9: case 0x88: case 0x55AA: case 0x9C: case 0xAA: case 0x9A: case 0x9D:
        case 0x53B8: case 0x53C0: case 0x54B2: case 0x54B3:
        case 0x98: case 0x4598: case 0x4484: case 0x68CA:
            node.value = QString::number(static_cast<uint8_t>(readBeUInt(data, 1))); return;

        // 2-byte
        case 0x6DE7: case 0x6DF8:
            node.value = QString::number(static_cast<uint16_t>(readBeUInt(data, 2))); return;

        // 4-byte
        case 0xD7: case 0x73C5: node.value = QString::number(static_cast<uint32_t>(readBeUInt(data, 4))); return;

        // 8-byte
        case 0x23E383: case 0x23314F:
            { node.value = QString::number(readBeUInt(data, 8)); return; }

        // float (4 byte)
        case 0xB5: case 0x78B5: case 0x2FB523:
            node.value = QString::number(readBeFloat(data, 4), 'f', 6); return;

        // 字符串
        case 0x4282: { node.value = tryString(); result.doc_type = node.value; return; }

        // TimestampScale
        case 0x2AD7B1: {
            node.value = QString::number(readBeUInt(data, 8));
            result.timestamp_scale = readBeUInt(data, 8);
            return;
        }
        // Duration (float)
        case 0x4489: {
            double dur = readBeFloat(data, 4);
            if (result.timestamp_scale > 0)
                result.duration_seconds = dur * result.timestamp_scale / 1e9;
            node.value = QString::number(dur, 'f', 6);
            if (result.duration_seconds > 0) {
                int h = static_cast<int>(result.duration_seconds / 3600);
                int m = static_cast<int>(result.duration_seconds) % 3600 / 60;
                int s = static_cast<int>(result.duration_seconds) % 60;
                node.value += QString(" (%1:%2:%3)").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
            }
            return;
        }
        // Timecode (Cluster)
        case 0xE7: case 0xA7: case 0xAB: case 0xF1:
            node.value = QString::number(readBeUInt(data, 8)); return;

        // pixel dims (变长)
        case 0xB0: case 0xBA:
        case 0x54AA: case 0x54BB: case 0x54CC: case 0x54DD:
        case 0x54B0: case 0x54BA:
            node.value = QString::number(readBeUInt(data, data.size())); return;

        // 字符串
        case 0x4D80: case 0x5741: case 0x7384: case 0x7BA9:
        case 0x86: case 0x258688:
        case 0x22B59C: case 0x466E: case 0x4660: case 0x467E:
        case 0x447A: case 0x45A3: case 0x447B:
        case 0x63CA: case 0x437C:
            { node.value = tryString(); 
              if (node.id == 0x4D80) result.muxing_app = node.value;
              else if (node.id == 0x5741) result.writing_app = node.value;
              else if (node.id == 0x7BA9) result.title = node.value;
              return; }

        // SegmentUID / PrevUID / NextUID (16 bytes binary)
        case 0x73A4: case 0x3CB923: case 0x3C83AB: case 0x73C4:
            { node.value = data.toHex(); if (node.id == 0x73A4) result.segment_uid = node.value; return; }

        // CodecPrivate (二进制)
        case 0x63A2:
            node.value = QString("二进制 %1 字节").arg(data.size()); return;

        // Block 数据
        case 0xA1: { // Block
            model::EbmlBlockSummary s;
            s.cluster_offset = result.total_clusters > 0 ? 0 : 0;
            node.value = ParseBlockData(data, s);
            node.extra = node.value;
            if (result.blocks.size() < 1000) {
                s.block_offset = node.offset;
                result.blocks.push_back(s);
            }
            return;
        }
        case 0xA3: { // SimpleBlock
            model::EbmlBlockSummary s;
            node.value = ParseSimpleBlockData(data, s);
            node.extra = node.value;
            if (result.blocks.size() < 1000) {
                s.block_offset = node.offset;
                result.blocks.push_back(s);
            }
            return;
        }

        default: break;
    }

    // 宽/高/采样率/声道/码率/帧率 (VINT 变长)
    switch (node.id) {
        case 0xB0: case 0xBA:
        case 0x54AA: case 0x54BB: case 0x54CC: case 0x54DD:
        case 0x54B0: case 0x54BA:
            node.value = QString::number(readBeUInt(data, data.size())); return;
        case 0x9F: // Channels
            node.value = QString::number(static_cast<uint32_t>(readBeUInt(data, data.size()))); return;
        case 0x6264: // BitDepth
            node.value = QString::number(static_cast<uint32_t>(readBeUInt(data, data.size()))); return;
        default: break;
    }

    // 已知字符串元素
    switch (node.id) {
        case 0x536E: case 0x63A2: case 0x86: case 0x258688:
        case 0x22B59C:
        case 0x7384: case 0x7D7B: case 0x3A9697: case 0x3B4040: case 0x26B240:
        case 0x6E67: case 0x6EBC: case 0x6E80:
        case 0x467E: case 0x46AE: case 0x7446:
        case 0x5032: case 0x5033:
        case 0x4282: case 0x4660: case 0x466E:
        case 0x4487: case 0x447A: case 0x45A3: case 0x447B:
        case 0x63CA: case 0x437C:
        case 0x4D80: case 0x5741: case 0x7BA9:
            { node.value = tryString(); return; }
        default: break;
    }

    // 尝试字符串
    QString s = tryString();
    if (!s.isEmpty() && s.size() <= 256) { node.value = s; return; }

    // 十六进制截断
    if (data.size() <= 64) { node.value = data.toHex(); return; }
    node.value = data.left(32).toHex() + "...(" + QString::number(data.size()) + " bytes)";
}

// ============================================================
// TrackEntry 子树 → 提取轨道信息
// ============================================================
void EbmlAnalyzer::ExtractTrackInfo(const model::EbmlElementNode& track_entry,
                                     model::EbmlAnalysisResult& result) {
    model::EbmlTrackInfo ti;
    int tn = result.tracks.size() + 1;
    std::function<void(const QVector<model::EbmlElementNode>&)> walk;
    walk = [&](const QVector<model::EbmlElementNode>& nodes) {
        for (const auto& n : nodes) {
            switch (n.id) {
                case 0xD7: ti.track_number = n.value.toInt(); break;
                case 0x73C5: ti.track_uid = n.value.toULongLong(); break;
                case 0x83:   ti.track_type = n.value.toInt(); break;
                case 0x86:   ti.codec_id = n.value; break;
                case 0x22B59C: ti.language = n.value; break;
                case 0x536E: ti.track_name = n.value; break;
                case 0xB9:   ti.enabled = (n.value.toInt() != 0); break;
                case 0x88:   ti.default_track = (n.value.toInt() != 0); break;
                case 0x55AA: ti.forced = (n.value.toInt() != 0); break;
                case 0x9C:   ti.lacing = (n.value.toInt() != 0); break;
                case 0x23E383: ti.default_duration = n.value.toULongLong(); break;
                case 0xB0:   ti.pixel_width = n.value.toInt(); break;
                case 0xBA:   ti.pixel_height = n.value.toInt(); break;
                case 0xB5:   ti.sampling_frequency = n.value.toDouble(); break;
                case 0x9F:   ti.channels = n.value.toInt(); break;
                case 0x6264: ti.bit_depth = n.value.toInt(); break;
                case 0x63A2: ti.codec_private_size = static_cast<int>(n.size); break;
                default: break;
            }
            walk(n.children);
        }
    };
    walk(track_entry.children);

    if (ti.track_number == 0) ti.track_number = tn;
    ti.track_type_name = TrackTypeName(ti.track_type);
    ti.codec_name = CodecIdToName(ti.codec_id);

    // 从 DefaultDuration 计算帧率
    if (ti.default_duration > 0 && ti.track_type == 1) {
        ti.frame_rate = 1e9 / ti.default_duration;
    }

    result.tracks.push_back(ti);
}

// ============================================================
// CuePoint → 提取 Cue 条目
// ============================================================
void EbmlAnalyzer::ExtractCueInfo(const model::EbmlElementNode& cue_point,
                                   model::EbmlAnalysisResult& result) {
    model::EbmlCueEntry ce;
    std::function<void(const QVector<model::EbmlElementNode>&)> walk;
    walk = [&](const QVector<model::EbmlElementNode>& nodes) {
        for (const auto& n : nodes) {
            switch (n.id) {
                case 0xB3: ce.time = n.value.toULongLong(); break;
                case 0xF7: ce.track_number = n.value.toInt(); break;
                case 0xF1: ce.cluster_position = n.value.toULongLong(); break;
                case 0x5378: ce.block_number = n.value.toULongLong(); break;
                default: break;
            }
            walk(n.children);
        }
    };
    walk(cue_point.children);
    result.cues.push_back(ce);
}

// ============================================================
// 主解析循环
// ============================================================
bool EbmlAnalyzer::ParseElement(QDataStream& ds, qint64 end_offset, int depth,
                                 model::EbmlElementNode* parent,
                                 model::EbmlAnalysisResult& result) {
    while (ds.device() && ds.device()->pos() < end_offset) {
        int id_size = 0;
        uint64_t id = readIdVInt(ds, id_size);   // ID 保留原始字节值
        if (id_size == 0 || ds.status() != QDataStream::Ok) break;

        int size_size = 0;
        uint64_t size = ReadVInt(ds, size_size);
        if (size_size == 0 || ds.status() != QDataStream::Ok) break;

        model::EbmlElementNode node;
        node.id = id;
        node.id_hex = QString("0x%1").arg(id, 0, 16);
        node.name = ElementName(id);
        node.size = size;
        node.header_size = static_cast<uint64_t>(id_size + size_size);
        node.offset = static_cast<uint64_t>(ds.device()->pos());
        node.depth = depth;

        // --- 容器元素：递归解析子元素 ---
        if (IsContainerElement(id) && size > 0 && size != 0xFFFFFFFFFFFFFFULL) {
            qint64 child_end = node.offset + static_cast<qint64>(size);
            parent->children.push_back(node);
            model::EbmlElementNode& child = parent->children.last();
            ParseElement(ds, child_end, depth + 1, &child, result);

            // --- 后处理：提取表格数据 ---
            if (id == 0xAE) { // TrackEntry
                ExtractTrackInfo(child, result);
            }
            if (id == 0xBB) { // CuePoint
                ExtractCueInfo(child, result);
            }
            if (id == 0x1F43B675) { // Cluster
                result.total_clusters++;
            }
            continue;
        }

        // --- 未知大小 ---
        if (size == 0xFFFFFFFFFFFFFFULL) {
            parent->children.push_back(node);
            continue;
        }

        // --- 叶子节点 ---
        if (size > 0 && size < 16 * 1024 * 1024) {
            QByteArray data = ds.device()->read(static_cast<qint64>(size));
            ParseLeafValue(node, data, result);
            parent->children.push_back(node);
        } else if (size > 0) {
            ds.device()->skip(static_cast<qint64>(size));
        } else {
            parent->children.push_back(node);
        }
    }
    return true;
}

// ============================================================
EbmlAnalyzer::EbmlAnalyzer() = default;
EbmlAnalyzer::~EbmlAnalyzer() = default;
void EbmlAnalyzer::Reset() {}

bool EbmlAnalyzer::Analyze(const QString& filePath, model::EbmlAnalysisResult& result) {
    result = model::EbmlAnalysisResult{};
    result.file_path = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error_message = QString("无法打开文件: %1").arg(filePath);
        LOG_ERROR(result.error_message.toStdString());
        return false;
    }

    QByteArray header = file.read(4);
    if (header.size() < 4 || static_cast<uint8_t>(header[0]) != 0x1A ||
        static_cast<uint8_t>(header[1]) != 0x45 ||
        static_cast<uint8_t>(header[2]) != 0xDF ||
        static_cast<uint8_t>(header[3]) != 0xA3) {
        result.error_message = QString("不是有效的 EBML/Matroska/WebM 文件");
        return false;
    }

    file.seek(0);
    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::BigEndian);

    model::EbmlElementNode root;
    root.name = "root";
    root.depth = -1;

    ParseElement(ds, file.size(), 0, &root, result);

    // 统计 BlockGroup / SimpleBlock 计数
    int bg = 0, sb = 0;
    std::function<void(const QVector<model::EbmlElementNode>&)> countBlocks;
    countBlocks = [&](const QVector<model::EbmlElementNode>& nodes) {
        for (const auto& n : nodes) {
            if (n.id == 0xA0) bg++;       // BlockGroup
            if (n.id == 0xA3) sb++;       // SimpleBlock
            countBlocks(n.children);
        }
    };
    countBlocks(root.children);
    result.total_blockgroups = bg;
    result.total_simpleblocks = sb;

    result.element_tree = root.children;
    result.valid = true;

    LOG_INFO("EBML 分析完成: " + result.doc_type.toStdString()
             + " | " + std::to_string(result.tracks.size()) + " 轨道"
             + " | " + std::to_string(result.cues.size()) + " 索引点"
             + " | " + std::to_string(result.total_clusters) + " Cluster");
    return true;
}

} // namespace analyzer
} // namespace videoeye
