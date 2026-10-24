#pragma once

// 分析维度的"证据与覆盖"状态（阶段 5：让结论说得清覆盖了多少内容）。
//
// 背景问题（2026-10 审计）：
//   * AnalysisResult 上只有 7 个平行的 *_analyzed 布尔，命名与语义各不相同
//     （analyzed / valid / has_data / applicable），报告层只能一个一个 if 猜；
//   * "跑完了没问题" 与 "根本没跑" 在结构上无法区分 —— 前者 analyzed=true、
//     后者也常常是 false，但没有任何字段说明**为什么**没跑；
//   * 抽样扫描（命中 max_packets）只把 scan_status 改成 Sampled，
//     上限是多少、覆盖了整段的百分之多少，结果对象里一个字都没留。
//
// 本结构把这些收成一张表：每个维度一行，回答四个问题：
//   1. 跑了没有、跑到什么程度   —— executed / capability
//   2. 扫了多少、总量多少       —— units_scanned / units_total / coverage_ratio
//   3. 有没有失败、失败多少     —— failures / failure_reason
//   4. 结论能不能当"通过"用     —— conclusive
//
// 约束：本文件纯 C++17，不依赖 Qt / FFmpeg（与其它 domain/model 保持一致），
// 便于单测直接构造，也便于报告层与 UI 共用同一份口径。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/AnalysisCapability.h"

namespace videoeye {
namespace model {

// 分析维度。取值一旦发布不要改名（JSON 的 dimension 字段就是它）。
enum class AnalysisDimension : uint8_t {
    FileProbe = 0,      // 文件/容器探测（格式、时长、流表）
    ContainerStructure, // 容器结构（box/atom/包级结构）
    VideoBitstream,     // 视频参数集（extradata / 带内）
    FrameStructure,     // 帧级结构（帧类型、GOP、slice/SEI）
    ParameterTimeline,  // 参数集随时间变化
    BitrateGop,         // 码率与 GOP 统计
    ColorHdr,           // 色彩与 HDR
    AudioQc,            // 音频 PCM QC（响度/峰值/静音/相位）
    AudioBitstream,     // 音频位流头
    Subtitle,           // 字幕
    Timecode,           // 时码/章节
    AuxiliaryData,      // 辅助数据（SEI/SCTE-35 等）
    Mp4SampleTable,     // MP4 样本表
    TsTransport,        // MPEG-TS 传输层
    Streaming,          // HLS/DASH 流媒体包
    LegacyContainer,    // AVI/FLV/ASF/OGG 体检
    // 帧级显示顺序（POC 重建）。与 FrameStructure 分开：后者靠包/巡检统计"帧类型与
    // slice"，这条靠逐 slice 的 POC 回答"显示顺序怎么排、GOP 边界在哪、POC 有没有
    // 重复或缺失"。两者开关不同（analyze_bitstream vs analyze_frame_order），
    // 支持范围也不同（FrameStructure 覆盖多种编码，POC 目前只有 H.264）。
    // 追加在末尾（Count 之前）：已发布的取值不能改名/改编号。
    FrameOrder,
    Count
};

// 稳定的机器可读码（JSON / 规则 id 用）
const char* DimensionCode(AnalysisDimension dimension);
// 人类可读名称（中文，UI 与报告直接展示）
const char* DimensionName(AnalysisDimension dimension);

// 一个维度的证据与覆盖
struct DimensionStatus {
    AnalysisDimension dimension = AnalysisDimension::FileProbe;

    // 是否真的执行过（false = 选项关闭 / 输入不适用 / 依赖缺失）
    bool executed = false;
    AnalysisCapability capability = AnalysisCapability::Unavailable;

    // ---- 扫描范围 ----
    // units 的含义由 scope_unit 说明："包" / "帧" / "分片" / "样本" / "轨"。
    std::string scope_unit;
    int64_t units_scanned = 0;
    // 0 = 总量未知（例如裸流不知道总帧数）。不要用 0 冒充"全覆盖"。
    int64_t units_total = 0;
    // 只有 units_total > 0 时才成立
    bool has_coverage = false;
    double coverage_ratio = 0.0;

    // ---- 失败与降级 ----
    int64_t failures = 0;
    // 非空 = 该维度的主要限制（"没扫全"/"没拿到参数集"/"编码未实现"）
    std::string failure_reason;
    // 该维度覆盖了哪些流（空 = 不按流区分）
    std::vector<int> stream_indices;
    std::vector<std::string> notes;

    // 结论能否当"已通过"用。为了不制造假通过，默认 false：
    // 只有 executed && capability==Supported && 无失败 && 覆盖达标 才置 true，
    // 由 MarkConclusive() 统一判定，调用方不要手工置位。
    bool conclusive = false;

    void MarkConclusive();

    // 覆盖率百分比（0..100）；无覆盖信息时返回 -1（不要返回 0 冒充）
    double CoveragePercent() const;
    // 一行人类可读摘要，直接进报告/UI tooltip
    std::string Summary() const;
};

// ---- 便捷构造（Assembler 里逐维度填）----

// 由"没执行"的原因生成一行：capability=Unavailable + failure_reason
DimensionStatus MakeSkippedDimension(AnalysisDimension dimension, const std::string& reason);

// 由"执行了但失败"生成：capability=Failed + failures/reason
DimensionStatus MakeFailedDimension(AnalysisDimension dimension, const std::string& reason, int64_t failures = 0);

} // namespace model
} // namespace videoeye
