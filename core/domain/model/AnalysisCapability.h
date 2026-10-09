#pragma once

// 分析能力状态的统一口径（2026-10 阶段 4：统一能力状态，消除"假完成"）。
//
// 背景：部分功能在文档和 UI 里"看起来支持"，实际只是部分实现。之前结果里
// 只有 analyzed 一个 bool，"没 extradata"、"编码不支持"、"解析失败"、
// "只识别出类型"全部折叠成同一个状态，上层无法区分也就无法诚实展示。
//
// 两层状态：
//   * AnalysisCapability     —— 粗粒度：这项能力整体处于什么水平（给 UI/报告）
//   * BitstreamParseOutcome   —— 细粒度：这一次解析具体发生了什么（给排查）
//
// 映射关系（BitstreamAnalyzer 负责落值）：
//   NoExtradata      -> Unavailable   流没有 extradata，无从解析
//   UnsupportedCodec -> Unavailable   编码格式不在支持列表
//   TypeOnly         -> Partial       只识别出编码类型，没有可信字段
//   ParseFailed      -> Failed        有参数集输入但解析失败（截断/畸形）
//   FullParse        -> Supported     参数集解析成功且该编码的能力已验证
//                     （VVC 例外：解析能跑通，但畸形码流验证不充分，
//                       按方案 A 降为 Partial，partial=true）

#include <cstdint>

namespace videoeye {
namespace model {

// 分析能力状态（按编码/功能维度，粗粒度）。
enum class AnalysisCapability : uint8_t {
    Supported,    // 完整实现且有测试覆盖，字段可信
    Partial,      // 部分实现：只识别类型 / 字段解析可用但验证不充分 / 数据源不全
    Unavailable,  // 该输入/该构建下没有此能力（无 extradata、编码不支持、依赖缺失）
    Failed        // 尝试过但失败（输入存在，解析报错或产出不合理）
};

// 码流解析的细粒度结果状态。
enum class BitstreamParseOutcome : uint8_t {
    NotAnalyzed,      // 未尝试解析（结果还没生成）
    NoExtradata,      // 流没有 extradata（参数集只可能在码流内，容器未携带）
    UnsupportedCodec, // 编码格式不在支持列表（H.264/HEVC/AV1/VVC 之外）
    TypeOnly,         // 只识别出编码类型：extradata 里没有可解析的参数集
    ParseFailed,      // 参数集存在但解析失败（截断、畸形、字段超出合理范围）
    FullParse         // 完整解析成功，参数集字段可信
};

// 供 JSON 导出与日志使用的枚举名（与字段值一一对应，不要改动已有取值）。
inline const char* CapabilityToString(AnalysisCapability c) {
    switch (c) {
        case AnalysisCapability::Supported:   return "supported";
        case AnalysisCapability::Partial:     return "partial";
        case AnalysisCapability::Unavailable: return "unavailable";
        case AnalysisCapability::Failed:     return "failed";
    }
    return "unavailable";
}

inline const char* ParseOutcomeToString(BitstreamParseOutcome o) {
    switch (o) {
        case BitstreamParseOutcome::NotAnalyzed:     return "not_analyzed";
        case BitstreamParseOutcome::NoExtradata:     return "no_extradata";
        case BitstreamParseOutcome::UnsupportedCodec: return "unsupported_codec";
        case BitstreamParseOutcome::TypeOnly:        return "type_only";
        case BitstreamParseOutcome::ParseFailed:      return "parse_failed";
        case BitstreamParseOutcome::FullParse:       return "full_parse";
    }
    return "not_analyzed";
}

} // namespace model
} // namespace videoeye
