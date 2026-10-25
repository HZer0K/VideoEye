// 解析层回归：UTF-8 路径、主轨选择与封面图语义。
//
// 这些用例覆盖的是"改起来只有几行、坏起来只在一部分机器上出现"的缺陷：
//   * std::ifstream 在 Windows 上按 ANSI 代码页解释 char* 路径 → 中文目录直接打不开；
//   * "第一条视频流"常常是内嵌封面（ATTACHED_PIC），把它当主视频会让后续统计全错。
//
// 说明：TimelineAnalyzer.cpp 是直接编进本目标的（见 tests/CMakeLists.txt），
// 不链 VideoEyeAnalysis —— 该分析器只依赖 core/domain，拉整层会让这个
// "纯解析"用例跟着 FFmpeg 一起编，跑一次要多等十几秒。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analysis/detail/SeqFileReader.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/media/container/IsobmffParser.h"
#include "core/media/probe/FileProbe.h"
#include "core/media/streaming/ManifestReader.h"

namespace {
using namespace videoeye;

model::PacketTiming Packet(int type, double pts, double dts, bool key_frame = true) {
    model::PacketTiming packet;
    packet.stream_index = 0;
    packet.media_type = type;
    packet.pts_ms = pts;
    packet.dts_ms = dts;
    packet.duration_ms = 40.0;
    packet.key_frame = key_frame;
    return packet;
}

// 中文目录 + 中文文件名：这是用户最常见的"打不开"场景（开发机的英文代码页下
// 一切正常，只有换到 GBK/其它代码页时才复现）。
class UnicodeFiles : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
                     ("videoeye-parser-review-" + std::to_string(stamp) + "-" + std::to_string(serial.fetch_add(1))) /
                     std::filesystem::u8path(u8"\u4e2d\u6587-\u89c6\u9891");
        ASSERT_TRUE(std::filesystem::create_directories(directory_));
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory_.parent_path(), ignored);
    }
    std::filesystem::path Write(const std::string& name, const std::string& contents) {
        const auto path = directory_ / std::filesystem::u8path(name);
        std::ofstream out(path, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        EXPECT_TRUE(out.good());
        return path;
    }
    std::filesystem::path directory_;
};
} // namespace

// ---------------------------------------------------------------------------
// 时间轴：B 帧重排不是"PTS 非单调"
//
// 逐包看到的是**解码顺序**：0,120,40,80,240,160,200。真正的显示顺序是单调的，
// 原来这套序列会让每个含 B 帧的文件都报 PtsNonMonotonic + DuplicatedTimestamp。
// ---------------------------------------------------------------------------
TEST(ParserReviewTimeline, BFrameDecodeOrderDoesNotReportPtsRollback) {
    TimelineAnalyzer analyzer;
    const double pts[] = {0, 120, 40, 80, 240, 160, 200};
    for (int i = 0; i < 7; ++i) {
        // DTS 单调递增 = 解码顺序正常，回退可由重排解释
        analyzer.OnPacket(Packet(0, pts[i], (i - 2) * 40.0, false));
    }
    analyzer.Finish();
    EXPECT_FALSE(analyzer.result().HasIssue(model::TimelineIssueType::PtsNonMonotonic));
    EXPECT_FALSE(analyzer.result().HasIssue(model::TimelineIssueType::DuplicatedTimestamp));
    EXPECT_FALSE(analyzer.result().HasIssue(model::TimelineIssueType::DtsNonMonotonic));
}

// 解码顺序本身坏了：DTS 回退，PTS 回退也照报
TEST(ParserReviewTimeline, DtsRollbackStillReportsError) {
    TimelineAnalyzer analyzer;
    analyzer.OnPacket(Packet(0, 120, 80, false));
    analyzer.OnPacket(Packet(0, 80, 40, false));
    analyzer.Finish();
    EXPECT_TRUE(analyzer.result().HasIssue(model::TimelineIssueType::DtsNonMonotonic));
    EXPECT_TRUE(analyzer.result().HasIssue(model::TimelineIssueType::PtsNonMonotonic));
}

// 关键帧回退无法用重排解释（拼接/断点重置时间戳）：这就是拼接诊断的入口
TEST(ParserReviewTimeline, KeyframePtsRollbackIsStillReported) {
    TimelineAnalyzer analyzer;
    analyzer.OnPacket(Packet(0, 1000, 1000, true));
    analyzer.OnPacket(Packet(0, 0, 1040, true));
    analyzer.Finish();
    EXPECT_TRUE(analyzer.result().HasIssue(model::TimelineIssueType::PtsNonMonotonic));
}

// 没有 DTS 就没有"解码顺序"的证据，不能假定包序等于显示序
TEST(ParserReviewTimeline, VideoWithoutDtsStillReportsRollback) {
    TimelineAnalyzer analyzer;
    analyzer.OnPacket(Packet(0, 120, model::kNoTimestamp, false));
    analyzer.OnPacket(Packet(0, 40, model::kNoTimestamp, false));
    analyzer.Finish();
    EXPECT_TRUE(analyzer.result().HasIssue(model::TimelineIssueType::PtsNonMonotonic));
}

TEST(ParserReviewTimeline, AudioPtsRollbackStillReportsError) {
    TimelineAnalyzer analyzer;
    analyzer.OnPacket(Packet(1, 80, 0));
    analyzer.OnPacket(Packet(1, 40, 40));
    analyzer.Finish();
    EXPECT_TRUE(analyzer.result().HasIssue(model::TimelineIssueType::PtsNonMonotonic));
}

// 解码帧按显示顺序：这里的回退才是真异常，且必须按流分别比较
TEST(ParserReviewTimeline, DisplayOrderRollbackIsDetectedPerStream) {
    TimelineAnalyzer analyzer;
    model::FrameTimingInfo frame;

    frame.stream_index = 0;
    frame.display_ms = 100;
    analyzer.OnFrame(frame);
    frame.stream_index = 1;
    frame.display_ms = 0;
    analyzer.OnFrame(frame);
    EXPECT_FALSE(analyzer.result().HasIssue(model::TimelineIssueType::PtsNonMonotonic));

    frame.stream_index = 0;
    frame.display_ms = 60;
    analyzer.OnFrame(frame);
    EXPECT_EQ(analyzer.result().CountOf(model::TimelineIssueType::PtsNonMonotonic), 1);
}

// 包级摘要要用调用方指定的主轨，而不是"流号最小的那条"
TEST(ParserReviewTimeline, SelectedVideoIsUsedForSummary) {
    TimelineAnalyzer analyzer;
    TimelineAnalyzer::Options options;
    options.preferred_video_stream_index = 1;
    analyzer.SetOptions(options);

    analyzer.OnPacket(Packet(0, 900, 900));
    auto primary = Packet(0, 100, 100);
    primary.stream_index = 1;
    analyzer.OnPacket(primary);
    analyzer.Finish();

    EXPECT_EQ(analyzer.result().video_stream_index, 1);
    EXPECT_DOUBLE_EQ(analyzer.result().video_start_ms, 100.0);
}

TEST_F(UnicodeFiles, SequentialReaderOpensUtf8Paths) {
    const auto path = Write(u8"\u89c6\u9891.bin", "test-data");
    SeqFileReader reader(path.u8string());
    ASSERT_TRUE(reader.IsOpen());
    EXPECT_EQ(reader.Read(9), "test-data");
}

TEST_F(UnicodeFiles, Mp4ParserAndProbeOpenUtf8Paths) {
    const unsigned char bytes[] = {0, 0, 0,   20,  'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0,   0,
                                   0, 0, 'i', 's', 'o', 'm', 0,   0,   0,   8,   'm', 'o', 'o', 'v'};
    const auto path = Write(u8"\u89c6\u9891.mp4", std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes)));
    EXPECT_EQ(ProbeFileHeader(path.u8string()).first_box_type, "ftyp");
    IsobmffFile parsed;
    ASSERT_TRUE(IsobmffParser::Parse(path.u8string(), parsed));
    EXPECT_TRUE(parsed.ok);
    ASSERT_GE(parsed.top_level_order.size(), 2u);
    EXPECT_EQ(parsed.top_level_order.front(), "ftyp");
    EXPECT_EQ(parsed.top_level_order.back(), "moov");
}

TEST_F(UnicodeFiles, ManifestSizeTextAndLineReadersOpenUtf8Paths) {
    const std::string contents = "#EXTM3U\n#EXT-X-ENDLIST\n";
    const auto path = Write(u8"\u6e05\u5355.m3u8", contents);
    int64_t size = 0;
    ASSERT_TRUE(manifest::FileSizeOf(path.u8string(), size));
    EXPECT_EQ(size, static_cast<int64_t>(contents.size()));
    std::string text;
    EXPECT_EQ(manifest::ReadManifestText(path.u8string(), {}, nullptr, text), manifest::ManifestReadStatus::Ok);
    EXPECT_EQ(text, contents);
    std::vector<std::string> lines;
    EXPECT_EQ(manifest::ReadManifestLines(path.u8string(), {}, nullptr, lines), manifest::ManifestReadStatus::Ok);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1], "#EXT-X-ENDLIST");
}

// 封面图在 FFmpeg 里就是一条一帧的 MJPEG 视频流：它既不该算"这个文件有视频"，
// 也不该在有多条视频轨时抢走主轨的位置。
TEST(ParserReviewStreams, AttachedPicturesAreExcludedAndPrimarySelectionIsHonored) {
    model::AnalysisResult result;
    model::StreamDigest cover;
    cover.index = 0;
    cover.media_type = 0;
    cover.attached_picture = true;
    result.streams.push_back(cover);
    EXPECT_EQ(result.VideoStreamCount(), 0);
    EXPECT_TRUE(result.FirstVideoStream() == nullptr);

    model::StreamDigest video;
    video.index = 1;
    video.media_type = 0;
    result.streams.push_back(video);
    video.index = 2;
    result.streams.push_back(video);
    result.primary_video_stream_index = 2;
    ASSERT_TRUE(result.FirstVideoStream() != nullptr);
    EXPECT_EQ(result.FirstVideoStream()->index, 2);
    EXPECT_EQ(result.VideoStreamCount(), 2);
}
