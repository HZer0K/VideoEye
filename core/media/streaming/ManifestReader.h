#pragma once

// 清单文件的读取层：分块读、可取消、有体积上限。
//
// 为什么要有这一层:
//   HLS / DASH 的 AnalyzeFile 以前都是一把梭 —— `std::ostringstream ss; ss << file.rdbuf();`
//   把整个清单一次读进内存。正常包（几百 KB）里看不出问题，但对着一个超大或恶意的清单，
//   三个缺陷会同时爆发：
//     1) 读的过程完全不可中断。用户在 UI 上点了"取消"，线程还在整块搬运数据；而
//        QtAnalysisController 要 join 旧线程才能起新扫描，于是表现为"取消没反应"。
//     2) 内存被同一份内容占两遍。HLS 侧拿到整块文本后还要切出 vector<string> 行数组，
//        于是"整块字符串"与"逐行数组"同时在堆上。
//     3) 没有统一的体积上限 —— 文件多大就敢读多大。
//   这一层一次解决三件事：按块读 → 每块前后检查取消 → 超过 max_bytes 立即停。
//   行模式下每行直接从块里切给调用方，中间不再生成整文件字符串。
//
// 为什么同时暴露"整块文本"和"逐行"两种模式:
//   HLS 是行格式（一条标签一行），行模式不必保留整块文本；
//   DASH 的 MPD 是 XML，扫描器按 '<' 逐标签前进，还要跨标签做栈配对，改成流式解析
//   的成本与收益不成正比，所以保留整块文本 —— 但同样受上限与取消约束。
//
// 依赖边界（与同目录的 ManifestText.h 一致）:
//   只用 C++17 标准库。不碰 Qt / FFmpeg；也不用 std::filesystem ——
//   在老一些的 GCC 上要多链一个 -lstdc++fs，为取个文件大小不值得。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "core/media/streaming/ManifestText.h"

namespace videoeye {
namespace utils {
namespace manifest {

// 清单读取的终态。调用方据此区分"文件有问题"与"用户叫停了两件事"。
enum class ManifestReadStatus {
    Ok,         // 完整读完，既没触上限也没被取消
    OpenFailed, // 打不开（不存在 / 没权限 / 是个目录）
    ReadFailed, // 打开成功了，但读的过程中流出错
    TooLarge,   // 超过 ManifestReadOptions::max_bytes，读到的内容不完整
    Cancelled,  // cancel 被置位，还没读完就停了
};

// 体积上限与分块粒度。两个解析器共用同一份默认值，避免"HLS 能读、DASH 读不动"。
struct ManifestReadOptions {
    // 单个清单文件的字节上限。默认 32 MiB：一个 5000 分片、每片 URL 100 字符的
    // media playlist 也就 500 KB 量级，32 MiB 装得下任何真实世界的清单，
    // 同时对"故意喂一个大文件"保持硬约束。
    uint64_t max_bytes = 32ULL * 1024ULL * 1024ULL;
    // 每次从磁盘读多少。64 KiB 是"取消响应粒度"与"系统调用次数"的折中：一个块的
    // 处理在微秒级，用户感知不到这个粒度；再小下去只是白给系统调用。
    size_t chunk_size = 64ULL * 1024ULL;
};

// 统一的错误描述。两个解析器各自拼串必然会用不同措辞描述同一种失败。
inline std::string ManifestReadErrorMessage(ManifestReadStatus status, const std::string& path, uint64_t max_bytes) {
    switch (status) {
    case ManifestReadStatus::OpenFailed:
        return "无法读取清单文件: " + path;
    case ManifestReadStatus::ReadFailed:
        return "清单文件读取中断（可能已损坏或不可读）: " + path;
    case ManifestReadStatus::TooLarge:
        return "清单文件超过大小上限 " + std::to_string(max_bytes) + " 字节，已放弃读取: " + path;
    case ManifestReadStatus::Cancelled:
        return "清单读取已取消: " + path;
    case ManifestReadStatus::Ok:
    default:
        return std::string();
    }
}

// UTF-8 BOM（EF BB BF）。有些 CDN / Windows 编辑器会在 m3u8 首行前面写这一串，
// 不剥掉的话第一行就不是 "#EXTM3U"，整个清单会被判成"不是 HLS"。
inline constexpr char kUtf8Bom[] = "\xEF\xBB\xBF";

inline bool StartsWithBom(const std::string& text) {
    return text.size() >= 3 &&
           static_cast<unsigned char>(text[0]) == 0xEF &&
           static_cast<unsigned char>(text[1]) == 0xBB &&
           static_cast<unsigned char>(text[2]) == 0xBF;
}

// 把已在内存里的整块文本切成行。
// 与 ReadManifestLines 用同一套行定义（'\n' 分隔、末尾的 '\r' 剥掉、末尾没有换行时
// 最后一段算一行、空文本产出空数组）—— 否则"文件读出来"和"字符串直接在手上"两条路
// 会给出两种切分结果，单测通过但真实跑起来行为不一致。
inline std::vector<std::string> SplitManifestLines(const std::string& text) {
    std::vector<std::string> lines;
    // 带 BOM 的要把这 3 个字节切掉再起头：
    // 不从 3 起的话，BOM 会被并进第一行，头一行就成了 "\xEF\xBB\xBF#EXTM3U"，
    // HLS 的头标记认不出来。不带 BOM 时仍从 0 起，别把真正的 "#EXT..." 吃掉。
    size_t begin = StartsWithBom(text) ? 3u : 0u;
    while (begin < text.size()) {
        const size_t nl = text.find('\n', begin);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        if (end > begin && text[end - 1] == '\r')
            --end;
        lines.push_back(text.substr(begin, end - begin));
        if (nl == std::string::npos)
            break;
        begin = nl + 1;
    }
    return lines;
}

namespace detail {

// 读取 + 切行的公共部分。sink 收到一行，返回 false 表示"够了"，收读提前停止。
//
// 之所以是模板而不是 std::function：这条循环会跑很多次， std::function 的间接调用
// 与可能的堆分配在这里没有收益，而调用方只有两处（push 进 vector / 直接丢弃）。
template <typename LineSink>
ManifestReadStatus ReadManifestLinesImpl(const std::string& path, const ManifestReadOptions& options,
                                         const std::atomic<bool>* cancel, LineSink sink) {
    // 先 stat 一遍：已知超限就没必要再做一次读 IO。stat 失败不算错误 —— 交给后面的
    // open 去给出 OpenFailed（目录、权限不足等情况 stat 也可能只是拿不到大小）。
    int64_t size = 0;
    if (FileSizeOf(path, size) && size > 0 && static_cast<uint64_t>(size) > options.max_bytes)
        return ManifestReadStatus::TooLarge;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return ManifestReadStatus::OpenFailed;

    const size_t chunk = options.chunk_size > 0 ? options.chunk_size : 65536u;
    std::vector<char> buffer(chunk);
    std::string pending; // 上一块末尾没凑齐一行的残部
    bool bom_done = false; // 首行 BOM 是否已剥离（只可能出现在整个文件最开头）
    uint64_t consumed = 0;
    bool too_large = false;
    bool cancelled = false;
    bool read_error = false;
    bool at_eof = false;
    bool sink_done = false;

    for (;;) {
        // 取消检查放在读之前："已经点了取消"就不该再产生任何磁盘 IO。
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            cancelled = true;
            break;
        }

        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();

        if (got > 0) {
            uint64_t allowed = static_cast<uint64_t>(got);
            if (consumed + allowed > options.max_bytes) {
                // 只读到上限为止。consumed 恒 <= max_bytes，这个减法不会下溢。
                allowed = options.max_bytes - consumed;
                too_large = true;
            }
            pending.append(buffer.data(), static_cast<size_t>(allowed));
            consumed += allowed;

            // 首行要单独处理：BOM 只可能出现在文件最开头，而它往往和第一行挨着，
            // 等切行轮到它时已经被并进第一行里了。先剥掉再走正常切行。
            if (!bom_done && StartsWithBom(pending)) {
                pending.erase(0, 3);
                bom_done = true;
            }

            size_t begin = 0;
            size_t nl = 0;
            while ((nl = pending.find('\n', begin)) != std::string::npos) {
                size_t end = nl;
                if (end > begin && pending[end - 1] == '\r')
                    --end;
                if (!sink(pending.substr(begin, end - begin))) {
                    sink_done = true;
                    break;
                }
                begin = nl + 1;
            }
            // 每次都从开头抹掉已经切出去的部分，pending 因此始终只有"一块 + 最长那行"
            // 那么大，不会随文件大小增长。
            pending.erase(0, begin);
            if (sink_done)
                break;
            if (too_large)
                break;
        }

        if (file.bad()) {
            read_error = true;
            break;
        }
        if (file.eof()) {
            at_eof = true;
            break;
        }
    }

    // 文件结尾（且没触上限、没被取消）时，最后一段没有换行的内容也算一行。
    if (at_eof && !pending.empty()) {
        size_t end = pending.size();
        if (end > 0 && pending[end - 1] == '\r')
            --end;
        sink(pending.substr(0, end));
    }

    if (read_error)
        return ManifestReadStatus::ReadFailed;
    if (cancelled)
        return ManifestReadStatus::Cancelled;
    if (too_large)
        return ManifestReadStatus::TooLarge;
    return ManifestReadStatus::Ok;
}

} // namespace detail

// 逐行读出一个清单文件。每行不含 '\n'，行尾 '\r' 已剥掉，结果追加进 lines。
//
// 与"先把整个文件读成 std::string 再切"相比：峰值内存从"整块文本 + 行数组"
// 降到"行数组 + 一个块"，并且在读的过程中就能响应取消。
inline ManifestReadStatus ReadManifestLines(const std::string& path, const ManifestReadOptions& options,
                                            const std::atomic<bool>* cancel, std::vector<std::string>& lines) {
    return detail::ReadManifestLinesImpl(path, options, cancel, [&lines](const std::string& line) {
        lines.push_back(line);
        return true;
    });
}

// 流式的读取原语：一边按块读，一边把切出来的行交给 on_line —— 内存里从不会有整份清单。
//
// on_line 返回 false 表示"够了"，读取随即收尾；这是调用方主动的提前结束，不算失败，
// 所以返回的仍是 Ok（除非同时也触到了上限或取消）。
// ReadManifestLines / ReadManifestText 都建立在这之上；连行数组都不想持有的调用方
// （比如将来真正逐行解析清单）可以直接用它。
template <typename LineSink>
ManifestReadStatus ForEachManifestLine(const std::string& path, const ManifestReadOptions& options,
                                       const std::atomic<bool>* cancel, LineSink on_line) {
    return detail::ReadManifestLinesImpl(path, options, cancel, on_line);
}

// 整块读出一个清单文件（保留原始换行符）。给按字符扫描的解析器（DASH MPD）用。
// 同样是分块读 + 每块检查取消 + 体积上限，只是不对内容做切分。
inline ManifestReadStatus ReadManifestText(const std::string& path, const ManifestReadOptions& options,
                                           const std::atomic<bool>* cancel, std::string& text) {
    int64_t size = 0;
    if (FileSizeOf(path, size) && size > 0 && static_cast<uint64_t>(size) > options.max_bytes)
        return ManifestReadStatus::TooLarge;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return ManifestReadStatus::OpenFailed;

    const size_t chunk = options.chunk_size > 0 ? options.chunk_size : 65536u;
    std::vector<char> buffer(chunk);
    uint64_t consumed = 0;
    bool too_large = false;

    for (;;) {
        if (cancel != nullptr && cancel->load(std::memory_order_acquire))
            return ManifestReadStatus::Cancelled;

        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) {
            uint64_t allowed = static_cast<uint64_t>(got);
            if (consumed + allowed > options.max_bytes) {
                allowed = options.max_bytes - consumed;
                too_large = true;
            }
            text.append(buffer.data(), static_cast<size_t>(allowed));
            consumed += allowed;
            // 带 BOM 的清单：首块（首个请求）拿到的数据最前面就是 BOM，
            // 不剥掉的话 DASH 那一侧扫到的第一个字符是 0xEF，XML/标签全对不上。
            // 极端情况下 BOM 被拆到两个块里，这里只保证最常见的"整块内"能吃掉。
            if (StartsWithBom(text)) text.erase(0, 3);
            if (too_large)
                return ManifestReadStatus::TooLarge;
        }
        if (file.bad())
            return ManifestReadStatus::ReadFailed;
        if (file.eof())
            break;
    }
    return ManifestReadStatus::Ok;
}

} // namespace manifest
} // namespace utils
} // namespace videoeye
