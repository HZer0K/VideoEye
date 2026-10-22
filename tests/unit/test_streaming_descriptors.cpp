// HLS / DASH "业务语义"标签的单元测试
//
// 覆盖两组过去完全没有解析的元素：
//   HLS: EXT-X-DATERANGE / EXT-X-PROGRAM-DATE-TIME / EXT-X-I-FRAME-STREAM-INF /
//        EXT-X-START / EXT-X-SKIP
//   DASH: ContentProtection / UTCTiming / EventStream / Role / EssentialProperty
//
// 为什么单独一个文件：test_streaming_package.cpp 守的是"分片与码率阶梯"那几条
// 验收标准，这一份守的是"时间轴与业务标记"——广告插播、DVR、快进档、DRM 都
// 建立在这些标签上，缺一项就有一类功能静默失效。
//
// 依赖边界：纯文本解析，不落盘、不碰 Qt / FFmpeg。
// 注意 HlsManifestAnalyzer::ParseText 不做 Validate（AnalyzeFile 才做），
// 所以这里的每个用例都显式调一次 Validate。

#include <gtest/gtest.h>

#include <string>

#include "core/analysis/streaming/DashManifestAnalyzer.h"
#include "core/analysis/streaming/HlsManifestAnalyzer.h"

namespace StreamingIssueCode = videoeye::model::StreamingIssueCode;
using videoeye::model::IssueSeverity;
using videoeye::model::StreamingIssue;
using videoeye::model::StreamingPackageResult;

namespace {

// ---------------------------------------------------------------------------
// HLS 帮助函数
// ---------------------------------------------------------------------------

StreamingPackageResult ParseHls(const std::string& text) {
    StreamingPackageResult out;
    EXPECT_TRUE(videoeye::HlsManifestAnalyzer::ParseText(text, ".", out));
    videoeye::HlsManifestAnalyzer::Validate(out);
    return out;
}

const StreamingIssue* FindIssue(const StreamingPackageResult& out, const char* code) {
    return out.FindIssue(code);
}

StreamingPackageResult ParseDash(const std::string& text) {
    StreamingPackageResult out;
    EXPECT_TRUE(videoeye::DashManifestAnalyzer::ParseText(text, ".", out));
    videoeye::DashManifestAnalyzer::Validate(out);
    return out;
}

// DASH 的 ContentProtection 用例骨架：一个视频表示 + 可选的音频表示
std::string DashWithDrm(const std::string& content_protection, const std::string& extra_adaptation = "",
                        const std::string& mpd_attrs =
                            "type=\"static\" mediaPresentationDuration=\"PT10S\"") {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" " +
           mpd_attrs +
           " minBufferTime=\"PT2S\">\n"
           "  <Period id=\"p0\">\n"
           "    <AdaptationSet id=\"1\" contentType=\"video\" mimeType=\"video/mp4\">\n" +
           content_protection +
           "      <SegmentTemplate timescale=\"90000\" duration=\"360000\" "
           "initialization=\"init.mp4\" media=\"seg-$Number$.m4s\" startNumber=\"1\"/>\n"
           "      <Representation id=\"v0\" bandwidth=\"2000000\" width=\"1920\" height=\"1080\" "
           "codecs=\"avc1.640028\"/>\n"
           "    </AdaptationSet>\n" +
           extra_adaptation + "  </Period>\n"
                              "</MPD>\n";
}

}  // namespace

// ===========================================================================
// HLS: EXT-X-DATERANGE（广告/章节标记）
// ===========================================================================
TEST(StreamingDescriptorTest, HlsDateRangeIsParsedAndOpenEndedIsReported) {
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-VERSION:6\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-MEDIA-SEQUENCE:0\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00.000Z\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXT-X-DATERANGE:ID=\"ad1\",CLASS=\"com.example.ad\","
                                 "START-DATE=\"2026-01-01T00:00:04.000Z\",DURATION=8.0,SCTE35-OUT=0xFC30\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-DATERANGE:ID=\"ad2\",START-DATE=\"2026-01-01T00:00:08.000Z\","
                                 "SCTE35-OUT=0xFC30\n"
                                 "#EXTINF:4.000,\n"
                                 "seg2.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    ASSERT_EQ(1u, out.playlists.size());
    const auto& pl = out.playlists[0];
    ASSERT_EQ(2u, pl.date_ranges.size());

    // 第一条：带 DURATION，结束点明确
    EXPECT_EQ("ad1", pl.date_ranges[0].id);
    EXPECT_EQ("com.example.ad", pl.date_ranges[0].class_id);
    EXPECT_TRUE(pl.date_ranges[0].has_duration);
    EXPECT_DOUBLE_EQ(8.0, pl.date_ranges[0].duration_seconds);
    EXPECT_TRUE(pl.date_ranges[0].HasScte35());
    // 墙钟要换算成"清单内第几秒"（靠 PDT 基准），并能落到具体分片
    EXPECT_NEAR(4.0, pl.date_ranges[0].at_seconds, 0.001);
    EXPECT_EQ(1, pl.date_ranges[0].segment_index);

    // 第二条：只有 SCTE35-OUT，没有结束点 —— 必须报出来
    EXPECT_FALSE(pl.date_ranges[1].HasDefiniteEnd());
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kHlsDateRangeOpenEnded);
    ASSERT_TRUE(issue != nullptr) << "开口的广告标记必须报出来";
    EXPECT_EQ(IssueSeverity::Warning, issue->severity) << "带 SCTE-35 的开口按 Warning 报";
    EXPECT_EQ(1, issue->occurrence_count);
    EXPECT_NEAR(8.0, issue->at_seconds, 0.001);

    // 两条 START-DATE 都合法，不该出现"日期不合法"
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kHlsDateRangeInvalidDate) == nullptr);
}

TEST(StreamingDescriptorTest, HlsDateRangeWithInvalidDateIsReported) {
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXT-X-DATERANGE:ID=\"broken\",START-DATE=\"2026-13-45 99:99\",DURATION=4\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kHlsDateRangeInvalidDate);
    ASSERT_TRUE(issue != nullptr);
    EXPECT_EQ(IssueSeverity::Warning, issue->severity);
    // 没有 PDT 基准时算不出相对秒，不能编一个 0 出来
    ASSERT_EQ(1u, out.playlists[0].date_ranges.size());
    EXPECT_LT(out.playlists[0].date_ranges[0].at_seconds, 0.0);
}

// ===========================================================================
// HLS: EXT-X-PROGRAM-DATE-TIME（墙钟）
// ===========================================================================
TEST(StreamingDescriptorTest, HlsProgramDateTimeIsParsedInUtc) {
    // 带 +08:00 偏移：UTC 是 2025-12-31T16:00:00Z
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00.000+08:00\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    const auto& pl = out.playlists[0];
    EXPECT_TRUE(pl.has_program_date_time);
    EXPECT_TRUE(pl.program_date_time_valid);
    EXPECT_EQ("2026-01-01T00:00:00.000+08:00", pl.program_date_time) << "原文要保留，报告直接展示它";
    // 2025-12-31T16:00:00Z
    EXPECT_DOUBLE_EQ(1767196800.0, pl.program_date_time_epoch_s);
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kHlsProgramDateTimeInvalid) == nullptr);
}

TEST(StreamingDescriptorTest, HlsProgramDateTimeInvalidIsReported) {
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026/01/01 00:00:00\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    EXPECT_TRUE(out.playlists[0].has_program_date_time) << "写了就要记下来，哪怕解析不了";
    EXPECT_FALSE(out.playlists[0].program_date_time_valid);
    ASSERT_TRUE(FindIssue(out, StreamingIssueCode::kHlsProgramDateTimeInvalid) != nullptr);
}

TEST(StreamingDescriptorTest, HlsProgramDateTimeDriftIsReported) {
    // 两处 PDT 之间实际过了 9 秒，但 EXTINF 只累计 4 秒
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:09Z\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    ASSERT_EQ(2u, out.playlists[0].program_date_time_samples.size());
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kHlsProgramDateTimeDrift);
    ASSERT_TRUE(issue != nullptr);
    EXPECT_NEAR(5.0, issue->metric_value, 0.001) << "偏差 = |9 - 4| = 5 秒";
}

TEST(StreamingDescriptorTest, HlsProgramDateTimeResetAfterDiscontinuityIsAllowed) {
    // 拼接点之后重设墙钟是规范允许的，不能报漂移
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-DISCONTINUITY-SEQUENCE:1\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:00Z\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXT-X-DISCONTINUITY\n"
                                 "#EXT-X-PROGRAM-DATE-TIME:2026-01-01T03:00:00Z\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    EXPECT_EQ(2u, out.playlists[0].program_date_time_samples.size());
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kHlsProgramDateTimeDrift) == nullptr)
        << "discontinuity 处的墙钟跳变不是缺陷";
}

// ===========================================================================
// HLS: EXT-X-I-FRAME-STREAM-INF（快进档）
// ===========================================================================
TEST(StreamingDescriptorTest, HlsIframeVariantIsParsedFromAttribute) {
    const std::string master = "#EXTM3U\n"
                               "#EXT-X-VERSION:4\n"
                               "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS=\"avc1.4d401f\"\n"
                               "v1/index.m3u8\n"
                               "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=180000,RESOLUTION=1280x720,"
                               "CODECS=\"avc1.4d401f\",URI=\"iframe/index.m3u8\"\n";

    const StreamingPackageResult out = ParseHls(master);
    ASSERT_EQ(2u, out.variants.size());
    EXPECT_FALSE(out.variants[0].iframe_only);
    EXPECT_TRUE(out.variants[1].iframe_only);
    EXPECT_EQ("iframe/index.m3u8", out.variants[1].uri) << "URI 写在属性里，不吃下一行";
    EXPECT_EQ(180000, out.variants[1].bandwidth_bps);
    EXPECT_EQ(1u, out.IframeVariantCount());
    // 快进档也登记成一条 playlist，便于后续探测它的分片
    ASSERT_GE(out.variants[1].playlist_index, 0);
    EXPECT_EQ("iframe", out.playlists[static_cast<size_t>(out.variants[1].playlist_index)].role);
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kHlsIframeVariantIncomplete) == nullptr);
}

TEST(StreamingDescriptorTest, HlsIframeVariantWithoutUriIsReported) {
    const std::string master = "#EXTM3U\n"
                               "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\n"
                               "v1/index.m3u8\n"
                               "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=180000,RESOLUTION=1280x720\n";

    const StreamingPackageResult out = ParseHls(master);
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kHlsIframeVariantIncomplete);
    ASSERT_TRUE(issue != nullptr);
    EXPECT_EQ(IssueSeverity::Warning, issue->severity);
    EXPECT_NE(issue->detail.find("URI"), std::string::npos);
}

// ===========================================================================
// HLS: EXT-X-START / EXT-X-SKIP
// ===========================================================================
TEST(StreamingDescriptorTest, HlsStartOffsetInsideRangeIsAccepted) {
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-START:TIME-OFFSET=-4.0,PRECISE=YES\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    EXPECT_TRUE(out.playlists[0].has_start);
    EXPECT_DOUBLE_EQ(-4.0, out.playlists[0].start_time_offset_s);
    EXPECT_TRUE(out.playlists[0].start_precise);
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kHlsStartOffsetOutOfRange) == nullptr);
}

TEST(StreamingDescriptorTest, HlsStartOffsetBeyondDurationIsReported) {
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-START:TIME-OFFSET=120.0\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-ENDLIST\n";

    const StreamingPackageResult out = ParseHls(playlist);
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kHlsStartOffsetOutOfRange);
    ASSERT_TRUE(issue != nullptr) << "起点超出台长时播放器会忽略它";
    EXPECT_DOUBLE_EQ(120.0, issue->metric_value);
}

TEST(StreamingDescriptorTest, HlsDeltaUpdateRequiresMediaSequence) {
    const std::string without_seq = "#EXTM3U\n"
                                    "#EXT-X-TARGETDURATION:4\n"
                                    "#EXT-X-SKIP:SKIPPED-SEGMENTS=12\n"
                                    "#EXTINF:4.000,\n"
                                    "seg0.ts\n"
                                    "#EXT-X-ENDLIST\n";
    const StreamingPackageResult bad = ParseHls(without_seq);
    EXPECT_TRUE(bad.playlists[0].has_skip);
    EXPECT_EQ(12, bad.playlists[0].skipped_segments);
    ASSERT_TRUE(FindIssue(bad, StreamingIssueCode::kHlsDeltaUpdateWithoutSequence) != nullptr);

    const std::string with_seq = "#EXTM3U\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-MEDIA-SEQUENCE:120\n"
                                 "#EXT-X-SKIP:SKIPPED-SEGMENTS=12\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXT-X-ENDLIST\n";
    const StreamingPackageResult good = ParseHls(with_seq);
    EXPECT_TRUE(good.playlists[0].has_skip);
    EXPECT_TRUE(FindIssue(good, StreamingIssueCode::kHlsDeltaUpdateWithoutSequence) == nullptr);
}

// ===========================================================================
// DASH: ContentProtection（DRM）
// ===========================================================================
TEST(StreamingDescriptorTest, DashContentProtectionIsParsedAndRegistered) {
    const std::string mpd = DashWithDrm(
        "      <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\" value=\"cenc\" "
        "cenc:default_KID=\"11111111-2222-3333-4444-555555555555\"/>\n"
        "      <ContentProtection schemeIdUri=\"urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\">\n"
        "        <cenc:pssh>AAAAPXBzc2gAAAAA</cenc:pssh>\n"
        "      </ContentProtection>\n");

    const StreamingPackageResult out = ParseDash(mpd);
    ASSERT_FALSE(out.representations.empty());
    const auto& rep = out.representations[0];
    EXPECT_TRUE(rep.has_content_protection);
    ASSERT_EQ(2u, rep.content_protections.size()) << "编解码器继承的 AdaptationSet 级 ContentProtection";
    EXPECT_TRUE(rep.content_protections[0].IsCencDeclaration());
    EXPECT_EQ("11111111-2222-3333-4444-555555555555", rep.content_protections[0].default_kid);
    EXPECT_EQ("edef8ba9", rep.content_protections[1].scheme_id_uri.substr(9, 8));
    EXPECT_TRUE(rep.content_protections[1].has_pssh);
    EXPECT_TRUE(out.IsEncrypted());

    // 加密本身只是登记（Info），不该被当成缺陷
    const StreamingIssue* info = FindIssue(out, StreamingIssueCode::kDashDrmContentProtection);
    ASSERT_TRUE(info != nullptr);
    EXPECT_EQ(IssueSeverity::Info, info->severity);
    EXPECT_NE(info->detail.find("Widevine"), std::string::npos);
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kDashContentProtectionMissingKeyId) == nullptr);
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kDashContentProtectionUnknownScheme) == nullptr);
}

TEST(StreamingDescriptorTest, DashCencWithoutDefaultKidIsReported) {
    const std::string mpd = DashWithDrm(
        "      <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\" value=\"cenc\"/>\n");
    const StreamingPackageResult out = ParseDash(mpd);
    ASSERT_TRUE(FindIssue(out, StreamingIssueCode::kDashContentProtectionMissingKeyId) != nullptr);
}

TEST(StreamingDescriptorTest, DashUnknownDrmSchemeIsReported) {
    const std::string mpd = DashWithDrm(
        "      <ContentProtection schemeIdUri=\"urn:uuid:00000000-dead-beef-0000-000000000000\"/>\n");
    const StreamingPackageResult out = ParseDash(mpd);
    ASSERT_TRUE(FindIssue(out, StreamingIssueCode::kDashContentProtectionUnknownScheme) != nullptr);
}

// ===========================================================================
// DASH: UTCTiming（直播对时）
// ===========================================================================
TEST(StreamingDescriptorTest, DashLiveWithoutUtcTimingIsReported) {
    const std::string mpd = DashWithDrm(
        "",
        "",
        "type=\"dynamic\" availabilityStartTime=\"2026-01-01T00:00:00Z\" "
        "minimumUpdatePeriod=\"PT2S\" timeShiftBufferDepth=\"PT30S\"");
    const StreamingPackageResult out = ParseDash(mpd);
    EXPECT_TRUE(out.IsLive());
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kDashLiveMissingUtcTiming);
    ASSERT_TRUE(issue != nullptr);
    EXPECT_EQ(IssueSeverity::Warning, issue->severity);
}

TEST(StreamingDescriptorTest, DashUtcTimingIsParsedAndIncompleteOneIsReported) {
    // 一条合法的 http-head + 一条缺 value 的
    std::string mpd = DashWithDrm("", "", "type=\"static\" mediaPresentationDuration=\"PT10S\"");
    const size_t pos = mpd.find("<Period");
    mpd.insert(pos,
               "  <UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:http-head:2014\" "
               "value=\"https://example.com/time\"/>\n"
               "  <UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:direct:2014\"/>\n");

    const StreamingPackageResult out = ParseDash(mpd);
    ASSERT_EQ(2u, out.utc_timings.size());
    EXPECT_EQ("urn:mpeg:dash:utc:http-head:2014", out.utc_timings[0].scheme_id_uri);
    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kDashUtcTimingInvalid);
    ASSERT_TRUE(issue != nullptr);
    EXPECT_EQ(1, issue->occurrence_count);
    // 静态包不要求 UTCTiming
    EXPECT_TRUE(FindIssue(out, StreamingIssueCode::kDashLiveMissingUtcTiming) == nullptr);
}

// ===========================================================================
// DASH: EventStream（广告/章节）
// ===========================================================================
TEST(StreamingDescriptorTest, DashEventStreamIsParsedAndInvalidEventIsReported) {
    const std::string events = "    <EventStream schemeIdUri=\"urn:scte:scte35:2013:bin\" timescale=\"90000\" "
                               "value=\"splice\">\n"
                               "      <Event presentationTime=\"0\" duration=\"90000\" id=\"s1\">\n"
                               "        <messageData>AAAA</messageData>\n"
                               "      </Event>\n"
                               "      <Event id=\"s2\"/>\n"
                               "    </EventStream>\n";
    const std::string mpd = DashWithDrm("", events);

    const StreamingPackageResult out = ParseDash(mpd);
    ASSERT_EQ(1u, out.event_streams.size());
    const auto& es = out.event_streams[0];
    EXPECT_EQ("urn:scte:scte35:2013:bin", es.scheme_id_uri);
    EXPECT_EQ(90000u, es.timescale);
    ASSERT_EQ(2u, es.events.size());
    EXPECT_TRUE(es.events[0].has_presentation_time);
    EXPECT_DOUBLE_EQ(0.0, es.events[0].presentation_time_s);
    EXPECT_TRUE(es.events[0].has_duration);
    EXPECT_DOUBLE_EQ(1.0, es.events[0].duration_s);
    EXPECT_TRUE(es.events[0].has_message_data);
    EXPECT_FALSE(es.events[1].has_presentation_time);
    EXPECT_EQ(2u, out.EventCount());

    // 登记一条 Info + 一条"缺 presentationTime"的 Warning
    const StreamingIssue* info = FindIssue(out, StreamingIssueCode::kDashEventStream);
    ASSERT_TRUE(info != nullptr);
    EXPECT_EQ(IssueSeverity::Info, info->severity);
    ASSERT_TRUE(FindIssue(out, StreamingIssueCode::kDashEventStreamInvalid) != nullptr);
}

// ===========================================================================
// DASH: Role（主轨/备轨）
// ===========================================================================
TEST(StreamingDescriptorTest, DashMissingRoleAmongSameTypeSetsIsReported) {
    const std::string audio_sets =
        "    <AdaptationSet id=\"2\" contentType=\"audio\" mimeType=\"audio/mp4\" lang=\"zh\">\n"
        "      <Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"main\"/>\n"
        "      <SegmentTemplate timescale=\"90000\" duration=\"360000\" initialization=\"a-init.mp4\" "
        "media=\"a-$Number$.m4s\"/>\n"
        "      <Representation id=\"a0\" bandwidth=\"128000\" codecs=\"mp4a.40.2\"/>\n"
        "    </AdaptationSet>\n"
        "    <AdaptationSet id=\"3\" contentType=\"audio\" mimeType=\"audio/mp4\" lang=\"en\">\n"
        "      <SegmentTemplate timescale=\"90000\" duration=\"360000\" initialization=\"a-init.mp4\" "
        "media=\"a-$Number$.m4s\"/>\n"
        "      <Representation id=\"a1\" bandwidth=\"128000\" codecs=\"mp4a.40.2\"/>\n"
        "    </AdaptationSet>\n";
    const std::string mpd = DashWithDrm("", audio_sets);

    const StreamingPackageResult out = ParseDash(mpd);
    // 0 = 视频（DashWithDrm 自带的），1/2 = 两条音频
    ASSERT_EQ(3u, out.periods[0].adaptation_sets.size());
    EXPECT_EQ(1u, out.periods[0].adaptation_sets[1].roles.size());
    EXPECT_TRUE(out.periods[0].adaptation_sets[2].roles.empty());

    const StreamingIssue* issue = FindIssue(out, StreamingIssueCode::kDashMissingRole);
    ASSERT_TRUE(issue != nullptr);
    EXPECT_EQ(1, issue->occurrence_count) << "只有英文轨缺 Role";
}

// ===========================================================================
// DASH: EssentialProperty（客户端必须理解的扩展）
// ===========================================================================
TEST(StreamingDescriptorTest, DashEssentialPropertyIsParsed) {
    const std::string mpd = DashWithDrm(
        "      <EssentialProperty schemeIdUri=\"urn:mpeg:dash:ssr:2023\" value=\"1\"/>\n"
        "      <EssentialProperty value=\"missing-scheme\"/>\n");
    const StreamingPackageResult out = ParseDash(mpd);
    ASSERT_FALSE(out.representations.empty());
    ASSERT_EQ(2u, out.representations[0].essential_properties.size())
        << "AdaptationSet 级的 EssentialProperty 要继承到 Representation";

    const StreamingIssue* info = FindIssue(out, StreamingIssueCode::kDashEssentialProperty);
    ASSERT_TRUE(info != nullptr);
    EXPECT_EQ(IssueSeverity::Info, info->severity);
    EXPECT_NE(info->detail.find("urn:mpeg:dash:ssr:2023"), std::string::npos);
    ASSERT_TRUE(FindIssue(out, StreamingIssueCode::kDashEssentialPropertyUnknown) != nullptr);
}

// ===========================================================================
// 健康包不该因为新规则而多出告警
// ===========================================================================
TEST(StreamingDescriptorTest, CleanHlsVodHasNoNewWarnings) {
    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-VERSION:3\n"
                                 "#EXT-X-TARGETDURATION:4\n"
                                 "#EXT-X-MEDIA-SEQUENCE:0\n"
                                 "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                 "#EXTINF:4.000,\n"
                                 "seg0.ts\n"
                                 "#EXTINF:4.000,\n"
                                 "seg1.ts\n"
                                 "#EXT-X-ENDLIST\n";
    const StreamingPackageResult out = ParseHls(playlist);
    for (const auto& issue : out.issues) {
        EXPECT_LT(static_cast<int>(issue.severity), static_cast<int>(IssueSeverity::Warning))
            << "干净的点播清单不该出新告警：" << issue.code;
    }
}
