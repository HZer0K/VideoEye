// container.extension_mismatch（扩展名与实际容器不符）单元测试
//
// 为什么单独一份：
//   这条规则判错的方向是"对着一个完全正常的文件报扩展名不符"——
//   Ogg / Matroska / MOV 家族的扩展名远远多于解复用器名本身，
//   别名表漏一条就是一次假警报。测试分三层：
//     1) 口径层：真实 iformat->name × 全部已支持扩展名，逐个断言"不报";
//     2) 覆盖层：FormatDetector 的扩展名表里每一条都必须在本文件登记过，
//        新增格式忘了同步 → 这里先红（扩展名清单的唯一事实源在
//        core/analysis/orchestration/FormatDetector.cpp 的 ExtensionTable）；
//     3) 规则层：真不一致的文件仍然要报，且门控（裸流 / 网络源 / 空字段）不误伤。
//
// 样本一律手工构造 AnalysisResult —— 规则只认摘要，不读文件。

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "core/analysis/orchestration/FormatDetector.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/qc/ContainerExtensionMatch.h"
#include "core/qc/QcRuleDefaults.h"
#include "core/qc/QcRuleEngine.h"

namespace {

using namespace videoeye;
using videoeye::qc::QcRuleEngine;

constexpr const char* kRuleId = "container.extension_mismatch";

// FFmpeg 8.x 各解复用器的 iformat->name（即 AnalysisResult::container_format）。
// 刻意写完整串：MOV 解复用器一口气认 6 个扩展名，"mov,mp4,m4a,3gp,3g2,mj2"
// 里的 m4a/3gp/mj2 都必须能直接命中，只写 "mp4" 会把这条能力测没了。
constexpr const char* kMovName = "mov,mp4,m4a,3gp,3g2,mj2";

struct ExtensionFamily {
    const char* iformat;                 // 解复用器名
    std::vector<const char*> extensions; // 这个解复用器会吃下的扩展名（含别名）
};

// 覆盖 FormatDetector::SupportedExtensions() 的全部条目，外加一批
// 「不在默认打开清单里、但批量 QC 一定会遇到」的音频/容器扩展名。
const std::vector<ExtensionFamily>& Families() {
    static const std::vector<ExtensionFamily> kFamilies = {
        {kMovName, {"mp4", "m4v", "m4a", "m4b", "m4p", "m4r", "f4v", "mov", "mj2", "3gp", "3g2"}},
        {"matroska,webm", {"mkv", "mka", "mk3d", "webm"}},
        {"mpegts", {"ts", "mts", "m2ts", "tts", "m2t"}},
        {"avi", {"avi"}},
        {"flv", {"flv"}},
        {"asf", {"asf", "wmv", "wma"}},
        // .opus / .spx 是编解码器名当扩展名，.ogv / .oga / .ogx 是 Ogg 的
        // 视频/音频/通用后缀 —— 它们全都是 "ogg" 解复用器。
        {"ogg", {"ogg", "oga", "ogv", "ogx", "spx", "opus"}},
        {"mpeg", {"mpg", "mpe", "mpeg", "m2p", "vob", "ps"}},
        // MPEG-1/2 视频裸流走 mpegvideo，与 PS 的 "mpeg" 是两个解复用器。
        {"mpegvideo", {"m2v", "m1v", "mpv"}},
        {"mxf", {"mxf"}},
        {"h264", {"h264", "264", "avc"}},
        {"hevc", {"hevc", "h265", "265", "hvc"}},
        {"vvc", {"vvc", "266", "h266"}},
        {"obu", {"obu", "av1"}},
        {"aac", {"aac"}},
        // mp1/mp2/mp3 共用一个解复用器，iformat 名是 "mp3"。
        {"mp3", {"mp3", "mp2", "mp1"}},
        {"flac", {"flac"}},
        {"wav", {"wav"}},
        {"ac3", {"ac3"}},
        {"eac3", {"eac3"}},
        {"dts", {"dts"}},
        {"ape", {"ape"}},
        {"amr", {"amr"}},
        {"aiff", {"aiff", "aif"}},
        {"hls", {"m3u8", "m3u"}},
        {"dash", {"mpd"}},
    };
    return kFamilies;
}

// 全部 iformat token（含逗号列表拆分），用于校验别名表的右值没有写错。
std::set<std::string> KnownIformatTokens() {
    std::set<std::string> tokens;
    for (const auto& family : Families()) {
        const std::string name = family.iformat;
        size_t start = 0;
        while (start <= name.size()) {
            const size_t comma = name.find(',', start);
            const size_t end = (comma == std::string::npos) ? name.size() : comma;
            tokens.insert(name.substr(start, end - start));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    return tokens;
}

model::AnalysisResult MakeResult(const std::string& ext, const std::string& format,
                                 const std::string& path = std::string()) {
    model::AnalysisResult result;
    result.file_extension = ext;
    result.container_format = format;
    result.file_path = path.empty() ? ("clip." + ext) : path;
    result.duration_seconds = 60.0;
    return result;
}

bool HasMismatch(const model::AnalysisResult& result, model::DiagnosticIssue* out = nullptr) {
    QcRuleEngine engine; // 默认规则集
    const model::QcReport report = engine.Evaluate(result);
    for (const auto& issue : report.issues) {
        if (issue.rule_id != kRuleId)
            continue;
        if (out != nullptr)
            *out = issue;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1) 口径层：真实解复用器名 × 已支持扩展名，一条都不许报
// ---------------------------------------------------------------------------

TEST(ContainerExtensionMismatchTest, SupportedExtensionsNeverLookLikeMismatch) {
    for (const auto& family : Families()) {
        for (const char* ext : family.extensions) {
            EXPECT_TRUE(qc::detail::ExtensionMatchesContainer(ext, family.iformat))
                << "." << ext << " 属于解复用器 " << family.iformat
                << "，但匹配函数认为不一致 —— 漏了别名，会对正常文件报扩展名不符";
            // 走完整规则链再确认一次（含门控），确保不是只有 helper 的结果好看
            EXPECT_FALSE(HasMismatch(MakeResult(ext, family.iformat)))
                << "." << ext << " + " << family.iformat << " 被规则误报了";
        }
    }
}

// ---------------------------------------------------------------------------
// 2) 覆盖层：扩展名表新增条目必须在这里登记
// ---------------------------------------------------------------------------

TEST(ContainerExtensionMismatchTest, EveryDetectorExtensionIsClassified) {
    std::set<std::string> classified;
    for (const auto& family : Families()) {
        for (const char* ext : family.extensions) {
            classified.insert(ext);
        }
    }

    for (const auto& ext : FormatDetector::SupportedExtensions()) {
        EXPECT_TRUE(classified.count(ext) != 0)
            << "FormatDetector 支持的扩展名 ." << ext
            << " 没有在本测试的 Families() 里登记 —— 新增格式时把它归到对应的解复用器名，"
               "否则 container.extension_mismatch 可能在真实文件上误报";
    }
}

TEST(ContainerExtensionMismatchTest, AliasTargetsAreRealDemuxerNames) {
    const std::set<std::string> tokens = KnownIformatTokens();
    for (const auto& [ext, target] : qc::detail::ExtensionAliasMap()) {
        EXPECT_TRUE(tokens.count(target) != 0)
            << "别名表把 ." << ext << " 映射到 \"" << target
            << "\"，但这不是本测试里出现过的任何解复用器名（多半是拼错，如 mpeg2video / mp4 写成 moov）";
    }
}

// ---------------------------------------------------------------------------
// 3) 规则层：真不一致要报，门控不许误伤
// ---------------------------------------------------------------------------

TEST(ContainerExtensionMismatchTest, ReportsRealMismatch) {
    const struct {
        const char* ext;
        const char* format;
        const char* expect_name; // 报告里应出现的可读容器名
    } cases[] = {
        {"ts", kMovName, "QuickTime/MOV"},   // 标成 TS 的 MOV（改名事故最常见）
        {"mp4", "mpegts", "MPEG-TS"},        // 标成 MP4 的 TS
        {"mkv", "flv", "Flash Video (FLV)"}, // 标成 MKV 的 FLV
        {"mov", "mxf", "MXF"},               // 标成 MOV 的 MXF
        {"mkv", "ogg", "Ogg"},               // 标成 MKV 的 Ogg
    };

    for (const auto& c : cases) {
        model::DiagnosticIssue issue;
        ASSERT_TRUE(HasMismatch(MakeResult(c.ext, c.format), &issue)) << "." << c.ext << " + " << c.format;
        EXPECT_EQ(kRuleId, issue.rule_id);
        EXPECT_EQ(model::IssueSeverity::Warning, issue.severity);
        EXPECT_EQ(model::IssueCategory::Container, issue.category);
        EXPECT_NE(std::string::npos, issue.detail.find(std::string(".") + c.ext)) << issue.detail;
        EXPECT_NE(std::string::npos, issue.detail.find(c.expect_name)) << issue.detail;
        // 报告里要带上原始 iformat 串，方便用户回查
        EXPECT_NE(std::string::npos, issue.detail.find(c.format)) << issue.detail;
    }
}

TEST(ContainerExtensionMismatchTest, SkipsRawPixelAndNetworkAndEmptyFields) {
    // 裸像素：FFmpeg 侧看到的是 rawvideo，扩展名是 yuv/nv12/... 属正常
    for (const char* ext : {"yuv", "nv12", "rgb", "bgr", "yuy2", "raw", "pcm"}) {
        EXPECT_FALSE(HasMismatch(MakeResult(ext, "rawvideo"))) << ext;
    }
    // 网络源没有可信扩展名
    EXPECT_FALSE(HasMismatch(MakeResult("ts", kMovName, "http://cdn.example.com/a.ts")));
    // 缺字段 = 无从判断
    EXPECT_FALSE(HasMismatch(MakeResult("", kMovName)));
    EXPECT_FALSE(HasMismatch(MakeResult("ts", "")));
}

TEST(ContainerExtensionMismatchTest, ToleratesCaseDotsAndSpaces) {
    // AnalysisEngine 已把扩展名统一小写，但 helper 被其它调用方（含单测）直接用时
    // 不该因为 ".MOV" / " MOV " 这种写法就翻脸。
    EXPECT_TRUE(qc::detail::ExtensionMatchesContainer("MOV", " MOV , MP4 "));
    EXPECT_TRUE(qc::detail::ExtensionMatchesContainer(".mov", kMovName));
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("MOV", "Matroska,WebM"));
    // 归一化后为空 = 无法判断 → 当作一致，不报
    EXPECT_TRUE(qc::detail::ExtensionMatchesContainer("", ""));
    EXPECT_TRUE(qc::detail::ExtensionMatchesContainer(".", "mpegts"));
}

// 交叉校验：别名只在自家解复用器里生效，不能变成"什么都能过"
TEST(ContainerExtensionMismatchTest, AliasDoesNotLeakAcrossFamilies) {
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("oga", "mpegts"));
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("m4v", "matroska,webm"));
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("h265", "h264"));
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("opus", "matroska,webm"));
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("ts", "mpeg"));
    EXPECT_FALSE(qc::detail::ExtensionMatchesContainer("mp3", "aac"));
}

TEST(ContainerExtensionMismatchTest, RuleIsRegisteredAsContainerWarning) {
    std::vector<model::QcRule> rules = qc::DefaultQcRules();
    const model::QcRule* rule = model::FindQcRule(rules, kRuleId);
    ASSERT_TRUE(rule != nullptr) << "默认规则表里漏了 " << kRuleId;
    EXPECT_EQ(model::IssueSeverity::Warning, rule->severity);
    EXPECT_EQ(model::IssueCategory::Container, rule->category);
    EXPECT_TRUE(rule->enabled);
    EXPECT_FALSE(rule->suggestion.empty()) << "命中后必须给出可执行的改名/转封装建议";
}

} // namespace
