// 命令文本的两种排版（单行 ↔ 多行）—— 「展开编辑」对话框的底层契约。
//
// 这一层最容易出的错不是崩，而是**悄悄改写了命令**:
//   * 用 ParseCommandLine 的 token 拼回去 → 含空格的路径被拆成两个参数；
//   * 压行时顺手把行内连续空格压成一个 → 引号里带空格的路径被改掉；
//   * 折行时把 `-map -0:d` 的负号值当成选项 → 只是难看，但如果为了"修好看"
//     去动字符，就会变成另一个命令。
//
// 因此最关键的用例是 RoundTrip：折行再压回来，必须与原文**等价**，
// 而且交给 ParseCommandLine 之后参数数组要**逐个相同** —— 只看字符串相等
// 看不出"多了一个空参数"这种差异。

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegCommandText.h"

using videoeye::ffmpeg::FfmpegCommandWord;
using videoeye::ffmpeg::FlattenCommandLines;
using videoeye::ffmpeg::ParseCommandLine;
using videoeye::ffmpeg::SplitCommandWords;
using videoeye::ffmpeg::WrapCommandIntoLines;

namespace {

std::vector<std::string> TextsOf(const QString& text, const std::vector<FfmpegCommandWord>& words) {
    std::vector<std::string> out;
    out.reserve(words.size());
    for (const FfmpegCommandWord& word : words) {
        out.push_back(text.mid(word.begin, word.end - word.begin).toStdString());
    }
    return out;
}

std::vector<std::string> ArgsOf(const QString& command) {
    const auto parsed = ParseCommandLine(command);
    std::vector<std::string> out;
    out.reserve(parsed.arguments.size());
    for (const QString& arg : parsed.arguments) {
        out.push_back(arg.toStdString());
    }
    return out;
}

std::vector<std::string> ToStd(const QStringList& list) {
    std::vector<std::string> out;
    out.reserve(list.size());
    for (const QString& item : list) {
        out.push_back(item.toStdString());
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 切词：原文区间，引号算词的一部分
// ---------------------------------------------------------------------------

TEST(FfmpegCommandTextTests, SplitWordsKeepsQuotedSpacesInsideOneWord) {
    const QString text = QStringLiteral("ffmpeg -i \"my file.mp4\" out.mp4");
    const auto words = SplitCommandWords(text);

    ASSERT_EQ(4u, words.size());
    EXPECT_EQ(std::vector<std::string>({"ffmpeg", "-i", "\"my file.mp4\"", "out.mp4"}), TextsOf(text, words));
}

TEST(FfmpegCommandTextTests, SplitWordsHandlesSingleQuotesAndAdjacentTokens) {
    const QString text = QStringLiteral("ffmpeg -i 'a b.mp4'-crf 23");
    const auto words = SplitCommandWords(text);

    // 引号结束后没有空白，仍属于同一个词 —— 这种拼接 ffmpeg 也不会帮你分开，
    // 我们照原文切片，不替它猜
    ASSERT_EQ(4u, words.size());
    EXPECT_EQ(std::string("'a b.mp4'-crf"), TextsOf(text, words).at(2));
}

TEST(FfmpegCommandTextTests, SplitWordsUnterminatedQuoteRunsToEnd) {
    // 引号没闭合时切词不报错（报错是 ParseCommandLine 的活），词一直延续到行尾
    const QString text = QStringLiteral("ffmpeg -i \"unclosed.mp4");
    const auto words = SplitCommandWords(text);

    ASSERT_EQ(3u, words.size());
    EXPECT_EQ(std::vector<std::string>({"ffmpeg", "-i", "\"unclosed.mp4"}), TextsOf(text, words));
}

// ---------------------------------------------------------------------------
// 2. 折行：选项与它的值同行
// ---------------------------------------------------------------------------

TEST(FfmpegCommandTextTests, WrapKeepsOptionWithItsValue) {
    const auto lines = WrapCommandIntoLines(QStringLiteral("ffmpeg -i in.mp4 -c:v libx264 -crf 23 out.mp4"));

    EXPECT_EQ(std::vector<std::string>({"ffmpeg", "-i in.mp4", "-c:v libx264", "-crf 23", "out.mp4"}), ToStd(lines));
}

TEST(FfmpegCommandTextTests, WrapKeepsNegativeMapValueWithItsOption) {
    // -map 的值可以是负数（-0:d 表示"排除第 0 个输入的数据流"）。字典标了
    // value_may_start_with_dash，折行时不能把它当成新选项拆出去。
    const auto lines = WrapCommandIntoLines(QStringLiteral("ffmpeg -i in.mp4 -map 0:v -map -0:d out.mp4"));

    EXPECT_EQ(std::vector<std::string>({"ffmpeg", "-i in.mp4", "-map 0:v", "-map -0:d", "out.mp4"}), ToStd(lines));
}

TEST(FfmpegCommandTextTests, NegativeValueIsOnlyMergedForOptionsThatAllowIt) {
    // -i 的值不可能以 '-' 开头：这里的 -0:d 就是下一个选项，别硬并进去
    const auto lines = WrapCommandIntoLines(QStringLiteral("ffmpeg -i -0:d out.mp4"));

    EXPECT_EQ(std::vector<std::string>({"ffmpeg", "-i", "-0:d", "out.mp4"}), ToStd(lines));
}

TEST(FfmpegCommandTextTests, WrapKeepsMultiBytePathIntact) {
    const auto lines =
        WrapCommandIntoLines(QStringLiteral("ffmpeg -i \"D:/素材/测试 片段.mp4\" -c copy \"D:/素材/导出 结果.mp4\""));

    EXPECT_EQ(
        std::vector<std::string>({"ffmpeg", "-i \"D:/素材/测试 片段.mp4\"", "-c copy", "\"D:/素材/导出 结果.mp4\""}),
        ToStd(lines));
}

// ---------------------------------------------------------------------------
// 3. 压行：trim、丢空行、单空格连接；行内空白不动
// ---------------------------------------------------------------------------

TEST(FfmpegCommandTextTests, FlattenDropsBlankLinesAndTrimsEachLine) {
    const QString text = QStringLiteral("\n  ffmpeg   \n\t-i in.mp4\n\n out.mp4 \n");
    EXPECT_EQ(QStringLiteral("ffmpeg -i in.mp4 out.mp4"), FlattenCommandLines(text));
}

TEST(FfmpegCommandTextTests, FlattenKeepsInnerSpacesInsideQuotes) {
    // 行内连续空格可能是引号里的路径的一部分，压掉就等于改了文件名
    const QString text = QStringLiteral("-i \"a  b.mp4\"\n-c copy");
    EXPECT_EQ(QStringLiteral("-i \"a  b.mp4\" -c copy"), FlattenCommandLines(text));
}

TEST(FfmpegCommandTextTests, FlattenOfEmptyTextIsEmpty) {
    EXPECT_TRUE(FlattenCommandLines(QString()).isEmpty());
    EXPECT_TRUE(FlattenCommandLines(QStringLiteral("\n\n   \n\t\n")).isEmpty());
}

// ---------------------------------------------------------------------------
// 4. 往返：折行再压回来，参数数组必须逐个相同
// ---------------------------------------------------------------------------

TEST(FfmpegCommandTextTests, RoundTripKeepsArgumentListIdentical) {
    const QStringList commands = {
        QStringLiteral("ffmpeg -i in.mp4 out.mp4"),
        QStringLiteral("ffmpeg -i \"my file.mp4\" -c:v libx264 -crf 23 -preset medium \"out file.mp4\""),
        QStringLiteral("ffmpeg -i in.mp4 -map 0:v -map -0:d -c copy out.mp4"),
        QStringLiteral("ffmpeg -i in.mp4 -vf \"scale=1280:-2,format=yuv420p\" out.mp4"),
        QStringLiteral("ffmpeg -ss 00:00:10 -i in.mkv -t 00:00:05 -c copy out.mkv"),
        QStringLiteral("ffmpeg -i a.mp4 -i b.mp4 -filter_complex \"[0:v][1:v]hstack[v]\" -map \"[v]\" out.mp4"),
        QStringLiteral("ffmpeg -f lavfi -i testsrc2=s=320x240:d=2 -c:v libx264 out.mp4"),
    };

    for (const QString& command : commands) {
        const QStringList lines = WrapCommandIntoLines(command);
        const QString flattened = FlattenCommandLines(lines.join(QLatin1Char('\n')));

        // 折行不许吞字符，也不许多出字符（空白已规范化）
        EXPECT_EQ(command.toStdString(), flattened.toStdString())
            << "lines: " << lines.join(QLatin1Char('|')).toStdString();

        // 真正的证明：交给解析器后参数数组逐个相同
        EXPECT_EQ(ArgsOf(command), ArgsOf(flattened))
            << "command: " << command.toStdString() << " flattened: " << flattened.toStdString();
    }
}
