#pragma once

#include <string>
#include <vector>

#include "core/domain/model/ColorHdrResult.h"
#include "core/analysis/AnalysisOptions.h"

// 只做前向声明，头文件保持与 FFmpeg 无关：
// AnalysisOptions.h / QcReport.h / QcRuleEngine.cpp / 单元测试都能直接包含本文件。
struct AVStream;
struct AVFrame;
struct AVPacket;
struct AVPacketSideData;

namespace videoeye {

// 结果类型（ColorHdrAnalysis / ColorKeyValueRow）住在
// core/domain/model/ColorHdrResult.h —— model 层要消费结果，但不能为了拿类型
// 反向包含分析器。这里做别名，既有调用方继续用 videoeye:: 前缀也不用改。
using model::ColorHdrAnalysis;
using model::ColorKeyValueRow;

// 注意: BuildColorRows / BuildHdrRows 于 2026-10 下放到了
// core/domain/model/ColorHdrResult.h —— 它们是纯展示逻辑(吃 domain 结果、吐
// ColorKeyValueRow), 留在分析器头上会让 UI 页面为了几行格式化被迫 include 分析器。
// 现在 UI / 报告请直接用 model::BuildColorRows / model::BuildHdrRows。

// 色彩与 HDR 元数据分析器
//
// 职责：把散落在 FFmpeg 各处的色彩/HDR 信息汇总成一份 ColorHdrAnalysis
//   1) AVCodecParameters: color_primaries / color_trc / color_space / color_range /
//                         format / profile / level / bits_per_raw_sample
//   2) AVStream / AVCodecParameters 的 coded_side_data:
//                         HDR10 (mastering display / content light level)、Dolby Vision 配置记录
//   3) AVPacket side data: 逐包补充上面两类（部分封装只在包上带）
//   4) 解码首帧的 AVFrame side data: HDR10+ / DV RPU / HDR Vivid / 环境光等动态元数据兜底
//
// 反复调用 UpdateFromXxx() 是安全的：已有字段不会被覆盖，后来的调用只补充缺失项。
// 因此 QtAnalysisController 可以在 find_stream_info 后、逐包扫描中、解码首帧后各调用一次。
class ColorHdrAnalyzer {
public:
    void Reset(const ColorHdrOptions& options = ColorHdrOptions{});
    const ColorHdrOptions& options() const { return options_; }

    void UpdateFromStream(const AVStream* stream);
    void UpdateFromPacket(const AVPacket* packet, int stream_index);
    void UpdateFromFrame(const AVFrame* frame);

    const ColorHdrAnalysis& Finish();
    const ColorHdrAnalysis& result() const { return result_; }
    bool finished() const { return finished_; }

    // 是否还有必要为补元数据去解一帧（供 QtAnalysisController 判断是否开解码器）
    bool NeedsFrameProbe() const;

private:
    // 从一段 AVPacketSideData 数组里提取 HDR 元数据，返回是否至少命中一项
    bool ScanSideData(const AVPacketSideData* side_data, int count, const std::string& source);
    void ApplyMasteringDisplay(const unsigned char* data, int size, const std::string& source);
    void ApplyContentLight(const unsigned char* data, int size, const std::string& source);
    void ApplyDolbyVision(const unsigned char* data, int size, const std::string& source);
    void ApplyPixelFormat(int pix_fmt, int bits_per_raw_sample);
    void AddSource(const std::string& source);
    void AddNote(const std::string& note);

    ColorHdrOptions options_;
    ColorHdrAnalysis result_;
    bool finished_ = false;
};

}  // namespace videoeye
