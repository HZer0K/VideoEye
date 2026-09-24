#pragma once

// DASH (MPD) 清单解析器
//
// 职责:
//   1) 解析 MPD XML：Period / AdaptationSet / Representation / SegmentTemplate /
//      SegmentTimeline / SegmentBase / SegmentList / BaseURL；
//   2) 把 <S t= d= r=/> 展开成 SegmentInfo 序列；
//   3) Validate() 做 DASH 侧校验（SegmentTimeline 缺口与重叠、缺少分片定位信息）。
//
// 依赖边界（与 HlsManifestAnalyzer 一致）:
//   - 头文件不依赖 Qt / FFmpeg / 第三方 XML 库，只用 C++17 标准库。
//     项目硬依赖只有 Qt Widgets + FFmpeg，不为一个清单解析引入 libxml2。
//     MPD 是结构固定的 XML，一个极简的标签扫描器就够用（不处理 CDATA / 实体 / DTD）。
//   - 只读本地文件；远程 URI 只登记不下载。

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/StreamPackageInfo.h"

namespace videoeye {
namespace analyzer {

// 见 HlsManifestOptions 的注释：带 NSDMI 的结构体必须在 namespace 作用域。
struct DashManifestOptions {
    uint32_t max_periods = 16;
    uint32_t max_adaptation_sets_per_period = 32;
    uint32_t max_representations = 64;
    uint32_t max_segments_per_representation = 5000;
    uint32_t max_timeline_entries = 20000;
    // SegmentTimeline 相邻条目允许的时间误差（timescale 单位）。
    // 给 1 是为了容忍 t 的整数舍入，不给会让正常的静态 MPD 大量误报。
    uint64_t timeline_tolerance = 1;
    bool expand_segments = true; // 是否把 <S> / duration 展开成 SegmentInfo
};

class DashManifestAnalyzer {
public:
    DashManifestAnalyzer();
    ~DashManifestAnalyzer();

    bool AnalyzeFile(const std::string& file_path, model::StreamingPackageResult& out,
                     const DashManifestOptions& options = DashManifestOptions{});

    static bool ParseText(const std::string& text, const std::string& base_dir, model::StreamingPackageResult& out,
                          const DashManifestOptions& options = DashManifestOptions{});

    // 纯逻辑校验：在 out 上补齐 DASH 侧 issues。幂等（只删自己产出的 code 再重加）。
    static void Validate(model::StreamingPackageResult& out,
                         const DashManifestOptions& options = DashManifestOptions{});

    void Reset();

private:
    DashManifestOptions options_;
};

} // namespace analyzer
} // namespace videoeye
