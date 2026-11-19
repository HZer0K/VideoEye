// 功能 9（字幕 / 时码 / 辅助数据轨）的解析与校验单元测试
//
// 与分析器测试的常规分工：
//   * 文本字幕、时码换算、SCTE-35 位流解析都做成不依赖 FFmpeg 的纯函数，
//     这里全部用构造出来的输入验证（不需要真实媒体文件）；
//   * 校验规则的严重度（重叠=Warning 之类）在这里断言，避免规则表改了以后
//     报告级别悄悄漂移。

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include "core/analysis/diagnostics/Scte35Analyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/diagnostics/TimecodeAnalyzer.h"
#include "core/domain/model/SubtitleCueInfo.h"
#include "core/domain/model/TimecodeInfo.h"

namespace {

using namespace videoeye;

constexpr double kNtscFps = 30000.0 / 1001.0; // 29.97

// ---- 小工具: 大端比特写入（用来拼 SCTE-35 section）----
class BitWriter {
public:
    void Write(uint64_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            const uint8_t bit = static_cast<uint8_t>((value >> i) & 1u);
            const size_t index = bits_;
            if (index % 8 == 0)
                bytes_.push_back(0);
            bytes_[index / 8] |= static_cast<uint8_t>(bit << (7 - (index % 8)));
            ++bits_;
        }
    }
    void WriteBytes(const std::vector<uint8_t>& data) {
        for (uint8_t b : data)
            Write(b, 8);
    }
    size_t BitCount() const {
        return bits_;
    }
    std::vector<uint8_t> Bytes() const {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    size_t bits_ = 0;
};

// 拼一段 splice_insert（含 segmentation_descriptor），返回完整 section（含 CRC）
std::vector<uint8_t> BuildSpliceInsertSection() {
    // ---- 命令体（splice_insert）----
    BitWriter body;
    body.Write(0x12345678, 32); // splice_event_id
    body.Write(0, 1);           // splice_event_cancel_indicator
    body.Write(0, 7);
    body.Write(1, 1); // out_of_network_indicator
    body.Write(1, 1); // program_splice_flag
    body.Write(1, 1); // duration_flag
    body.Write(0, 1); // splice_immediate_flag
    body.Write(0, 4);
    body.Write(1, 1); // splice_time: time_specified_flag
    body.Write(0, 6);
    body.Write(900000, 33); // pts_time = 10 s（90000 时基）
    body.Write(1, 1);       // break_duration: auto_return
    body.Write(0, 6);
    body.Write(2700000, 33); // duration = 30 s
    body.Write(0x0001, 16);  // unique_program_id
    body.Write(1, 8);        // avail_num
    body.Write(2, 8);        // avails_expected
    const std::vector<uint8_t> command_body = body.Bytes();
    const size_t command_length = command_body.size();

    // ---- segmentation_descriptor ----
    BitWriter seg_body;
    seg_body.Write(0xABCD1234, 32); // segmentation_event_id
    seg_body.Write(0, 1);           // cancel
    seg_body.Write(0, 7);
    seg_body.Write(1, 1); // program_segmentation_flag
    seg_body.Write(1, 1); // segmentation_duration_flag
    seg_body.Write(1, 1); // delivery_not_restricted_flag
    seg_body.Write(0, 5);
    seg_body.Write(2700000, 40); // segmentation_duration = 30 s
    seg_body.Write(0x08, 8);     // upid_type
    seg_body.Write(0, 8);        // upid_length
    seg_body.Write(0x30, 8);     // type_id = Provider Advertisement Start
    seg_body.Write(1, 8);        // segment_num
    seg_body.Write(3, 8);        // segments_expected
    const std::vector<uint8_t> seg_bytes = seg_body.Bytes();
    const size_t descriptor_length = 4 + seg_bytes.size(); // identifier("CUEI") + body

    // descriptor_loop_length = tag(8) + length(8) + body，与下面按 8 bit 写 tag 对应
    const size_t descriptor_loop_length = 2 + descriptor_length;
    const size_t section_length = 1 /*protocol_version*/ + 5 /*flags + pts_adjustment*/ + 1 /*cw_index*/ +
                                  3 /*tier + splice_command_length*/ + 1 /*splice_command_type*/ + command_length +
                                  2 /*descriptor_loop_length*/ + descriptor_loop_length + 4 /*CRC_32*/;

    BitWriter out;
    out.Write(0xFC, 8); // table_id
    out.Write(0, 1);    // section_syntax_indicator
    out.Write(0, 1);    // private_indicator
    out.Write(0, 2);    // sap_type
    out.Write(section_length, 12);
    out.Write(0, 8);               // protocol_version
    out.Write(0, 1);               // encrypted_packet
    out.Write(0, 6);               // encryption_algorithm
    out.Write(0, 33);              // pts_adjustment
    out.Write(0, 8);               // cw_index
    out.Write(0, 12);              // tier
    out.Write(command_length, 12); // splice_command_length
    out.Write(0x05, 8);            // splice_command_type = splice_insert
    out.WriteBytes(command_body);
    out.Write(descriptor_loop_length, 16);
    // splice_descriptor_tag 按 8 bit 写（SCTE-35 里就是 1 字节）：
    // 写成 16 bit 会多吐一个前导 0x00，Scte35Analyzer 读出的 tag 恒为 0x00，
    // segmentation 描述符整段被当成"不认识的描述符"跳过，插入点永远判不出来。
    out.Write(0x02, 8); // splice_descriptor_tag = segmentation
    out.Write(descriptor_length, 8);
    out.Write('C', 8); // identifier "CUEI"
    out.Write('U', 8);
    out.Write('E', 8);
    out.Write('I', 8);
    out.WriteBytes(seg_bytes);

    std::vector<uint8_t> bytes = out.Bytes();
    const uint32_t crc = videoeye::Scte35Analyzer::Crc32Mpeg2(bytes.data(), bytes.size());
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
    cue.char_count = videoeye::SubtitleAnalyzer::CountVisibleChars(text);
    cue.empty = (cue.char_count == 0);
    return cue;
}

int CountType(const std::vector<model::SubtitleIssue>& issues, model::SubtitleIssueType type) {
    int count = 0;
    for (const model::SubtitleIssue& issue : issues) {
        if (issue.type == type)
            ++count;
    }
    return count;
}

// 造一条只含字幕轨的 AVFormatContext 跑 RegisterStreams + Finish，返回 issues。
// 不需要真实媒体文件：RegisterStreams 只读 stream 的 codecpar 与 metadata。
// language 为空串表示"这个 tag 压根不存在"。
std::vector<model::SubtitleIssue> RunStreamLevelCheck(const std::string& language) {
    std::vector<model::SubtitleIssue> issues;
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr)
        return issues;
    AVStream* st = avformat_new_stream(fmt, nullptr);
    if (st == nullptr) {
        avformat_free_context(fmt);
        return issues;
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = AV_CODEC_ID_MOV_TEXT;
    if (!language.empty())
        av_dict_set(&st->metadata, "language", language.c_str(), 0);
    // handler_name 一并写上：MP4 里 demuxer 一定会给，免得 missing_handler 混进计数
    av_dict_set(&st->metadata, "handler_name", "SubtitleHandler", 0);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);
    analyzer.Finish(60.0);
    issues = analyzer.result().issues;
    avformat_free_context(fmt);
    return issues;
}

int CountMissingLanguage(const std::string& language) {
    return CountType(RunStreamLevelCheck(language), model::SubtitleIssueType::MissingLanguage);
}

} // namespace

// ============================================================
// 字幕：文本解析
// ============================================================

TEST(SubtitleAnalyzerTest, ParseSrtCues) {
    const std::string srt = "1\n"
                            "00:00:01,000 --> 00:00:04,000\n"
                            "第一行\n"
                            "\n"
                            "2\n"
                            "00:00:05,500 --> 00:00:07,000\n"
                            "第二行\n"
                            "换行也在一起\n";

    std::vector<videoeye::ParsedSubtitleCue> cues;
    ASSERT_TRUE(videoeye::SubtitleAnalyzer::ParseSrtText(srt, cues));
    ASSERT_EQ(cues.size(), 2u);

    EXPECT_EQ(cues[0].cue_number, 1);
    EXPECT_NEAR(cues[0].start_seconds, 1.0, 1e-6);
    EXPECT_NEAR(cues[0].end_seconds, 4.0, 1e-6);
    EXPECT_EQ(cues[0].text, "第一行");

    EXPECT_EQ(cues[1].cue_number, 2);
    EXPECT_NEAR(cues[1].start_seconds, 5.5, 1e-6);
    EXPECT_NEAR(cues[1].end_seconds, 7.0, 1e-6);
    EXPECT_EQ(cues[1].text, "第二行 换行也在一起"); // 多行压成单行显示
}

TEST(SubtitleAnalyzerTest, ParseWebVttSkipsHeaderAndNote) {
    const std::string vtt = "WEBVTT - 测试\n"
                            "\n"
                            "NOTE 这不是 cue\n"
                            "\n"
                            "1\n"
                            "00:00:02.000 --> 00:00:05.000 align:start position:10%\n"
                            "<v Speaker>你好</v>\n";

    std::vector<videoeye::ParsedSubtitleCue> cues;
    ASSERT_TRUE(videoeye::SubtitleAnalyzer::ParseWebVttText(vtt, cues));
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_NEAR(cues[0].start_seconds, 2.0, 1e-6);
    EXPECT_NEAR(cues[0].end_seconds, 5.0, 1e-6);
    EXPECT_EQ(cues[0].text, "你好"); // <v> 标签被剥掉
}

TEST(SubtitleAnalyzerTest, ParseAssDialogueLine) {
    const std::string line = "Dialogue: 0,0:00:01.50,0:00:04.00,Default,,0,0,0,,{\\pos(10,20)}你好\\N世界";
    videoeye::ParsedSubtitleCue cue;
    ASSERT_TRUE(videoeye::SubtitleAnalyzer::ParseAssDialogueLine(line, cue));
    EXPECT_NEAR(cue.start_seconds, 1.5, 1e-6);
    EXPECT_NEAR(cue.end_seconds, 4.0, 1e-6);
    EXPECT_EQ(cue.text, "你好 世界"); // 覆盖指令与 \N 都处理掉
}

TEST(SubtitleAnalyzerTest, ParseMovTextPacketPayload) {
    // tx3g 样本: uint16 长度前缀 + 文本
    const std::string payload = "Hello";
    std::vector<uint8_t> data;
    data.push_back(0x00);
    data.push_back(static_cast<uint8_t>(payload.size()));
    for (char c : payload)
        data.push_back(static_cast<uint8_t>(c));
    data.push_back(0x00); // 尾部样式 box，应当被忽略

    videoeye::ParsedSubtitleCue cue;
    ASSERT_TRUE(
        videoeye::SubtitleAnalyzer::ParsePacketPayload(AV_CODEC_ID_MOV_TEXT, data.data(), data.size(), 12.0, 2.0, cue));
    EXPECT_EQ(cue.text, "Hello");
    EXPECT_NEAR(cue.start_seconds, 12.0, 1e-6); // 包里没写时间 -> 用 PTS
    EXPECT_NEAR(cue.end_seconds, 14.0, 1e-6);   // 用包时长补结束时间
}

TEST(SubtitleAnalyzerTest, DecodeCea608BasicText) {
    // 'H' 'i' 后面跟一个控制码对 (0x14 0x20)，控制码不应出现在文本里
    const std::vector<uint8_t> data = {'H', 'i', 0x14, 0x20, 'y', 'o'};
    const std::string text = videoeye::SubtitleAnalyzer::DecodeCea608Field(data.data(), data.size());
    EXPECT_EQ(text, "Hiyo");
}

// ============================================================
// 字幕：校验规则（验收项: 重叠 / 空字幕必须 warning）
// ============================================================

TEST(SubtitleAnalyzerTest, OverlappingCuesProduceWarning) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 4.0, "第一条"));
    cues.push_back(MakeCue(1, 3.0, 5.0, "第二条")); // 3.0 < 4.0 -> 重叠

    std::vector<model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);

    ASSERT_EQ(CountType(issues, model::SubtitleIssueType::Overlap), 1);
    const model::SubtitleIssue& overlap = issues.front();
    EXPECT_EQ(overlap.type, model::SubtitleIssueType::Overlap);
    EXPECT_EQ(overlap.severity, model::IssueSeverity::Warning);
    EXPECT_EQ(overlap.cue_index, 1);
    EXPECT_TRUE(cues[1].has_issue);
    EXPECT_FALSE(cues[0].has_issue);
}

// ASS/SSA 分层：不同文本同时上屏是合法排版（正片字幕 + 屏幕字），不能报重叠；
// 同一句话重复上屏才是时间轴写错 —— 用文本是否相同区分这两种情况。
TEST(SubtitleAnalyzerTest, AssLayeredCuesAreNotOverlapButDuplicatesAre) {
    std::vector<model::SubtitleCue> layered;
    model::SubtitleCue dialogue = MakeCue(0, 1.0, 4.0, "正片字幕");
    dialogue.format = model::SubtitleFormat::Ass;
    model::SubtitleCue sign = MakeCue(1, 3.0, 5.0, "屏幕字");
    sign.format = model::SubtitleFormat::Ass;
    layered.push_back(dialogue);
    layered.push_back(sign);

    std::vector<model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(layered, issues, videoeye::SubtitleOptions{}, 60.0);
    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::Overlap), 0);
    EXPECT_FALSE(layered[1].has_issue);

    std::vector<model::SubtitleCue> duplicate;
    model::SubtitleCue first = MakeCue(0, 1.0, 4.0, "同一句");
    first.format = model::SubtitleFormat::Ass;
    model::SubtitleCue second = MakeCue(1, 3.0, 5.0, "同一句");
    second.format = model::SubtitleFormat::Ass;
    duplicate.push_back(first);
    duplicate.push_back(second);

    std::vector<model::SubtitleIssue> dup_issues;
    videoeye::SubtitleAnalyzer::ValidateCues(duplicate, dup_issues, videoeye::SubtitleOptions{}, 60.0);
    EXPECT_EQ(CountType(dup_issues, model::SubtitleIssueType::Overlap), 1);
}

TEST(SubtitleAnalyzerTest, EmptyCueProducesWarning) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 4.0, ""));
    cues.push_back(MakeCue(1, 5.0, 8.0, "   ")); // 只有空白也算空

    std::vector<model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);

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
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);

    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::NonMonotonic), 1);
    for (const model::SubtitleIssue& issue : issues) {
        if (issue.type == model::SubtitleIssueType::NonMonotonic) {
            EXPECT_EQ(issue.severity, model::IssueSeverity::Error);
        }
    }
}

TEST(SubtitleAnalyzerTest, DurationWindowAndOutOfRange) {
    std::vector<model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 1.2, "太快了"));     // 0.2s < 0.5s
    cues.push_back(MakeCue(1, 2.0, 12.0, "太慢了"));    // 10s > 8s
    cues.push_back(MakeCue(2, 90.0, 92.0, "超出时长")); // 媒体只有 30s

    std::vector<model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 30.0);

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
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);
    EXPECT_EQ(CountType(issues, model::SubtitleIssueType::Overlap), 0);
}

// und 是 demuxer 在文件没写语言时填的占位值（MP4 mdhd 的 0x55C4），非空但语义上
// 等于"没标语言"。以前只判空串，这类字幕轨会被漏报。
TEST(SubtitleAnalyzerTest, MissingLanguageCoversUnspecifiedPlaceholders) {
    EXPECT_EQ(CountMissingLanguage(""), 1);    // 压根没写 tag
    EXPECT_EQ(CountMissingLanguage("und"), 1); // ISO 639-2 未指定
    EXPECT_EQ(CountMissingLanguage("UND"), 1); // 大小写不敏感
    EXPECT_EQ(CountMissingLanguage("mul"), 1); // 多语言
    EXPECT_EQ(CountMissingLanguage("zxx"), 1); // 无语言内容

    EXPECT_EQ(CountMissingLanguage("chi"), 0); // 真语言码不能误报
    EXPECT_EQ(CountMissingLanguage("zh"), 0);
    EXPECT_EQ(CountMissingLanguage("eng"), 0);
}

TEST(SubtitleAnalyzerTest, MissingLanguageDetailNamesPlaceholder) {
    const std::vector<model::SubtitleIssue> issues = RunStreamLevelCheck("und");
    ASSERT_EQ(CountType(issues, model::SubtitleIssueType::MissingLanguage), 1);
    for (const model::SubtitleIssue& issue : issues) {
        if (issue.type != model::SubtitleIssueType::MissingLanguage)
            continue;
        // 文案要把 und 点出来：修法是改成具体语言码，不是"补一个 tag"
        EXPECT_NE(issue.detail.find("und"), std::string::npos) << issue.detail;
    }
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
    EXPECT_EQ(df.ToString(), "01:02:03;04"); // drop frame 用 ';' 分隔
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
    const double non_drop_label_seconds =
        static_cast<double>(non_drop.hours) * 3600.0 + static_cast<double>(non_drop.minutes) * 60.0 +
        static_cast<double>(non_drop.seconds) + static_cast<double>(non_drop.frames) / 30.0;
    EXPECT_NEAR(3600.0 - non_drop_label_seconds, 3.6, 0.05);
}

TEST(TimecodeTest, TmcdSampleGivesFirstFrameTimecode) {
    // tmcd 样本是 4 字节大端帧序号：360 帧 @30fps = 00:00:12:00
    const std::vector<uint8_t> sample = {0x00, 0x00, 0x01, 0x68};
    model::Timecode tc;
    ASSERT_TRUE(videoeye::TimecodeAnalyzer::DecodeTmcdSample(sample.data(), sample.size(), 30.0, false, tc));
    ASSERT_TRUE(tc.valid);
    EXPECT_EQ(tc.ToString(), "00:00:12:00");

    // 载荷太短 / 帧率为 0 都要失败，不能给出假时码
    model::Timecode bad;
    EXPECT_FALSE(videoeye::TimecodeAnalyzer::DecodeTmcdSample(sample.data(), 2, 30.0, false, bad));
    EXPECT_FALSE(videoeye::TimecodeAnalyzer::DecodeTmcdSample(sample.data(), 4, 0.0, false, bad));
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
    const uint32_t crc =
        videoeye::Scte35Analyzer::Crc32Mpeg2(reinterpret_cast<const uint8_t*>(check.data()), check.size());
    EXPECT_EQ(crc, 0x0376E6E7u); // CRC-32/MPEG-2 的标准校验值
}

TEST(Scte35AnalyzerTest, ParseSpliceInsertWithSegmentation) {
    const std::vector<uint8_t> section = BuildSpliceInsertSection();
    model::Scte35Cue cue;
    ASSERT_TRUE(videoeye::Scte35Analyzer::ParseSection(section.data(), section.size(), cue)) << cue.parse_error;

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
    EXPECT_FALSE(videoeye::Scte35Analyzer::ParseSection(section.data(), 12, cue));
    EXPECT_FALSE(cue.valid);
    EXPECT_FALSE(cue.parse_error.empty());

    model::Scte35Cue empty;
    EXPECT_FALSE(videoeye::Scte35Analyzer::ParseSection(nullptr, 0, empty));
}

// ============================================================
// 字幕：文本规范化 / 格式覆盖 / 整文件解析 / 截断
// ============================================================

namespace {

// 落一个临时字幕文件，析构时删掉：这些用例要真过一遍 demuxer，不能只测纯函数
class TempSubtitleFile {
public:
    TempSubtitleFile(const std::string& name, const std::string& content) {
        path_ = (std::filesystem::temp_directory_path() / ("videoeye_sub_" + name)).string();
        std::ofstream out(path_, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempSubtitleFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempSubtitleFile(const TempSubtitleFile&) = delete;
    TempSubtitleFile& operator=(const TempSubtitleFile&) = delete;
    const std::string& path() const {
        return path_;
    }

private:
    std::string path_;
};

// 完整跑一遍：打开 -> RegisterStreams -> 逐包 -> Finish
videoeye::model::SubtitleAnalysisResult AnalyzeLocalFile(const std::string& path,
                                                         const videoeye::SubtitleOptions& options) {
    videoeye::model::SubtitleAnalysisResult empty;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || fmt == nullptr)
        return empty;
    avformat_find_stream_info(fmt, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(options);
    analyzer.SetSourcePath(path);
    analyzer.RegisterStreams(fmt);

    AVPacket* packet = av_packet_alloc();
    while (packet != nullptr && av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index >= 0 && packet->stream_index < static_cast<int>(fmt->nb_streams))
            analyzer.OnPacket(packet, fmt->streams[packet->stream_index]);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    const double duration = (fmt->duration > 0) ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 10.0;
    analyzer.Finish(duration);
    videoeye::model::SubtitleAnalysisResult result = analyzer.result();
    avformat_close_input(&fmt);
    return result;
}

} // namespace

TEST(SubtitleAnalyzerTest, SrtTextStripsAssOverridesButKeepsComparisonSymbols) {
    // SRT 里常见的 {\an8} 必须剥掉；正文里的比较符号不是标签，不能连内容一起删
    const std::string srt = "1\n"
                            "00:00:01,000 --> 00:00:04,000\n"
                            "{\\an8}2<3,5>1,4<6\n";

    std::vector<videoeye::ParsedSubtitleCue> cues;
    ASSERT_TRUE(videoeye::SubtitleAnalyzer::ParseSrtText(srt, cues));
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_EQ(cues[0].text, "2<3,5>1,4<6");
}

TEST(SubtitleAnalyzerTest, ParseMicroDvdSkipsClearEvents) {
    const std::string text = "{0}{25}Hello|World\n"
                             "{26}{26}\n" // 空文本 + 单帧 = 清屏指令，不是空字幕
                             "{50}{75}{\\i1}Italic\n";

    std::vector<videoeye::ParsedSubtitleCue> cues;
    ASSERT_TRUE(videoeye::SubtitleAnalyzer::ParseMicroDvdText(text, 25.0, cues));
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_NEAR(cues[0].start_seconds, 0.0, 1e-6);
    EXPECT_NEAR(cues[0].end_seconds, 1.0, 1e-6);
    EXPECT_EQ(cues[0].text, "Hello World");
    EXPECT_NEAR(cues[1].start_seconds, 2.0, 1e-6);
    EXPECT_EQ(cues[1].text, "Italic");
}

TEST(SubtitleAnalyzerTest, WholeFileParseRecoversEmptyEvents) {
    // demuxer 会把空事件直接丢掉；纯字幕文件必须整文件重解析才能检出空字幕
    const std::string srt = "1\n"
                            "00:00:01,000 --> 00:00:02,000\n"
                            "\n"
                            "2\n"
                            "00:00:03,000 --> 00:00:04,000\n"
                            "有内容\n";
    const TempSubtitleFile file("empty_events.srt", srt);
    const videoeye::model::SubtitleAnalysisResult result = AnalyzeLocalFile(file.path(), videoeye::SubtitleOptions{});

    ASSERT_EQ(result.cues.size(), 2u);
    EXPECT_TRUE(result.cues[0].empty);
    EXPECT_FALSE(result.cues[1].empty);
    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::EmptyText), 1);
}

TEST(SubtitleAnalyzerTest, MicroDvdDecodedThroughFfmpegDecoder) {
    // 非原生格式走 FFmpeg 解码器：时间来自 PTS，文本由解码器还原（不含 {帧号} 前缀）
    // 至少 3 行，microdvd 的 probe 才会认（2 行会被判成无法识别的输入）
    const std::string text = "{0}{25}Hello|World\n"
                             "{25}{50}Second line\n"
                             "{50}{75}Third line\n";
    const TempSubtitleFile file("demo.sub", text);
    const videoeye::model::SubtitleAnalysisResult result = AnalyzeLocalFile(file.path(), videoeye::SubtitleOptions{});

    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_EQ(result.streams[0].format, videoeye::model::SubtitleFormat::MicroDvd);
    ASSERT_EQ(result.cues.size(), 3u);
    EXPECT_EQ(result.cues[0].text, "Hello World");
    EXPECT_NEAR(result.cues[0].start_seconds, 0.0, 0.2);
    EXPECT_EQ(result.cues[1].text, "Second line");
}

TEST(SubtitleAnalyzerTest, UnsupportedSubtitleStreamIsReported) {
    // 认不出承载方式的流必须留下明确结论，不能静默当成"没问题"
    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* st = avformat_new_stream(fmt, nullptr);
    ASSERT_NE(st, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = AV_CODEC_ID_NONE;

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);
    analyzer.Finish(10.0);
    const videoeye::model::SubtitleAnalysisResult result = analyzer.result();
    avformat_free_context(fmt);

    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::UnsupportedFormat), 1);
    EXPECT_EQ(result.text_stream_count, 0); // 不能把未知流算成"文本字幕已检查"
}

TEST(SubtitleAnalyzerTest, TextStreamWithoutCuesIsReported) {
    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* st = avformat_new_stream(fmt, nullptr);
    ASSERT_NE(st, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = AV_CODEC_ID_TEXT;
    av_dict_set(&st->metadata, "language", "chi", 0);
    av_dict_set(&st->metadata, "handler_name", "SubtitleHandler", 0);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);
    analyzer.Finish(10.0);
    const videoeye::model::SubtitleAnalysisResult result = analyzer.result();
    avformat_free_context(fmt);

    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::NoCues), 1);
}

TEST(SubtitleAnalyzerTest, CueLimitMarksTruncated) {
    const std::string srt = "1\n"
                            "00:00:01,000 --> 00:00:02,000\n"
                            "一\n"
                            "\n"
                            "2\n"
                            "00:00:03,000 --> 00:00:04,000\n"
                            "二\n";
    const TempSubtitleFile file("truncate.srt", srt);
    videoeye::SubtitleOptions options;
    options.max_cues_per_stream = 1;
    const videoeye::model::SubtitleAnalysisResult result = AnalyzeLocalFile(file.path(), options);

    EXPECT_EQ(result.cues.size(), 1u);
    EXPECT_TRUE(result.cues_truncated);
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_TRUE(result.streams[0].truncated);
}

TEST(SubtitleAnalyzerTest, NonUtf8SubtitleIsReported) {
    // CP1251 的 "Привет"：不是合法 UTF-8，但字节仍可当近似字符统计
    std::string srt = "1\n"
                      "00:00:01,000 --> 00:00:04,000\n";
    srt += "\xCF\xF0\xE8\xE2\xE5\xF2";
    srt += "\n";
    const TempSubtitleFile file("cp1251.srt", srt);
    const videoeye::model::SubtitleAnalysisResult result = AnalyzeLocalFile(file.path(), videoeye::SubtitleOptions{});

    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::NonUtf8Encoding), 1);
    ASSERT_EQ(result.cues.size(), 1u);
}

// ============================================================
// 视频内嵌 CC（A53）/ 时间轴偏移 / 文本质量
// ============================================================

namespace {

// MPEG-2 user_data 包：00 00 01 B2 + "GA94" + 0x03 + cc_data 头 + 保留字节 + 三元组 + 0xFF
std::vector<uint8_t> Mpeg2UserDataPacket(const std::vector<uint8_t>& triplets) {
    const size_t cc_count = triplets.size() / 3;
    std::vector<uint8_t> packet = {
        0x00, 0x00, 0x01, 0xB2, 'G', 'A', '9', '4', 0x03, static_cast<uint8_t>(0x40 | cc_count), 0xFF};
    packet.insert(packet.end(), triplets.begin(), triplets.end());
    packet.push_back(0xFF);
    return packet;
}

// H.264 Annex B 包：一条 SEI NAL，payload type 4（T.35 / GA94）
std::vector<uint8_t> H264SeiPacket(const std::vector<uint8_t>& triplets) {
    const size_t cc_count = triplets.size() / 3;
    std::vector<uint8_t> payload = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03, static_cast<uint8_t>(0x40 | cc_count),
                                    0xFF};
    payload.insert(payload.end(), triplets.begin(), triplets.end());
    payload.push_back(0x80); // rbsp_trailing_bits

    std::vector<uint8_t> packet = {0x00, 0x00, 0x00, 0x01, 0x06, 0x04, static_cast<uint8_t>(payload.size())};
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

void FeedPacket(videoeye::SubtitleAnalyzer& analyzer, AVStream* stream, const std::vector<uint8_t>& data,
                int64_t pts_ms) {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr)
        return;
    av_new_packet(packet, static_cast<int>(data.size()));
    std::memcpy(packet->data, data.data(), data.size());
    packet->stream_index = stream->index;
    packet->pts = pts_ms;
    analyzer.OnPacket(packet, stream);
    av_packet_free(&packet);
}

AVStream* AddStream(AVFormatContext* fmt, AVMediaType type, AVCodecID codec_id) {
    AVStream* stream = avformat_new_stream(fmt, nullptr);
    if (stream == nullptr)
        return nullptr;
    stream->codecpar->codec_type = type;
    stream->codecpar->codec_id = codec_id;
    stream->time_base = AVRational{1, 1000};
    return stream;
}

} // namespace

TEST(SubtitleAnalyzerTest, EmbeddedMpeg2UserDataProducesCaptionCue) {
    // 608 控制码：RCL(0x14 0x20) / EOC(0x14 0x2F) / EDM(0x14 0x2C)
    const std::vector<uint8_t> rcl = {0xFC, 0x14, 0x20};
    const std::vector<uint8_t> hi = {0xFC, 'H', 'I'};
    const std::vector<uint8_t> eoc = {0xFC, 0x14, 0x2F};
    const std::vector<uint8_t> edm = {0xFC, 0x14, 0x2C};

    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* video = AddStream(fmt, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO);
    ASSERT_NE(video, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);

    FeedPacket(analyzer, video, Mpeg2UserDataPacket(rcl), 0);
    FeedPacket(analyzer, video, Mpeg2UserDataPacket(hi), 500);
    FeedPacket(analyzer, video, Mpeg2UserDataPacket(eoc), 1000);
    FeedPacket(analyzer, video, Mpeg2UserDataPacket(edm), 3000);
    analyzer.Finish(10.0);

    const videoeye::model::SubtitleAnalysisResult result = analyzer.result();
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_TRUE(result.streams[0].embedded);
    EXPECT_EQ(result.streams[0].format, videoeye::model::SubtitleFormat::Cea608);
    ASSERT_EQ(result.cues.size(), 1u);
    EXPECT_EQ(result.cues[0].text, "HI");
    EXPECT_NEAR(result.cues[0].start_seconds, 1.0, 1e-6);
    EXPECT_NEAR(result.cues[0].end_seconds, 3.0, 1e-6);
    // 内嵌 CC 没有独立字幕轨，不应被要求补 language tag
    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::MissingLanguage), 0);

    avformat_free_context(fmt);
}

TEST(SubtitleAnalyzerTest, EmbeddedH264SeiProducesCaptionCue) {
    const std::vector<uint8_t> preload = {0xFC, 0x14, 0x20, 0xFC, 'O', 'K', 0xFC, 0x14, 0x2F};
    const std::vector<uint8_t> erase = {0xFC, 0x14, 0x2C};

    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* video = AddStream(fmt, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264);
    ASSERT_NE(video, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);

    FeedPacket(analyzer, video, H264SeiPacket(preload), 2000);
    FeedPacket(analyzer, video, H264SeiPacket(erase), 5000);
    analyzer.Finish(10.0);

    const videoeye::model::SubtitleAnalysisResult result = analyzer.result();
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_TRUE(result.streams[0].embedded);
    ASSERT_EQ(result.cues.size(), 1u);
    EXPECT_EQ(result.cues[0].text, "OK");
    EXPECT_NEAR(result.cues[0].start_seconds, 2.0, 1e-6);
    EXPECT_NEAR(result.cues[0].end_seconds, 5.0, 1e-6);

    avformat_free_context(fmt);
}

TEST(SubtitleAnalyzerTest, TimelineOffsetBetweenCueAndPacketPtsIsReported) {
    const std::string srt = "1\n"
                            "00:00:01,000 --> 00:00:04,000\n"
                            "hello\n";

    auto run = [&srt](int64_t pts_ms) {
        AVFormatContext* fmt = avformat_alloc_context();
        AVStream* subtitle = AddStream(fmt, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_SRT);
        videoeye::SubtitleAnalyzer analyzer;
        analyzer.Reset(videoeye::SubtitleOptions{});
        analyzer.RegisterStreams(fmt);

        std::vector<uint8_t> bytes(srt.begin(), srt.end());
        FeedPacket(analyzer, subtitle, bytes, pts_ms);
        analyzer.Finish(60.0);
        const auto result = analyzer.result();
        avformat_free_context(fmt);
        return result;
    };

    // 包 PTS 与 cue 文本时间一致：不应报偏移
    EXPECT_EQ(CountType(run(1000).issues, videoeye::model::SubtitleIssueType::TimelineOffset), 0);
    // 包 PTS 比 cue 晚 4 秒：容器时间轴与字幕文本整体错位
    EXPECT_EQ(CountType(run(5000).issues, videoeye::model::SubtitleIssueType::TimelineOffset), 1);
}

TEST(SubtitleAnalyzerTest, TextQualityFindsGarbledCodePoints) {
    std::vector<videoeye::model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 4.0, "正常文本"));
    cues.push_back(MakeCue(1, 5.0, 8.0, "bad \xEF\xBF\xBD")); // U+FFFD 替换字符
    cues.push_back(MakeCue(2, 9.0, 12.0, "ctrl\x01"));        // C0 控制字符
    cues.push_back(MakeCue(3, 13.0, 16.0, "\xEE\x80\x80"));   // 私用区 U+E000

    std::vector<videoeye::model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);

    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::GarbledText), 3);
}

TEST(SubtitleAnalyzerTest, TextQualityFindsDuplicateAndOverlongLines) {
    std::vector<videoeye::model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 4.0, "同一句"));
    cues.push_back(MakeCue(1, 5.0, 8.0, "同一句"));
    cues.push_back(MakeCue(2, 9.0, 12.0, std::string(61, 'A')));

    std::vector<videoeye::model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);

    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::DuplicateCue), 1);
    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::LineTooLong), 1);
}

// ============================================================
// 语言自适应阅读速度 / 相邻间隔 / 图形字幕段头 / AV1 与 DVD CC
// ============================================================

namespace {

std::vector<uint8_t> PgsSegment(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = {type, static_cast<uint8_t>((payload.size() >> 8) & 0xFF),
                                static_cast<uint8_t>(payload.size() & 0xFF)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void AppendBe16(std::vector<uint8_t>& out, int value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

std::vector<uint8_t> PgsPcsPayload(int width, int height, int composition_number, int object_count,
                                   const std::vector<int>& object_x) {
    std::vector<uint8_t> payload;
    AppendBe16(payload, width);
    AppendBe16(payload, height);
    payload.push_back(0x00);
    AppendBe16(payload, composition_number);
    payload.push_back(0x00); // composition_state = normal
    payload.push_back(0x00); // palette_update_flag
    payload.push_back(0x00); // palette_id
    payload.push_back(static_cast<uint8_t>(object_count));
    for (int i = 0; i < object_count; ++i) {
        payload.push_back(0x00);
        payload.push_back(static_cast<uint8_t>(i)); // object_id
        payload.push_back(0x00);                    // window_id
        payload.push_back(0x00);                    // flags（无 crop）
        AppendBe16(payload, object_x[i]);
        AppendBe16(payload, 10);
    }
    return payload;
}

std::vector<uint8_t> PgsWdsPayload(const std::vector<int>& xs, const std::vector<int>& widths, int height) {
    std::vector<uint8_t> payload = {static_cast<uint8_t>(xs.size())};
    for (size_t i = 0; i < xs.size(); ++i) {
        payload.push_back(static_cast<uint8_t>(i));
        AppendBe16(payload, xs[i]);
        AppendBe16(payload, 0);
        AppendBe16(payload, widths[i]);
        AppendBe16(payload, height);
    }
    return payload;
}

std::vector<uint8_t> DvbSegment(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = {0x0F, type, 0x00, 0x01};
    AppendBe16(out, static_cast<int>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// DVD 私有 CC：'CC' 0x01 0xF8 + 场序 + 6 字节 caption block
std::vector<uint8_t> DvdUserDataPacket(const std::vector<uint8_t>& triplet) {
    std::vector<uint8_t> packet = {0x00, 0x00, 0x01, 0xB2, 'C', 'C', 0x01, 0xF8, 0x80};
    packet.push_back(0xFF);
    packet.push_back(triplet.size() > 1 ? triplet[1] : 0);
    packet.push_back(triplet.size() > 2 ? triplet[2] : 0);
    packet.push_back(0xFF);
    packet.push_back(0x00);
    packet.push_back(0x00);
    return packet;
}

// AV1：metadata OBU（type 5），payload = metadata_type 4（T.35）+ A53 cc_data
std::vector<uint8_t> Av1MetadataPacket(const std::vector<uint8_t>& triplets) {
    const size_t cc_count = triplets.size() / 3;
    std::vector<uint8_t> payload = {
        0x04, 0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03, static_cast<uint8_t>(0x40 | cc_count), 0xFF};
    payload.insert(payload.end(), triplets.begin(), triplets.end());
    payload.push_back(0xFF);
    std::vector<uint8_t> packet = {0x2A, static_cast<uint8_t>(payload.size())}; // OBU 头 + leb128 长度
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

} // namespace

TEST(SubtitleAnalyzerTest, ReadingSpeedUsesCjkThreshold) {
    std::string cjk;
    for (int i = 0; i < 20; ++i)
        cjk += "字";

    std::vector<videoeye::model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 2.0, cjk));
    cues.push_back(MakeCue(1, 20.0, 21.0, cjk));

    std::vector<videoeye::model::SubtitleIssue> issues;
    videoeye::SubtitleOptions options;
    // 20 字符/秒：超过中文阈值 16，但低于通用阈值 21
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, options, 60.0);
    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::TooFast), 2);

    issues.clear();
    options.language_aware_reading_speed = false;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, options, 60.0);
    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::TooFast), 0);
}

TEST(SubtitleAnalyzerTest, CueGapReportsOnlyShortPositiveGaps) {
    std::vector<videoeye::model::SubtitleCue> cues;
    cues.push_back(MakeCue(0, 1.0, 2.0, "第一句"));
    cues.push_back(MakeCue(1, 2.05, 3.0, "第二句")); // 间隔 0.05s -> 闪烁
    cues.push_back(MakeCue(2, 3.0, 4.0, "第三句"));  // 首尾相接（roll-up 形态）-> 不算
    cues.push_back(MakeCue(3, 6.0, 7.0, "第四句"));  // 间隔 2s -> 正常

    std::vector<videoeye::model::SubtitleIssue> issues;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, videoeye::SubtitleOptions{}, 60.0);
    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::CueGap), 1);

    issues.clear();
    videoeye::SubtitleOptions options;
    options.check_cue_gap = false;
    videoeye::SubtitleAnalyzer::ValidateCues(cues, issues, options, 60.0);
    EXPECT_EQ(CountType(issues, videoeye::model::SubtitleIssueType::CueGap), 0);
}

TEST(SubtitleAnalyzerTest, PgsSegmentHeadersProduceGraphicStats) {
    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* video = AddStream(fmt, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264);
    ASSERT_NE(video, nullptr);
    video->codecpar->width = 1280;
    video->codecpar->height = 720;
    AVStream* pgs = AddStream(fmt, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_HDMV_PGS_SUBTITLE);
    ASSERT_NE(pgs, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);

    // 第一次上屏：2 个对象 + 2 个窗口，其中一个窗口右边超出 1280
    std::vector<uint8_t> first = PgsSegment(0x16, PgsPcsPayload(1280, 720, 1, 2, {10, 1300}));
    const std::vector<uint8_t> wds = PgsSegment(0x17, PgsWdsPayload({0, 1200}, {640, 640}, 360));
    first.insert(first.end(), wds.begin(), wds.end());
    FeedPacket(analyzer, pgs, first, 0);

    // 40ms 后清屏：上一条有内容但实际不可见
    FeedPacket(analyzer, pgs, PgsSegment(0x16, PgsPcsPayload(1280, 720, 2, 0, {})), 40);
    analyzer.Finish(10.0);

    const auto result = analyzer.result();
    ASSERT_EQ(result.streams.size(), 1u);
    const videoeye::model::SubtitleStreamInfo& stream = result.streams[0];
    EXPECT_TRUE(stream.graphic.present);
    EXPECT_EQ(stream.graphic.presentation_count, 2);
    EXPECT_EQ(stream.graphic.empty_presentation_count, 1);
    EXPECT_EQ(stream.graphic.object_count_max, 2);
    EXPECT_EQ(stream.graphic.window_count_max, 2);
    EXPECT_EQ(stream.graphic.screen_width, 1280);
    EXPECT_EQ(stream.graphic.screen_height, 720);
    EXPECT_EQ(stream.graphic.out_of_bounds_count, 1);
    EXPECT_EQ(stream.graphic.zero_duration_count, 1);

    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::GraphicOutOfBounds), 1);
    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::GraphicZeroDuration), 1);
    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::GraphicEmpty), 0);

    avformat_free_context(fmt);
}

TEST(SubtitleAnalyzerTest, DvbPageCompositionProducesGraphicStats) {
    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* video = AddStream(fmt, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264);
    ASSERT_NE(video, nullptr);
    video->codecpar->width = 1280;
    video->codecpar->height = 720;
    AVStream* dvb = AddStream(fmt, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_DVB_SUBTITLE);
    ASSERT_NE(dvb, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);

    // 区域 0 放在 (1200, 0)，尺寸 200x100：右边界 1400 > 1280
    std::vector<uint8_t> page = {0x00, 0x10}; // page_time_out、version=1 + page_state=0
    page.push_back(0x00);                     // region_id
    page.push_back(0x00);                     // reserved
    AppendBe16(page, 1200);
    AppendBe16(page, 0);

    std::vector<uint8_t> region = {0x00, 0x10}; // region_id、version=1 + fill=0
    AppendBe16(region, 200);
    AppendBe16(region, 100);
    region.push_back(0x00); // level/depth
    region.push_back(0x00); // CLUT
    region.push_back(0x00); // 8-bit pixel code
    region.push_back(0x00); // 4/2-bit pixel code

    std::vector<uint8_t> packet = DvbSegment(0x10, page);
    const std::vector<uint8_t> region_segment = DvbSegment(0x11, region);
    packet.insert(packet.end(), region_segment.begin(), region_segment.end());
    FeedPacket(analyzer, dvb, packet, 1000);
    analyzer.Finish(10.0);

    const auto result = analyzer.result();
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_TRUE(result.streams[0].graphic.present);
    EXPECT_EQ(result.streams[0].graphic.presentation_count, 1);
    EXPECT_EQ(result.streams[0].graphic.empty_presentation_count, 0);
    EXPECT_EQ(result.streams[0].graphic.out_of_bounds_count, 1);
    EXPECT_EQ(CountType(result.issues, videoeye::model::SubtitleIssueType::GraphicOutOfBounds), 1);

    avformat_free_context(fmt);
}

TEST(SubtitleAnalyzerTest, EmbeddedDvdClosedCaptionProducesCue) {
    const std::vector<uint8_t> rcl = {0xFC, 0x14, 0x20};
    const std::vector<uint8_t> hi = {0xFC, 'H', 'I'};
    const std::vector<uint8_t> eoc = {0xFC, 0x14, 0x2F};
    const std::vector<uint8_t> edm = {0xFC, 0x14, 0x2C};

    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* video = AddStream(fmt, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO);
    ASSERT_NE(video, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);

    FeedPacket(analyzer, video, DvdUserDataPacket(rcl), 0);
    FeedPacket(analyzer, video, DvdUserDataPacket(hi), 500);
    FeedPacket(analyzer, video, DvdUserDataPacket(eoc), 1000);
    FeedPacket(analyzer, video, DvdUserDataPacket(edm), 3000);
    analyzer.Finish(10.0);

    const auto result = analyzer.result();
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_TRUE(result.streams[0].embedded);
    ASSERT_EQ(result.cues.size(), 1u);
    EXPECT_EQ(result.cues[0].text, "HI");
    EXPECT_NEAR(result.cues[0].start_seconds, 1.0, 1e-6);
    EXPECT_NEAR(result.cues[0].end_seconds, 3.0, 1e-6);

    avformat_free_context(fmt);
}

TEST(SubtitleAnalyzerTest, EmbeddedAv1MetadataProducesCaptionCue) {
    const std::vector<uint8_t> preload = {0xFC, 0x14, 0x20, 0xFC, 'O', 'K', 0xFC, 0x14, 0x2F};
    const std::vector<uint8_t> erase = {0xFC, 0x14, 0x2C};

    AVFormatContext* fmt = avformat_alloc_context();
    ASSERT_NE(fmt, nullptr);
    AVStream* video = AddStream(fmt, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_AV1);
    ASSERT_NE(video, nullptr);

    videoeye::SubtitleAnalyzer analyzer;
    analyzer.Reset(videoeye::SubtitleOptions{});
    analyzer.RegisterStreams(fmt);

    FeedPacket(analyzer, video, Av1MetadataPacket(preload), 2000);
    FeedPacket(analyzer, video, Av1MetadataPacket(erase), 5000);
    analyzer.Finish(10.0);

    const auto result = analyzer.result();
    ASSERT_EQ(result.streams.size(), 1u);
    EXPECT_TRUE(result.streams[0].embedded);
    ASSERT_EQ(result.cues.size(), 1u);
    EXPECT_EQ(result.cues[0].text, "OK");
    EXPECT_NEAR(result.cues[0].start_seconds, 2.0, 1e-6);
    EXPECT_NEAR(result.cues[0].end_seconds, 5.0, 1e-6);

    avformat_free_context(fmt);
}
