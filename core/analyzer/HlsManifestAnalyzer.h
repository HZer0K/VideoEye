#pragma once

// HLS (m3u8) 清单解析器
//
// 职责:
//   1) 解析 master playlist（EXT-X-STREAM-INF / EXT-X-MEDIA）与 media playlist
//      （EXT-X-TARGETDURATION / EXTINF / EXT-X-MAP / EXT-X-DISCONTINUITY /
//       EXT-X-KEY / EXT-X-PART / EXT-X-BYTERANGE）；
//   2) 产出 StreamingPackageResult 里的 variants / renditions / playlists / segments；
//   3) Validate() 做 HLS 侧的清单级校验（时长越界、抖动、discontinuity 配对、
//      初始化段缺失、加密标签、LL-HLS 部分分片）。
//
// 依赖边界（与 Mp4SampleTableAnalyzer 一致）:
//   - 头文件不依赖 Qt / FFmpeg，只用 C++17 标准库，纯逻辑可单测；
//   - 只读本地文件（std::ifstream）。绝不把 .m3u8 交给 avformat_open_input ——
//     FFmpeg 会当 HLS 播放列表去发网络请求，离线 QC 场景不可控也无法单测。
//   - 网络 URI 只登记不下载（第二阶段再做 Qt Network + Range 采样）。
//
// 规模保护: 全部列表都有 max_* 上限，超限置 truncated 后停止收录。

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/StreamPackageInfo.h"

namespace videoeye {
namespace analyzer {

// 注意: 带默认成员初始化器的结构体必须定义在 namespace 作用域。
// 嵌进类里 GCC 会报 "default member initializer ... required before the end of
// its enclosing class"（MSVC 不报，但 CI 有 Linux）。
struct HlsManifestOptions {
    uint32_t max_variants = 64;                 // master playlist 最多收多少条 variant
    uint32_t max_playlists = 64;                // 最多加载多少个子播放列表
    uint32_t max_segments_per_playlist = 5000;  // 单个 media playlist 最多收多少完整分片
    uint32_t max_partials_per_playlist = 512;   // EXT-X-PART 最多收多少
    bool load_sub_playlists = true;             // master 是否递归加载子播放列表
    double target_duration_tolerance_s = 0.001; // EXTINF 与目标时长的比较容差（秒）
    double duration_jitter_ratio = 0.25;        // (max-min)/mean 超过即判为时长抖动
};

class HlsManifestAnalyzer {
public:
    HlsManifestAnalyzer();
    ~HlsManifestAnalyzer();

    // 解析 + 校验。返回 true 表示清单被成功解析（不代表没有问题）。
    bool AnalyzeFile(const std::string& file_path, model::StreamingPackageResult& out,
                     const HlsManifestOptions& options = HlsManifestOptions{});

    // 从文本解析（单测与网络阶段共用）。base_dir 是相对 URI 的解析基准。
    static bool ParseText(const std::string& text, const std::string& base_dir, model::StreamingPackageResult& out,
                          const HlsManifestOptions& options = HlsManifestOptions{});

    // 纯逻辑校验：在 out 上补齐 HLS 侧 issues。幂等（只删自己产出的 code 再重加）。
    static void Validate(model::StreamingPackageResult& out, const HlsManifestOptions& options = HlsManifestOptions{});

    void Reset();

private:
    HlsManifestOptions options_;
};

} // namespace analyzer
} // namespace videoeye
