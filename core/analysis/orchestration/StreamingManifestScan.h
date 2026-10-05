#pragma once

// 流媒体清单（.m3u8 / .mpd）的分析路径。
//
// 为什么是一条独立路径: FFmpeg 会把清单当成 HLS / DASH **播放列表**去发网络请求，
// 离线分析既不可控（卡在网络超时），也拿不到有意义的时长 / 码率序列。所以引擎在
// 打开任何 IO 之前就分流到这里，全程不碰 avformat —— 这条路径可以完全离线单测。
//
// 与逐包扫描的关系: 两者都写同一个 model::AnalysisResult，但输入、取消轮询点、
// 终态分支完全不同（清单是"解析 + 分片校验"两阶段三态），混在一个函数里的话
// "取消"要在两套循环里各判一次，迟早漏一处。
//
// 名字里的 Scan 而不是 Analyzer: core/analysis/streaming/ 下已经有
// DashManifestAnalyzer / HlsManifestAnalyzer / SegmentQcAnalyzer 三个**解析器**，
// 本类是它们之上的流程编排，两者不是同一层东西。

#include <atomic>
#include <string>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace analyzer {

class StreamingManifestScan {
public:
    // cancel_source: 本次 Run 实际使用的那颗取消标志。清单解析的逐行 / 逐分片 /
    //   逐时间轴条目循环，以及分片落盘探测，都会轮询它 —— 没有这条通道时，
    //   一个几十万行 EXTINF 的 m3u8 会让"取消"和"重新扫描"都点不动。
    //   引擎的 CancelRequested() 与 CancelSource() 指向的就是同一颗，所以判"被取消"
    //   直接读它即可，不必再回问引擎。
    static void Run(const std::string& file_path, const AnalysisOptions& options,
                    model::AnalysisResult& result, const AnalysisCallbacks& callbacks,
                    const std::atomic<bool>* cancel_source);

private:
    static bool CancelRequested(const std::atomic<bool>* cancel_source);
};

}  // namespace analyzer
}  // namespace videoeye
