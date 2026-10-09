// 字幕文本 + SCTE-35 二进制段 fuzz 入口（阶段 3.3）。
//
// 覆盖：
//   SubtitleAnalyzer::ParseSrtText / ParseWebVttText / ParseAssText —— 纯文本，
//   不涉及文件 IO 与 FFmpeg；
//   Scte35Analyzer::ParseSection —— 二进制段级解析（含 CRC 校验路径）。
//
// 终态约束：只允许成功 / 失败（这几个接口没有取消参数；取消行为由确定性单测
// 覆盖）。不允许崩溃、无限循环或按输入声明分配无界内存 —— 后者由解析器内部
// 预算兜底，本入口再把输入卡在 1 MiB。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/diagnostics/Scte35Analyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"

namespace {

constexpr size_t kMaxInputBytes = 1u << 20; // 1 MiB

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > kMaxInputBytes)
        return 0;
    const std::string text(reinterpret_cast<const char*>(data), size);

    std::vector<videoeye::ParsedSubtitleCue> cues;
    videoeye::SubtitleAnalyzer::ParseSrtText(text, cues);
    videoeye::SubtitleAnalyzer::ParseWebVttText(text, cues);
    videoeye::SubtitleAnalyzer::ParseAssText(text, cues);

    videoeye::model::Scte35Cue scte35;
    videoeye::Scte35Analyzer::ParseSection(data, size, scte35);

    return 0;
}