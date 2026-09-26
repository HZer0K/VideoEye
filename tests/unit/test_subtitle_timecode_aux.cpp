// 功能 9（字幕 / 时码 / 辅助数据轨）的解析与校验单元测试
//
// 与分析器测试的常规分工：
//   * 文本字幕、时码换算、SCTE-35 位流解析都做成不依赖 FFmpeg 的纯函数，
//     这里全部用构造出来的输入验证（不需要真实媒体文件）；
//   * 校验规则的严重度（重叠=Warning 之类）在这里断言，避免规则表改了以后
//     报告级别悄悄漂移。

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "core/analyzer/Scte35Analyzer.h"
#include "core/analyzer/SubtitleAnalyzer.h"
#include "core/analyzer/TimecodeAnalyzer.h"
#include "core/model/SubtitleCueInfo.h"
#include "core/model/TimecodeInfo.h"

namespace {

using namespace videoeye;

constexpr double kNtscFps = 30000.0 / 1001.0;   // 29.97

// ---- 小工具: 大端比特写入（用来拼 SCTE-35 section）----
class BitWriter {
public:
    void Write(uint64_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            const uint8_t bit = static_cast<uint8_t>((value >> i) & 1u);
            const size_t index = bits_;
            if (index % 8 == 0) bytes_.push_back(0);
            bytes_[index / 8] |= static_cast<uint8_t>(bit << (7 - (index % 8)));
            ++bits_;
        }
    }
    void WriteBytes(const std::vector<uint8_t>& data) {
        for (uint8_t b : data) Write(b, 8);
    }
    size_t BitCount() const { return bits_; }
    std::vector<uint8_t> Bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    size_t bits_ = 0;
};

// 拼一段 splice_insert（含 segmentation_descriptor），返回完整 section（含 CRC）
std::vector<uint8_t> BuildSpliceInsertSection() {
    // ---- 命令体（splice_insert）----
    BitWriter body;
    body.Write(0x12345678, 32);          // splice_event_id
    body.Write(0, 1);                    // splice_event_cancel_indicator
    body.Write(0, 7);
    body.Write(1, 1);                    // out_of_network_indicator
    body.Write(1, 1);                    // program_splice_flag
    body.Write(1, 1);                    // duration_flag
    body.Write(0, 1);                    // splice_immediate_flag
    body.Write(0, 4);
    body.Write(1, 1);                    // splice_time: time_specified_flag
    body.Write(0, 6);
    body.Write(900000, 33);              // pts_time = 10 s（90000 时基）
    body.Write(1, 1);                    // break_duration: auto_return
    body.Write(0, 6);
    body.Write(2700000, 33);             // duration = 30 s
    body.Write(0x0001, 16);              // unique_program_id
    body.Write(1, 8);                    // avail_num
    body.Write(2, 8);                    // avails_expected
    const std::vector<uint8_t> command_body = body.Bytes();
    const size_t command_length = command_body.size();

    // ---- segmentation_descriptor ----
    BitWriter seg_body;
    seg_body.Write(0xABCD1234, 32);      // segmentation_event_id
    seg_body.Write(0, 1);                // cancel
    seg_body.Write(0, 7);
    seg_body.Write(1, 1);                // program_segmentation_flag
    seg_body.Write(1, 1);                // segmentation_duration_flag
    seg_body.Write(1, 1);                // delivery_not_restricted_flag
    seg_body.Write(0, 5);
    seg_body.Write(2700000, 40);         // segmentation_duration = 30 s
    seg_body.Write(0x08, 8);             // upid_type
    seg_body.Write(0, 8);                // upid_length
    seg_body.Write(0x30, 8);             // type_id = Provider Advertisement Start
    seg_body.Write(1, 8);                // segment_num
    seg_body.Write(3, 8);                // segments_expected
    const std::vector<uint8_t> seg_bytes = seg_body.Bytes();
    const size_t descriptor_length = 4 + seg_bytes.size();   // identifier("CUEI") + body

    const size_t descriptor_loop_length = 2 + 1 + descriptor_length;  // tag + length + body
    const size_t section_length = 1 /*protocol_version*/ + 5 /*flags + pts_adjustment*/ +
                                  1 /*cw_index*/ + 3 /*tier + splice_command_length*/ +
                                  1 /*splice_command_type*/ + command_length +
                                  2 /*descriptor_loop_length*/ + descriptor_loop_length +
                                  4 /*CRC_32*/;

    BitWriter out;
    out.Write(0xFC, 8);                  // table_id
    out.Write(0, 1);                     // section_syntax_indicator
    out.Write(0, 1);                     // private_indicator
    out.Write(0, 2);                     // sap_type
    out.Write(section_length, 12);
    out.Write(0, 8);                     // protocol_version
    out.Write(0, 1);                     // encrypted_packet
    out.Write(0, 6);                     // encryption_algorithm
    out.Write(0, 33);                    // pts_adjustment
    out.Write(0, 8);                     // cw_index
    out.Write(0, 12);                    // tier
    out.Write(command_length, 12);       // splice_command_length
    out.Write(0x05, 8);                  // splice_command_type = splice_insert
    out.WriteBytes(command_body);
    out.Write(descriptor_loop_length, 16);
    out.Write(0x02, 16);                 // splice_descriptor_tag = segmentation
    out.Write(descriptor_length, 8);
    out.Write('C', 8);                   // identifier "CUEI"
    out.Write('U', 8);
    out.Write('E', 8);
    out.Write('I', 8);
    out.WriteBytes(seg_bytes);

    std::vector<uint8_t> bytes = out.Bytes();
    const uint32_t crc = analyzer::Scte35Analyzer::Crc32Mpeg2(bytes.data(), bytes.size());
    bytes.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>(crc & 0xFF));
    return bytes;
}

model::SubtitleCue MakeCue(int index, double start, double end, const std::string& text) {
    model::SubtitleCue cue;
    cue.index = index;
    cue.stream_index = 0;
    cue.start_seconds = start;
    cue.end_seconds = end;
    cue.duration_seconds = end - start;
    cue.text = text;
    cue.raw_text = text;
    cue.format = model::SubtitleFormat::Srt;
    cue.char_count = analyzer::SubtitleAnalyzer::CountVisibleChars(text);
    cue.empty = (cue.char_count == 0);
    return cue;
}

int CountType(const std::vector<model::SubtitleIssue>& issues, model::SubtitleIssueType type) {
    int count = 0;
    for (const model::SubtitleIssue& issue : issues) {
        if (issue.type == type) ++count;
    }
    return count;
}

}  // namespace

// ============================================================
// 字幕：文本解析
// ============================================================

TEST(SubtitleAnalyzerTest, ParseSrtCues) {
    const std::string srt =
        "1\n"
        "00:00:01,000 --> 00:00:04,000\n"
        "第一行\n"
        "\n"
        "2\n"
        "00:00:05,500 --> 00:00:07,000\n"
        "第二行\n"
        "换行也在一起\n";

    std::vector<analyzer::ParsedSubtitleCue> cues;
    ASSERT_TRUE(analyzer::SubtitleAnalyzer::ParseSrtText(srt, cues));
    ASSERT_EQ(cues.size(), 2u);

    EXPECT_EQ(cues[0].cue_number, 1);
    EXPECT_NEAR(cues[0].start_seconds, 1.0, 1e-6);
    EXPECT_NEAR(cues[0].end_seconds, 4.0, 1e-6);
    EXPECT_EQ(cues[0].text, "第一行");

    EXPECT_EQ(cues[1].cue_number, 2);
    EXPECT_NEAR(cues[1].start_seconds, 5.5, 1e-6);
    EXPECT_NEAR(cues[1].end_seconds, 7.0, 1e-6);
    EXPECT_EQ(cues[1].text, "第二行 换行也在一起");   // 多行压成单行显示
}

TEST(SubtitleAnalyzerTest, ParseWebVttSkipsHeaderAndNote) {
    const std::string vtt =
        "WEBVTT - 测试\n"
        "\n"
        "NOTE 这不是 cue\n"
        "\n"
        "1\n"
        "00:00:02.000 --> 00:00:05.000 align:start position:10%\n"
        "<v Speaker>你好</v>\n";

    std::vector<analyzer::ParsedSubtitleCue> cues;
    ASSERT_TRUE(analyzer::SubtitleAnalyzer::ParseWebVttText(vtt, cues));
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_NEAR(cues[0].start_seconds, 2.0, 1e-6);
    EXPECT_NEAR(cues[0].end_seconds, 5.0, 1e-6);
    EXPECT_EQ(cues[0].text, "你好");   // <v> 标签被剥掉
}

TEST(SubtitleAnalyzerTest, ParseAssDialogueLine) {
    const std::string line =
        "Dialogue: 0,0:00:01.50,0:00:04.00,Default,,0,0,0,,{\\pos(10,20)}你好\\N世界";
    analyzer::ParsedSubtitleCue cue;
    ASSERT_TRUE(analyzer::SubtitleAnalyzer::ParseAssDialogueLine(line, cue));
    EXPECT_NEAR(cue.start_seconds, 1.5, 1e-6);
    EXPECT_NEAR(cue.end_seconds, 4.0, 1e-6);
    EXPECT_EQ(cue.text, "你好 世界");   // 覆盖指令与 \N 都处理掉
}

TEST(SubtitleAnalyzerTest, ParseMovTextPacketPayload) {
    // tx3g 样本: uint16 长度前缀 + 文本
    const std::string payload = "Hello";
    std::vector<uint8_t> data;
    data.push_back(0x00);
    data.push_back(static_cast<uint8_t>(payload.size()));
    for (char c : payload) data.push_back(static_cast<uint8_t>(c));
    data.push_back(0x00);   // 尾部样式 box，应当被忽略

    analyzer::ParsedSubtitleCue cue;
    ASSERT_TRUE(analyzer::SubtitleAnalyzer::ParsePacketPayload(
        AV_CODEC_ID_MOV_TEXT, data.data(), data.size(), 12.0, 2.0, cue));
    EXPECT_EQ(cue.text, "Hello");
    EXPECT_NEAR(cue.start_seconds, 12.0, 1e-6);   // 包里没写时间 -> 用 PTS
    EXPECT_NEAR(cue.end_seconds, 14.0, 1e-6);     // 用包时长补结束时间
}

TEST(SubtitleAnalyzerTest, DecodeCea608BasicText) {
    // 'H' 'i' 后面跟一个控制码对 (0x14 0x20)，控制码不应出现在文本里
    const std::vector<uint8_t> data = {'H', 'i', 0x14, 0x20, 'y', 'o'};
    const std::string text = analyzer::SubtitleAnalyzer::DecodeCea608Field(data.data(), data.size());
    EXPECT_EQ(text, "Hiyo");
}

// ============================================================
// 字幕：校验规则（验收项: 重叠 / 空字幕必须 warning）
// ============================================================

TEST(SubtitleAnalyzerTest, OverlappingCuesProduceWarning) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 4.0, "第一条"));
    cues.push_back(MakeCue(1, 3.0, 5.0, "第二条"));   // 3.0 < 4.0 -> 重叠

    std::vector<model::SubtitleIssue> issues;
    analyzer::SubtitleAnalyzer::ValidateCues(cues, issues, analyzer::SubtitleOptions{}, 60.0);

    ASSERT_EQ(CountType(issues, model::SubtitleIssueType::Overlap), 1);
    const model::SubtitleIssue& overlap = issues.front();
    EXPECT_EQ(overlap.type, model::SubtitleIssueType::Overlap);
    EXPECT_EQ(overlap.severity, model::IssueSeverity::Warning);
    EXPECT_EQ(overlap.cue_index, 1);
    EXPECT_TRUE(cues[1].has_issue);
    EXPECT_FALSE(cues[0].has_issue);
}

TEST(SubtitleAnalyzerTest, EmptyCueProducesWarning) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 4.0, ""));
    cues.push_back(MakeCue(1, 5.0, 8.0, "   "));   // 只有空白也算空

    std::vector<model::SubtitleIssue> issues;
    analyzer::SubtitleAnalyzer::ValidateCues(cues, issues, analyzer::SubtitleOptions{}, 60.0);

    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::EmptyText), 2);
    for (const model::SubtitleIssue& issue : issues) {
        EXPECT_EQ(issue.severity, model::IssueSeverity::Warning);
    }
}

TEST(SubtitleAnalyzerTest, NonMonotonicCuesProduceError) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 10.0, 12.0, "后"));
    cues.push_back(MakeCue(1, 5.0, 7.0, "前"));

    std::vector<model::SubtitleIssue> issues;
    analyzer::SubtitleAnalyzer::ValidateCues(cues, issues, analyzer::SubtitleOptions{}, 60.0);

    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::NonMonotonic), 1);
    for (const model::SubtitleIssue& issue : issues) {
        if (issue.type == model::SubtitleIssueType::NonMonotonic) {
            EXPECT_EQ(issue.severity, model::IssueSeverity::Error);
        }
    }
}

TEST(SubtitleAnalyzerTest, DurationWindowAndOutOfRange) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 1.2, "太快了"));      // 0.2s < 0.5s
    cues.push_back(MakeCue(1, 2.0, 12.0, "太慢了"));     // 10s > 8s
    cues.push_back(MakeCue(2, 90.0, 92.0, "超出时长"));  // 媒体只有 30s

    std::vector<model::SubtitleIssue> issues;
    analyzer::SubtitleAnalyzer::ValidateCues(cues, issues, analyzer::SubtitleOptions{}, 30.0);

    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::TooShort), 1);
    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::TooLong), 1);
    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::OutOfRange), 1);
}

TEST(SubtitleAnalyzerTest, AdjacentCuesAreNotOverlap) {
    // 首尾相接（差 0）在容差内不算重叠
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 2.0, "甲"));
    cues.push_back(MakeCue(1, 2.0, 3.0, "乙"));

    std::vector<model::SubtitleIssue> issues;
    analyzer::SubtitleAnalyzer::ValidateCues(cues, issues, analyzer::SubtitleOptions{}, 60.0);
    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::Overlap), 0);
}

// ============================================================
// 时码
// ============================================================

TEST(TimecodeTest, ParseAndFormat) {
    const model::Timecode tc = model::TimecodeFromString("01:02:03:04");
    ASSERT_TRUE(tc.valid);
    EXPECT_EQ(tc.hours, 1);
    EXPECT_EQ(tc.minutes, 2);
    EXPECT_EQ(tc.seconds, 3);
    EXPECT_EQ(tc.frames, 4);
    EXPECT_FALSE(tc.drop_frame);
    EXPECT_EQ(tc.ToString(), "01:02:03:04");

    const model::Timecode df = model::TimecodeFromString("01:02:03;04");
    ASSERT_TRUE(df.valid);
    EXPECT_TRUE(df.drop_frame);
    EXPECT_EQ(df.ToString(), "01:02:03;04");   // drop frame 用 ';' 分隔
}

TEST(TimecodeTest, FrameCountRoundTrip) {
    // 29.97 drop frame: 一小时的帧数（107892）读出来正好是 01:00:00;00
    const model::Timecode drop = model::TimecodeFromFrameCount(107892, kNtscFps, true);
    ASSERT_TRUE(drop.valid);
    EXPECT_EQ(drop.ToString(), "01:00:00;00");
    EXPECT_EQ(drop.ToFrameCount(kNtscFps), 107892);

    // 同样的帧数按 non-drop 读，读数只有 00:59:56:12 —— 比真实时间慢约 3.6 秒，
    // 这就是"29.97 素材必须查 drop-frame"的原因。
    const model::Timecode non_drop = model::TimecodeFromFrameCount(107892, kNtscFps, false);
    EXPECT_EQ(non_drop.ToString(), "00:59:56:12");

    // 真实经过时间由帧数决定，两种写法算出来应当一致（≈1 小时）
    EXPECT_NEAR(drop.ToSeconds(kNtscFps), 3600.0, 0.05);
    EXPECT_NEAR(non_drop.ToSeconds(kNtscFps), 3600.0, 0.05);
    const double non_drop_label_seconds = static_cast<double>(non_drop.hours) * 3600.0 +
                                          static_cast<double>(non_drop.minutes) * 60.0 +
                                          static_cast<double>(non_drop.seconds) +
                                          static_cast<double>(non_drop.frames) / 30.0;
    EXPECT_NEAR(3600.0 - non_drop_label_seconds, 3.6, 0.05);
}

TEST(TimecodeTest, TmcdSampleGivesFirstFrameTimecode) {
    // tmcd 样本是 4 字节大端帧序号：360 帧 @30fps = 00:00:12:00
    const std::vector<uint8_t> sample = {0x00, 0x00, 0x01, 0x68};
    model::Timecode tc;
    ASSERT_TRUE(analyzer::TimecodeAnalyzer::DecodeTmcdSample(sample.data(), sample.size(), 30.0,
                                                             false, tc));
    ASSERT_TRUE(tc.valid);
    EXPECT_EQ(tc.ToString(), "00:00:12:00");

    // 载荷太短 / 帧率为 0 都要失败，不能给出假时码
    model::Timecode bad;
    EXPECT_FALSE(analyzer::TimecodeAnalyzer::DecodeTmcdSample(sample.data(), 2, 30.0, false, bad));
    EXPECT_FALSE(analyzer::TimecodeAnalyzer::DecodeTmcdSample(sample.data(), 4, 0.0, false, bad));
}

TEST(TimecodeTest, DropFrameDetectionAndValidity) {
    EXPECT_TRUE(model::IsDropFrameRate(kNtscFps));
    EXPECT_TRUE(model::IsDropFrameRate(60000.0 / 1001.0));
    EXPECT_FALSE(model::IsDropFrameRate(25.0));
    EXPECT_FALSE(model::IsDropFrameRate(30.0));

    // 帧号越界
    EXPECT_FALSE(model::IsValidTimecode(model::TimecodeFromString("00:00:00:30"), 30.0));
    EXPECT_TRUE(model::IsValidTimecode(model::TimecodeFromString("00:00:00:29"), 30.0));

    // drop frame 每分钟（除整十分钟）要跳过前两帧
    EXPECT_FALSE(model::IsValidTimecode(model::TimecodeFromString("00:01:00;00"), kNtscFps));
    EXPECT_TRUE(model::IsValidTimecode(model::TimecodeFromString("00:01:00;02"), kNtscFps));
}

// ============================================================
// SCTE-35
// ============================================================

TEST(Scte35AnalyzerTest, Crc32Mpeg2CheckValue) {
    const std::string check = "123456789";
    const uint32_t crc = analyzer::Scte35Analyzer::Crc32Mpeg2(
        reinterpret_cast<const uint8_t*>(check.data()), check.size());
    EXPECT_EQ(crc, 0x0376E6E7u);   // CRC-32/MPEG-2 的标准校验值
}

TEST(Scte35AnalyzerTest, ParseSpliceInsertWithSegmentation) {
    const std::vector<uint8_t> section = BuildSpliceInsertSection();
    model::Scte35Cue cue;
    ASSERT_TRUE(analyzer::Scte35Analyzer::ParseSection(section.data(), section.size(), cue))
        << cue.parse_error;

    EXPECT_TRUE(cue.valid);
    EXPECT_TRUE(cue.crc_checked);
    EXPECT_TRUE(cue.crc_valid);
    EXPECT_EQ(cue.command, model::Scte35Command::SpliceInsert);
    ASSERT_TRUE(cue.has_event_id);
    EXPECT_EQ(cue.event_id, 0x12345678u);

    EXPECT_TRUE(cue.out_of_network);
    EXPECT_TRUE(cue.program_splice);
    ASSERT_TRUE(cue.has_splice_time);
    EXPECT_NEAR(cue.splice_time_seconds, 10.0, 1e-6);
    ASSERT_TRUE(cue.has_duration);
    EXPECT_NEAR(cue.break_duration_seconds, 30.0, 1e-6);
    EXPECT_TRUE(cue.auto_return);
    EXPECT_EQ(cue.unique_program_id, 1);
    EXPECT_EQ(cue.avail_num, 1);
    EXPECT_EQ(cue.avails_expected, 2);
    EXPECT_STREQ(cue.NetworkIndicatorText(), "OUT");

    ASSERT_EQ(cue.segmentation.size(), 1u);
    EXPECT_EQ(cue.segmentation[0].segmentation_event_id, 0xABCD1234u);
    EXPECT_EQ(cue.segmentation[0].type_id, 0x30u);
    EXPECT_NEAR(cue.segmentation[0].duration_seconds, 30.0, 1e-6);
    EXPECT_EQ(cue.segmentation[0].type_name, "提供方广告开始 (Provider Advertisement Start)");
}

TEST(Scte35AnalyzerTest, TruncatedPayloadFails) {
    const std::vector<uint8_t> section = BuildSpliceInsertSection();
    model::Scte35Cue cue;
    EXPECT_FALSE(analyzer::Scte35Analyzer::ParseSection(section.data(), 12, cue));
    EXPECT_FALSE(cue.valid);
    EXPECT_FALSE(cue.parse_error.empty());

    model::Scte35Cue empty;
    EXPECT_FALSE(analyzer::Scte35Analyzer::ParseSection(nullptr, 0, empty));
}
