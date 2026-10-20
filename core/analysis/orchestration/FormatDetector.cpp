#include "core/analysis/orchestration/FormatDetector.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/SeqFileReader.h"
#include <algorithm>
#include <string>

namespace videoeye {

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

    // MP4/MOV: offset 4-7 = "ftyp"
    if (header.size() >= 8 && header.substr(4, 4) == "ftyp") {
        // 区分 MP4 vs MOV: 检查 ftyp 后的 brand
        std::string brand = header.substr(8, 4);
        if (brand == "qt  " || brand == "mdat" || StartsWith(brand, "qt")) {
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
    if (header.size() >= 4 &&
        (unsigned char)header[0] == 0x1A &&
        (unsigned char)header[1] == 0x45 &&
        (unsigned char)header[2] == 0xDF &&
        (unsigned char)header[3] == 0xA3) {
        // 在头部区域搜索 DocType 来区分 MKV vs WebM
        if (header.find("webm") != std::string::npos) {
            return model::ContainerFormat::WebM;
        }
        return model::ContainerFormat::MKV;
    }

    // AVI: "RIFF" + 4 bytes size + "AVI "
    if (header.size() >= 12 &&
        header.substr(0, 4) == "RIFF" &&
        header.substr(8, 4) == "AVI ") {
        return model::ContainerFormat::AVI;
    }

    // FLV: "FLV"
    if (header.size() >= 3 && header.substr(0, 3) == "FLV") {
        return model::ContainerFormat::FLV;
    }

    // MPEG-TS: sync byte 0x47 repeats at a fixed stride.
    //
    // 三种包长都要认，不能只试 188：
    //   188 = 标准 TS
    //   192 = M2TS / AVCHD，前面多 4 字节 TP_extra_header（同步字节落在偏移 4）
    //   204 = RS(204,188) 纠错包（同步字节仍在 0）
    // 以前只试 188，于是 .m2ts 的魔数判定必然失败、只能靠扩展名兜底；而扩展名兜底
    // 又会把它分给按 188 步长解析的 TS 分析器，最后报"未找到同步字节"。
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
                    if (static_cast<unsigned char>(header[static_cast<size_t>(at)]) != 0x47) break;
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

    // ASF: Header Object GUID = 30 26 B2 75 8E 66 CF 11
    if (header.size() >= 8 &&
        (unsigned char)header[0] == 0x30 &&
        (unsigned char)header[1] == 0x26 &&
        (unsigned char)header[2] == 0xB2 &&
        (unsigned char)header[3] == 0x75 &&
        (unsigned char)header[4] == 0x8E &&
        (unsigned char)header[5] == 0x66 &&
        (unsigned char)header[6] == 0xCF &&
        (unsigned char)header[7] == 0x11) {
        return model::ContainerFormat::ASF;
    }

    // OGG: "OggS"
    if (header.size() >= 4 && header.substr(0, 4) == "OggS") {
        return model::ContainerFormat::OGG;
    }

    // MPEG-PS: 以 pack header 起始码 00 00 01 BA 开头（.mpg/.vob/.m2p）。
    // 必须在"裸流"判断之前：PS 里同样全是 00 00 01 起始码。
    if (header.size() >= 4 &&
        (unsigned char)header[0] == 0x00 &&
        (unsigned char)header[1] == 0x00 &&
        (unsigned char)header[2] == 0x01 &&
        (unsigned char)header[3] == 0xBA) {
        return model::ContainerFormat::MPEG_PS;
    }

    // MXF: SMPTE 通用键前缀 06 0E 2B 34（UL 的前 4 字节）。
    if (header.size() >= 4 &&
        (unsigned char)header[0] == 0x06 &&
        (unsigned char)header[1] == 0x0E &&
        (unsigned char)header[2] == 0x2B &&
        (unsigned char)header[3] == 0x34) {
        return model::ContainerFormat::MXF;
    }

    // 裸流（Annex-B）：00 00 00 01 / 00 00 01 起始码 + 合理的 H.264/HEVC NAL 头。
    // 只认"头 4 字节 + 后面还能再看到一个起始码"的组合，避免被随机 0 字节骗到。
    if (header.size() >= 8) {
        size_t sc = std::string::npos;
        size_t nal = std::string::npos;
        if (header.compare(0, 4, std::string("\x00\x00\x00\x01", 4)) == 0) {
            sc = 0;
            nal = 4;
        } else if (header.compare(0, 3, std::string("\x00\x00\x01", 3)) == 0) {
            sc = 0;
            nal = 3;
        }
        if (sc != std::string::npos && nal < header.size()) {
            const unsigned char b = static_cast<unsigned char>(header[nal]);
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

    return model::ContainerFormat::Unknown;
}

model::ContainerFormat FormatDetector::DetectByExtension(const std::string& file_path) {
    std::string ext = FileExtension(file_path);

    if (ext == "mp4" || ext == "m4v" || ext == "m4a" || ext == "f4v") {
        return model::ContainerFormat::MP4;
    }
    if (ext == "mov") {
        return model::ContainerFormat::MOV;
    }
    if (ext == "mkv") {
        return model::ContainerFormat::MKV;
    }
    if (ext == "webm") {
        return model::ContainerFormat::WebM;
    }
    if (ext == "avi") {
        return model::ContainerFormat::AVI;
    }
    if (ext == "flv" || ext == "f4v") {
        return model::ContainerFormat::FLV;
    }
    if (ext == "ts" || ext == "mts" || ext == "m2ts") {
        return model::ContainerFormat::MPEG_TS;
    }
    if (ext == "asf" || ext == "wmv" || ext == "wma") {
        return model::ContainerFormat::ASF;
    }
    if (ext == "ogg" || ext == "oga" || ext == "ogv" || ext == "opus") {
        return model::ContainerFormat::OGG;
    }
    // MPEG-PS：.mpg/.mpeg/.vob/.m2p/.ps。规则层的别名表早就把它们归到 "mpeg"，
    // 但解析层以前不认识，这些文件只能落到 FFmpeg 通用分析。
    if (ext == "mpg" || ext == "mpe" || ext == "mpeg" || ext == "m2p" ||
        ext == "vob" || ext == "ps" || ext == "m2v") {
        return model::ContainerFormat::MPEG_PS;
    }
    if (ext == "mxf") {
        return model::ContainerFormat::MXF;
    }
    // 裸流：没有容器层，结构页走 FFmpeg 通用分析，但格式名要认出来。
    if (ext == "h264" || ext == "264" || ext == "avc" ||
        ext == "h265" || ext == "265" || ext == "hevc" || ext == "hvc" ||
        ext == "vvc" || ext == "266" || ext == "h266" ||
        ext == "av1" || ext == "obu") {
        return model::ContainerFormat::RAW_ES;
    }
    // 流媒体清单是纯文本（#EXTM3U / <MPD），魔数无从判断，只能看扩展名。
    if (ext == "m3u8" || ext == "m3u") {
        return model::ContainerFormat::HLS;
    }
    if (ext == "mpd") {
        return model::ContainerFormat::DASH;
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
    case model::ContainerFormat::MP4:            return "MP4";
    case model::ContainerFormat::MOV:            return "MOV";
    case model::ContainerFormat::MKV:            return "MKV";
    case model::ContainerFormat::WebM:           return "WebM";
    case model::ContainerFormat::AVI:            return "AVI";
    case model::ContainerFormat::FLV:            return "FLV";
    case model::ContainerFormat::MPEG_TS:        return "MPEG-TS";
    case model::ContainerFormat::ASF:            return "ASF";
    case model::ContainerFormat::OGG:            return "OGG";
    case model::ContainerFormat::MPEG_PS:        return "MPEG-PS";
    case model::ContainerFormat::MXF:            return "MXF";
    case model::ContainerFormat::RAW_ES:         return "Raw ES";
    case model::ContainerFormat::HLS:            return "HLS";
    case model::ContainerFormat::DASH:           return "DASH";
    case model::ContainerFormat::FFmpeg_Generic: return "Generic";
    default:                                     return "Unknown";
    }
}

std::string FormatDetector::FormatTitle(model::ContainerFormat fmt) {
    switch (fmt) {
    case model::ContainerFormat::MP4:            return "MP4 Box 结构";
    case model::ContainerFormat::MOV:            return "MOV (QuickTime) 结构";
    case model::ContainerFormat::MKV:            return "MKV (EBML) 结构";
    case model::ContainerFormat::WebM:           return "WebM (EBML) 结构";
    case model::ContainerFormat::AVI:            return "AVI (RIFF) 结构";
    case model::ContainerFormat::FLV:            return "FLV 结构";
    case model::ContainerFormat::MPEG_TS:        return "MPEG-TS 结构";
    case model::ContainerFormat::ASF:            return "ASF/WMV 结构";
    case model::ContainerFormat::OGG:            return "OGG 结构";
    case model::ContainerFormat::MPEG_PS:        return "MPEG-PS (Pack/PES) 结构";
    case model::ContainerFormat::MXF:            return "MXF (KLV) 结构";
    case model::ContainerFormat::RAW_ES:         return "裸流 (Annex-B) 结构";
    case model::ContainerFormat::HLS:            return "HLS 清单结构";
    case model::ContainerFormat::DASH:           return "DASH MPD 结构";
    case model::ContainerFormat::FFmpeg_Generic: return "FFmpeg 通用结构";
    default:                                     return "文件结构";
    }
}

} // namespace videoeye
