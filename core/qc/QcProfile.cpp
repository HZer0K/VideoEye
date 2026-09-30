#include "core/qc/QcProfile.h"

#include <algorithm>
#include <fstream>

#include "infrastructure/serialization/Json.h"

namespace videoeye {
namespace qc {
namespace {

QcRuleOverride MakeOverride(const char* id, double threshold) {
    QcRuleOverride item;
    item.id = id;
    item.has_threshold = true;
    item.threshold = threshold;
    return item;
}

QcRuleOverride MakeSeverity(const char* id, model::IssueSeverity severity) {
    QcRuleOverride item;
    item.id = id;
    item.has_severity = true;
    item.severity = severity;
    return item;
}

// 同一条规则既要改阈值又要改级别时用它，避免写两条互相打架的覆盖项
QcRuleOverride MakeBoth(const char* id, double threshold, model::IssueSeverity severity) {
    QcRuleOverride item = MakeOverride(id, threshold);
    item.has_severity = true;
    item.severity = severity;
    return item;
}

QcRuleOverride MakeDisabled(const char* id) {
    QcRuleOverride item;
    item.id = id;
    item.has_enabled = true;
    item.enabled = false;
    return item;
}

}  // namespace

const char* ToString(QcAnalysisDepth depth) {
    switch (depth) {
        case QcAnalysisDepth::Fast:     return "fast";
        case QcAnalysisDepth::Standard: return "standard";
        case QcAnalysisDepth::Deep:     return "deep";
    }
    return "standard";
}

bool ParseQcAnalysisDepth(const std::string& text, QcAnalysisDepth& out) {
    if (text == "fast" || text == "快速") {
        out = QcAnalysisDepth::Fast;
        return true;
    }
    if (text == "standard" || text == "标准") {
        out = QcAnalysisDepth::Standard;
        return true;
    }
    if (text == "deep" || text == "深度") {
        out = QcAnalysisDepth::Deep;
        return true;
    }
    return false;
}

std::string ToStableString(model::IssueSeverity severity) {
    switch (severity) {
        case model::IssueSeverity::Critical: return "critical";
        case model::IssueSeverity::Error:    return "error";
        case model::IssueSeverity::Warning:  return "warning";
        case model::IssueSeverity::Info:     return "info";
    }
    return "info";
}

bool ParseIssueSeverity(const std::string& text, model::IssueSeverity& out) {
    if (text == "critical" || text == "致命") {
        out = model::IssueSeverity::Critical;
        return true;
    }
    if (text == "error" || text == "错误") {
        out = model::IssueSeverity::Error;
        return true;
    }
    if (text == "warning" || text == "warn" || text == "警告") {
        out = model::IssueSeverity::Warning;
        return true;
    }
    if (text == "info" || text == "提示") {
        out = model::IssueSeverity::Info;
        return true;
    }
    return false;
}

bool QcProfile::IsBuiltin() const {
    return FindBuiltinQcProfile(id) != nullptr;
}

std::vector<QcProfile> BuiltinQcProfiles() {
    std::vector<QcProfile> profiles;

    // ---- 通用：默认规则集原样使用 ----
    {
        QcProfile profile;
        profile.id = "general";
        profile.name = "通用";
        profile.description =
            "按默认规则集全量检查，不做任何阈值偏移。适合拿到一份来历不明的文件先过一遍。";
        profile.depth = QcAnalysisDepth::Standard;
        profiles.push_back(std::move(profile));
    }

    // ---- 广播：EBU R128 交付口径 ----
    {
        QcProfile profile;
        profile.id = "broadcast";
        profile.name = "广播交付 (EBU R128)";
        profile.description =
            "电视台/广电交付口径：积分响度 -23 ±1 LUFS、真峰值不超过 -1 dBTP、48 kHz 采样率、"
            "色彩标注必须写全。响度与色彩两项按错误处理。";
        profile.depth = QcAnalysisDepth::Standard;
        profile.overrides = {
            MakeBoth("audio.loudness.target_high", -22.0, model::IssueSeverity::Error),
            MakeBoth("audio.loudness.target_low", -24.0, model::IssueSeverity::Error),
            MakeOverride("audio.sample_rate_low", 48000.0),
            MakeSeverity("video.color.unspecified", model::IssueSeverity::Error),
            MakeSeverity("video.resolution.odd", model::IssueSeverity::Error),
            MakeSeverity("container.missing_audio", model::IssueSeverity::Error),
            MakeOverride("video.gop.max_seconds", 2.0),
            MakeOverride("timing.dts_missing", 1.0),
        };
        profiles.push_back(std::move(profile));
    }

    // ---- HLS VOD ----
    {
        QcProfile profile;
        profile.id = "hls-vod";
        profile.name = "HLS VOD";
        profile.description =
            "点播 HLS 包：分片时长、码率阶梯、关键帧对齐、缺失分片一律按错误处理；"
            "视频侧要求 6 秒以内一个 IDR，保证 ABR 切换点够密。";
        profile.depth = QcAnalysisDepth::Standard;
        profile.overrides = {
            MakeSeverity("container.hls.missing_target_duration", model::IssueSeverity::Error),
            MakeSeverity("container.hls.segment_duration_over_target", model::IssueSeverity::Error),
            MakeSeverity("container.hls.discontinuity_unpaired", model::IssueSeverity::Error),
            MakeSeverity("container.hls.missing_init_section", model::IssueSeverity::Error),
            MakeSeverity("container.hls.variant_bandwidth_mismatch", model::IssueSeverity::Error),
            MakeSeverity("container.hls.variant_resolution_mismatch", model::IssueSeverity::Error),
            MakeSeverity("container.hls.variant_codec_mismatch", model::IssueSeverity::Error),
            MakeSeverity("container.hls.variant_keyframe_misalign", model::IssueSeverity::Error),
            MakeSeverity("container.dash.missing_segment_info", model::IssueSeverity::Error),
            MakeSeverity("container.streaming.segment_missing_file", model::IssueSeverity::Error),
            MakeOverride("container.hls.segment_duration_jitter", 0.15),
            MakeOverride("video.gop.max_seconds", 6.0),
            MakeSeverity("container.moov_after_mdat", model::IssueSeverity::Warning),
            MakeSeverity("video.gop.sparse_keyframes", model::IssueSeverity::Error),
        };
        profiles.push_back(std::move(profile));
    }

    // ---- 短视频 / 社媒平台 ----
    {
        QcProfile profile;
        profile.id = "short-video";
        profile.name = "短视频 / 社媒";
        profile.description =
            "抖音/快手/YouTube Shorts 一类平台口径：响度目标 -14 ±1 LUFS（-1 dBTP 上限），"
            "必须偶数分辨率与 faststart（否则首帧起播慢），长时间静音容忍度更低。";
        profile.depth = QcAnalysisDepth::Standard;
        profile.overrides = {
            MakeOverride("audio.loudness.target_high", -13.0),
            MakeOverride("audio.loudness.target_low", -15.0),
            MakeOverride("audio.silence.longest", 5.0),
            MakeOverride("audio.silence.ratio", 30.0),
            MakeOverride("video.gop.max_seconds", 5.0),
            MakeOverride("video.bitrate.peak_ratio", 2.5),
            MakeOverride("audio.sample_rate_low", 44100.0),
            MakeSeverity("video.resolution.odd", model::IssueSeverity::Error),
            MakeSeverity("container.moov_after_mdat", model::IssueSeverity::Warning),
            MakeSeverity("container.mp4.first_sample_not_sync", model::IssueSeverity::Warning),
        };
        profiles.push_back(std::move(profile));
    }

    // ---- 归档母版 ----
    {
        QcProfile profile;
        profile.id = "archive-master";
        profile.name = "归档母版";
        profile.description =
            "长期保存/再分发母版：色彩与 HDR 元数据必须完整（PQ 内容缺 mastering 直接判错）、"
            "时码与声道布局缺失升级为警告，并使用深度模式逐帧核对帧类型。";
        profile.depth = QcAnalysisDepth::Deep;
        profile.overrides = {
            MakeSeverity("video.color.unspecified", model::IssueSeverity::Error),
            MakeSeverity("video.color.hdr_missing_mastering", model::IssueSeverity::Error),
            MakeSeverity("video.color.matrix_mismatch", model::IssueSeverity::Error),
            MakeSeverity("timecode.missing", model::IssueSeverity::Warning),
            MakeSeverity("timecode.drop_frame_mismatch", model::IssueSeverity::Error),
            MakeSeverity("audio.metadata.layout", model::IssueSeverity::Warning),
            MakeSeverity("audio.metadata.duration_mismatch", model::IssueSeverity::Error),
            MakeSeverity("container.moov_after_mdat", model::IssueSeverity::Warning),
            MakeOverride("video.gop.max_seconds", 5.0),
            MakeOverride("audio.loudness.range", 18.0),
            // 默片/纯配乐母版本来就没有音轨，这里不把"缺少音频"当缺陷
            MakeDisabled("container.missing_audio"),
        };
        profiles.push_back(std::move(profile));
    }

    return profiles;
}

const QcProfile* FindBuiltinQcProfile(const std::string& id) {
    // BuiltinQcProfiles() 每次重建 5 个对象，调用频率很低（切换模板、解析命令行），不值得缓存。
    static const std::vector<QcProfile> kProfiles = BuiltinQcProfiles();
    for (const auto& profile : kProfiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

std::string SerializeQcProfile(const QcProfile& profile, bool pretty) {
    utils::JsonValue root = utils::JsonValue::MakeObject();
    root.Set("version", utils::JsonValue(profile.version));
    root.Set("id", utils::JsonValue(profile.id));
    root.Set("name", utils::JsonValue(profile.name));
    root.Set("description", utils::JsonValue(profile.description));
    root.Set("analysis_depth", utils::JsonValue(std::string(ToString(profile.depth))));

    utils::JsonValue rules = utils::JsonValue::MakeObject();
    for (const auto& item : profile.overrides) {
        utils::JsonValue entry = utils::JsonValue::MakeObject();
        if (item.has_enabled) entry.Set("enabled", utils::JsonValue(item.enabled));
        if (item.has_threshold) entry.Set("threshold", utils::JsonValue(item.threshold));
        if (item.has_severity) {
            entry.Set("severity", utils::JsonValue(ToStableString(item.severity)));
        }
        rules.Set(item.id, entry);
    }
    root.Set("rules", rules);

    return pretty ? root.ToPrettyString(2) + "\n" : root.ToString();
}

bool ParseQcProfileJson(const std::string& json, QcProfile& out, std::string& error) {
    utils::JsonValue root;
    if (!utils::JsonParse(json, root, &error)) {
        error = "JSON 解析失败: " + error;
        return false;
    }
    if (!root.IsObject()) {
        error = "模板根节点必须是对象";
        return false;
    }

    QcProfile profile;
    const int version = root.Find("version") != nullptr ? root.Find("version")->IntValueOr(1) : 1;
    if (version != 1) {
        error = "不支持的模板版本: " + std::to_string(version);
        return false;
    }
    profile.version = version;
    profile.id = root.Find("id") != nullptr ? root.Find("id")->StringValueOr("") : "";
    profile.name = root.Find("name") != nullptr ? root.Find("name")->StringValueOr("") : "";
    profile.description =
        root.Find("description") != nullptr ? root.Find("description")->StringValueOr("") : "";

    if (const utils::JsonValue* depth = root.Find("analysis_depth")) {
        if (!ParseQcAnalysisDepth(depth->StringValueOr(""), profile.depth)) {
            error = "analysis_depth 只能取 fast / standard / deep，实际为 " + depth->StringValueOr("");
            return false;
        }
    }

    if (const utils::JsonValue* rules = root.Find("rules")) {
        if (!rules->IsObject()) {
            error = "rules 必须是对象";
            return false;
        }
        const auto& keys = rules->MemberKeys();
        const auto& values = rules->MemberValues();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const utils::JsonValue& entry = values[i];
            if (!entry.IsObject()) {
                error = "规则 " + keys[i] + " 的覆盖项必须是对象";
                return false;
            }
            QcRuleOverride item;
            item.id = keys[i];
            if (const utils::JsonValue* enabled = entry.Find("enabled")) {
                item.has_enabled = enabled->IsBool();
                item.enabled = enabled->BoolValueOr(true);
            }
            if (const utils::JsonValue* threshold = entry.Find("threshold")) {
                item.has_threshold = threshold->IsNumber();
                item.threshold = threshold->NumberValueOr(0.0);
            }
            if (const utils::JsonValue* severity = entry.Find("severity")) {
                if (!ParseIssueSeverity(severity->StringValueOr(""), item.severity)) {
                    error = "规则 " + keys[i] + " 的 severity 无法识别: " + severity->StringValueOr("");
                    return false;
                }
                item.has_severity = true;
            }
            profile.overrides.push_back(item);
        }
    }

    if (profile.id.empty()) {
        error = "模板缺少 id";
        return false;
    }

    out = std::move(profile);
    return true;
}

bool SaveQcProfileToFile(const std::string& path, const QcProfile& profile) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file << SerializeQcProfile(profile, true);
    return file.good();
}

bool LoadQcProfileFromFile(const std::string& path, QcProfile& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "无法打开模板文件: " + path;
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ParseQcProfileJson(json, out, error);
}

std::vector<model::QcRule> BuildRulesForProfile(const QcProfile& profile) {
    std::vector<model::QcRule> rules = model::DefaultQcRules();
    ApplyProfileOverrides(profile, rules);
    return rules;
}

void ApplyProfileOverrides(const QcProfile& profile, std::vector<model::QcRule>& rules) {
    for (const auto& item : profile.overrides) {
        model::QcRule* rule = model::FindQcRule(rules, item.id);
        if (rule == nullptr) continue;  // 未知 id 由 UnknownRuleIds 单独报告
        if (item.has_enabled) rule->enabled = item.enabled;
        if (item.has_threshold) rule->threshold = item.threshold;
        if (item.has_severity) rule->severity = item.severity;
    }
}

std::vector<std::string> UnknownRuleIds(const QcProfile& profile) {
    const std::vector<model::QcRule> defaults = model::DefaultQcRules();
    std::vector<std::string> unknown;
    for (const auto& item : profile.overrides) {
        if (model::FindQcRule(defaults, item.id) == nullptr) unknown.push_back(item.id);
    }
    return unknown;
}

}  // namespace qc
}  // namespace videoeye
