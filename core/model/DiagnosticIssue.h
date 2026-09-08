#pragma once

#include <string>
#include <vector>

#include "core/model/TimeRange.h"

namespace videoeye {
namespace model {

// 问题严重度
enum class IssueSeverity {
    Info,      // 提示: 不影响播放，供优化参考
    Warning,   // 警告: 可能影响体验/兼容性
    Error,     // 错误: 明确不符合规范或会导致播放异常
    Critical   // 致命: 文件基本不可用
};

// 问题类别（与侧边栏分析维度对齐）
enum class IssueCategory {
    Container,  // 容器/封装
    Video,      // 视频编码与画面
    Audio,      // 音频编码与声道
    Timing,     // 时间戳/同步
    Bitrate,    // 码率
    Gop,        // GOP / 关键帧
    Metadata,   // 元数据/标签
    Other
};

const char* ToString(IssueSeverity severity);
const char* ToString(IssueCategory category);

// 单条诊断问题。规则引擎的唯一输出单元，UI 表格与报告导出都基于此结构。
struct DiagnosticIssue {
    std::string rule_id;        // 触发的规则 id，如 "video.gop.max_seconds"
    std::string title;          // 简短标题
    std::string detail;         // 详细说明（含实测值/阈值）
    std::string suggestion;     // 修复建议
    IssueSeverity severity = IssueSeverity::Info;
    IssueCategory category = IssueCategory::Other;
    TimeRange range;            // 发生位置（全局问题用 TimeRange::Global()）
    int stream_index = -1;      // 关联流（-1 表示文件级）
    double metric_value = 0.0;  // 实测值
    double threshold = 0.0;     // 触发阈值
    int occurrence_count = 1;   // 同类问题合并后的出现次数

    bool IsGlobal() const { return range.IsGlobal(); }
    std::string SeverityText() const { return ToString(severity); }
    std::string CategoryText() const { return ToString(category); }
};

} // namespace model
} // namespace videoeye
