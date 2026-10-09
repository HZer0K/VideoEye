// 码流参数集 / extradata 封装 fuzz 入口（阶段 3.3）。
//
// 覆盖两条链路：
//   1) BitstreamAnalyzer::Analyze —— 按 AVCodecID 分发到 H.264 / HEVC / AV1
//      解析器（不设置容器 metadata：这里只关心"裸参数集不许崩 / 不许无界"）；
//   2) ExtradataParser::Parse / ExtractAnnBNalUnits / ExtractObuUnits ——
//      封装格式探测（Annex B / avcC / hvcC / vvcC / av1C）与 NAL/OBU 抽取。
//
// 终态约束：只允许成功 / 失败；（这里没有取消参数，fuzz 场景不触发取消路径，
// 取消行为由确定性单测覆盖）。不允许崩溃、无限循环或按输入声明分配无界内存 ——
// 后者由各解析器内部的 ParserLimits 预算兜底，本入口再把输入卡在 1 MiB。

#include <cstddef>
#include <cstdint>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/media/codec/ExtradataParser.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

constexpr size_t kMaxInputBytes = 1u << 20; // 1 MiB

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxInputBytes)
        return 0;

    videoeye::BitstreamAnalyzer analyzer;
    analyzer.Analyze(data, size, AV_CODEC_ID_H264);
    analyzer.Analyze(data, size, AV_CODEC_ID_HEVC);
    analyzer.Analyze(data, size, AV_CODEC_ID_AV1);

    // 返回的 ExtradataResult 有意丢弃：fuzz 的目标是"边界安全"，
    // 结果正确性由 tests/corpus/ 的确定性断言钉住。
    videoeye::ExtradataParser::Parse(data, size);
    videoeye::ExtradataParser::ExtractAnnBNalUnits(data, size);
    videoeye::ExtradataParser::ExtractObuUnits(data, size);

    return 0;
}