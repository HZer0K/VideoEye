#pragma once

#include <string>
#include <vector>

#include "core/model/ColorInfo.h"
#include "core/model/HdrMetadataInfo.h"

// 只做前向声明，头文件保持与 FFmpeg 无关：
// AnalysisTask.h / QcReport.h / QcRuleEngine.cpp / 单元测试都能直接包含本文件。
struct AVStream;
struct AVFrame;
struct AVPacket;
struct AVPacketSideData;

namespace videoeye {
namespace analyzer {

// 色彩与 HDR 元数据分析选项
struct ColorHdrOptions {
    // 容器/码流层面拿不到 HDR 元数据时，是否解码首帧读取 AVFrame side data。
    // 静态元数据（SMPTE ST 2086 / MaxCLL-MaxFALL）通常写在容器盒或 SEI 里，
    // 不需要解码；HDR10+/DV 的动态元数据则多数只在解码后的帧上出现。
    bool probe_decoded_frame = true;

    // 最多往后解几帧去找动态元数据（通常 1 帧就够，动态元数据一般都有每帧副本）
    int max_probe_frames = 2;
};

// 一条视频流的色彩 + HDR 分析结果
struct ColorHdrAnalysis {
    bool analyzed = false;
    int stream_index = -1;

    model::ColorInfo color;
    model::HdrMetadataInfo hdr;

    std::vector<std::string> notes;  // 降级/来源说明

    // 静态元数据已齐备 -> 不必再为补元数据去解码帧
    bool StaticMetadataComplete() const;

    std::string ToString() const;
};

// UI 表格 / 报告导出共用的三列结构（项目 / 值 / 说明）
struct ColorKeyValueRow {
    std::string key;
    std::string value;
    std::string note;
};

std::vector<ColorKeyValueRow> BuildColorRows(const ColorHdrAnalysis& analysis);
std::vector<ColorKeyValueRow> BuildHdrRows(const ColorHdrAnalysis& analysis);

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
// 因此 AnalysisCoordinator 可以在 find_stream_info 后、逐包扫描中、解码首帧后各调用一次。
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

    // 是否还有必要为补元数据去解一帧（供 AnalysisCoordinator 判断是否开解码器）
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

}  // namespace analyzer
}  // namespace videoeye
