// 流媒体清单 → QC 报告 的端到端回归（真实引擎路径，不碰 Qt）
//
// 为什么单独一份：test_streaming_qc_rules 手工 MarkExecuted(Streaming) 后直接 Evaluate，
// 绕过了"清单结果到底有没有 dimensions 表"这个前提；test_streaming_scan_status 只验
// 终态状态、不验报告。两者都测不到下面这条曾经断掉的链：
//
//   引擎在打开任何 IO 之前就把 .m3u8/.mpd 分流到 StreamingManifestScan 并 return，
//   **不经过 AnalysisResultAssembler** → result.dimensions 为空 →
//   DimensionExecuted(Streaming) 恒 false →
//     ① 流媒体 QC 规则（守卫在该口径上）整体跳过：有问题的清单，问题进不了报告；
//     ② container.missing_video/missing_audio 没有流媒体豁免：合法清单被误报缺轨扣分。
//
// 修复后：StreamingManifestScan 收尾自己落 Streaming 维度，①②同时消失。
// 本文件用真实 AnalysisEngine 跑临时清单，把这条链整个钉住。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DimensionStatus.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/StreamPackageInfo.h"
#include "core/qc/QcRuleEngine.h"

namespace fs = std::filesystem;

using videoeye::AnalysisCallbacks;
using videoeye::AnalysisEngine;
using videoeye::AnalysisOptions;
using videoeye::model::AnalysisDimension;
using videoeye::model::AnalysisResult;

namespace {

fs::path MakeTempDir(const std::string& name) {
    const fs::path base = fs::temp_directory_path() / ("videoeye_streaming_report_" + name);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

bool WriteFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << content;
    return true;
}

// 结构有效的 MPEG-TS 占位（NULL 包，PID 0x1FFF）。SegmentQcAnalyzer 现在会真的逐包扫
// TS 分片，占位必须解析得动（同步字节 0x47 + 188 步长），否则会被判容器解析失败而扣分。
// NULL 包没有视频 PES，首帧实证拿不到证据，关键帧结论保持清单给的 Declared。
std::string MakeMinimalTs(size_t packets) {
    std::string out;
    out.reserve(packets * 188);
    for (size_t i = 0; i < packets; ++i) {
        out.push_back('\x47');
        out.push_back('\x1F');
        out.push_back('\xFF');
        out.push_back(static_cast<char>(0x10 | (i & 0x0F)));
        out.append(184, '\xFF');
    }
    return out;
}

// 跑一次真实引擎分析，返回 on_finished 里的结果对象（清单路径成功时必走它）。
AnalysisResult RunEngine(const fs::path& path) {
    AnalysisResult result;
    AnalysisCallbacks callbacks;
    callbacks.on_finished = [&result](bool, const AnalysisResult& r) { result = r; };
    AnalysisOptions options; // analyze_streaming_package 默认开
    AnalysisEngine engine;
    engine.Run(path.string(), options, callbacks, &result);
    return result;
}

bool HasIssue(const videoeye::model::QcReport& report, const std::string& rule_id) {
    for (const auto& issue : report.issues) {
        if (issue.rule_id == rule_id)
            return true;
    }
    return false;
}

// 合法的单码率 HLS VOD（分片本体是结构有效的 TS 占位，无视频首帧证据 → 关键帧保持 Declared）
const char* const kValidHls = "#EXTM3U\n"
                              "#EXT-X-VERSION:3\n"
                              "#EXT-X-TARGETDURATION:4\n"
                              "#EXT-X-MEDIA-SEQUENCE:0\n"
                              "#EXT-X-PLAYLIST-TYPE:VOD\n"
                              "#EXTINF:4.000,\n"
                              "seg0.ts\n"
                              "#EXTINF:4.000,\n"
                              "seg1.ts\n"
                              "#EXT-X-ENDLIST\n";

// 有缺陷的 HLS：seg1 时长 6.5s 超过 TARGETDURATION:4 → container.hls.segment_over_target
const char* const kOverTargetHls = "#EXTM3U\n"
                                   "#EXT-X-VERSION:3\n"
                                   "#EXT-X-TARGETDURATION:4\n"
                                   "#EXT-X-MEDIA-SEQUENCE:0\n"
                                   "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                   "#EXTINF:4.000,\n"
                                   "seg0.ts\n"
                                   "#EXTINF:6.500,\n"
                                   "seg1.ts\n"
                                   "#EXT-X-ENDLIST\n";

// 合法的静态 DASH（SegmentTemplate + duration，无时间轴缺口）
const char* const kValidDash = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                               "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                               "mediaPresentationDuration=\"PT8S\">\n"
                               "  <Period id=\"0\" duration=\"PT8S\">\n"
                               "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
                               "      <Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
                               "codecs=\"avc1.64001f\">\n"
                               "        <SegmentTemplate timescale=\"1000\" duration=\"4000\" startNumber=\"1\" "
                               "media=\"v0/seg-$Number$.m4s\"/>\n"
                               "      </Representation>\n"
                               "    </AdaptationSet>\n"
                               "  </Period>\n"
                               "</MPD>\n";

} // namespace

// ---------------- 合法 HLS：维度落表 + 无误报 + 满分 ----------------

TEST(StreamingQcReportTest, ValidHlsBuildsDimensionAndScoresFull) {
    const fs::path dir = MakeTempDir("hls_valid");
    ASSERT_TRUE(WriteFile(dir / "index.m3u8", kValidHls));
    for (const char* name : {"seg0.ts", "seg1.ts"})
        ASSERT_TRUE(WriteFile(dir / name, MakeMinimalTs(11)));

    const AnalysisResult result = RunEngine(dir / "index.m3u8");
    ASSERT_TRUE(result.streaming_analyzed);
    ASSERT_TRUE(result.streaming_package.valid);

    // 根因断言：清单路径不经过装配器，必须自己把 Streaming 维度落进 dimensions 表，
    // 否则下面所有以 DimensionExecuted(Streaming) 为闸门的消费方都会把结论丢掉。
    EXPECT_TRUE(result.DimensionExecuted(AnalysisDimension::Streaming))
        << "流媒体清单路径没有构建 dimensions 表 —— QC 规则/导出/面板会整体跳过流媒体结论";

    videoeye::qc::QcRuleEngine engine;
    const videoeye::model::QcReport report = engine.Evaluate(result);

    // 误报断言：清单没有 demux 流表，缺轨判定对它不成立。
    EXPECT_FALSE(HasIssue(report, "container.missing_video")) << "合法清单被误报缺视频轨";
    EXPECT_FALSE(HasIssue(report, "container.missing_audio")) << "合法清单被误报缺音频轨";
    EXPECT_DOUBLE_EQ(100.0, report.score) << "合法清单不该被扣分";
}

// ---------------- 有缺陷的 HLS：问题必须进报告 ----------------

TEST(StreamingQcReportTest, DefectiveHlsIssueReachesReport) {
    const fs::path dir = MakeTempDir("hls_defect");
    ASSERT_TRUE(WriteFile(dir / "index.m3u8", kOverTargetHls));
    for (const char* name : {"seg0.ts", "seg1.ts"})
        ASSERT_TRUE(WriteFile(dir / name, MakeMinimalTs(11)));

    const AnalysisResult result = RunEngine(dir / "index.m3u8");
    ASSERT_TRUE(result.DimensionExecuted(AnalysisDimension::Streaming));
    // 分析器确实产出了这条问题（否则规则层无米下锅）
    ASSERT_FALSE(result.streaming_package.issues.empty());

    videoeye::qc::QcRuleEngine engine;
    const videoeye::model::QcReport report = engine.Evaluate(result);

    // 核心断言：修复前 DimensionExecuted(Streaming) 恒 false，规则整体跳过，
    // 这条问题永远进不了报告。现在它必须在。
    EXPECT_TRUE(HasIssue(report, videoeye::model::StreamingIssueCode::kHlsSegmentOverTarget))
        << "流媒体问题被规则层丢弃，没有进入 QC 报告";
    // 缺陷包同样不该再叠加缺轨误报
    EXPECT_FALSE(HasIssue(report, "container.missing_video"));
    EXPECT_FALSE(HasIssue(report, "container.missing_audio"));
}

// ---------------- 合法 DASH：维度落表 + 无缺轨误报 ----------------

TEST(StreamingQcReportTest, ValidDashBuildsDimensionAndSkipsTrackRules) {
    const fs::path dir = MakeTempDir("dash_valid");
    ASSERT_TRUE(WriteFile(dir / "manifest.mpd", kValidDash));

    const AnalysisResult result = RunEngine(dir / "manifest.mpd");
    ASSERT_TRUE(result.streaming_analyzed);
    ASSERT_TRUE(result.streaming_package.valid);
    EXPECT_TRUE(result.DimensionExecuted(AnalysisDimension::Streaming)) << "DASH 清单路径同样需要自己落 dimensions 表";

    videoeye::qc::QcRuleEngine engine;
    const videoeye::model::QcReport report = engine.Evaluate(result);

    // DASH 结果也没有 demux 流表；分片本体不存在会触发流媒体自己的缺片问题，
    // 但那与 container.missing_video/audio 是两码事 —— 这里只钉"缺轨误报"被豁免。
    EXPECT_FALSE(HasIssue(report, "container.missing_video")) << "合法 DASH 被误报缺视频轨";
    EXPECT_FALSE(HasIssue(report, "container.missing_audio")) << "合法 DASH 被误报缺音频轨";
}
