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

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"
#include "core/analysis/streaming/SegmentQcAnalyzer.h"
#include "core/media/streaming/ManifestReader.h"

namespace fs = std::filesystem;

using videoeye::model::IssueSeverity;
// 问题码是 namespace 而非类型，用别名而不是 using-declaration
// （C++ 的 using-declaration 不允许指名命名空间，MSVC 会直接报错）
namespace StreamingIssueCode = videoeye::model::StreamingIssueCode;
using videoeye::model::MediaPlaylistInfo;
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

    videoeye::HlsManifestAnalyzer hls;
    StreamingPackageResult result;
    ASSERT_TRUE(hls.AnalyzeFile((root / "master.m3u8").string(), result));
    ASSERT_EQ(videoeye::SegmentQcAnalyzer::StageStatus::kDone,
              videoeye::SegmentQcAnalyzer::Analyze(result));

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
    videoeye::HlsManifestAnalyzer hls;
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

    videoeye::SegmentQcAnalyzer::Validate(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kHlsVariantKeyframeMisalign);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(IssueSeverity::Warning, issue->severity);
    EXPECT_EQ(1, issue->occurrence_count);
    EXPECT_GT(issue->metric_value, 0.45);

    // 对齐的包不该报警：把第二条改成完全一致，重新跑一遍（Validate 幂等）
    result.ladder[1].keyframe_times = {0.0, 4.0, 8.0};
    videoeye::SegmentQcAnalyzer::Validate(result);
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

    videoeye::DashManifestAnalyzer dash;
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
    videoeye::DashManifestAnalyzer dash;
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
    videoeye::DashManifestAnalyzer dash;
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
    videoeye::HlsManifestAnalyzer hls;
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
    videoeye::HlsManifestAnalyzer hls;
    ASSERT_TRUE(hls.AnalyzeFile((root / "index.m3u8").string(), result));
    videoeye::SegmentQcAnalyzer::Analyze(result);

    const auto* issue = result.FindIssue(StreamingIssueCode::kSegmentMissingFile);
    ASSERT_NE(nullptr, issue);
    EXPECT_EQ(2, issue->occurrence_count);

    fs::remove_all(root);
}

// ===========================================================================
// 清单读取层：体积上限 / 可取消 / 行切分一致性
//
// 这些用例盯的是"读文件"这一步本身，而不是解析结果：以前 AnalyzeFile 是
// `ostringstream << rdbuf()` 一把梭，读的过程中既不知道有取消也不知道有上限，
// 表现为"点取消没反应"和"多大的文件都敢读"。
// ===========================================================================
namespace {

using videoeye::DashManifestOptions;
using videoeye::HlsManifestOptions;
namespace manifest_read = videoeye::manifest;

std::string MakeMediaPlaylistText(int segment_count, bool crlf = false) {
    const char* eol = crlf ? "\r\n" : "\n";
    std::string text = std::string("#EXTM3U") + eol + "#EXT-X-TARGETDURATION:4" + eol;
    for (int i = 0; i < segment_count; ++i) {
        text += std::string("#EXTINF:4.000,") + eol + "seg" + std::to_string(i) + ".ts" + eol;
    }
    text += std::string("#EXT-X-ENDLIST") + eol;
    return text;
}

} // namespace

// ---- 超过 max_manifest_bytes 的清单必须被明确拒绝，而不是"读到哪算哪" ----
TEST(StreamingPackageTest, HlsManifestOverSizeLimitIsRejected) {
    const fs::path root = MakeTempDir("hls_oversize");
    const std::string text = MakeMediaPlaylistText(20000);
    ASSERT_TRUE(WriteFile(root / "index.m3u8", text));
    ASSERT_LT(4096u, text.size()); // 用例前提：确实超限

    HlsManifestOptions opt;
    opt.max_manifest_bytes = 4096;
    StreamingPackageResult result;
    videoeye::HlsManifestAnalyzer hls;
    EXPECT_FALSE(hls.AnalyzeFile((root / "index.m3u8").string(), result, opt));
    EXPECT_TRUE(result.truncated);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(std::string::npos, result.error_message.find("超过大小上限"));

    fs::remove_all(root);
}

// ---- 上限必须精确到字节：等于文件大小放行，小一个字节就拒绝（防 off-by-one）----
TEST(StreamingPackageTest, HlsManifestSizeLimitBoundaryIsExact) {
    const fs::path root = MakeTempDir("hls_size_boundary");
    ASSERT_TRUE(WriteFile(root / "index.m3u8", MakeMediaPlaylistText(4)));

    std::error_code ec;
    const uintmax_t exact = fs::file_size(root / "index.m3u8", ec);
    ASSERT_FALSE(ec);

    videoeye::HlsManifestAnalyzer hls;

    HlsManifestOptions at_limit;
    at_limit.max_manifest_bytes = exact;
    StreamingPackageResult ok_result;
    EXPECT_TRUE(hls.AnalyzeFile((root / "index.m3u8").string(), ok_result, at_limit));
    EXPECT_FALSE(ok_result.truncated);
    EXPECT_EQ(4, ok_result.playlists[0].SegmentCount());

    HlsManifestOptions below_limit;
    below_limit.max_manifest_bytes = exact - 1;
    StreamingPackageResult rejected;
    EXPECT_FALSE(hls.AnalyzeFile((root / "index.m3u8").string(), rejected, below_limit));
    EXPECT_TRUE(rejected.truncated);

    fs::remove_all(root);
}

// ---- 取消标志已置位时，连盘都不该读 ----
TEST(StreamingPackageTest, HlsManifestReadHonoursCancelFlag) {
    const fs::path root = MakeTempDir("hls_read_cancel");
    ASSERT_TRUE(WriteFile(root / "index.m3u8", MakeMediaPlaylistText(8)));

    videoeye::HlsManifestAnalyzer hls;
    const HlsManifestOptions opt;

    std::atomic<bool> cancelled{true};
    StreamingPackageResult cancelled_result;
    EXPECT_FALSE(hls.AnalyzeFile((root / "index.m3u8").string(), cancelled_result, opt, &cancelled));
    EXPECT_TRUE(cancelled_result.truncated);
    EXPECT_NE(std::string::npos, cancelled_result.error_message.find("已取消"));

    // 同一个文件、取消标志没置位时必须照常解析 —— 证明失败确实由取消引起
    std::atomic<bool> not_cancelled{false};
    StreamingPackageResult normal_result;
    EXPECT_TRUE(hls.AnalyzeFile((root / "index.m3u8").string(), normal_result, opt, &not_cancelled));
    EXPECT_FALSE(normal_result.truncated);

    fs::remove_all(root);
}

// ---- 已知超限的文件一个字节都不该读：既不浪费 IO，也不产出"像清单"的半成品 ----
TEST(StreamingPackageTest, ManifestLineReaderRefusesOversizeFile) {
    const fs::path root = MakeTempDir("manifest_oversize_read");
    const int kLines = 20000;
    ASSERT_TRUE(WriteFile(root / "index.m3u8", MakeMediaPlaylistText(kLines)));

    manifest_read::ManifestReadOptions read_options;
    read_options.max_bytes = 8192;
    std::vector<std::string> lines;
    EXPECT_EQ(manifest_read::ManifestReadStatus::TooLarge,
              manifest_read::ReadManifestLines((root / "index.m3u8").string(), read_options, nullptr, lines));
    EXPECT_TRUE(lines.empty());

    fs::remove_all(root);
}

// ---- 读到一半被取消时，必须在一个块的粒度内停下，而不是把文件读完 ----
TEST(StreamingPackageTest, ManifestLineReaderStopsWhenCancelledMidway) {
    const fs::path root = MakeTempDir("manifest_cancel_midway");
    const int kLines = 20000;
    ASSERT_TRUE(WriteFile(root / "index.m3u8", MakeMediaPlaylistText(kLines)));

    std::atomic<bool> cancel{false};
    size_t seen = 0;
    // 第一行就把取消位置上。断言"停得下来"而不是"停在第几行"—— 后者取决于分块大小，
    // 把这个数字写死只会让将来调 chunk_size 的改动白白挂掉。
    const manifest_read::ManifestReadStatus status = manifest_read::ForEachManifestLine(
        (root / "index.m3u8").string(), manifest_read::ManifestReadOptions{}, &cancel,
        [&cancel, &seen](const std::string&) {
            ++seen;
            cancel.store(true, std::memory_order_release);
            return true;
        });
    EXPECT_EQ(manifest_read::ManifestReadStatus::Cancelled, status);
    EXPECT_LT(seen, static_cast<size_t>(kLines * 2));

    fs::remove_all(root);
}

// ---- 分读取出来的行，必须和"整块文本直接切行"完全一致（CRLF 也不能有差异）----
TEST(StreamingPackageTest, ReadFileAndInMemoryTextSplitTheSameWay) {
    const fs::path root = MakeTempDir("manifest_crlf");
    ASSERT_TRUE(WriteFile(root / "crlf.m3u8", MakeMediaPlaylistText(3, true)));
    ASSERT_TRUE(WriteFile(root / "lf.m3u8", MakeMediaPlaylistText(3, false)));

    HlsManifestOptions opt;
    opt.load_sub_playlists = false;
    videoeye::HlsManifestAnalyzer hls;

    StreamingPackageResult crlf_result;
    ASSERT_TRUE(hls.AnalyzeFile((root / "crlf.m3u8").string(), crlf_result, opt));
    StreamingPackageResult lf_result;
    ASSERT_TRUE(hls.AnalyzeFile((root / "lf.m3u8").string(), lf_result, opt));

    ASSERT_EQ(1u, crlf_result.playlists.size());
    ASSERT_EQ(1u, lf_result.playlists.size());
    EXPECT_EQ(lf_result.playlists[0].SegmentCount(), crlf_result.playlists[0].SegmentCount());
    EXPECT_DOUBLE_EQ(lf_result.playlists[0].total_duration_seconds,
                     crlf_result.playlists[0].total_duration_seconds);
    EXPECT_EQ(lf_result.playlists[0].segments[0].uri, crlf_result.playlists[0].segments[0].uri);

    fs::remove_all(root);
}

// ---- DASH 走的是整块读取那条路，上限与取消同样要生效 ----
TEST(StreamingPackageTest, DashManifestSizeLimitAndCancelAreEnforced) {
    const fs::path root = MakeTempDir("dash_oversize");
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
                            // 注释是合法 XML 噪声，扫描器会跳过；用它把文件撑到超限
                            "  <!-- " + MakeBytes(8192) + " -->\n</MPD>\n";
    ASSERT_TRUE(WriteFile(root / "index.mpd", mpd));

    videoeye::DashManifestAnalyzer dash;

    DashManifestOptions capped;
    capped.max_manifest_bytes = 4096;
    StreamingPackageResult oversize;
    EXPECT_FALSE(dash.AnalyzeFile((root / "index.mpd").string(), oversize, capped));
    EXPECT_TRUE(oversize.truncated);
    EXPECT_NE(std::string::npos, oversize.error_message.find("超过大小上限"));

    std::atomic<bool> cancelled{true};
    StreamingPackageResult cancelled_result;
    EXPECT_FALSE(dash.AnalyzeFile((root / "index.mpd").string(), cancelled_result,
                                  DashManifestOptions{}, &cancelled));
    EXPECT_TRUE(cancelled_result.truncated);

    // 限制放宽后同一个文件必须照常解析 —— 证明失败确实由上限/取消引起，而非解析器不认这个 MPD
    StreamingPackageResult good;
    ASSERT_TRUE(dash.AnalyzeFile((root / "index.mpd").string(), good));
    EXPECT_TRUE(good.valid);
    ASSERT_EQ(1u, good.representations.size());
    EXPECT_EQ(4u, good.representations[0].segments.size());

    fs::remove_all(root);
}

// ---- 子播放列表超限：master 自己仍然可用，超限的那一条要有明确标记 ----
TEST(StreamingPackageTest, OversizeSubPlaylistIsMarkedInsteadOfBreakingMaster) {
    const fs::path root = MakeTempDir("hls_oversize_sub");
    ASSERT_TRUE(WriteValidHlsVodPackage(root));
    ASSERT_TRUE(WriteFile(root / "v0" / "index.m3u8", MakeMediaPlaylistText(20000)));

    HlsManifestOptions opt;
    opt.max_manifest_bytes = 4096;
    StreamingPackageResult result;
    videoeye::HlsManifestAnalyzer hls;
    ASSERT_TRUE(hls.AnalyzeFile((root / "master.m3u8").string(), result, opt));

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(2u, result.variants.size());
    ASSERT_EQ(3u, result.playlists.size());

    // 按 URI 定位，不假设遍历顺序
    const MediaPlaylistInfo* oversize_pl = nullptr;
    const MediaPlaylistInfo* v1_pl = nullptr;
    const MediaPlaylistInfo* audio_pl = nullptr;
    for (const auto& pl : result.playlists) {
        if (pl.uri == "v0/index.m3u8")
            oversize_pl = &pl;
        else if (pl.uri == "v1/index.m3u8")
            v1_pl = &pl;
        else if (pl.uri == "audio/index.m3u8")
            audio_pl = &pl;
    }
    ASSERT_NE(nullptr, oversize_pl);
    ASSERT_NE(nullptr, v1_pl);
    ASSERT_NE(nullptr, audio_pl);

    // 超限的那条：标记不完整，原因写在自己的 error_message 里
    EXPECT_TRUE(oversize_pl->parse_failed);
    EXPECT_TRUE(oversize_pl->truncated);
    EXPECT_NE(std::string::npos, oversize_pl->error_message.find("超过大小上限"));
    // 没超限的两条照常解析 —— 一条坏子清单不能拖垮整个包
    EXPECT_FALSE(v1_pl->parse_failed);
    EXPECT_FALSE(v1_pl->truncated);
    EXPECT_EQ(3, v1_pl->SegmentCount());
    EXPECT_FALSE(audio_pl->parse_failed);
    EXPECT_EQ(3, audio_pl->SegmentCount());

    fs::remove_all(root);
}

// ===========================================================================
// 取消链路：大包被取消后必须停手，且绝不允许把半成品当成有效结果发布
//
// 复查里点名的 P1：HLS/DASH 的 AnalyzeFile() 与 fMP4 分片探测都没把取消令牌
// 传下去，取消之后照样读完整份清单、照常建 ladder 跑校验，最后无条件置
// valid = true —— 用户点了"取消"，界面却收到一份看起来正常的成功结果。
// 这里守住两道闸门：
//   1) 清单解析阶段：已取消就不该把 4000 行读满（truncated 必须置位）；
//   2) SegmentQc 阶段：被取消必须返回 kCancelled，且**不得**产出 ladder / issues。
// ===========================================================================

namespace {

// 造一份有 N 个分片的 media playlist 文本。
// 复用上面那个带 crlf 参数的同名辅助函数 —— 这里只需要"行数够多"，不需要 CRLF 变体。
// （两个重载同时可见会让 call site 变成歧义，所以不另写一个）

// 手工铺一个大包（分片实体落盘，探测阶段会真的去 stat 它们）
bool MakeBigPackage(const fs::path& root, int seg_count, StreamingPackageResult& out) {
    for (int i = 0; i < seg_count; ++i) {
        if (!WriteFile(root / ("seg" + std::to_string(i) + ".ts"), "")) return false;
    }
    StreamingPackageResult result;
    result.valid = true;
    MediaPlaylistInfo pl;
    pl.index = 0;
    pl.uri = "media.m3u8";
    for (int i = 0; i < seg_count; ++i) {
        videoeye::model::SegmentInfo seg;
        seg.sequence = i;
        seg.duration_seconds = 4.0;
        seg.has_duration = true;
        seg.resolved_path = (root / ("seg" + std::to_string(i) + ".ts")).string();
        seg.container = videoeye::model::SegmentContainer::MPEG_TS;
        pl.segments.push_back(std::move(seg));
    }
    result.playlists.push_back(std::move(pl));
    out = std::move(result);
    return true;
}

}  // namespace

TEST(StreamingPackageCancelTest, HugeManifestIsNotReadToTheEndWhenCancelled) {
    const fs::path root = MakeTempDir("cancel_manifest");
    ASSERT_TRUE(WriteFile(root / "media.m3u8", MakeMediaPlaylistText(4000, false)));

    // 进门前就是取消态：令牌只要真传进了清单解析，4000 行绝不可能读满
    std::atomic<bool> cancel{true};
    StreamingPackageResult result;
    videoeye::HlsManifestAnalyzer hls;
    hls.AnalyzeFile((root / "media.m3u8").string(), result, {}, &cancel);

    const size_t parsed = result.playlists.empty() ? 0 : result.playlists[0].segments.size();
    EXPECT_LT(parsed, 4000u)
        << "取消令牌没传到清单解析: 4000 行的清单在已取消的情况下被读满了";
}

TEST(StreamingPackageCancelTest, CancelledSegmentQcSkipsLadderAndValidation) {
    constexpr int kSegCount = 2000;
    const fs::path root = MakeTempDir("cancel_segments");
    StreamingPackageResult pkg;
    ASSERT_TRUE(MakeBigPackage(root, kSegCount, pkg));

    // 必须在 analyzer 域里取: 与 SegmentQcAnalyzer 同命名空间, 测试文件里没有这个 using
    videoeye::SegmentQcOptions options;
    options.probe_segments = true;
    options.max_probe_segments = 64;  // 放宽上限, 保证"分片落盘探测"这个最耗时的阶段真会跑到

    // 对照组：不取消就必须跑完并产出 ladder —— 防止实现退化成"一律 kCancelled"
    std::atomic<bool> run_cancel{false};
    StreamingPackageResult control = pkg;
    // 先取返回值再比: 直接把 Analyze(...) 塞进 EXPECT_EQ 会被宏外的逗号拆成多个参数
    const auto control_status = videoeye::SegmentQcAnalyzer::Analyze(control, options, &run_cancel);
    ASSERT_EQ(videoeye::SegmentQcAnalyzer::StageStatus::kDone, control_status);
    EXPECT_FALSE(control.ladder.empty()) << "不取消时 ladder 必须建出来, 否则这条用例失去对照意义";

    // 取消态：三态必须落在 Cancelled，且不得产出 ladder / issues
    // （ladder 与 issues 都是 BuildLadder / Validate 的活，拿半份分片数据算出来的结论是假的）
    std::atomic<bool> cancel{true};
    StreamingPackageResult canceled = pkg;
    const auto cancel_status = videoeye::SegmentQcAnalyzer::Analyze(canceled, options, &cancel);
    EXPECT_EQ(videoeye::SegmentQcAnalyzer::StageStatus::kCancelled, cancel_status)
        << "探测链被取消时必须是 kCancelled, 不能走成 kDone";
    EXPECT_TRUE(canceled.ladder.empty()) << "取消后不得再建 ladder: 半份分片数据算不出可用的码率梯度";
    EXPECT_TRUE(canceled.issues.empty()) << "取消后不得再跑校验: 对着半份数据报问题等于报假问题";
    EXPECT_TRUE(canceled.playlists.empty() || canceled.playlists[0].segments.empty() ||
                canceled.playlists[0].segments[0].probed == false)
        << "探测阶段被取消时至少不该把分片标记为已探测";
}

// ===========================================================================
// Validate 内部的取消检查必须覆盖 DASH 分支（复查 P2）
//
// 修复前 Validate 的取消检查只写在"HLS playlist 的外层循环"上: DASH 包走的是
// representations 那条循环，从入口到出口一次都不查；关键帧比对、码率遍历、音视频
// 分片比对同样没有。于是"取消"要等这一整段跑完才生效 —— 分片数 × 码率层的量级，
// 关键帧比对还是 O(n²)，大型清单下点取消要等好几秒。
//
// 两个刻意的设计:
//   * 直接调 Validate 而不是 Analyze —— Analyze 末尾还有一次兜底的取消检查, 它会把
//     "Validate 内部根本没查"这件事擦干净, 测不出来;
//   * 单条流就给 4096 个分片(远大于 1024 的检查间隔), 且只有 representation 没有
//     playlist —— 免得"外层循环查一次"就误打误撞地提前退出, 那样测不到内层。
//
// 判定用"取消态不得产出任何 issue": 第 1 段要跑完全部分片才会 PushIssue,
// 在入口就退出则一个 issue 都不会有。对照组负责证明这条路径真的会报 issue。
// ===========================================================================
TEST(StreamingPackageCancelTest, ValidateChecksCancelInsideDashLoops) {
    constexpr int kRepCount = 8;
    constexpr int kSegCount = 4096;
    StreamingPackageResult pkg;
    pkg.valid = true;
    pkg.kind = videoeye::model::StreamingKind::Dash;
    for (int r = 0; r < kRepCount; ++r) {
        videoeye::model::DashRepresentationInfo rep;
        rep.index = r;
        rep.id = "v" + std::to_string(r);
        rep.content_type = "video";
        rep.bandwidth_bps = 800000 + r * 100000;
        rep.width = 640;
        rep.height = 360;
        for (int i = 0; i < kSegCount; ++i) {
            videoeye::model::SegmentInfo seg;
            seg.sequence = i;
            seg.duration_seconds = 4.0;
            seg.has_duration = true;
            seg.resolved_path = "/definitely/not/on/disk/seg" + std::to_string(i) + ".m4s";
            seg.exists = false;  // 磁盘上没有 -> 第 1 段跑完必然报 kSegmentMissingFile
            rep.segments.push_back(std::move(seg));
        }
        pkg.representations.push_back(std::move(rep));
    }

    videoeye::SegmentQcOptions options;
    options.probe_segments = false;  // 探测阶段自带取消检查, 会掩盖 Validate 这一段

    // 对照组: 不取消必须跑完并真的报出缺失分片, 否则"取消态没有 issue"可能只是
    // "压根没进循环"的假通过。
    std::atomic<bool> run_cancel{false};
    StreamingPackageResult control = pkg;
    const auto control_status =
        videoeye::SegmentQcAnalyzer::Validate(control, options, &run_cancel);
    ASSERT_EQ(videoeye::SegmentQcAnalyzer::StageStatus::kDone, control_status);
    ASSERT_FALSE(control.issues.empty()) << "对照组必须报出缺失分片, 否则本用例失去意义";

    std::atomic<bool> cancel{true};
    StreamingPackageResult canceled = pkg;
    const auto cancel_status =
        videoeye::SegmentQcAnalyzer::Validate(canceled, options, &cancel);
    EXPECT_EQ(videoeye::SegmentQcAnalyzer::StageStatus::kCancelled, cancel_status)
        << "Validate 必须自己报告取消, 不能让调用方靠 Analyze 末尾的兜底检查来擦屁股";
    EXPECT_TRUE(canceled.issues.empty())
        << "取消态下 Validate 必须在跑完长循环之前退出: 带着半份统计报出来的 issue 是假问题";
}
