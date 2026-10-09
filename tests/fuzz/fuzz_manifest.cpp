// HLS / DASH 清单文本 fuzz 入口（阶段 3.3）。
//
// 两个解析器都有纯内存的 ParseText：不落盘、不联网 —— HLS 侧显式关掉
// load_sub_playlists，杜绝 fuzz 通过 master 清单触发递归读文件；DASH 侧
// ParseText 本身只做标签扫描（BaseURL 只登记不下载）。输入即清单文本。
//
// 终态约束：只允许成功 / 失败 / 取消（取消标志恒假，让带取消参数的实现路径
// 被编译并执行到）。不允许崩溃、无限循环或按输入声明分配无界内存 —— 后者由
// 各解析器的 max_* 预算兜底（见 ParserLimits / HlsManifestOptions /
// DashManifestOptions），本入口再把输入卡在 1 MiB。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"

namespace {

constexpr size_t kMaxInputBytes = 1u << 20; // 1 MiB
const std::atomic<bool> kNeverCancel{false};
// 相对 URI 的解析基准。fuzz 不触发文件 IO（HLS 子清单已关、DASH 只扫描），
// 这个值只影响 URI 拼接结果，不影响内存安全。
const char* const kBaseDir = ".";

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxInputBytes)
        return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);

    videoeye::HlsManifestOptions hls_options;
    hls_options.load_sub_playlists = false; // fuzz 只解析这份文本，不递归读子清单
    videoeye::model::StreamingPackageResult hls;
    videoeye::HlsManifestAnalyzer::ParseText(text, kBaseDir, hls, hls_options, &kNeverCancel);

    videoeye::model::StreamingPackageResult dash;
    videoeye::DashManifestAnalyzer::ParseText(text, kBaseDir, dash, videoeye::DashManifestOptions{}, &kNeverCancel);

    return 0;
}