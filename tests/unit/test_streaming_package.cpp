// HLS / DASH 流媒体包解析与校验的单元测试
//
// 覆盖 9.6 的四条验收标准:
//   1) 合法 HLS VOD 包不应误报
//   2) segment duration 超过 target duration 时必须 warning
//   3) 多码率 variant keyframe 不对齐时必须 warning
//   4) DASH SegmentTimeline 缺口必须 error
//
// 依赖边界: 只用 std::filesystem 造临时文件，不碰 Qt / FFmpeg。
// SegmentQcAnalyzer 会链到 Mp4SampleTableAnalyzer（fMP4 探测用），
// 那一支是纯 C++ 的自研 IsobmffParser，不含 Qt。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analyzer/DashManifestAnalyzer.h"
#include "core/analyzer/HlsManifestAnalyzer.h"
#include "core/analyzer/SegmentQcAnalyzer.h"

namespace fs = std::filesystem;

using videoeye::model::IssueSeverity;
// 问题码是 namespace 而非类型，用别名而不是 using-declaration
// （C++ 的 using-declaration 不允许指名命名空间，MSVC 会直接报错）
namespace StreamingIssueCode = videoeye::model::StreamingIssueCode;
using videoeye::model::StreamingLadderEntry;
using videoeye::model::StreamingPackageResult;

namespace {

// ---------------------------------------------------------------------------
// 临时目录工具
// ---------------------------------------------------------------------------

fs::path MakeTempDir(const std::string& name) {
    const fs::path base = fs::temp_directory_path() / ("videoeye_streaming_" + name);
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

std::string MakeBytes(size_t n) {
    return std::string(n, '\0');
}

int CountAtLeast(const StreamingPackageResult& result, IssueSeverity severity) {
    int n = 0;
    for (const auto& issue : result.issues) {
        if (static_cast<int>(issue.severity) >= static_cast<int>(severity))
            ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// 一个"合法"的 HLS VOD 包：master + 两条视频 variant + 一条音轨
// ---------------------------------------------------------------------------

bool WriteValidHlsVodPackage(const fs::path& root) {
    const std::string video_body = "#EXTM3U\n"
                                   "#EXT-X-VERSION:3\n"
                                   "#EXT-X-TARGETDURATION:4\n"
                                   "#EXT-X-MEDIA-SEQUENCE:0\n"
                                   "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                   "#EXTINF:4.000,\n"
                                   "seg0.ts\n"
                                   "#EXTINF:4.000,\n"
                                   "seg1.ts\n"
                                   "#EXTINF:4.000,\n"
                                   "seg2.ts\n"
                                   "#EXT-X-ENDLIST\n";

    const std::string master = "#EXTM3U\n"
                               "#EXT-X-VERSION:3\n"
                               "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Japanese\",LANGUAGE=\"ja\","
                               "DEFAULT=YES,AUTOSELECT=YES,URI=\"audio/index.m3u8\"\n"
                               "#EXT-X-STREAM-INF:BANDWIDTH=800000,AVERAGE-BANDWIDTH=750000,"
                               "RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\",AUDIO=\"aac\"\n"
                               "v0/index.m3u8\n"
                               "#EXT-X-STREAM-INF:BANDWIDTH=2500000,AVERAGE-BANDWIDTH=2200000,"
                               "RESOLUTION=1280x720,CODECS=\"avc1.4d401f,mp4a.40.2\",AUDIO=\"aac\"\n"
                               "v1/index.m3u8\n";

    if (!WriteFile(root / "master.m3u8", master))
        return false;
    for (const char* dir : {"v0", "v1", "audio"}) {
        if (!WriteFile(root / dir / "index.m3u8", video_body))
            return false;
        for (int i = 0; i < 3; ++i) {
            const std::string name = "seg" + std::to_string(i) + ".ts";
            // 分片本体只要存在即可：本阶段不解析 TS 内容（那是 TsStructureAnalyzer 的活）
            if (!WriteFile(root / dir / name, MakeBytes(2048)))
                return false;
        }
    }
    return true;
}

} // namespace

// ===========================================================================
// 验收 1: 合法 HLS VOD 包不应误报
// ===========================================================================
TEST(StreamingPackageTest, ValidHlsVodPackageHasNoWarningOrError) {
    const fs::path root = MakeTempDir("valid_vod");
    ASSERT_TRUE(WriteValidHlsVodPackage(root));

    videoeye::analyzer::HlsManifestAnalyzer hls;
    StreamingPackageResult result;
    ASSERT_TRUE(hls.AnalyzeFile((root / "master.m3u8").string(), result));
    ASSERT_TRUE(videoeye::analyzer::SegmentQcAnalyzer::Analyze(result));

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(videoeye::model::StreamingKind::HlsMaster, result.kind);
    EXPECT_EQ(2u, result.variants.size());
    EXPECT_EQ(1u, result.renditions.size());
    EXPECT_EQ(3u, result.playlists.size());
    EXPECT_EQ(2u, result.ladder.size());
    EXPECT_FALSE(result.IsLive());
    EXPECT_FALSE(result.remote);

    // 两条 variant 的分辨率/编码都要解析出来
    EXPECT_EQ(640, result.variants[0].width);
    EXPECT_EQ(360, result.variants[0].height);
    EXPECT_EQ("avc1", result.variants[0].video_codec);
    EXPECT_EQ("mp4a", result.variants[0].audio_codec);
    EXPECT_EQ(2500000, result.variants[1].bandwidth_bps);

    // 9 个分片（3 条播放列表 × 3 段）都在盘上
    EXPECT_EQ(9u, result.TotalSegments());
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kSegmentMissingFile));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kHlsSegmentOverTarget));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kHlsVariantKeyframeMisalign));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kHlsAvSegmentCountMismatch));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kHlsMissingInitSection));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kHlsDiscontinuityUnpaired));

    EXPECT_EQ(0, CountAtLeast(result, IssueSeverity::Warning)) << "合法包不该有 Warning 及以上级别的问题";

    fs::remove_all(root);
}

// ===========================================================================
// 验收 2: segment duration 超过 target duration 时必须 warning
// ===========================================================================
TEST(StreamingPackageTest, SegmentDurationOverTargetIsWarning) {
    videoeye::analyzer::HlsManifestAnalyzer hls;
    StreamingPackageResult result;
    result.manifest_path = "over_target.m3u8";

    const std::string text = "#EXTM3U\n"
                             "#EXT-X-TARGETDURATION:4\n"
                             "#EXT-X-MEDIA-SEQUENCE:0\n"
                             "#EXTINF:4.000,\n"
                             "seg0.ts\n"
                             "#EXTINF:6.500,\n"
                             "seg1.ts\n"
                             "#EXTINF:4.000,\n"
                             "seg2.ts\n"
                             "#EXT-X-ENDLIST\n";

    ASSERT_TRUE(hls.ParseText(text, ".", result));
    hls.Validate(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kHlsSegmentOverTarget);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(IssueSeverity::Warning, issue->severity);
    EXPECT_DOUBLE_EQ(6.5, issue->metric_value);
    EXPECT_DOUBLE_EQ(4.0, issue->threshold);
    EXPECT_EQ(1, issue->occurrence_count);

    fs::remove_all(fs::temp_directory_path() / "videoeye_streaming_valid_vod");
}

// ===========================================================================
// 验收 3: 多码率 variant keyframe 不对齐时必须 warning
// ===========================================================================
TEST(StreamingPackageTest, VariantKeyframeMisalignIsWarning) {
    StreamingPackageResult result;
    result.kind = videoeye::model::StreamingKind::HlsMaster;
    result.valid = true;

    StreamingLadderEntry low;
    low.label = "variant #0";
    low.source_index = 0;
    low.bandwidth_bps = 800000;
    low.width = 640;
    low.height = 360;
    low.video_codec = "avc1";
    low.segment_count = 3;
    low.keyframe_times = {0.0, 4.0, 8.0};

    StreamingLadderEntry high;
    high.label = "variant #1";
    high.source_index = 1;
    high.bandwidth_bps = 2500000;
    high.width = 1280;
    high.height = 720;
    high.video_codec = "avc1";
    high.segment_count = 3;
    // 关键帧整体偏了 0.5 秒：ABR 切换会掉帧/黑屏
    high.keyframe_times = {0.0, 4.5, 8.5};

    result.ladder.push_back(low);
    result.ladder.push_back(high);

    videoeye::analyzer::SegmentQcAnalyzer::Validate(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kHlsVariantKeyframeMisalign);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(IssueSeverity::Warning, issue->severity);
    EXPECT_EQ(1, issue->occurrence_count);
    EXPECT_GT(issue->metric_value, 0.45);

    // 对齐的包不该报警：把第二条改成完全一致，重新跑一遍（Validate 幂等）
    result.ladder[1].keyframe_times = {0.0, 4.0, 8.0};
    videoeye::analyzer::SegmentQcAnalyzer::Validate(result);
    EXPECT_EQ(nullptr, result.FindIssue(StreamingIssueCode::kHlsVariantKeyframeMisalign));
}

// ===========================================================================
// 验收 4: DASH SegmentTimeline 缺口必须 error
// ===========================================================================
TEST(StreamingPackageTest, DashSegmentTimelineGapIsError) {
    const std::string mpd = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                            "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                            "mediaPresentationDuration=\"PT16S\" maxSegmentDuration=\"PT4S\">\n"
                            "  <Period id=\"0\" duration=\"PT16S\">\n"
                            "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
                            "      <Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
                            "codecs=\"avc1.64001f\">\n"
                            "        <SegmentTemplate timescale=\"1000\" initialization=\"v0/init.mp4\" "
                            "media=\"v0/seg-$Number%03d$.m4s\" startNumber=\"1\">\n"
                            "          <SegmentTimeline>\n"
                            "            <S t=\"0\" d=\"4000\" r=\"1\"/>\n"
                            "            <S t=\"12000\" d=\"4000\"/>\n"
                            "          </SegmentTimeline>\n"
                            "        </SegmentTemplate>\n"
                            "      </Representation>\n"
                            "    </AdaptationSet>\n"
                            "  </Period>\n"
                            "</MPD>\n";

    videoeye::analyzer::DashManifestAnalyzer dash;
    StreamingPackageResult result;
    ASSERT_TRUE(dash.ParseText(mpd, ".", result));
    dash.Validate(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kDashSegmentTimelineGap);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(IssueSeverity::Error, issue->severity);
    EXPECT_DOUBLE_EQ(4.0, issue->metric_value); // 12000 - 8000 = 4000ms
}

TEST(StreamingPackageTest, DashSegmentTimelineOverlapIsError) {
    const std::string mpd = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                            "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                            "mediaPresentationDuration=\"PT16S\">\n"
                            "  <Period id=\"0\" duration=\"PT16S\">\n"
                            "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
                            "      <Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
                            "codecs=\"avc1.64001f\">\n"
                            "        <SegmentTemplate timescale=\"1000\" media=\"v0/seg-$Number$.m4s\">\n"
                            "          <SegmentTimeline>\n"
                            "            <S t=\"0\" d=\"4000\"/>\n"
                            "            <S t=\"2000\" d=\"4000\"/>\n"
                            "          </SegmentTimeline>\n"
                            "        </SegmentTemplate>\n"
                            "      </Representation>\n"
                            "    </AdaptationSet>\n"
                            "  </Period>\n"
                            "</MPD>\n";

    StreamingPackageResult result;
    videoeye::analyzer::DashManifestAnalyzer dash;
    ASSERT_TRUE(dash.ParseText(mpd, ".", result));
    dash.Validate(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kDashSegmentTimelineOverlap);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(IssueSeverity::Error, issue->severity);
    EXPECT_DOUBLE_EQ(2.0, issue->metric_value);
}

// ===========================================================================
// DASH 正常包：SegmentTimeline 连续，不应误报
// ===========================================================================
TEST(StreamingPackageTest, DashStaticPackageExpandsSegments) {
    const std::string mpd = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                            "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                            "mediaPresentationDuration=\"PT16S\">\n"
                            "  <Period id=\"0\" duration=\"PT16S\">\n"
                            "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
                            "      <Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
                            "codecs=\"avc1.64001f\">\n"
                            "        <SegmentTemplate timescale=\"1000\" media=\"v0/seg-$Number%03d$.m4s\">\n"
                            "          <SegmentTimeline>\n"
                            "            <S t=\"0\" d=\"4000\" r=\"1\"/>\n"
                            "            <S d=\"4000\"/>\n"
                            "            <S d=\"4000\"/>\n"
                            "          </SegmentTimeline>\n"
                            "        </SegmentTemplate>\n"
                            "      </Representation>\n"
                            "    </AdaptationSet>\n"
                            "  </Period>\n"
                            "</MPD>\n";

    StreamingPackageResult result;
    videoeye::analyzer::DashManifestAnalyzer dash;
    ASSERT_TRUE(dash.ParseText(mpd, ".", result));

    ASSERT_EQ(1u, result.representations.size());
    const auto& rep = result.representations[0];
    EXPECT_EQ("v0", rep.id);
    EXPECT_EQ(800000, rep.bandwidth_bps);
    EXPECT_EQ(640, rep.width);
    EXPECT_EQ(1000u, rep.timescale);
    // r=1 展开成 2 段，再接两个 <S> → 共 4 段
    ASSERT_EQ(4u, rep.segments.size());
    EXPECT_EQ("v0/seg-001.m4s", rep.segments[0].uri);
    EXPECT_DOUBLE_EQ(4.0, rep.segments[0].duration_seconds);
    EXPECT_DOUBLE_EQ(0.0, rep.segments[0].start_seconds);
    EXPECT_DOUBLE_EQ(8.0, rep.segments[2].start_seconds);

    dash.Validate(result);
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kDashSegmentTimelineGap));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kDashSegmentTimelineOverlap));
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kDashMissingSegmentInfo));
}

// ===========================================================================
// HLS 标签解析: EXT-X-MAP / EXT-X-KEY / EXT-X-PART / EXT-X-DISCONTINUITY
// ===========================================================================
TEST(StreamingPackageTest, HlsParsesMapKeyPartAndDiscontinuity) {
    const std::string text =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-VERSION:6\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://key\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
        "#EXTINF:4.000,\n"
        "seg0.m4s\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4.000,\n"
        "seg1.m4s\n"
        "#EXT-X-PART:DURATION=0.5,URI=\"part0.m4s\",INDEPENDENT=YES\n"
        "#EXT-X-PART:DURATION=0.5,URI=\"part1.m4s\"\n"
        "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"part2.m4s\"\n";

    StreamingPackageResult result;
    result.manifest_path = "ll.m3u8";
    videoeye::analyzer::HlsManifestAnalyzer hls;
    ASSERT_TRUE(hls.ParseText(text, ".", result));

    ASSERT_EQ(1u, result.playlists.size());
    const auto& pl = result.playlists[0];
    EXPECT_TRUE(pl.has_init_section);
    EXPECT_EQ("init.mp4", pl.init_uri);
    EXPECT_TRUE(pl.encrypted);
    EXPECT_EQ("SAMPLE-AES", pl.key_method);
    EXPECT_EQ("com.apple.streamingkeydelivery", pl.key_format);
    EXPECT_TRUE(pl.low_latency);
    EXPECT_EQ(2, pl.partial_count);
    EXPECT_EQ(1, pl.discontinuity_count);
    EXPECT_EQ(2, pl.SegmentCount()); // EXT-X-PART 不计入完整分片

    hls.Validate(result);
    // fMP4 有 EXT-X-MAP，不该报"缺初始化段"
    EXPECT_FALSE(result.HasIssue(StreamingIssueCode::kHlsMissingInitSection));
    // 没写 EXT-X-DISCONTINUITY-SEQUENCE → 提示未配对
    const auto* disc = result.FindIssue(StreamingIssueCode::kHlsDiscontinuityUnpaired);
    ASSERT_NE(nullptr, disc);
    EXPECT_EQ(IssueSeverity::Warning, disc->severity);
    // 加密与 LL-HLS 只是登记，级别是 Info
    const auto* key = result.FindIssue(StreamingIssueCode::kHlsEncryptionKey);
    ASSERT_NE(nullptr, key);
    EXPECT_EQ(IssueSeverity::Info, key->severity);
    const auto* part = result.FindIssue(StreamingIssueCode::kHlsPartialSegment);
    ASSERT_NE(nullptr, part);
    EXPECT_EQ(IssueSeverity::Info, part->severity);
}

// ===========================================================================
// 分片文件缺失必须有告警
// ===========================================================================
TEST(StreamingPackageTest, MissingSegmentFileIsReported) {
    const fs::path root = MakeTempDir("missing_seg");
    const std::string text = "#EXTM3U\n"
                             "#EXT-X-TARGETDURATION:4\n"
                             "#EXTINF:4.000,\n"
                             "seg0.ts\n"
                             "#EXTINF:4.000,\n"
                             "seg1.ts\n"
                             "#EXT-X-ENDLIST\n";
    ASSERT_TRUE(WriteFile(root / "index.m3u8", text));

    StreamingPackageResult result;
    videoeye::analyzer::HlsManifestAnalyzer hls;
    ASSERT_TRUE(hls.AnalyzeFile((root / "index.m3u8").string(), result));
    videoeye::analyzer::SegmentQcAnalyzer::Analyze(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kSegmentMissingFile);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(2, issue->occurrence_count);

    fs::remove_all(root);
}
