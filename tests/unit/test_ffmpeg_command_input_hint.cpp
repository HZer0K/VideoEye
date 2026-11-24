// 「边打边提示」的核心逻辑：光标处的词是谁、它要什么、下一个词该写什么。
//
// 为什么值得单测：这段逻辑决定的是"用户按下 Tab 之后命令会变成什么"以及"提示行说的
// 是不是真的"。判错的代价很实在 —— 把 `-crf` 的值位置判成输出文件名，补全就会把
// 用户正要写的数字换成一条路径；把 `-cr` 当成完整选项，下一步就会劝用户填值。
//
// 这些用例都盯着**位置敏感**这件事：同一段文字，光标放在不同词上结论必须不同。
// 逻辑在 core 里、不带 Widget，所以这里不需要 QApplication。

#include <gtest/gtest.h>

#include <QString>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandInputHint.h"

using videoeye::ffmpeg::ApplyCompletion;
using videoeye::ffmpeg::BuildInputHint;
using videoeye::ffmpeg::FfmpegCapabilityCache;
using videoeye::ffmpeg::FfmpegInputCandidate;
using videoeye::ffmpeg::FfmpegInputHint;
using videoeye::ffmpeg::FfmpegInputSlot;

namespace {

FfmpegInputHint HintAt(const QString& command, int cursor) {
    return BuildInputHint(command, cursor);
}

FfmpegInputHint HintAtEnd(const QString& command) {
    return BuildInputHint(command, static_cast<int>(command.size()));
}

bool HasCandidate(const FfmpegInputHint& hint, const QString& text) {
    for (const auto& candidate : hint.candidates) {
        if (candidate.text == text) {
            return true;
        }
    }
    return false;
}

QString CandidateTexts(const FfmpegInputHint& hint) {
    QStringList texts;
    for (const auto& candidate : hint.candidates) {
        texts.append(candidate.text);
    }
    return texts.join(QLatin1Char(' '));
}

} // namespace

TEST(FfmpegCommandInputHintTests, EmptyCommandAsksForAnInput) {
    const FfmpegInputHint hint = HintAt(QString(), 0);

    EXPECT_FALSE(hint.has_subject); // 还没写东西，没有可解释的词
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("-i"))) << hint.next_label.toStdString();
    // 空前缀给"常用清单"，第一项就是输入
    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("-i"))) << CandidateTexts(hint).toStdString();
    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("-y"))) << CandidateTexts(hint).toStdString();
}

TEST(FfmpegCommandInputHintTests, AfterDashITheNextThingIsAPath) {
    // 光标停在 `-i ` 之后：这不是"选项值"，是输入文件路径 —— 提示要能分开。
    const QString command = QStringLiteral("ffmpeg -i ");
    const FfmpegInputHint hint = HintAtEnd(command);

    EXPECT_TRUE(hint.slot == FfmpegInputSlot::InputUrl);
    ASSERT_TRUE(hint.has_subject);
    EXPECT_EQ(QStringLiteral("输入文件"), hint.subject_title);
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("路径"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, FinishedInputPointsAtOutputSideOptions) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i in.mp4 "));

    // 光标在空白处 → 解释刚打完的那个词（输入文件），而不是"没有内容"
    ASSERT_TRUE(hint.has_subject);
    EXPECT_EQ(QStringLiteral("输入文件"), hint.subject_title);
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("输出侧选项"))) << hint.next_label.toStdString();
    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("-c:v"))) << CandidateTexts(hint).toStdString();
    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("-crf"))) << CandidateTexts(hint).toStdString();
    // 还没写输入时不该推输出侧参数
    EXPECT_FALSE(HasCandidate(hint, QStringLiteral("-i"))) << CandidateTexts(hint).toStdString();
}

TEST(FfmpegCommandInputHintTests, HalfTypedOptionIsGuessedByPrefixAndOffersCompletions) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i in.mp4 -cr"));

    ASSERT_TRUE(hint.has_subject);
    EXPECT_TRUE(hint.subject_is_prefix); // 明确告诉用户"这是猜的，还没打完"
    EXPECT_TRUE(hint.subject_title.contains(QStringLiteral("CRF"))) << hint.subject_title.toStdString();
    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("-crf"))) << CandidateTexts(hint).toStdString();
    // 半截选项名不能按"选项已写完"去要值
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("打完"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, CompleteOptionAsksForItsValue) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i in.mp4 -crf "));

    ASSERT_TRUE(hint.has_subject);
    EXPECT_EQ(QStringLiteral("视频质量 (CRF)"), hint.subject_title);
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("值"))) << hint.next_label.toStdString();
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("CRF"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, EncoderValueOffersOnlyMatchingEncoders) {
    auto& caps = FfmpegCapabilityCache::Instance();
    caps.SetEncoders({QStringLiteral("libx264"), QStringLiteral("libx265"), QStringLiteral("h264_nvenc")});

    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i a.mp4 -c:v libx26"));

    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("libx264"))) << CandidateTexts(hint).toStdString();
    EXPECT_TRUE(HasCandidate(hint, QStringLiteral("libx265"))) << CandidateTexts(hint).toStdString();
    EXPECT_FALSE(HasCandidate(hint, QStringLiteral("h264_nvenc"))) << CandidateTexts(hint).toStdString();

    caps.Clear(); // 单例是全局的，别把清单漏给后面的用例
}

TEST(FfmpegCommandInputHintTests, ValueExplainsWhichOptionItBelongsTo) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i in.mp4 -crf 23"));

    ASSERT_TRUE(hint.has_subject);
    EXPECT_EQ(QStringLiteral("23"), hint.subject);
    EXPECT_TRUE(hint.subject_title.contains(QStringLiteral("CRF"))) << hint.subject_title.toStdString();
    // 值已经填完 → 下一步回到"继续写选项 / 写输出文件"，不是再要一个值
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("输出侧选项"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, BarePathWithoutDashIGetsCalledOut) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg in.mp4"));

    ASSERT_TRUE(hint.has_subject);
    EXPECT_TRUE(hint.missing_dash_i);
    EXPECT_TRUE(hint.subject_summary.contains(QStringLiteral("缺 -i"))) << hint.subject_summary.toStdString();
}

TEST(FfmpegCommandInputHintTests, OutputPathIsRecognisedAsOutput) {
    const QString command = QStringLiteral("ffmpeg -i in.mp4 out.mp4");
    // 光标放在 out.mp4 中间：解释的是"这个词"，不是它前面的 -i
    const FfmpegInputHint hint = HintAt(command, command.indexOf(QStringLiteral("out.mp4")) + 2);

    ASSERT_TRUE(hint.has_subject);
    EXPECT_TRUE(hint.slot == FfmpegInputSlot::OutputUrl);
    EXPECT_EQ(QStringLiteral("输出文件"), hint.subject_title);
}

TEST(FfmpegCommandInputHintTests, MapNegativeValueIsAValueNotAnOption) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i a.mp4 -map -0:v"));

    ASSERT_TRUE(hint.has_subject);
    EXPECT_TRUE(hint.slot == FfmpegInputSlot::OptionValue);
    EXPECT_TRUE(hint.subject_title.contains(QStringLiteral("的值"))) << hint.subject_title.toStdString();
    EXPECT_TRUE(hint.subject_detail.contains(QStringLiteral("-map"))) << hint.subject_detail.toStdString();
}

TEST(FfmpegCommandInputHintTests, TerminatedQuoteIsFineAndUnterminatedOneIsReported) {
    const FfmpegInputHint closed = HintAtEnd(QStringLiteral("ffmpeg -i \"my file.mp4\""));
    EXPECT_TRUE(closed.problem.isEmpty()) << closed.problem.toStdString();

    const FfmpegInputHint open = HintAtEnd(QStringLiteral("ffmpeg -i \"my file.mp4"));
    EXPECT_FALSE(open.problem.isEmpty());
    // 引号没闭合也照样能说出"这里是输入文件"，不能用"判不了"把整个提示关掉
    ASSERT_TRUE(open.has_subject);
    EXPECT_TRUE(open.slot == FfmpegInputSlot::InputUrl);
}

TEST(FfmpegCommandInputHintTests, QuotedPathGetsNoCandidates) {
    // 已经在引号里打字：候选替换会把引号吃掉，宁可不出候选
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i \"D:/素材/in.mp4"));
    EXPECT_TRUE(hint.candidates.empty()) << CandidateTexts(hint).toStdString();
}

TEST(FfmpegCommandInputHintTests, ProgramNameIsNotMistakenForAnInput) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg"));

    ASSERT_TRUE(hint.has_subject);
    EXPECT_TRUE(hint.slot == FfmpegInputSlot::Program);
    EXPECT_TRUE(hint.has_subject);
    EXPECT_FALSE(hint.missing_dash_i); // 程序名不是"缺 -i 的输入"
}

TEST(FfmpegCommandInputHintTests, FinishedCommandReportsItsStructure) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i a.mp4 -c:v libx264 b.mp4"));

    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("结构完整"))) << hint.next_label.toStdString();
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("1 个输入"))) << hint.next_label.toStdString();
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("1 个输出"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, SecondOutputIsCounted) {
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i a.mp4 -c:v copy b.mp4 -c:v libx264 c.mp4"));

    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("2 个输出"))) << hint.next_label.toStdString();
}

TEST(FfmpegCompletionEditTests, ReplacesTheWordInPlaceAndKeepsSpacing) {
    const QString command = QStringLiteral("ffmpeg -i in.mp4 -crf 23 out.mp4");
    const int begin = command.indexOf(QStringLiteral("-crf"));

    const auto applied = ApplyCompletion(command, begin, begin + 4, QStringLiteral("-c:v"));

    EXPECT_EQ(QStringLiteral("ffmpeg -i in.mp4 -c:v 23 out.mp4"), applied.text);
    // 光标落在替换后的词之后（尾随空格之前）：接着敲的是下一个词，不会粘成 -c:v23
    EXPECT_EQ(begin + 4, applied.cursor);
}

TEST(FfmpegCompletionEditTests, CompletionAtTheEndAddsATrailingSpace) {
    const QString command = QStringLiteral("ffmpeg -i in.mp4 -c");
    const int begin = static_cast<int>(command.size()) - 2;

    const auto applied = ApplyCompletion(command, begin, static_cast<int>(command.size()), QStringLiteral("-crf"));

    EXPECT_EQ(QStringLiteral("ffmpeg -i in.mp4 -crf "), applied.text);
    EXPECT_EQ(static_cast<int>(applied.text.size()), applied.cursor);
}

TEST(FfmpegCompletionEditTests, CompletionInsideTheLineDoesNotDoubleSpace) {
    const QString command = QStringLiteral("ffmpeg -i in.mp4 -c:v out.mp4");
    const int begin = command.indexOf(QStringLiteral("-c:v"));

    const auto applied = ApplyCompletion(command, begin, begin + 4, QStringLiteral("-c:a"));

    // 词两侧本来就各有一个空格，替换后不该再多补一个
    EXPECT_EQ(QStringLiteral("ffmpeg -i in.mp4 -c:a out.mp4"), applied.text);
}

TEST(FfmpegCompletionEditTests, EmptyReplacementKeepsTheText) {
    const QString command = QStringLiteral("ffmpeg -i in.mp4 out.mp4");
    const auto applied = ApplyCompletion(command, 7, 9, QString());

    EXPECT_EQ(command, applied.text);
    EXPECT_EQ(7, applied.cursor);
}

TEST(FfmpegCommandInputHintTests, ValueCandidatesAfterACompleteOptionBecomeANewWord) {
    // 光标停在已经打完的 `-c` 上：候选给的是它的值。这些值必须"接成新词"，
    // 不能原地替换 —— 替换会把 `-c` 整个吃掉，`-c copy` 变成 `copy`，意思全变了。
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffmpeg -i a.mp4 -c"));
    ASSERT_FALSE(hint.candidates.empty());
    for (const FfmpegInputCandidate& candidate : hint.candidates) {
        EXPECT_TRUE(candidate.starts_new_word) << candidate.text.toStdString();
    }

    // 半截选项名的候选仍然是"替换当前词"（-cr → -crf）
    const FfmpegInputHint partial = HintAtEnd(QStringLiteral("ffmpeg -i a.mp4 -cr"));
    ASSERT_FALSE(partial.candidates.empty());
    EXPECT_FALSE(partial.candidates.at(0).starts_new_word);
}

TEST(FfmpegCommandInputHintTests, FfprobeTreatsBareWordAsInputNotAnOutput) {
    // ffprobe 的裸参数就是输入文件：不需要 -i，也没有输出文件。
    // 拿 ffmpeg 的语法去套，这里会被判成"缺 -i 的输入"，然后劝用户写输出文件名 —— 全错。
    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffprobe -show_streams in.mp4"));

    EXPECT_EQ(hint.slot, FfmpegInputSlot::InputUrl);
    EXPECT_FALSE(hint.missing_dash_i) << "ffprobe 不需要 -i";
    EXPECT_EQ(hint.subject_title, QStringLiteral("输入文件"));
    EXPECT_FALSE(hint.next_label.contains(QStringLiteral("输出文件"))) << hint.next_label.toStdString();
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("结构完整"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, FfprobeCandidatesOnlyComeFromItsOwnDictionary) {
    const FfmpegInputHint show = HintAtEnd(QStringLiteral("ffprobe -show"));
    EXPECT_EQ(show.slot, FfmpegInputSlot::Option);
    EXPECT_TRUE(HasCandidate(show, QStringLiteral("-show_streams"))) << CandidateTexts(show).toStdString();

    // -crf 是 ffmpeg 的输出编码选项，ffprobe 里根本不存在 —— 不能进候选
    const FfmpegInputHint crf = HintAtEnd(QStringLiteral("ffprobe -cr"));
    EXPECT_FALSE(HasCandidate(crf, QStringLiteral("-crf"))) << CandidateTexts(crf).toStdString();

    // 反过来：-show_streams 也不该出现在 ffmpeg 的候选里
    const FfmpegInputHint ffmpeg_show = HintAtEnd(QStringLiteral("ffmpeg -show"));
    EXPECT_FALSE(HasCandidate(ffmpeg_show, QStringLiteral("-show_streams")))
        << CandidateTexts(ffmpeg_show).toStdString();
}

TEST(FfmpegCommandInputHintTests, FfplaySuggestsAutoexitAndTakesBareInput) {
    const FfmpegInputHint partial = HintAtEnd(QStringLiteral("ffplay -auto"));
    EXPECT_TRUE(HasCandidate(partial, QStringLiteral("-autoexit"))) << CandidateTexts(partial).toStdString();

    // ffmpeg 的输出编码选项不该混进 ffplay 的候选
    const FfmpegInputHint codec = HintAtEnd(QStringLiteral("ffplay -c"));
    EXPECT_FALSE(HasCandidate(codec, QStringLiteral("-c:v"))) << CandidateTexts(codec).toStdString();

    const FfmpegInputHint hint = HintAtEnd(QStringLiteral("ffplay in.mp4"));
    EXPECT_EQ(hint.slot, FfmpegInputSlot::InputUrl);
    EXPECT_FALSE(hint.missing_dash_i);
    // 不加 -autoexit 窗口会一直开着 —— 这一步必须提醒
    EXPECT_TRUE(hint.next_label.contains(QStringLiteral("-autoexit"))) << hint.next_label.toStdString();
}

TEST(FfmpegCommandInputHintTests, FfplayYMeansWindowHeightNotOverwrite) {
    // -y 同名不同义：ffmpeg 是"覆盖输出"，ffplay 是"窗口高度"。
    // 解释区必须按工具取条目，否则 ffplay 的 -y 会被解释成覆盖文件。
    const QString command = QStringLiteral("ffplay -y 720 in.mp4");
    const int cursor = command.indexOf(QStringLiteral("720")) + 1;
    const FfmpegInputHint hint = HintAt(command, cursor);

    EXPECT_EQ(hint.slot, FfmpegInputSlot::OptionValue);
    EXPECT_TRUE(hint.subject_title.contains(QStringLiteral("窗口高度"))) << hint.subject_title.toStdString();
    EXPECT_FALSE(hint.subject_title.contains(QStringLiteral("覆盖"))) << hint.subject_title.toStdString();
}
