// AnalysisEngine 的失败终态一致性（复查 P2 + 统一 FFmpeg 侧分支）
//
// 根因(已修): 各条失败分支只调 NotifyFailed()，不动 result.scan_status ——
// 而 AnalysisResult::scan_status 的默认值是 Complete。于是"回调说失败、结果对象
// 说完成"同时成立；结果一旦被上层缓存或复用（批处理回传 / 对比视图 / 报告导出），
// 就会拿一份空的流媒体包当成跑完的结果展示。
//
// 清单分支（解析失败 / 分片 QC kFailed）与 FFmpeg 分支（打开 / 探测 / 读取失败）
// 现在统一走 MarkFailed()：状态、文案、错误码三件事一起落进结果对象。
//
// 断言全部落在 Run() 的 out_result 出参上: 失败分支不走 on_finished，想看结果
// 对象里的终态只有这一条通道。
//
// 依赖边界: 只用 std::filesystem 造临时文件，不碰 Qt。清单路径不调 avformat
// （解析是自研的），不需要真实媒体文件 —— 分片本体只要存在即可。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/orchestration/AnalysisEngine.h"

namespace fs = std::filesystem;

using videoeye::analyzer::AnalysisEngine;
using videoeye::analyzer::AnalysisOptions;
using videoeye::model::AnalysisResult;
using videoeye::model::AnalysisStatus;

namespace {

fs::path MakeTempDir(const std::string& name) {
    const fs::path base = fs::temp_directory_path() / ("videoeye_manifest_status_" + name);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

bool WriteFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << content;
    return true;
}

// 一次清单分析的完整观测：两个回调各来几次 + 结果对象的终态
struct RunOutcome {
    int failed_calls = 0;
    int finished_calls = 0;
    std::string failed_message;
    AnalysisResult result;
};

// 失败分支不会走 on_finished，所以结果对象只能靠 Run() 的出参拿到。
RunOutcome RunEngine(const fs::path& path) {
    RunOutcome out;
    videoeye::analyzer::AnalysisCallbacks callbacks;
    callbacks.on_failed = [&out](const std::string& message) {
        ++out.failed_calls;
        out.failed_message = message;
    };
    callbacks.on_finished = [&out](bool, const AnalysisResult& result) {
        ++out.finished_calls;
        out.result = result;
    };

    AnalysisOptions options;  // analyze_streaming_package 默认开
    AnalysisEngine engine;
    engine.Run(path.string(), options, callbacks, &out.result);
    return out;
}

// 一条合法的单码率 HLS VOD 媒体播放列表（不带 EXT-X-STREAM-INF，走 HlsMedia 分支）
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

bool WriteValidHlsPackage(const fs::path& root) {
    if (!WriteFile(root / "index.m3u8", kValidHls)) return false;
    // 分片本体只要存在即可：本阶段不解析 TS 内容（那是 TsStructureAnalyzer 的活）
    for (const char* name : {"seg0.ts", "seg1.ts"}) {
        if (!WriteFile(root / name, std::string(2048, '\0'))) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 无效清单：两条失败分支都必须把 scan_status 改成 Failed
// ---------------------------------------------------------------------------

TEST(StreamingManifestScanStatus, InvalidHlsReportsFailedStatus) {
    const fs::path dir = MakeTempDir("hls");
    const fs::path manifest = dir / "broken.m3u8";
    // 没有 #EXTM3U 头：HlsManifestAnalyzer 直接判"不是 HLS 清单"
    ASSERT_TRUE(WriteFile(manifest, "this is not a playlist\njust some text\n"));

    const RunOutcome out = RunEngine(manifest);

    EXPECT_EQ(1, out.failed_calls) << "无效清单必须且只能报一次失败";
    EXPECT_EQ(0, out.finished_calls) << "失败分支不该再走 on_finished";

    // 核心断言: 回调说失败，结果对象也必须说失败（默认值是 Complete）
    EXPECT_EQ(AnalysisStatus::Failed, out.result.scan_status)
        << "scan_status 仍停在默认的 Complete";
    EXPECT_FALSE(out.result.error_message.empty()) << "失败终态必须写明原因";
    EXPECT_NE(0, out.result.scan_error_code) << "失败终态不该留着 0(=无错误)";
    EXPECT_FALSE(out.result.streaming_analyzed) << "没跑完就不能标成已分析";
    EXPECT_FALSE(out.result.streaming_package.valid);

    // 回调文案与结果对象里的文案必须是同一份，避免两处各说各话
    EXPECT_EQ(out.failed_message, out.result.error_message);
    EXPECT_EQ("hls", out.result.container_format);
}

TEST(StreamingManifestScanStatus, InvalidDashReportsFailedStatus) {
    const fs::path dir = MakeTempDir("dash");
    const fs::path manifest = dir / "broken.mpd";
    // 一个"看起来是 XML 但没有 MPD 根节点"的清单（典型的下到了错误页）
    ASSERT_TRUE(WriteFile(manifest, "<html><body>404 Not Found</body></html>\n"));

    const RunOutcome out = RunEngine(manifest);

    EXPECT_EQ(1, out.failed_calls);
    EXPECT_EQ(0, out.finished_calls);
    EXPECT_EQ(AnalysisStatus::Failed, out.result.scan_status)
        << "scan_status 仍停在默认的 Complete";
    EXPECT_FALSE(out.result.error_message.empty());
    EXPECT_NE(0, out.result.scan_error_code);
    EXPECT_FALSE(out.result.streaming_analyzed);
    EXPECT_EQ(out.failed_message, out.result.error_message);
    EXPECT_EQ("dash", out.result.container_format);
}

// ---------------------------------------------------------------------------
// FFmpeg 侧的失败分支同样必须是 Failed（和清单分支一个写法）
// ---------------------------------------------------------------------------

TEST(AnalysisScanStatus, UnopenableFileReportsFailedStatus) {
    const fs::path dir = MakeTempDir("open");
    const fs::path missing = dir / "does_not_exist.mp4";  // 故意不创建
    ASSERT_FALSE(fs::exists(missing));

    const RunOutcome out = RunEngine(missing);

    EXPECT_EQ(1, out.failed_calls);
    EXPECT_EQ(0, out.finished_calls);
    EXPECT_EQ(AnalysisStatus::Failed, out.result.scan_status)
        << "scan_status 仍停在默认的 Complete";
    EXPECT_FALSE(out.result.error_message.empty());
    // 打开失败这条路上有真 AVERROR，不能退化成 -1
    EXPECT_LT(out.result.scan_error_code, 0);
    EXPECT_EQ(out.failed_message, out.result.error_message);
}

// ---------------------------------------------------------------------------
// 反向对照：合法清单仍然要报 Complete，别把失败状态写进了正常路径
// ---------------------------------------------------------------------------

TEST(StreamingManifestScanStatus, ValidHlsKeepsCompleteStatus) {
    const fs::path dir = MakeTempDir("hls_ok");
    ASSERT_TRUE(WriteValidHlsPackage(dir));

    const RunOutcome out = RunEngine(dir / "index.m3u8");

    EXPECT_EQ(0, out.failed_calls) << "合法清单不该报失败";
    EXPECT_EQ(1, out.finished_calls);
    EXPECT_EQ(AnalysisStatus::Complete, out.result.scan_status);
    EXPECT_EQ(0, out.result.scan_error_code);
    EXPECT_TRUE(out.result.error_message.empty()) << "成功路径不该留下错误文案";
    EXPECT_TRUE(out.result.streaming_analyzed);
    EXPECT_TRUE(out.result.streaming_package.valid);
    EXPECT_DOUBLE_EQ(8.0, out.result.duration_seconds);
}
