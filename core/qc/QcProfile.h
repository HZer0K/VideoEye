#pragma once

// QC 模板（profile）
//
// 为什么需要它：默认规则集 DefaultQcRules() 是一套"什么都查一遍"的中庸配置，
// 而实际交付场景的差别很大 —— 短视频平台听 -14 LUFS、电视台按 EBU R128 -23 LUFS、
// HLS VOD 关心分片与码率阶梯、归档母版连时码和色彩标注都不能少。
// 把这套差异固化成可序列化的 JSON 模板，CLI / UI / CI 三方才能共用同一套判定口径。
//
// 存储格式（version = 1）：
//   {
//     "version": 1,
//     "id": "hls-vod",
//     "name": "HLS VOD",
//     "description": "...",
//     "analysis_depth": "standard",          // fast | standard | deep
//     "rules": {
//       "video.gop.max_seconds": {"enabled": true, "threshold": 6.0, "severity": "error"}
//     }
//   }
//
// 设计取舍：模板只存"与默认规则不同的部分"（overrides），而不是整份规则表。
// 这样内置规则以后加字段、改文案，老模板照样能用；也不会把 70 条规则复制得到处都是。

#include <string>
#include <vector>

#include "core/analyzer/AnalysisTask.h"
#include "core/model/DiagnosticIssue.h"
#include "core/model/QcRule.h"

namespace videoeye {
namespace qc {

// 分析强度档位。直接对应 AnalysisOptions 的开关组合：
//   Fast     只跑不解码/不重采样的统计（毫秒级，适合批量初筛）
//   Standard 加上音频解码（响度/真峰值）与码流解析（默认档）
//   Deep     再加视频解码逐帧判帧类型，长文件会明显变慢
enum class QcAnalysisDepth {
    Fast,
    Standard,
    Deep,
};

const char* ToString(QcAnalysisDepth depth);
bool ParseQcAnalysisDepth(const std::string& text, QcAnalysisDepth& out);

// 严重度的文本形式：为兼容手写模板，中英文都收。
std::string ToStableString(model::IssueSeverity severity);
bool ParseIssueSeverity(const std::string& text, model::IssueSeverity& out);

// 单条规则的覆盖项。每个字段单独带 has_* 标记 —— "没写"和"写了默认值"是两回事。
struct QcRuleOverride {
    std::string id;
    bool has_enabled = false;
    bool enabled = true;
    bool has_threshold = false;
    double threshold = 0.0;
    bool has_severity = false;
    model::IssueSeverity severity = model::IssueSeverity::Warning;
};

struct QcProfile {
    std::string id;
    std::string name;
    std::string description;
    int version = 1;
    QcAnalysisDepth depth = QcAnalysisDepth::Standard;
    std::vector<QcRuleOverride> overrides;

    bool IsBuiltin() const;
};

// ---- 内置模板 ----
// 顺序即 UI 下拉框顺序。首项为通用，其余按交付场景排列。
std::vector<QcProfile> BuiltinQcProfiles();
const QcProfile* FindBuiltinQcProfile(const std::string& id);

// ---- 序列化 ----
std::string SerializeQcProfile(const QcProfile& profile, bool pretty = true);
bool ParseQcProfileJson(const std::string& json, QcProfile& out, std::string& error);

bool SaveQcProfileToFile(const std::string& path, const QcProfile& profile);
bool LoadQcProfileFromFile(const std::string& path, QcProfile& out, std::string& error);

// ---- 应用 ----
// 在 DefaultQcRules() 之上套用模板覆盖项，得到本次检查实际使用的规则表。
std::vector<model::QcRule> BuildRulesForProfile(const QcProfile& profile);
void ApplyProfileOverrides(const QcProfile& profile, std::vector<model::QcRule>& rules);

// 模板里引用了规则表里没有的 id（多半是手抄写错了），调用方据此给出警告。
std::vector<std::string> UnknownRuleIds(const QcProfile& profile);

// 分析强度 -> AnalysisOptions。基础档位与"具体要分析哪些维度"无关，
// 后者由调用方决定（UI 有自己的开关面板）。
analyzer::AnalysisOptions OptionsForDepth(QcAnalysisDepth depth);

}  // namespace qc
}  // namespace videoeye
