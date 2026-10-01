#pragma once

// 流媒体包的分片级 / ladder 级交叉校验
//
// 与 HlsManifestAnalyzer、DashManifestAnalyzer 的分工：
//   那两个负责"把清单读出来"，这个负责"读出来之后横向比对"：
//     - 多码率 ladder 的分辨率 / 编码 / 声明码率是否一致；
//     - 各码率的关键帧（分片起点）时间轴是否对齐 —— ABR 切换的硬要求；
//     - 音视频分片数量是否对得上；
//     - 分片文件是否真的在磁盘上、能否被已有容器分析器解析。
//   协议无关的比对逻辑全在这里，HLS 与 DASH 共用一份实现。
//
// 依赖边界：
//   - 头文件不依赖 Qt / FFmpeg；
//   - fMP4/CMAF 分片走 Mp4SampleTableAnalyzer（自研 IsobmffParser，纯 C++）；
//     MPEG-TS 分片的逐包解析要用到 TsStructureAnalyzer（依赖 Qt），
//     由 ContainerStructureAnalyzer 在 Qt 侧补做，不进这个纯 C++ 层。

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/container/Mp4SampleTableAnalyzer.h"
#include "core/domain/model/StreamPackageInfo.h"
#include "core/analysis/AnalysisOptions.h"

namespace videoeye {
namespace analyzer {

class SegmentQcAnalyzer {
public:
    // 探测 + 建 ladder + 校验。result 必须已经由 Hls/Dash 解析器填好数据。
    // cancel: 可选的取消标志（nullptr = 不关心取消）。分片落盘探测是逐文件的磁盘 IO，
    // 大型包（上千分片）在这里耗时最久，必须能被中断。
    static bool Analyze(model::StreamingPackageResult& result, const SegmentQcOptions& options = SegmentQcOptions{},
                        const std::atomic<bool>* cancel = nullptr);

    // 落盘探测：分片是否存在、多大、fMP4 分片的 tfdt 起点（关键帧时间）。
    static bool ProbeSegments(model::StreamingPackageResult& result,
                              const SegmentQcOptions& options = SegmentQcOptions{},
                              const std::atomic<bool>* cancel = nullptr);

    // 把 variants / representations 投影成统一的 ladder（UI 只认这个）
    static void BuildLadder(model::StreamingPackageResult& result);

    // 纯逻辑校验：在 result 上补齐跨分片 / 跨码率的 issues。幂等。
    static void Validate(model::StreamingPackageResult& result, const SegmentQcOptions& options = SegmentQcOptions{},
                         const std::atomic<bool>* cancel = nullptr);

private:
    SegmentQcAnalyzer() = delete;
};

} // namespace analyzer
} // namespace videoeye
