#include "core/analysis/container/AviStructureAnalyzer.h"
#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/SeqFileReader.h"
#include <algorithm>
#include <cstdint>
#include <string>

#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {

namespace {
// 小端读取辅助 (AVI/RIFF 为小端)
uint16_t readLE16(const std::string& d, int off) {
    if (off + 2 > d.size())
        return 0;
    return static_cast<uint16_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(d[off + 1])) << 8);
}
uint32_t readLE32(const std::string& d, int off) {
    if (off + 4 > d.size())
        return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3])) << 24);
}
// WAVE 格式 tag → 可读音频编码名
std::string waveFormatName(uint16_t tag) {
    switch (tag) {
    case 0x0001:
        return "PCM";
    case 0x0002:
        return "ADPCM";
    case 0x0003:
        return "IEEE Float";
    case 0x0006:
        return "A-law";
    case 0x0007:
        return "mu-law";
    case 0x0011:
        return "IMA ADPCM";
    case 0x0031:
    case 0x0032:
        return "GSM 6.10";
    case 0x0050:
        return "MPEG";
    case 0x0055:
        return "MP3";
    case 0x00FF:
        return "AAC";
    case 0x2000:
        return "AC-3";
    case 0x2001:
        return "DTS";
    case 0xF1AC:
        return "FLAC";
    case 0x674F:
    case 0x6750:
    case 0x6751:
        return "Ogg Vorbis";
    default:
        return HexFill(tag, 4);
    }
}
} // namespace

bool AviStructureAnalyzer::Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                                   const std::atomic<bool>* cancel, const Options& options) {
    build_tree_ = options.build_tree;
    scan_media_data_ = options.scan_media_data;

    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::AVI;
    result.format_name = "AVI";
    result.file_path = file_path;
    result.diagnostics.avi = model::AviDiagnostics();
    result.diagnostics.avi.analyzed = true;
    result.diagnostics.avi.file_size = static_cast<uint64_t>(file.Size());

    // 读取 RIFF header: "RIFF" + size(4) + "AVI "
    std::string header = file.Read(12);
    if (header.size() < 12 || header.substr(0, 4) != "RIFF" || header.substr(8, 4) != "AVI ") {
        result.error_message = "不是有效的 AVI 文件";
        return false;
    }

    uint32_t riff_size = *reinterpret_cast<const uint32_t*>(header.data() + 4);

    // 创建根元素
    model::ContainerElement root;
    root.name = "RIFF 'AVI '";
    root.type = "RIFF";
    root.size = riff_size + 8;
    root.offset = 0;
    root.depth = 0;
    root.value = StrCat("size=%1", riff_size + 8);

    // RIFF 声明的长度超过文件实际大小 = 文件被截断（或录制中断）。
    // 注意 RIFF 的 size 字段是 32 位，OpenDML 用 'AVIX' 扩展 RIFF 突破 4 GB；
    // 这里只判断"声明长度是否装得下"，不把它当成 64 位总长。
    if (static_cast<uint64_t>(riff_size) + 8u > static_cast<uint64_t>(file.Size())) {
        result.diagnostics.avi.riff_size_exceeds_file = true;
    }

    // 递归解析子块
    int64_t end_offset = std::min(static_cast<int64_t>(riff_size + 8), file.Size());
    if (!ParseChunk(file, end_offset, 1, root, result, cancel))
        return false;

    // OpenDML 把超出 4 GB 的媒体数据放在后续的 RIFF 'AVIX' 里，它是首个 RIFF 的
    // **同级块**而不是子块。上面的递归只看首个 RIFF，所以这里单独走一遍顶层。
    {
        int64_t top = static_cast<int64_t>(riff_size) + 8;
        if ((riff_size & 1u) != 0)
            ++top; // RIFF 块按偶数字节对齐
        int guard = 0;
        while (top + 12 <= file.Size() && ++guard < 100000) {
            if (infrastructure::Checkpoint(cancel))
                return false;
            file.Seek(top);
            const std::string top_header = file.Read(12);
            if (top_header.size() < 12 || top_header.substr(0, 4) != "RIFF")
                break;
            const std::string list_type = top_header.substr(8, 4);
            if (list_type == "AVIX")
                result.diagnostics.avi.has_avix = true;
            const uint32_t top_size = readLE32(top_header, 4);
            int64_t next = top + 8 + static_cast<int64_t>(top_size);
            if ((top_size & 1u) != 0)
                ++next;
            if (next <= top)
                break; // 畸形 size：别在这里死循环
            top = next;
        }
    }

    if (build_tree_)
        result.element_tree.push_back(root);
    result.valid = true;
    result.summary = StrCat("AVI (RIFF) | 文件大小: %1 字节", file.Size());

    file.Close();
    return true;
}

bool AviStructureAnalyzer::ParseChunk(SeqFileReader& file, int64_t end_offset, int depth,
                                      model::ContainerElement& parent, model::ContainerStructureResult& result,
                                      const std::atomic<bool>* cancel) {
    // 限制递归深度防止栈溢出
    if (depth > 8)
        return false;

    auto& diag = result.diagnostics.avi;
    const int64_t file_size = file.Size();

    // 当前 strl 内 strh 记录的流类型/流索引，供随后的 strf 解释格式头
    std::string cur_strh_type; // "vids" / "auds" / ...
    int cur_stream_idx = -1;

    while (file.Pos() < end_offset - 8) {
        if (infrastructure::Checkpoint(cancel))
            return false;
        std::string chunk_header = file.Read(8);
        if (chunk_header.size() < 8)
            break;

        std::string fourcc = chunk_header.substr(0, 4);
        uint32_t chunk_size = readLE32(chunk_header, 4);
        int64_t chunk_offset = file.Pos() - 8;

        // 声明的块长越过文件末尾 = 文件被截断（录制中断 / 下载不全）。
        // 这是 >1 GB 的 AVI 最常见的"能打开、播放到某处就卡"的成因。
        if (chunk_offset + 8 + static_cast<int64_t>(chunk_size) > file_size) {
            ++diag.truncated_chunks;
        }

        model::ContainerElement elem;
        elem.name = (fourcc);
        elem.size = chunk_size;
        elem.offset = chunk_offset;
        elem.depth = depth;

        // 判断是否为 LIST 容器
        if (fourcc == "LIST" || fourcc == "RIFF") {
            std::string list_type = file.Read(4);
            elem.name = StrCat("%1 '%2'", fourcc, list_type);
            elem.type = "LIST";
            elem.value = StrCat("size=%1", chunk_size);

            if (list_type == "AVIX")
                diag.has_avix = true;

            int64_t list_end = file.Pos() + static_cast<int64_t>(chunk_size) - 4;
            if (list_end > file_size)
                list_end = file_size;

            // QC 扫描只取结构事实：movi 里是真正的音视频数据，块数可达百万级，
            // 逐块走一遍会把全文件扫描拖成分钟级，这里整段跳过。
            const bool skip_media = (list_type == "movi" && !scan_media_data_);
            if (!skip_media) {
                ParseChunk(file, list_end, depth + 1, elem, result, cancel);
            }

            // 确保跳到正确位置 (chunk_size 对齐到偶数)
            int64_t expected_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                expected_pos++;
            if (expected_pos <= file_size) {
                file.Seek(expected_pos);
            }
        } else if (fourcc == "strh") {
            // 流头信息: 提取流类型和编解码器
            elem.type = "Stream Header";
            std::string data = file.Read(chunk_size);
            if (data.size() >= 8) {
                std::string fcc_type = (data.substr(0, 4));
                std::string fcc_handler = (data.substr(4, 4));
                elem.value = StrCat("type=%1 codec=%2", fcc_type, fcc_handler);

                model::ContainerStreamInfo si;
                si.index = static_cast<int>(result.streams.size());
                if (fcc_type == "vids")
                    si.type = "video";
                else if (fcc_type == "auds")
                    si.type = "audio";
                else if (fcc_type == "txts" || fcc_type == "subs")
                    si.type = "subtitle";
                else
                    si.type = fcc_type;
                si.codec = TrimCopy(fcc_handler);
                result.streams.push_back(si);

                cur_strh_type = fcc_type;
                cur_stream_idx = si.index;

                // dwLength 位于 AVIStreamHeader 的 offset 32（单位：dwScale/dwRate 折算的样本数）。
                // 0 通常意味着写入器没有回填（中断录制 / 非流式写入），
                // 是"时长显示正常但进度条拖不动"这类问题的前兆。
                const uint32_t dw_length = (data.size() >= 36) ? readLE32(data, 32) : 0;
                if (fcc_type == "vids") {
                    ++diag.video_streams;
                    diag.max_video_stream_length = std::max<uint64_t>(diag.max_video_stream_length, dw_length);
                } else if (fcc_type == "auds") {
                    ++diag.audio_streams;
                }
                if (fcc_type == "vids" || fcc_type == "auds") {
                    if (dw_length == 0)
                        ++diag.zero_length_streams;
                }
            }
            // 对齐
            int64_t next_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                next_pos++;
            if (next_pos <= file_size)
                file.Seek(next_pos);
        } else if (fourcc == "strf") {
            // 流格式头: 视频=BITMAPINFOHEADER, 音频=WAVEFORMATEX
            elem.type = "Stream Format";
            std::string data = file.Read(chunk_size);
            if (cur_strh_type == "vids" && data.size() >= 40) {
                uint32_t biWidth = readLE32(data, 4);
                int32_t biHeight = static_cast<int32_t>(readLE32(data, 8));
                uint16_t biBitCount = readLE16(data, 14);
                std::string comp = data.substr(16, 4);
                std::string fourccStr = TrimCopy(comp);
                bool rawRgb = (readLE32(data, 16) == 0);
                std::string codecName = rawRgb ? std::string("RGB") : fourccStr;
                elem.value = (StrCat("%1x%2 %3bit codec=%4", biWidth, std::abs(biHeight), biBitCount, codecName));
                if (cur_stream_idx >= 0 && cur_stream_idx < static_cast<int>(result.streams.size())) {
                    auto& s = result.streams[cur_stream_idx];
                    if (!codecName.empty())
                        s.codec = codecName;
                    s.details = (StrCat("%1x%2, %3-bit", biWidth, std::abs(biHeight), biBitCount));
                }
            } else if (cur_strh_type == "auds" && data.size() >= 16) {
                uint16_t wFormatTag = readLE16(data, 0);
                uint16_t nChannels = readLE16(data, 2);
                uint32_t nSamplesPerSec = readLE32(data, 4);
                uint32_t nAvgBytesPerSec = readLE32(data, 8);
                uint16_t wBitsPerSample = readLE16(data, 14);
                std::string codecName = waveFormatName(wFormatTag);
                elem.value = (StrCat("%1 %2Hz %3ch %4bit %5kbps", codecName, nSamplesPerSec, nChannels, wBitsPerSample,
                                     nAvgBytesPerSec * 8 / 1000));
                if (cur_stream_idx >= 0 && cur_stream_idx < static_cast<int>(result.streams.size())) {
                    auto& s = result.streams[cur_stream_idx];
                    s.codec = codecName;
                    s.details = (StrCat("%1 Hz, %2 ch, %3-bit, %4 kbps", nSamplesPerSec, nChannels, wBitsPerSample,
                                        nAvgBytesPerSec * 8 / 1000));
                }
            } else {
                elem.value = StrCat("size=%1", chunk_size);
            }
            int64_t next_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                next_pos++;
            if (next_pos <= file_size)
                file.Seek(next_pos);
        } else if (fourcc == "idx1") {
            // 传统 AVI 索引: 每项 16 字节 (ckid, dwFlags, dwChunkOffset, dwChunkLength)。
            // 只做"偏移 + 长度是否落在文件内"的粗校验：越界项说明索引写坏或文件被截断，
            // 表现就是拖动进度条后花屏 / 卡住。
            elem.type = "Index (idx1)";
            elem.value = StrCat("size=%1 entries=%2", chunk_size, chunk_size / 16);
            diag.has_idx1 = true;
            diag.idx1_entries = chunk_size / 16;

            // 流式校验，避免把上百 MB 的索引整块读进内存
            constexpr int64_t kBlockBytes = 64 * 1024;
            const int64_t index_end = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            const int64_t read_end = std::min(index_end, file_size);
            int64_t cursor = file.Pos();
            while (cursor + 16 <= read_end) {
                if (infrastructure::Checkpoint(cancel))
                    return false;
                const int64_t want = std::min<int64_t>(kBlockBytes, read_end - cursor);
                const std::string block = file.Read(static_cast<size_t>(want));
                if (block.size() < 16)
                    break;
                for (size_t i = 0; i + 16 <= block.size(); i += 16) {
                    const uint32_t entry_offset = readLE32(block, static_cast<int>(i + 8));
                    const uint32_t entry_length = readLE32(block, static_cast<int>(i + 12));
                    if (static_cast<uint64_t>(entry_offset) + entry_length > static_cast<uint64_t>(file_size)) {
                        ++diag.idx1_bad_offsets;
                    }
                }
                cursor += static_cast<int64_t>(block.size());
            }

            int64_t next_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                next_pos++;
            if (next_pos <= file_size)
                file.Seek(next_pos);
        } else if (fourcc == "indx") {
            // OpenDML 标准索引: 头 24 字节 (wLongsPerEntry, bIndexSubType, bIndexType,
            // nEntriesInUse, dwChunkId, dwReserved[3])，其后是条目。
            elem.type = "Index (indx)";
            std::string data = file.Read(chunk_size);
            diag.has_indx = true;
            if (data.size() >= 24) {
                const uint16_t w_longs_per_entry = readLE16(data, 0);
                const uint32_t n_entries = readLE32(data, 4);
                diag.indx_entries += n_entries;
                const uint64_t need = 24ull + static_cast<uint64_t>(n_entries) * w_longs_per_entry * 4ull;
                elem.value = StrCat("entries=%1 longsPerEntry=%2", n_entries, w_longs_per_entry);
                if (need > static_cast<uint64_t>(data.size())) {
                    // 索引头声明的条目数装不下 —— 索引被截断，长跳转定位会失真
                    diag.indx_truncated = true;
                }
            } else {
                elem.value = StrCat("size=%1 (头部不完整)", chunk_size);
            }
            int64_t next_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                next_pos++;
            if (next_pos <= file_size)
                file.Seek(next_pos);
        } else if (fourcc == "dmlh") {
            // OpenDML 扩展头: dwTotalFrames 是跨 AVIX 累计的**总帧数**，
            // 也是 >1 GB 文件能正确定位的前提。
            elem.type = "OpenDML Header (dmlh)";
            std::string data = file.Read(chunk_size);
            diag.has_dmlh = true;
            if (data.size() >= 4) {
                diag.dmlh_total_frames = readLE32(data, 0);
                elem.value = StrCat("totalFrames=%1", diag.dmlh_total_frames);
            } else {
                elem.value = StrCat("size=%1 (头部不完整)", chunk_size);
            }
            int64_t next_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                next_pos++;
            if (next_pos <= file_size)
                file.Seek(next_pos);
        } else {
            // 普通叶子块
            elem.type = "Chunk";
            elem.value = StrCat("size=%1", chunk_size);

            // 跳过数据
            int64_t next_pos = chunk_offset + 8 + static_cast<int64_t>(chunk_size);
            if (chunk_size % 2 != 0)
                next_pos++;
            if (next_pos <= file_size) {
                file.Seek(next_pos);
            } else {
                file.Seek(file_size);
            }
        }

        if (build_tree_)
            parent.children.push_back(elem);
    }
    return true;
}

} // namespace videoeye
