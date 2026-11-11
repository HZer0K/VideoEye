#include "core/repair/RepairBitstreamResync.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/repair/RepairSampleScan.h"

namespace videoeye {
namespace repair {
namespace {

// 单个 NAL 的长度上限。超过这个值的"长度前缀"不可能是真实 NAL，
// 一定是扫描落到了载荷中间（AAC / SEI 数据被当成前缀）。
constexpr std::uint32_t kMaxNalBytes = 8u * 1024u * 1024u;
// 块尾重叠：长度前缀(4) + NAL 头(2) + 余量，保证跨块边界的候选不被漏掉。
constexpr std::size_t kOverlapBytes = 8;
// 顶层盒数量上限：防御畸形盒链把扫描拖进死循环。
constexpr int kMaxTopLevelBoxes = 64;

std::uint32_t Be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::string LowerAscii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// NAL 头合法性。这是剪枝的第一道闸门：载荷中间被误认成"长度前缀"的 4 字节
// 绝大多数会在这里被挡掉（长度过大 / forbidden_zero_bit 置位 / type 越界）。
bool ParseNalHead(const std::string& codec, std::uint8_t b0, std::uint8_t* type) {
    if (codec == "h264") {
        if ((b0 & 0x80) != 0)
            return false; // forbidden_zero_bit 必须为 0
        const std::uint8_t nal_type = static_cast<std::uint8_t>(b0 & 0x1F);
        if (nal_type < 1 || nal_type > 12)
            return false; // 8~12 保留，13+ 属 SVC/MVC 扩展，第一版不认
        *type = nal_type;
        return true;
    }
    if (codec == "hevc") {
        if ((b0 & 0x80) != 0)
            return false;
        const std::uint8_t nal_type = static_cast<std::uint8_t>((b0 >> 1) & 0x3F);
        if (nal_type > 40)
            return false; // 41+ 为保留值
        *type = nal_type;
        return true;
    }
    return false;
}

bool IsParamNal(const std::string& codec, std::uint8_t type) {
    if (codec == "h264")
        return type == 7 || type == 8; // SPS / PPS
    if (codec == "hevc")
        return type == 32 || type == 33 || type == 34; // VPS / SPS / PPS
    return false;
}

bool IsRapNal(const std::string& codec, std::uint8_t type) {
    if (codec == "h264")
        return type == 5; // IDR slice
    if (codec == "hevc")
        return type == 16 || type == 19 || type == 20 || type == 21 || type == 22; // BLA/IDR/CRA
    return false;
}

bool IsVclNal(const std::string& codec, std::uint8_t type) {
    if (codec == "h264")
        return type >= 1 && type <= 5;
    if (codec == "hevc")
        return type <= 31;
    return false;
}

struct ChainProbe {
    bool ok = false;
    bool has_rap = false;
    bool has_params = false;
    std::uint64_t au_bytes = 0;
    int nals = 0;
};

// 从 start 起按 4 字节长度前缀走链。判据只有"链自洽 + 出现 VCL"：
// 不在这里判断 AU 边界（那是重建器需要的事），只要这一段能走通就说明
// 起点确实落在 AVCC 码流上，而不是载荷中间的随机字节。
ChainProbe WalkChain(const std::string& codec, SampleByteSource& src, std::uint64_t start, int max_nals) {
    ChainProbe probe;
    const std::uint64_t end = src.Size();
    std::uint64_t cursor = start;
    bool saw_vcl = false;
    for (int i = 0; i < max_nals; ++i) {
        if (cursor >= end || end - cursor < 4)
            break;
        std::uint8_t length_bytes[4] = {};
        if (!src.Read(cursor, length_bytes, sizeof(length_bytes)))
            break;
        const std::uint32_t nal_size = Be32(length_bytes);
        if (nal_size < 2 || nal_size > kMaxNalBytes || nal_size > end - cursor - 4)
            break;
        std::uint8_t head = 0;
        if (!src.Read(cursor + 4, &head, 1))
            break;
        std::uint8_t type = 0;
        if (!ParseNalHead(codec, head, &type))
            break;
        if (IsParamNal(codec, type)) {
            if (saw_vcl)
                break; // 参数集出现在 VCL 之后 => 已经进入下一个 AU
            probe.has_params = true;
        } else if (IsRapNal(codec, type)) {
            probe.has_rap = true;
            saw_vcl = true;
        } else if (IsVclNal(codec, type)) {
            saw_vcl = true;
        } else if (saw_vcl) {
            break; // AUD / SEI / 填充数据出现在 VCL 之后 => 下一个 AU 开始
        }
        probe.au_bytes += static_cast<std::uint64_t>(nal_size) + 4;
        probe.nals += 1;
        cursor += static_cast<std::uint64_t>(nal_size) + 4;
    }
    probe.ok = probe.au_bytes > 0 && saw_vcl;
    return probe;
}

std::optional<std::uint64_t> FileSize(const std::string& path) {
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(ToNativePath(path), ec);
    if (ec)
        return std::nullopt;
    return size;
}

} // namespace

RepairMdatWindow FindPrimaryMdatPayload(const std::string& path) {
    RepairMdatWindow window;
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in) {
        window.error = "无法打开文件做顶层盒扫描";
        return window;
    }
    const std::optional<std::uint64_t> size = FileSize(path);
    if (!size.has_value() || *size < 8) {
        window.error = "文件大小不可读或过小";
        return window;
    }
    std::uint64_t offset = 0;
    int boxes = 0;
    while (offset + 8 <= *size && boxes < kMaxTopLevelBoxes) {
        std::uint8_t header[16] = {};
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in.read(reinterpret_cast<char*>(header), 8)) {
            window.error = "读取顶层盒头失败";
            return window;
        }
        std::uint64_t box_size = Be32(header);
        const std::uint32_t type = Be32(header + 4);
        std::uint32_t header_size = 8;
        if (box_size == 1) {
            if (!in.read(reinterpret_cast<char*>(header), 8)) {
                window.error = "读取 largesize 失败";
                return window;
            }
            box_size = (static_cast<std::uint64_t>(Be32(header)) << 32) | Be32(header + 4);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = *size - offset; // 延伸到文件尾
        }
        if (box_size < header_size || offset + box_size > *size) {
            // 盒链已经损坏：这里不修，只把已知的 mdat 结论交出去。
            window.error = "顶层盒链在偏移 " + std::to_string(offset) + " 处不自洽（已扫 " + std::to_string(boxes) +
                           " 个盒），mdat 窗口可能不完整";
            return window;
        }
        if (type == 0x6D646174u) { // "mdat"
            if (!window.found) {
                window.found = true;
                window.payload_offset = offset + header_size;
                window.payload_size = box_size - header_size;
            } else {
                window.multiple_mdats = true;
            }
        }
        offset += box_size;
        ++boxes;
    }
    if (!window.found)
        window.error = "顶层盒链里没有 mdat";
    return window;
}

RepairBitstreamResyncResult ScanForRandomAccessPoints(const std::string& path, uint64_t window_start,
                                                      uint64_t window_end, const std::string& codec,
                                                      const RepairBitstreamResyncOptions& options,
                                                      const std::atomic<bool>* cancel, const RepairRunBudget* budget) {
    RepairBitstreamResyncResult result;
    result.attempted = true;
    result.codec = LowerAscii(codec);
    if (result.codec != "h264" && result.codec != "hevc") {
        result.attempted = false;
        result.error = "第一版码流级重同步只覆盖 H.264 / HEVC";
        return result;
    }
    if (window_end <= window_start) {
        result.attempted = false;
        result.error = "扫描窗口为空";
        return result;
    }

    const std::optional<std::uint64_t> file_size = FileSize(path);
    if (!file_size.has_value()) {
        result.error = "无法读取源文件大小";
        return result;
    }
    std::uint64_t start = window_start;
    std::uint64_t end = std::min(window_end, *file_size);
    if (start >= end) {
        result.error = "扫描窗口落在文件范围之外";
        return result;
    }
    if (end - start > options.max_scan_bytes) {
        end = start + options.max_scan_bytes;
        result.truncated_by_budget = true;
        result.limitations = "扫描窗口超过单次上限 " + std::to_string(options.max_scan_bytes) +
                             " 字节，只扫描了窗口前缀；结论不代表整段载荷";
    }

    ScanWindow window;
    window.file_offset = start;
    window.size = end - start;
    ChunkedByteSource src(path, window, options.chunk_bytes, budget, cancel);
    if (src.Size() == 0) {
        result.error = src.Error().empty() ? std::string("扫描窗口不可读") : src.Error();
        return result;
    }

    const std::uint64_t size = src.Size();
    const std::size_t block_capacity = options.chunk_bytes == 0 ? 64u * 1024u : options.chunk_bytes;
    std::vector<std::uint8_t> block(block_capacity + kOverlapBytes);
    const int max_candidates = options.max_candidates > 0 ? options.max_candidates : 8;
    const int max_nals = options.max_nals_per_chain > 0 ? options.max_nals_per_chain : 16;

    std::uint64_t pos = 0;
    while (pos < size && static_cast<int>(result.candidates.size()) < max_candidates) {
        if (budget != nullptr && !budget->Checkpoint()) {
            result.error = budget->Canceled() ? "扫描被取消" : "扫描超出共享预算";
            break;
        }
        if (cancel != nullptr && cancel->load()) {
            result.error = "扫描被取消";
            break;
        }
        const std::uint64_t want = std::min<std::uint64_t>(block.size(), size - pos);
        if (!src.Read(pos, block.data(), want)) {
            result.error = src.Error().empty() ? std::string("分块读取失败") : src.Error();
            break;
        }
        result.bytes_scanned += want;
        const std::uint64_t usable = want > kOverlapBytes ? want - kOverlapBytes : 0;
        if (usable == 0)
            break; // 剩余不足一个「长度前缀 + NAL 头」，不可能再有候选
        std::uint64_t i = 0;
        while (i + 6 <= usable && static_cast<int>(result.candidates.size()) < max_candidates) {
            const std::uint32_t nal_size = Be32(&block[static_cast<std::size_t>(i)]);
            if (nal_size < 2 || nal_size > kMaxNalBytes) {
                ++i;
                continue;
            }
            std::uint8_t type = 0;
            if (!ParseNalHead(result.codec, block[static_cast<std::size_t>(i) + 4], &type)) {
                ++i;
                continue;
            }
            // 只在参数集与随机访问点上起链：普通 P 帧起点对恢复没有价值，
            // 逐字节起链会把 CPU 全烧在这上面。
            if (!IsParamNal(result.codec, type) && !IsRapNal(result.codec, type)) {
                ++i;
                continue;
            }
            const ChainProbe probe = WalkChain(result.codec, src, pos + i, max_nals);
            if (!probe.ok || !probe.has_rap || (options.require_parameter_sets && !probe.has_params)) {
                ++i;
                continue;
            }
            RepairBitstreamRap rap;
            rap.byte_offset = src.AbsoluteOffset(pos + i);
            rap.au_bytes = probe.au_bytes;
            rap.has_parameter_sets = probe.has_params;
            rap.codec = result.codec;
            rap.evidence = "4 字节长度前缀链走通 " + std::to_string(probe.nals) + " 个 NAL（" +
                           std::to_string(probe.au_bytes) + " 字节）" +
                           (probe.has_params ? "，VCL 前含参数集" : "，未见参数集");
            result.candidates.push_back(std::move(rap));
            i += std::max<std::uint64_t>(probe.au_bytes, 1);
        }
        pos += usable;
    }
    if (result.candidates.empty() && result.error.empty()) {
        result.error = result.truncated_by_budget ? "已扫描窗口前缀，未找到含随机访问点的 NAL 链"
                                                  : "窗口内未找到含随机访问点的 NAL 链";
    }
    result.found = !result.candidates.empty();
    result.chunks_loaded = src.ChunksLoaded();
    return result;
}

} // namespace repair
} // namespace videoeye
