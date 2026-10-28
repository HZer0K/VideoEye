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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/analysis/detail/SeqFileReader.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/media/codec/ExtradataParser.h"
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

// ---------------------------------------------------------------------------
// 零拷贝 NAL 遍历：视图必须与 Extract* 逐项等价
//
// ForEachAnnexBNalUnit / ForEachLengthPrefixedNalUnit 是逐包热路径的新入口
// （EP 巡检不再给每个 NAL 复制一份载荷）。这里用同一个输入跑两条路，逐项比对
// type / nal_ref_idc / payload 内容与长度 —— 任何"切分规则悄悄分叉"都会红。
// ---------------------------------------------------------------------------
namespace {

// 拼一条 AnnexB 流：每个 NAL 用 4 字节起始码分隔
std::vector<uint8_t> AnnexBStream(const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& nals) {
    std::vector<uint8_t> out;
    for (const auto& nal : nals) {
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.push_back(nal.first);
        out.insert(out.end(), nal.second.begin(), nal.second.end());
    }
    return out;
}

// 拼一条 4 字节长度前缀流
std::vector<uint8_t> LengthPrefixedStream(const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& nals) {
    std::vector<uint8_t> out;
    for (const auto& nal : nals) {
        const uint32_t len = static_cast<uint32_t>(nal.second.size() + 1);
        out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(nal.first);
        out.insert(out.end(), nal.second.begin(), nal.second.end());
    }
    return out;
}

void ExpectViewsMatchUnits(const std::vector<NalUnit>& units, const std::vector<NalView>& views,
                           const std::vector<uint8_t>& buffer) {
    ASSERT_EQ(units.size(), views.size());
    for (size_t i = 0; i < units.size(); ++i) {
        EXPECT_EQ(views[i].type, units[i].type) << "index " << i;
        EXPECT_EQ(views[i].nal_ref_idc, units[i].nal_ref_idc) << "index " << i;
        EXPECT_EQ(views[i].is_idr, units[i].is_idr) << "index " << i;
        ASSERT_EQ(views[i].size, units[i].data.size()) << "index " << i;
        EXPECT_EQ(0, std::memcmp(views[i].payload, units[i].data.data(), views[i].size)) << "index " << i;
        // 零拷贝的证据：payload 必须落在输入缓冲内部
        ASSERT_GE(views[i].payload, buffer.data()) << "index " << i;
        ASSERT_LE(views[i].payload + views[i].size, buffer.data() + buffer.size()) << "index " << i;
    }
}

} // namespace

TEST(ParserReviewExtradata, AnnexBNalViewsMatchExtractedUnits) {
    const auto buffer = AnnexBStream(
        {{0x67, {0x42, 0x00, 0x1F, 0xAA}}, {0x68, {0xCE, 0x3C, 0x80}}, {0x65, {0x88, 0x84, 0x00, 0x33, 0xFF}}});

    const std::vector<NalUnit> units =
        ExtradataParser::ExtractAnnBNalUnits(buffer.data(), buffer.size(), NalSyntax::H264);
    std::vector<NalView> views;
    ExtradataParser::ForEachAnnexBNalUnit(buffer.data(), buffer.size(), NalSyntax::H264,
                                          [&views](const NalView& view) { views.push_back(view); });

    ExpectViewsMatchUnits(units, views, buffer);
    // 顺带钉住 header 语义：SPS=7 / PPS=8 / IDR=5
    ASSERT_EQ(views.size(), 3u);
    EXPECT_EQ(views[0].type, 7);
    EXPECT_EQ(views[2].type, 5);
    EXPECT_TRUE(views[2].is_idr);
    EXPECT_EQ(views[2].nal_ref_idc, 3);
}

TEST(ParserReviewExtradata, LengthPrefixedNalViewsMatchExtractedUnits) {
    const auto buffer =
        LengthPrefixedStream({{0x67, {0x42, 0x00, 0x1F}}, {0x68, {0xCE, 0x3C, 0x80}}, {0x65, {0x88, 0x84, 0x00}}});

    const std::vector<NalUnit> units =
        ExtradataParser::ExtractLengthPrefixedNalUnits(buffer.data(), buffer.size(), NalSyntax::H264, 4, nullptr);
    std::vector<NalView> views;
    bool truncated = true;
    ExtradataParser::ForEachLengthPrefixedNalUnit(buffer.data(), buffer.size(), NalSyntax::H264, 4, &truncated,
                                                  [&views](const NalView& view) { views.push_back(view); });

    ExpectViewsMatchUnits(units, views, buffer);
    EXPECT_FALSE(truncated);
}

// SeqFileReader::Size() 改成打开时缓存后，这个不变量必须继续成立：
// 读位置怎么变、读到 EOF 再 seek 回来，Size() 都还是文件真实大小。
TEST_F(UnicodeFiles, SequentialReaderCachedSizeSurvivesSeeksAndEof) {
    const std::string contents = "0123456789";
    const auto path = Write("size.bin", contents);
    SeqFileReader reader(path.u8string());
    ASSERT_TRUE(reader.IsOpen());
    EXPECT_EQ(reader.Size(), static_cast<int64_t>(contents.size()));

    EXPECT_EQ(reader.Read(4), "0123");
    EXPECT_EQ(reader.Pos(), 4);
    EXPECT_EQ(reader.Size(), static_cast<int64_t>(contents.size())); // 读位置不影响缓存大小

    reader.Seek(reader.Size()); // 停到 EOF
    EXPECT_TRUE(reader.AtEnd());
    EXPECT_EQ(reader.Pos(), static_cast<int64_t>(contents.size()));

    reader.Seek(2);
    EXPECT_FALSE(reader.AtEnd());
    EXPECT_EQ(reader.Read(100), "23456789"); // 短读返回实际长度
    EXPECT_EQ(reader.Size(), static_cast<int64_t>(contents.size()));
}
