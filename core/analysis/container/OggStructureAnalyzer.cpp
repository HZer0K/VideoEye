#include "infrastructure/concurrency/Cancellation.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/container/OggStructureAnalyzer.h"
#include "core/analysis/detail/SeqFileReader.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace videoeye {
namespace analyzer {

namespace {
uint32_t oggLE32(const std::string& d, int off) {
    if (off + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3])) << 24);
}
// 解析 Vorbis/Opus 注释块 (vendor + KEY=VALUE 列表)，从 start 起
// 结果直接写进 domain 的 metadata（std::string），所以内部一律用 std 类型。
void parseVorbisComments(const std::string& d, int start,
                        std::map<std::string, std::string>& out) {
    int pos = start;
    if (pos + 4 > d.size()) return;
    uint32_t vlen = oggLE32(d, pos); pos += 4;
    if (pos + static_cast<int>(vlen) > d.size()) return;
    const std::string vendor = d.substr(pos, vlen);
    pos += vlen;
    if (!vendor.empty()) out["vendor"] = vendor;
    if (pos + 4 > d.size()) return;
    uint32_t count = oggLE32(d, pos); pos += 4;
    for (uint32_t i = 0; i < count && pos + 4 <= d.size(); ++i) {
        uint32_t clen = oggLE32(d, pos); pos += 4;
        if (pos + static_cast<int>(clen) > d.size()) break;
        const std::string comment = d.substr(pos, clen);
        pos += clen;
        size_t eq = comment.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string k = comment.substr(0, eq);
            std::transform(k.begin(), k.end(), k.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            std::string v = comment.substr(eq + 1);
            if (out.count(k) == 0) out[k] = v;
        }
    }
}
} // namespace

bool OggStructureAnalyzer::Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                                   const std::atomic<bool>* cancel) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::OGG;
    result.format_name = "OGG";
    result.file_path = file_path;

    model::ContainerElement root;
    root.name = "OGG Stream";
    root.type = "OGG";
    root.size = file.Size();
    root.offset = 0;
    root.depth = 0;

    // 跟踪 logical streams
    struct StreamInfo {
        int page_count = 0;
        std::string codec_name;
        bool bos_seen = false;
        uint32_t sample_rate = 0;
        int channels = 0;
        bool comments_parsed = false;
    };
    std::map<uint32_t, StreamInfo> streams;

    int total_pages = 0;
    const int max_pages = 2000;

    while (file.Pos() < file.Size() - 27 && total_pages < max_pages) {
        if (infrastructure::Checkpoint(cancel)) { result.error_message = "已取消"; return false; }
        // Ogg Page Header: "OggS" (4) + version(1) + type(1) + granule(8) + serial(4) + page_seq(4) + checksum(4) + segments(1)
        std::string header = file.Read(27);
        if (header.size() < 27) break;

        if (header.substr(0, 4) != "OggS") {
            // 尝试重新同步
            std::string sync = file.Read(1);
            while (!file.AtEnd() && sync != "O") {
                sync = file.Read(1);
            }
            if (file.AtEnd()) break;
            continue;
        }

        uint8_t header_type = static_cast<uint8_t>(header[5]);
        bool is_bos = (header_type & 0x02) != 0;
        bool is_eos = (header_type & 0x04) != 0;

        uint32_t serial = (static_cast<uint8_t>(header[14])) |
                          (static_cast<uint8_t>(header[15]) << 8) |
                          (static_cast<uint8_t>(header[16]) << 16) |
                          (static_cast<uint8_t>(header[17]) << 24);

        uint32_t page_seq = (static_cast<uint8_t>(header[18])) |
                            (static_cast<uint8_t>(header[19]) << 8) |
                            (static_cast<uint8_t>(header[20]) << 16) |
                            (static_cast<uint8_t>(header[21]) << 24);

        uint8_t num_segments = static_cast<uint8_t>(header[26]);

        // 读取 segment table
        std::string seg_table = file.Read(num_segments);
        if (seg_table.size() < num_segments) break;

        uint32_t page_data_size = 0;
        for (int i = 0; i < num_segments; ++i) {
            page_data_size += static_cast<uint8_t>(seg_table[i]);
        }

        // 读取页面数据 (前 4096 字节，足以覆盖标识头与注释头，用于 codec 识别与元数据)
        int64_t page_data_offset = file.Pos();
        std::string page_data = file.Read(std::min(static_cast<int64_t>(page_data_size), static_cast<int64_t>(4096)));

        // 跳到下一页
        file.Seek(page_data_offset + page_data_size);

        // 更新流信息
        auto& si = streams[serial];
        si.page_count++;

        // BOS 页面识别 codec + 标识头解析 (采样率/声道)
        if (is_bos && page_data.size() >= 7) {
            if (page_data.substr(1, 6) == "vorbis") {
                si.codec_name = "Vorbis";
                // \x01vorbis(7) + version(4) + channels(1) + sample_rate(4 LE)
                if (page_data.size() >= 16) {
                    si.channels = static_cast<uint8_t>(page_data[11]);
                    si.sample_rate = oggLE32(page_data, 12);
                }
            } else if (StartsWith(page_data,"OpusHead")) {
                si.codec_name = "Opus";
                // OpusHead(8)+version(1)+channels(1)+preskip(2)+input_sample_rate(4 LE)
                if (page_data.size() >= 16) {
                    si.channels = static_cast<uint8_t>(page_data[9]);
                    si.sample_rate = oggLE32(page_data, 12);
                }
            } else if (page_data.substr(0, 4) == "fLaC" || StartsWith(page_data,std::string("\x7F" "FLAC", 5))) {
                si.codec_name = "FLAC";
            } else if (page_data.substr(1, 6) == "theora") {
                si.codec_name = "Theora";
            } else if (page_data.substr(0, 5) == "\x80theora") {
                si.codec_name = "Theora";
            } else if (StartsWith(page_data,"Speex")) {
                si.codec_name = "Speex";
            } else {
                si.codec_name = "Unknown";
            }
        }

        // 注释头解析 (通常在 BOS 之后的第二个包)
        if (!si.comments_parsed) {
            if (page_data.size() > 7 && page_data.substr(1, 6) == "vorbis" &&
                static_cast<uint8_t>(page_data[0]) == 0x03) {
                parseVorbisComments(page_data, 7, result.metadata);
                si.comments_parsed = true;
            } else if (StartsWith(page_data,"OpusTags")) {
                parseVorbisComments(page_data, 8, result.metadata);
                si.comments_parsed = true;
            }
        }

        // 创建页面元素 (仅前 200 个页面加入树)
        if (total_pages < 200) {
            model::ContainerElement page;
            page.name = StrCat("Page #%1", page_seq);
            page.type = "Ogg Page";
            page.size = 27 + num_segments + page_data_size;
            page.offset = page_data_offset - 27 - num_segments;
            page.depth = 1;

            std::string flags;
            if (is_bos) flags += "BOS ";
            if (is_eos) flags += "EOS ";
            page.value = StrCat("serial=%1 %2size=%3", serial, flags,
                                std::to_string(page_data_size));
            root.children.push_back(page);
        }

        total_pages++;
    }

    // 添加流信息
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        const auto& info = it->second;
        std::string detail;
        if (info.sample_rate > 0)
            detail = StrCat("%1 Hz, %2 ch", info.sample_rate, info.channels);

        model::ContainerElement stream_elem;
        stream_elem.name = StrCat("Logical Stream (serial=%1)", it->first);
        stream_elem.type = "Logical Stream";
        stream_elem.depth = 1;
        stream_elem.value = (StrCat("codec=%1 pages=%2%3", info.codec_name, info.page_count, detail.empty() ? "" : " | " + detail));

        model::ContainerStreamInfo csi;
        csi.index = result.streams.size();
        csi.codec = info.codec_name;
        csi.details = detail;
        // 根据 codec 推断类型
        std::string codec_lower = ToLowerCopy(info.codec_name);
        if (codec_lower == "vorbis" || codec_lower == "opus" || codec_lower == "flac" || codec_lower == "speex") {
            csi.type = "audio";
        } else if (codec_lower == "theora") {
            csi.type = "video";
        } else {
            csi.type = "data";
        }
        result.streams.push_back(csi);

        root.children.push_back(stream_elem);
    }

    result.element_tree.push_back(root);
    result.valid = true;
    result.summary = StrCat("OGG | %1 页/已解析 | %2 逻辑流", total_pages, streams.size());

    file.Close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
