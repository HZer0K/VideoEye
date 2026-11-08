// media_corpus_validate 的语义字段抽取回归测试。
//
// 背景: 切片 2 把 ParseMediaInfoText 从 tools/media_corpus_validate/main.cpp 抽成
// 纯 std 头文件，字段从 7 个扩到 23 个，并让 mi_* / truth_* 共用同一套列定义。
// 这个工具默认不构建（需要外部几 GB 语料树），所以解析逻辑必须有 PR 流程跑得起的
// 单测 —— 否则「列顺序错位 / section 串味 / 单位换算错」要等 nightly 才发现。
//
// 重点守住两件不会有编译错误的事：
//   1) FieldNames() / ToColumns() / ExtendedColumns() 三者的顺序与数量必须一致；
//   2) Text/Data 段、第二条视频流、流段 Duration 都不能污染「第一条」口径。

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "tools/media_corpus_validate/MediaInfoFields.h"

namespace {

using corpus_fields::FieldNames;
using corpus_fields::ParseBitRate;
using corpus_fields::ParseHumanDuration;
using corpus_fields::ParseMediaInfoText;
using corpus_fields::StripBitSuffix;

// 覆盖守护：字段名清单、整行取值、扩展行三者的长度与顺序必须锁死。
TEST(MediaInfoFieldsTest, ColumnDefinitionsStayAligned) {
    const auto names = FieldNames();
    EXPECT_EQ(names.size(), 23u);
    EXPECT_EQ(corpus_fields::ExtendedFieldNames().size(), 16u);

    corpus_fields::SemanticFields fields;
    fields.duration_s = 12.5;
    fields.video_codec = "h264";
    fields.pix_fmt = "yuv420p";
    fields.frame_rate = 29.97;
    const auto columns = corpus_fields::ToColumns(fields);
    const auto extended = corpus_fields::ExtendedColumns(fields);
    ASSERT_EQ(columns.size(), names.size());
    ASSERT_EQ(extended.size(), names.size() - 7);
    EXPECT_EQ(columns[0], "12.500");
    EXPECT_EQ(columns[3], "h264");
    EXPECT_EQ(columns[12], "yuv420p");
    EXPECT_EQ(columns[13], "29.970");
    // 扩展列从 names[7] 开始，逐项与整行对应。
    for (size_t i = 0; i < extended.size(); ++i)
        EXPECT_EQ(extended[i], columns[i + 7]) << "扩展列 " << names[i + 7] << " 错位";
}

// 历史表头前 7 列的顺序守护：映射错了不会有任何编译/运行错误，只有真值比对会红，
// 但那时已经污染了一整轮 results.tsv。
TEST(MediaInfoFieldsTest, LegacyColumnOrderMatchesHistoricalHeader) {
    const auto& order = corpus_fields::LegacyMiColumnOrder();
    const auto& names = corpus_fields::LegacyMiFieldNames();
    ASSERT_EQ(order.size(), 7u);
    ASSERT_EQ(names.size(), 7u);
    for (size_t i = 0; i < order.size(); ++i) {
        ASSERT_LT(order[i], FieldNames().size());
        EXPECT_EQ(FieldNames()[order[i]], names[i]) << "历史第 " << i << " 列映射错位";
    }
    // 反向守护：映射必须是一个排列，不能有重复或漏项。
    std::vector<size_t> sorted(order.begin(), order.end());
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i)
        EXPECT_EQ(sorted[i], i);
}

TEST(MediaInfoFieldsTest, ParsesHumanDurationInAllShapes) {
    EXPECT_DOUBLE_EQ(ParseHumanDuration("678 ms"), 0.678);
    EXPECT_DOUBLE_EQ(ParseHumanDuration("12 s 345 ms"), 12.345);
    EXPECT_DOUBLE_EQ(ParseHumanDuration("4 min 5 s 20 ms"), 245.020);
    EXPECT_DOUBLE_EQ(ParseHumanDuration("1 h 2 min 3 s 4 ms"), 3723.004);
    EXPECT_DOUBLE_EQ(ParseHumanDuration("1 min 5 s 790 ms"), 65.790);
    EXPECT_DOUBLE_EQ(ParseHumanDuration("-"), 0.0);
}

TEST(MediaInfoFieldsTest, ParsesBitRateUnitsWithoutLettingMbMatchBps) {
    EXPECT_EQ(ParseBitRate("1.2 Mb/s"), 1200000);
    EXPECT_EQ(ParseBitRate("640.0 kb/s"), 640000);
    EXPECT_EQ(ParseBitRate("123 b/s"), 123);
    EXPECT_EQ(ParseBitRate("2.5 Mb/s"), 2500000);
    EXPECT_EQ(ParseBitRate("-"), 0);
}

TEST(MediaInfoFieldsTest, StripsDisplayOnlySuffixes) {
    EXPECT_EQ(StripBitSuffix("yuv420p (8 bit)"), "yuv420p");
    EXPECT_EQ(StripBitSuffix("yuv420p10le"), "yuv420p10le");
    EXPECT_EQ(corpus_fields::StripParenSuffix("matroska,webm (Matroska / WebM)"), "matroska,webm");
}

// 一段尽量贴近 FormatFromContext 输出的合成文本，覆盖全部 23 个字段。
const char* kFullText = R"(General
  Complete name              : D:/media/sample.mkv
  Format                     : matroska,webm (Matroska / WebM)
  Duration                   : 1 min 5 s 790 ms
  Overall bit rate           : 1.5 Mb/s
  Stream count               : 3
  Metadata:
    encoder                  : Lavf

Video #0
  ID                         : 0
  Format                     : h264
  Format profile             : High
  Format level               : 41
  Width                      : 1920
  Height                     : 1080
  Pixel format               : yuv420p (8 bit)
  Frame rate                 : 29.970 fps
  Real frame rate            : 59.940 fps
  Frame count                : 1950
  Color space                : bt709
  Color primaries            : bt709
  Transfer characteristics   : bt709
  Rotation                   : 90.0
  Bit rate                   : 1.2 Mb/s
  Duration                   : 1 min 5 s 100 ms

Audio #0
  Format                     : aac
  Sample format              : fltp
  Sample rate                : 48000 Hz
  Channels                   : 6
  Bit depth                  : 32 bit
  Bit rate                   : 256.0 kb/s
  Duration                   : 1 min 5 s 200 ms

Text #0
  Format                     : subrip
  Bit rate                   : 1 kb/s
)";

TEST(MediaInfoFieldsTest, ExtractsAllSemanticFieldsFromFullReport) {
    const auto f = ParseMediaInfoText(kFullText);
    EXPECT_DOUBLE_EQ(f.duration_s, 65.790); // 只认 General 的，不被三段流时长覆盖
    EXPECT_EQ(f.width, 1920);
    EXPECT_EQ(f.height, 1080);
    EXPECT_EQ(f.video_codec, "h264");
    EXPECT_EQ(f.audio_codec, "aac");
    EXPECT_EQ(f.sample_rate, 48000);
    EXPECT_EQ(f.channels, 6);
    EXPECT_EQ(f.format, "matroska,webm");
    EXPECT_EQ(f.stream_count, 3);
    EXPECT_EQ(f.overall_bitrate_bps, 1500000);
    EXPECT_EQ(f.video_profile, "High");
    EXPECT_EQ(f.video_level, 41);
    EXPECT_EQ(f.pix_fmt, "yuv420p");
    EXPECT_DOUBLE_EQ(f.frame_rate, 29.970); // Real frame rate 不能覆盖 avg
    EXPECT_EQ(f.frame_count, 1950);
    EXPECT_EQ(f.video_bitrate_bps, 1200000);
    EXPECT_EQ(f.color_space, "bt709");
    EXPECT_EQ(f.color_primaries, "bt709");
    EXPECT_EQ(f.color_transfer, "bt709");
    EXPECT_DOUBLE_EQ(f.rotation, 90.0);
    EXPECT_EQ(f.sample_fmt, "fltp");
    EXPECT_EQ(f.bit_depth, 32);
    EXPECT_EQ(f.audio_bitrate_bps, 256000);
}

TEST(MediaInfoFieldsTest, TextSectionAndSecondStreamDoNotLeakIntoFirstStream) {
    const std::string text = std::string(kFullText) + R"(
Video #1
  Format                     : mjpeg
  Width                      : 320
  Height                     : 240
  Pixel format               : yuvj420p (8 bit)
  Frame rate                 : 1.000 fps
  Color space                : bt470bg
  Bit rate                   : 8.0 kb/s

Audio #1
  Format                     : mp3
  Sample rate                : 22050 Hz
  Channels                   : 1
  Sample format              : s16p
  Bit rate                   : 64.0 kb/s
)";
    const auto f = ParseMediaInfoText(text);
    EXPECT_EQ(f.video_codec, "h264");
    EXPECT_EQ(f.width, 1920);
    EXPECT_EQ(f.pix_fmt, "yuv420p");
    EXPECT_EQ(f.color_space, "bt709");
    EXPECT_EQ(f.audio_codec, "aac");
    EXPECT_EQ(f.channels, 6);
    EXPECT_EQ(f.sample_fmt, "fltp");
    EXPECT_EQ(f.video_bitrate_bps, 1200000);
    EXPECT_EQ(f.audio_bitrate_bps, 256000);
}

// 反向守护：没有事实实时不能凭空造出数字（门禁靠这个把「不可比」跳过）。
TEST(MediaInfoFieldsTest, EmptyReportYieldsNoFacts) {
    const auto f = ParseMediaInfoText("");
    const auto columns = corpus_fields::ToColumns(f);
    ASSERT_EQ(columns.size(), FieldNames().size());
    for (size_t i = 0; i < columns.size(); ++i) {
        const std::string& value = columns[i];
        const bool empty_or_zero = value.empty() || value == "0" || value == "0.000" || value == "0.0";
        EXPECT_TRUE(empty_or_zero) << "字段 " << FieldNames()[i] << " 凭空取值: " << value;
    }
}

} // namespace
