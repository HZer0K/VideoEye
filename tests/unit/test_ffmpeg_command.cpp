// 「FFmpeg 命令工作台」的纯逻辑测试: 命令解析 + 指令字典 + 命令解释。
//
// 这一层刻意不碰 QProcess —— 真跑进程要依赖机器上有没有 ffmpeg，属于集成测试的活。
// 这里只验证"文本 → 参数数组 → 解释"这条链，它决定了用户看到的每一条提示对不对。

#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandDraft.h"
#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegCommandHistory.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegProgressTracker.h"
#include "core/ffmpeg/FfmpegToolLocator.h"

namespace {

using videoeye::ffmpeg::AnalyzeCommandStructure;
using videoeye::ffmpeg::ClassifyProgramToken;
using videoeye::ffmpeg::CommandParseStatus;
using videoeye::ffmpeg::ComposeInsertion;
using videoeye::ffmpeg::DefaultOutputPath;
using videoeye::ffmpeg::DetectStdoutMediaOutput;
using videoeye::ffmpeg::ExistingOutputTargets;
using videoeye::ffmpeg::FfmpegArgRole;
using videoeye::ffmpeg::FfmpegCapabilityCache;
using videoeye::ffmpeg::FfmpegCatalogEntry;
using videoeye::ffmpeg::FfmpegCommandCatalog;
using videoeye::ffmpeg::FfmpegCommandExplainer;
using videoeye::ffmpeg::FfmpegCommandExplanation;
using videoeye::ffmpeg::FfmpegCommandHistory;
using videoeye::ffmpeg::FfmpegEntryCategory;
using videoeye::ffmpeg::FfmpegEntryKind;
using videoeye::ffmpeg::FfmpegFormatLists;
using videoeye::ffmpeg::FfmpegInstallGuide;
using videoeye::ffmpeg::FfmpegProgressSnapshot;
using videoeye::ffmpeg::FfmpegProgressTracker;
using videoeye::ffmpeg::FfmpegTokenExplanation;
using videoeye::ffmpeg::FfmpegTokenRole;
using videoeye::ffmpeg::FfmpegToolInfo;
using videoeye::ffmpeg::FfmpegToolKind;
using videoeye::ffmpeg::FfmpegToolLocator;
using videoeye::ffmpeg::FormatByteSize;
using videoeye::ffmpeg::FormatProgressClock;
using videoeye::ffmpeg::FormatProgressDetail;
using videoeye::ffmpeg::FormatProgressSummary;
using videoeye::ffmpeg::InformationOptionDescription;
using videoeye::ffmpeg::IsExplicitProgramPath;
using videoeye::ffmpeg::IsInformationCommand;
using videoeye::ffmpeg::LooksLikeFfmpegProgram;
using videoeye::ffmpeg::ParseCommandLine;

std::vector<std::string> ToStdList(const QStringList& list) {
    std::vector<std::string> out;
    out.reserve(list.size());
    for (const QString& item : list) {
        out.push_back(item.toStdString());
    }
    return out;
}

/// 按选项名找解释条目（找不到返回 nullptr）。
const FfmpegTokenExplanation* FindToken(const FfmpegCommandExplanation& explanation, const QString& token) {
    for (const FfmpegTokenExplanation& item : explanation.tokens) {
        if (item.token == token) {
            return &item;
        }
    }
    return nullptr;
}

// ===================== 解析 =====================

TEST(FfmpegCommandParser, SplitsPlainCommand) {
    const auto parsed = ParseCommandLine(QStringLiteral("ffmpeg -i in.mp4 -c:v libx264 out.mp4"));
    ASSERT_EQ(parsed.status, CommandParseStatus::Ok);
    EXPECT_EQ(ToStdList(parsed.arguments), std::vector<std::string>({"-i", "in.mp4", "-c:v", "libx264", "out.mp4"}));
    EXPECT_EQ(parsed.program_token, QStringLiteral("ffmpeg"));
}

TEST(FfmpegCommandParser, KeepsQuotedPathWithSpaces) {
    const auto parsed =
        ParseCommandLine(QStringLiteral("ffmpeg -i \"D:/my media/源 文件.mp4\" -c copy \"out file.mp4\""));
    ASSERT_EQ(parsed.status, CommandParseStatus::Ok);
    ASSERT_EQ(parsed.arguments.size(), 5);
    EXPECT_EQ(parsed.arguments.at(1), QStringLiteral("D:/my media/源 文件.mp4"));
    EXPECT_EQ(parsed.arguments.at(4), QStringLiteral("out file.mp4"));
}

TEST(FfmpegCommandParser, AcceptsSingleQuotesAndOmittedProgram) {
    const auto with_quote = ParseCommandLine(QStringLiteral("-i 'a b.mp4' out.mp4"));
    ASSERT_EQ(with_quote.status, CommandParseStatus::Ok);
    EXPECT_EQ(with_quote.arguments.at(1), QStringLiteral("a b.mp4"));

    // 省略程序名: 第一个 token 以 '-' 开头时整个命令行都是参数
    const auto bare = ParseCommandLine(QStringLiteral("-version"));
    ASSERT_EQ(bare.status, CommandParseStatus::Ok);
    EXPECT_EQ(ToStdList(bare.arguments), std::vector<std::string>({"-version"}));
}

TEST(FfmpegCommandParser, AcceptsFullPathToFfmpeg) {
    EXPECT_TRUE(LooksLikeFfmpegProgram(QStringLiteral("ffmpeg")));
    EXPECT_TRUE(LooksLikeFfmpegProgram(QStringLiteral("ffmpeg.exe")));
    EXPECT_TRUE(LooksLikeFfmpegProgram(QStringLiteral("C:/tools/ffmpeg-8.1/bin/ffmpeg.exe")));
    EXPECT_FALSE(LooksLikeFfmpegProgram(QStringLiteral("ffprobe")));
    EXPECT_FALSE(LooksLikeFfmpegProgram(QStringLiteral("-i")));

    const auto parsed = ParseCommandLine(QStringLiteral("C:/tools/ffmpeg.exe -i in.mp4 out.mp4"));
    ASSERT_EQ(parsed.status, CommandParseStatus::Ok);
    EXPECT_EQ(parsed.arguments.at(0), QStringLiteral("-i"));
}

TEST(FfmpegToolKind, ClassifiesTheThreeFfmpegTools) {
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("ffmpeg")), FfmpegToolKind::Ffmpeg);
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("ffprobe")), FfmpegToolKind::Ffprobe);
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("ffplay")), FfmpegToolKind::Ffplay);

    // 带后缀 / 带路径 / 大小写不同，都是同一个程序
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("FFPROBE.EXE")), FfmpegToolKind::Ffprobe);
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("C:/tools/ffmpeg-8.1/bin/ffplay.exe")), FfmpegToolKind::Ffplay);

    // 选项、别的程序、空串都不算
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("-i")), FfmpegToolKind::Unknown);
    EXPECT_EQ(ClassifyProgramToken(QStringLiteral("ffmpeg-probe")), FfmpegToolKind::Unknown);
    EXPECT_EQ(ClassifyProgramToken(QString()), FfmpegToolKind::Unknown);

    // LooksLikeFfmpegProgram 的语义收窄成"就是 ffmpeg"，不能被 ffprobe 带偏
    EXPECT_TRUE(LooksLikeFfmpegProgram(QStringLiteral("ffmpeg")));
    EXPECT_FALSE(LooksLikeFfmpegProgram(QStringLiteral("ffprobe")));
}

TEST(FfmpegCommandParser, ParsesFfprobeAndFfplayCommands) {
    // ffprobe / ffplay 的第一个词同样是程序名，要丢掉并记住是哪个工具
    const auto probe = ParseCommandLine(QStringLiteral("ffprobe -show_streams \"D:/素材/a.mp4\""));
    ASSERT_EQ(probe.status, CommandParseStatus::Ok);
    EXPECT_EQ(probe.tool_kind, FfmpegToolKind::Ffprobe);
    EXPECT_EQ(probe.program_token, QStringLiteral("ffprobe"));
    EXPECT_FALSE(probe.program_is_path);
    ASSERT_EQ(probe.arguments.size(), 2);
    EXPECT_EQ(probe.arguments.at(0), QStringLiteral("-show_streams"));
    EXPECT_EQ(probe.arguments.at(1), QStringLiteral("D:/素材/a.mp4"));

    const auto play = ParseCommandLine(QStringLiteral("ffplay -autoexit a.mp4"));
    ASSERT_EQ(play.status, CommandParseStatus::Ok);
    EXPECT_EQ(play.tool_kind, FfmpegToolKind::Ffplay);
    EXPECT_EQ(ToStdList(play.arguments), std::vector<std::string>({"-autoexit", "a.mp4"}));

    // 省略程序名时工具种类未知 —— 由调用方决定落到哪个工具上
    const auto bare = ParseCommandLine(QStringLiteral("-version"));
    ASSERT_EQ(bare.status, CommandParseStatus::Ok);
    EXPECT_EQ(bare.tool_kind, FfmpegToolKind::Unknown);
}

TEST(FfmpegCommandCatalog, ScopesEntriesToTheRightTool) {
    // 通用选项：三个工具都认
    EXPECT_NE(FfmpegCommandCatalog::Find(QStringLiteral("-hide_banner"), FfmpegToolKind::Ffplay), nullptr);
    EXPECT_NE(FfmpegCommandCatalog::Find(QStringLiteral("-hide_banner"), FfmpegToolKind::Ffprobe), nullptr);

    // -i 只有 ffmpeg / ffprobe 有：ffplay 把文件直接写在裸参数位
    EXPECT_NE(FfmpegCommandCatalog::Find(QStringLiteral("-i"), FfmpegToolKind::Ffprobe), nullptr);
    EXPECT_EQ(FfmpegCommandCatalog::Find(QStringLiteral("-i"), FfmpegToolKind::Ffplay), nullptr);

    // 输出编码选项不该出现在 ffprobe / ffplay 的候选里
    EXPECT_EQ(FfmpegCommandCatalog::Find(QStringLiteral("-crf"), FfmpegToolKind::Ffprobe), nullptr);
    EXPECT_EQ(FfmpegCommandCatalog::Find(QStringLiteral("-crf"), FfmpegToolKind::Ffplay), nullptr);

    // ffprobe 专有选项反方向也不该漏到 ffmpeg 里
    ASSERT_NE(FfmpegCommandCatalog::Find(QStringLiteral("-show_streams"), FfmpegToolKind::Ffprobe), nullptr);
    EXPECT_EQ(FfmpegCommandCatalog::Find(QStringLiteral("-show_streams"), FfmpegToolKind::Ffmpeg), nullptr);

    // 不指定工具时仍是全字典（历史行为不变，解释器和字典页都靠它）
    EXPECT_NE(FfmpegCommandCatalog::Find(QStringLiteral("-show_streams")), nullptr);
}

TEST(FfmpegCommandCatalog, ResolvesSameNameOptionsPerTool) {
    // -y 在 ffmpeg 里是"覆盖输出"，在 ffplay 里是"窗口高度" —— 同名不同义。
    // 这就是 Find(name, tool) 必须遍历到底、不能先取第一条的原因。
    const FfmpegCatalogEntry* ffmpeg_y = FfmpegCommandCatalog::Find(QStringLiteral("-y"), FfmpegToolKind::Ffmpeg);
    const FfmpegCatalogEntry* ffplay_y = FfmpegCommandCatalog::Find(QStringLiteral("-y"), FfmpegToolKind::Ffplay);
    ASSERT_NE(ffmpeg_y, nullptr);
    ASSERT_NE(ffplay_y, nullptr);
    EXPECT_NE(ffmpeg_y, ffplay_y);
    EXPECT_EQ(ffmpeg_y->category, FfmpegEntryCategory::Global);
    EXPECT_EQ(ffplay_y->category, FfmpegEntryCategory::Playback);
    EXPECT_FALSE(ffmpeg_y->takes_value); // ffmpeg 的 -y 是开关
    EXPECT_TRUE(ffplay_y->takes_value);  // ffplay 的 -y 要一个高度
}

TEST(FfmpegCommandCatalog, EntriesForFiltersByTool) {
    const auto probe_entries = FfmpegCommandCatalog::EntriesFor(FfmpegToolKind::Ffprobe);
    bool has_show_streams = false;
    bool has_crf = false;
    for (const FfmpegCatalogEntry* entry : probe_entries) {
        if (entry->name == QLatin1String("-show_streams")) {
            has_show_streams = true;
        }
        if (entry->name == QLatin1String("-crf")) {
            has_crf = true;
        }
    }
    EXPECT_TRUE(has_show_streams);
    EXPECT_FALSE(has_crf);

    // Unknown 不做筛选：工具还没定下来时不能把候选砍掉
    EXPECT_EQ(FfmpegCommandCatalog::EntriesFor(FfmpegToolKind::Unknown).size(), FfmpegCommandCatalog::Entries().size());
}

TEST(FfmpegCommandParser, MarksExplicitProgramPath) {
    // 命令里写了完整路径就必须**真的执行它**（原生命令行的直觉）；
    // 只写裸的 "ffmpeg" 才用页面设置里那个程序。
    EXPECT_TRUE(IsExplicitProgramPath(QStringLiteral("C:/tools/ffmpeg-8.1/bin/ffmpeg.exe")));
    EXPECT_TRUE(IsExplicitProgramPath(QStringLiteral("C:\\tools\\ffmpeg.exe")));
    EXPECT_TRUE(IsExplicitProgramPath(QStringLiteral("./ffmpeg")));
    EXPECT_FALSE(IsExplicitProgramPath(QStringLiteral("ffmpeg")));
    EXPECT_FALSE(IsExplicitProgramPath(QStringLiteral("ffmpeg.exe")));

    const auto with_path = ParseCommandLine(QStringLiteral("C:/tools/ffmpeg.exe -i in.mp4 out.mp4"));
    ASSERT_EQ(with_path.status, CommandParseStatus::Ok);
    EXPECT_TRUE(with_path.program_is_path);
    EXPECT_EQ(with_path.program_token, QStringLiteral("C:/tools/ffmpeg.exe"));

    const auto bare = ParseCommandLine(QStringLiteral("ffmpeg -i in.mp4 out.mp4"));
    ASSERT_EQ(bare.status, CommandParseStatus::Ok);
    EXPECT_FALSE(bare.program_is_path);
}

TEST(FfmpegCommandParser, RecognizesInformationCommands) {
    for (const QString& option : {
             QStringLiteral("-version"),
             QStringLiteral("-buildconf"),
             QStringLiteral("-formats"),
             QStringLiteral("-demuxers"),
             QStringLiteral("-muxers"),
             QStringLiteral("-encoders"),
             QStringLiteral("-decoders"),
             QStringLiteral("-filters"),
             QStringLiteral("-pix_fmts"),
             QStringLiteral("-layouts"),
             QStringLiteral("-sample_fmts"),
             QStringLiteral("-protocols"),
             QStringLiteral("-devices"),
             QStringLiteral("-hwaccels"),
             QStringLiteral("-bsfs"),
             QStringLiteral("-h"),
         }) {
        EXPECT_TRUE(IsInformationCommand({option})) << option.toStdString();
        EXPECT_FALSE(InformationOptionDescription(option).isEmpty()) << option.toStdString();
    }
    // 转码命令不能因为参数里恰好有个 -h 之类就被当成查询命令
    EXPECT_FALSE(IsInformationCommand({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("out.mp4")}));
    EXPECT_TRUE(InformationOptionDescription(QStringLiteral("-i")).isEmpty());
}

TEST(FfmpegCommandParser, RejectsUnterminatedQuote) {
    const auto parsed = ParseCommandLine(QStringLiteral("ffmpeg -i \"D:/media/in.mp4 out.mp4"));
    EXPECT_EQ(parsed.status, CommandParseStatus::UnterminatedQuote);
    EXPECT_FALSE(parsed.message.isEmpty());
}

TEST(FfmpegCommandParser, RejectsShellOperators) {
    for (const QString& cmd : {
             QStringLiteral("ffmpeg -i in.mp4 -f null - | cat"),
             QStringLiteral("ffmpeg -i in.mp4 out.mp4 && echo done"),
             QStringLiteral("ffmpeg -i in.mp4 out.mp4 > log.txt"),
             QStringLiteral("ffmpeg -i in.mp4 out.mp4; echo done"),
         }) {
        const auto parsed = ParseCommandLine(cmd);
        EXPECT_EQ(parsed.status, CommandParseStatus::ShellOperator) << cmd.toStdString();
    }
}

TEST(FfmpegCommandParser, RejectsForeignProgramAndEmptyInput) {
    // ffprobe / ffplay 现在是合法程序（本页支持三个 FFmpeg 工具），
    // 所以"外来程序"要拿真正不相干的程序来验。
    const auto other = ParseCommandLine(QStringLiteral("vlc in.mp4"));
    EXPECT_EQ(other.status, CommandParseStatus::UnsupportedProgram);
    EXPECT_FALSE(other.message.isEmpty());

    const auto empty = ParseCommandLine(QStringLiteral("   "));
    EXPECT_EQ(empty.status, CommandParseStatus::Empty);
}

TEST(FfmpegCommandParser, DetectsPipeOutput) {
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("pipe:1")}),
              QStringLiteral("pipe:1"));
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"),
                                       QStringLiteral("matroska"), QStringLiteral("-")}),
              QStringLiteral("-"));
    // -f null - 是"只解码不出片"的基准测试写法，不往 stdout 写数据，不能误判
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"),
                                         QStringLiteral("null"), QStringLiteral("-")})
                    .isEmpty());
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c"),
                                         QStringLiteral("copy"), QStringLiteral("out.mp4")})
                    .isEmpty());
    // 只产出文本的 muxer 写到管道上也只是几行字，不必拦
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"),
                                         QStringLiteral("md5"), QStringLiteral("-")})
                    .isEmpty());
}

TEST(FfmpegCommandParser, DetectsPipeWriteToStderr) {
    // pipe:2 写的是标准错误，一样会把二进制灌进日志区
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"),
                                       QStringLiteral("copy"), QStringLiteral("pipe:2")}),
              QStringLiteral("pipe:2"));
}

TEST(FfmpegCommandParser, DetectsLaterPipeOutputInMultiOutputCommand) {
    // 这正是"-f null 被记成全命令状态"漏掉的情形: 前一个输出是基准测试用的 -f null -，
    // 后一个输出才是真的往通道写 —— 必须各自按自己生效的 -f 判断。
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"),
                                       QStringLiteral("null"), QStringLiteral("-"), QStringLiteral("-c"),
                                       QStringLiteral("copy"), QStringLiteral("-f"), QStringLiteral("matroska"),
                                       QStringLiteral("pipe:2")}),
              QStringLiteral("pipe:2"));
    // 反过来：两个输出都是文件，不该因为命令里有管道名而误报
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"),
                                         QStringLiteral("null"), QStringLiteral("-"), QStringLiteral("-f"),
                                         QStringLiteral("mp4"), QStringLiteral("out.mp4")})
                    .isEmpty());
}

// ===================== 命令结构（输入组 / 输出组）=====================

TEST(FfmpegCommandStructure, SplitsInputsAndOutputs) {
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("a.mp4"), QStringLiteral("-i"), QStringLiteral("b.mp4"),
         QStringLiteral("-c:v"), QStringLiteral("copy"), QStringLiteral("out1.mp4"), QStringLiteral("out2.mkv")});
    EXPECT_EQ(structure.input_count, 2);
    EXPECT_EQ(structure.output_count, 2);
    EXPECT_TRUE(structure.complete);
    EXPECT_EQ(structure.args.at(1).role, FfmpegArgRole::InputUrl);
    EXPECT_EQ(structure.args.at(3).role, FfmpegArgRole::InputUrl);
    EXPECT_EQ(structure.args.at(6).role, FfmpegArgRole::OutputUrl);
    EXPECT_EQ(structure.args.at(6).output_index, 0);
    EXPECT_EQ(structure.args.at(7).role, FfmpegArgRole::OutputUrl);
    EXPECT_EQ(structure.args.at(7).output_index, 1);
}

TEST(FfmpegCommandStructure, ResolvesFormatPerOutput) {
    const auto structure = AnalyzeCommandStructure({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                                    QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-"),
                                                    QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("-f"),
                                                    QStringLiteral("matroska"), QStringLiteral("-")});
    ASSERT_EQ(structure.output_count, 2);
    EXPECT_EQ(structure.args.at(4).output_format, QStringLiteral("null"));
    EXPECT_EQ(structure.args.at(9).output_format, QStringLiteral("matroska"));
}

TEST(FfmpegCommandStructure, InputSideFormatIsNotTreatedAsOutputFormat) {
    const auto structure = AnalyzeCommandStructure({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                                                    QStringLiteral("testsrc"), QStringLiteral("out.mp4")});
    EXPECT_EQ(structure.input_count, 1);
    EXPECT_EQ(structure.output_count, 1);
    EXPECT_TRUE(structure.args.at(0).applies_to_input);
    EXPECT_TRUE(structure.args.at(4).output_format.isEmpty());
}

TEST(FfmpegCommandStructure, FormatAfterAnOutputStillBelongsToTheNextInput) {
    // 回归: 判断 -f 方向不能看"之前出过几个输出"。合法命令可以在已有输出之后
    // 继续添加输入组 —— 这里的 -f lavfi 依然是**输入**格式。
    const auto structure =
        AnalyzeCommandStructure({QStringLiteral("-i"), QStringLiteral("a.mp4"), QStringLiteral("out1.mp4"),
                                 QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                                 QStringLiteral("testsrc=size=640x360"), QStringLiteral("out2.mp4")});

    ASSERT_EQ(structure.input_count, 2);
    ASSERT_EQ(structure.output_count, 2);
    // 第二个 -f 在下标 3
    EXPECT_TRUE(structure.args.at(3).applies_to_input);
    // 它是输入格式，不能挂到后一个输出头上
    EXPECT_TRUE(structure.args.at(7).output_format.isEmpty());

    // 反过来：同一条命令里"输出前的 -f"仍然要算输出格式
    const auto with_output_format = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("a.mp4"), QStringLiteral("out1.mp4"), QStringLiteral("-f"),
         QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc"), QStringLiteral("-f"),
         QStringLiteral("mp4"), QStringLiteral("out2.mp4")});
    EXPECT_TRUE(with_output_format.args.at(3).applies_to_input);
    EXPECT_FALSE(with_output_format.args.at(7).applies_to_input);
    EXPECT_EQ(with_output_format.args.at(9).output_format, QStringLiteral("mp4"));
}

TEST(FfmpegCommandStructure, UnknownOptionLeavesNextTokenUnresolved) {
    // 字典没有这个选项 → 后面那个 token 是它的值还是输出文件，不能猜。
    // 这里它后面没有**别的**裸 token，两种解释都说得通，所以保持 Uncertain。
    // （后面还有裸 token 时才能按位置定成值，见 UnknownOptionValueIsInferredWhenAnotherOutputFollows）
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-zzz-unknown"), QStringLiteral("out.mp4")});
    EXPECT_FALSE(structure.complete);
    EXPECT_EQ(structure.args.at(3).role, FfmpegArgRole::Uncertain);
}

TEST(FfmpegCommandStructure, UnknownOptionValueIsInferredWhenAnotherOutputFollows) {
    // 未收录选项后面紧跟的裸 token 有两种可能。判据: 它后面还有没有**别的**裸 token。
    // 有 → 前者只能是值（一条命令里连续两个裸 token 都属于选项值的情况不存在，输出却可以有多个）。
    // 少了这条推断，`-bsf:v h264_mp4toannexb out.ts` 会把 out.ts 当成值，
    // 整条命令变成"没有输出文件"、覆盖检查也一并失效。
    // （-bsf:v 现在已收进字典，但用户命令里永远会有下一个没收录的。）
    const auto structure =
        AnalyzeCommandStructure({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-zzz-unknown"),
                                 QStringLiteral("value"), QStringLiteral("out.ts")});
    ASSERT_EQ(structure.args.size(), 5u);
    EXPECT_EQ(structure.args.at(3).role, FfmpegArgRole::OptionValue);
    EXPECT_EQ(structure.args.at(4).role, FfmpegArgRole::OutputUrl);
    EXPECT_TRUE(structure.complete);
    EXPECT_EQ(structure.output_count, 1);
}

TEST(FfmpegCommandStructure, NegativeMapValueIsAValueNotAnOption) {
    // 回归: `-map -0:d` 是合法的"排除第 0 个输入里某类流"。它后面的输出文件
    // 不能被当成"这个以 - 开头的选项的值" —— 那样会连带误报"没有输出文件"。
    const auto structure = AnalyzeCommandStructure({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                                    QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-map"),
                                                    QStringLiteral("-0:d"), QStringLiteral("out.mp4")});

    ASSERT_EQ(structure.args.size(), 7u);
    EXPECT_EQ(structure.args.at(5).role, FfmpegArgRole::OptionValue); // -0:d 是 -map 的值
    EXPECT_EQ(structure.args.at(6).role, FfmpegArgRole::OutputUrl);   // out.mp4 仍然是输出
    EXPECT_TRUE(structure.complete);
    EXPECT_EQ(structure.output_count, 1);

    // -map_metadata -1 同理（清掉元数据）
    const auto metadata =
        AnalyzeCommandStructure({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-map_metadata"),
                                 QStringLiteral("-1"), QStringLiteral("out.mp4")});
    EXPECT_EQ(metadata.args.at(3).role, FfmpegArgRole::OptionValue);
    EXPECT_EQ(metadata.args.at(4).role, FfmpegArgRole::OutputUrl);
    EXPECT_TRUE(metadata.complete);
}

TEST(FfmpegCommandParser, ExistingOutputTargetsOnlyReportsRealFiles) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString existing = dir.filePath(QStringLiteral("out.mp4"));
    {
        QFile file(existing);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    const QString absent = dir.filePath(QStringLiteral("new.mp4"));

    EXPECT_EQ(ExistingOutputTargets({QStringLiteral("-i"), QStringLiteral("in.mp4"), existing, absent}),
              QStringList{existing});

    // 结构不明时宁可少问一次: 猜出来的"输出路径"绝不能拿去做覆盖确认
    // （未收录选项 + 它后面就是最后一个裸 token —— 无法判断那是值还是输出）
    EXPECT_TRUE(ExistingOutputTargets(
                    {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-zzz-unknown"), existing})
                    .isEmpty());

    // 管道目标没有磁盘文件可撞名
    EXPECT_TRUE(ExistingOutputTargets({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"),
                                       QStringLiteral("null"), QStringLiteral("-")})
                    .isEmpty());

    // 没有 -y 时页面才会问；这条只验"哪些文件会撞名"，不替调用方做决定
    EXPECT_EQ(ExistingOutputTargets({QStringLiteral("-y"), QStringLiteral("-i"), QStringLiteral("in.mp4"), existing}),
              QStringList{existing});
}

// ===================== 字典 =====================

TEST(FfmpegCommandCatalog, FindsKnownOptions) {
    const FfmpegCatalogEntry* crf = FfmpegCommandCatalog::Find(QStringLiteral("-crf"));
    ASSERT_TRUE(crf != nullptr);
    EXPECT_FALSE(crf->summary.isEmpty());
    EXPECT_FALSE(crf->position.isEmpty());
    EXPECT_FALSE(crf->example.isEmpty());
    EXPECT_TRUE(crf->takes_value);
}

TEST(FfmpegCommandCatalog, UnknownOptionIsNull) {
    EXPECT_TRUE(FfmpegCommandCatalog::Find(QStringLiteral("-no_such_option")) == nullptr);
}

TEST(FfmpegCommandCatalog, CoversHighFrequencyOptions) {
    // 首版承诺的是"高频参数够用"，这里把必须覆盖的入口钉住，删词条会立刻红
    const std::vector<QString> required = {
        QStringLiteral("-i"),   QStringLiteral("-c:v"), QStringLiteral("-c:a"),      QStringLiteral("-crf"),
        QStringLiteral("-b:v"), QStringLiteral("-b:a"), QStringLiteral("-preset"),   QStringLiteral("-pix_fmt"),
        QStringLiteral("-vf"),  QStringLiteral("-af"),  QStringLiteral("-map"),      QStringLiteral("-ss"),
        QStringLiteral("-t"),   QStringLiteral("-y"),   QStringLiteral("-movflags"), QStringLiteral("-filter_complex"),
        QStringLiteral("-g"),   QStringLiteral("-ar"),
    };
    for (const QString& name : required) {
        EXPECT_TRUE(FfmpegCommandCatalog::Find(name) != nullptr) << name.toStdString();
    }
    EXPECT_GE(FfmpegCommandCatalog::Entries().size(), 30u);
}

TEST(FfmpegCommandCatalog, SearchHitsByKeywordAndCategory) {
    const auto by_keyword = FfmpegCommandCatalog::Search(QStringLiteral("码率"));
    EXPECT_FALSE(by_keyword.empty());

    const auto in_encoding = FfmpegCommandCatalog::Search(
        QString(), FfmpegCommandCatalog::CategoryName(videoeye::ffmpeg::FfmpegEntryCategory::Encoding));
    ASSERT_FALSE(in_encoding.empty());
    for (const auto* entry : in_encoding) {
        EXPECT_EQ(FfmpegCommandCatalog::CategoryName(entry->category),
                  FfmpegCommandCatalog::CategoryName(videoeye::ffmpeg::FfmpegEntryCategory::Encoding));
    }
}

TEST(FfmpegCommandCatalog, FiltersAreMarkedAndInsertable) {
    const FfmpegCatalogEntry* scale = FfmpegCommandCatalog::Find(QStringLiteral("scale"));
    ASSERT_TRUE(scale != nullptr);
    EXPECT_EQ(scale->kind, FfmpegEntryKind::Filter);
    EXPECT_FALSE(scale->insert_text.isEmpty());
}

// `ffmpeg -formats` 的行: 列对齐空格 + D/E 两个标志位 + 名字（可带逗号别名）
TEST(FfmpegCommandCatalog, SplitsFormatsIntoDemuxersAndMuxers) {
    const QString formats = QStringLiteral("File formats:\n"
                                           " D. = Demuxing supported\n"
                                           " .E = Muxing supported\n"
                                           " --\n"
                                           " D  3dostr          3DO STR\n"
                                           "  E 3g2             3GP2 (3GPP2 file format)\n"
                                           " DE mov,mp4,m4a     QuickTime / MPEG-4\n");
    const FfmpegFormatLists lists = FfmpegCommandCatalog::ParseFormatNames(formats);

    // "  E" 与 " DE" 都表示能写 —— trim() 之后只看首字母会把后者整批漏掉
    EXPECT_TRUE(lists.muxers.contains(QStringLiteral("3g2")));
    EXPECT_TRUE(lists.muxers.contains(QStringLiteral("mp4")));
    // 逗号分隔的别名要拆开，否则用户写 `-f mp4` 时查不到
    EXPECT_TRUE(lists.muxers.contains(QStringLiteral("m4a")));
    EXPECT_TRUE(lists.demuxers.contains(QStringLiteral("mov")));
    EXPECT_TRUE(lists.demuxers.contains(QStringLiteral("3dostr")));
    // 只能读的格式不该出现在能写清单里（反之亦然）
    EXPECT_FALSE(lists.muxers.contains(QStringLiteral("3dostr")));
    EXPECT_FALSE(lists.demuxers.contains(QStringLiteral("3g2")));
    // 表头与图例行不能混进来
    EXPECT_FALSE(lists.muxers.contains(QStringLiteral("=")));
    EXPECT_FALSE(lists.demuxers.contains(QStringLiteral("=")));
}

TEST(FfmpegCommandCatalog, CapabilityCacheKeepsDemuxersApartFromMuxers) {
    FfmpegCapabilityCache::Instance().Clear();
    FfmpegCapabilityCache::Instance().SetFormats({QStringLiteral("lavfi")}, {QStringLiteral("ffmetadata")});
    auto& caps = FfmpegCapabilityCache::Instance();
    EXPECT_TRUE(caps.HasDemuxer(QStringLiteral("lavfi")));
    EXPECT_FALSE(caps.HasMuxer(QStringLiteral("lavfi")));
    EXPECT_TRUE(caps.HasMuxer(QStringLiteral("ffmetadata")));
    EXPECT_FALSE(caps.HasDemuxer(QStringLiteral("ffmetadata")));
    EXPECT_TRUE(caps.HasFormat(QStringLiteral("lavfi")));
    EXPECT_FALSE(caps.HasFormat(QStringLiteral("unknown-fmt")));
    caps.Clear();
    // 清过之后回到"不知道"，不能反过来给用户造假报错
    EXPECT_TRUE(caps.HasMuxer(QStringLiteral("whatever")));
    EXPECT_FALSE(caps.formats_known());
}

TEST(FfmpegCommandCatalog, ParsesCapabilityLists) {
    const QString encoders = QStringLiteral("Encoders:\n"
                                            " V..... = Video\n"
                                            " ------\n"
                                            " V..... libx264              libx264 H.264 / AVC\n"
                                            " A..... aac                  AAC (Advanced Audio Coding)\n");
    const auto names = FfmpegCommandCatalog::ParseEncoderNames(encoders);
    EXPECT_TRUE(names.contains(QStringLiteral("libx264")));
    EXPECT_TRUE(names.contains(QStringLiteral("aac")));
    EXPECT_FALSE(names.contains(QStringLiteral("=")));
}

// ===================== 解释 =====================

TEST(FfmpegCommandExplainer, ExplainsTypicalTranscode) {
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
         QStringLiteral("-crf"), QStringLiteral("23"), QStringLiteral("out.mp4")});

    EXPECT_TRUE(result.has_input);
    EXPECT_TRUE(result.has_output);
    ASSERT_GE(result.tokens.size(), 4u);

    const auto& input = result.tokens.front();
    EXPECT_EQ(input.role, FfmpegTokenRole::InputFile);
    EXPECT_EQ(input.value, QStringLiteral("in.mp4"));

    const auto& output = result.tokens.back();
    EXPECT_EQ(output.role, FfmpegTokenRole::OutputFile);

    // 流程里必须出现"输入文件 → ... → 输出文件"
    EXPECT_TRUE(result.flow.contains(QStringLiteral("输入文件")));
    EXPECT_TRUE(result.flow.contains(QStringLiteral("输出文件")));
    EXPECT_TRUE(result.flow.contains(QStringLiteral("视频编码器")));
    EXPECT_TRUE(result.flow.contains(QStringLiteral("视频质量 (CRF)")));
}

TEST(FfmpegCommandExplainer, ExplainsEveryFilterInChain) {
    // 原来只查第一个滤镜名，串里后面的滤镜既不解释也不校验
    FfmpegCapabilityCache::Instance().Clear();
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-vf"),
                                         QStringLiteral("scale=1280:-2,unsharp=5:5"), QStringLiteral("out.mp4")});

    const auto* vf = FindToken(result, QStringLiteral("-vf"));
    ASSERT_TRUE(vf != nullptr);
    EXPECT_TRUE(vf->description.contains(QStringLiteral("滤镜链拆解"))) << vf->description.toStdString();
    EXPECT_TRUE(vf->description.contains(QStringLiteral("scale 1280:-2"))) << vf->description.toStdString();
    EXPECT_TRUE(vf->description.contains(QStringLiteral("unsharp 5:5"))) << vf->description.toStdString();
    // 没探测过能力清单就不许说人家不可用
    EXPECT_TRUE(vf->warning.isEmpty()) << vf->warning.toStdString();
}

TEST(FfmpegCommandExplainer, WarnsForUnavailableFilterAnywhereInGraph) {
    FfmpegCapabilityCache::Instance().Clear();
    FfmpegCapabilityCache::Instance().SetFilters({QStringLiteral("scale")});
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-filter_complex"),
         QStringLiteral("[0:v]scale=1[v];[0:a]volumex=2[a]"), QStringLiteral("-map"), QStringLiteral("[v]"),
         QStringLiteral("out.mp4")});

    const auto* graph = FindToken(result, QStringLiteral("-filter_complex"));
    ASSERT_TRUE(graph != nullptr);
    EXPECT_TRUE(graph->description.contains(QStringLiteral("2 条链"))) << graph->description.toStdString();
    // 第二个链上的那个假滤镜要被抓出来，不能被第一个滤镜的"可用"掩盖
    EXPECT_TRUE(graph->warning.contains(QStringLiteral("volumex"))) << graph->warning.toStdString();
    EXPECT_FALSE(graph->warning.contains(QStringLiteral("scale"))) << graph->warning.toStdString();

    FfmpegCapabilityCache::Instance().Clear();
}

TEST(FfmpegCommandExplainer, RecognizesMultipleOutputs) {
    // 一条命令可以有两个输出：写在两个输出之间的参数归属后一个输出
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-s"), QStringLiteral("hd480"),
         QStringLiteral("out1.mp4"), QStringLiteral("-s"), QStringLiteral("hd720"), QStringLiteral("out2.mp4")});

    EXPECT_TRUE(result.has_output);
    EXPECT_EQ(result.output_count, 2);
    int output_tokens = 0;
    for (const auto& token : result.tokens) {
        if (token.role == FfmpegTokenRole::OutputFile) {
            ++output_tokens;
        }
    }
    EXPECT_EQ(output_tokens, 2);
    EXPECT_EQ(result.tokens.back().role, FfmpegTokenRole::OutputFile);
    EXPECT_EQ(result.tokens.back().group_index, 1);
    EXPECT_FALSE(result.notes.empty());
}

TEST(FfmpegCommandExplainer, SecondOutputIsNotCalledMisplaced) {
    // 旧实现只认最后一个裸参数，前面的输出文件会被说成"位置不明"
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("out1.mp4"),
                                         QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("out2.mp4")});
    for (const auto& token : result.tokens) {
        if (token.token == QStringLiteral("out1.mp4") || token.token == QStringLiteral("out2.mp4")) {
            EXPECT_EQ(token.role, FfmpegTokenRole::OutputFile) << token.token.toStdString();
            EXPECT_TRUE(token.warning.isEmpty()) << token.token.toStdString();
        }
    }
}

TEST(FfmpegCommandExplainer, UnresolvedTokensCarryNoAdvice) {
    // 认不出来的结构必须说"未解析"，而不是给一条可能错的纠正建议。
    // 这里选项没收录、后面又只剩一个裸 token —— 它可能是值，也可能是输出文件。
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-zzz-unknown"), QStringLiteral("out.mp4")});
    bool found_unresolved = false;
    for (const auto& token : result.tokens) {
        if (token.role == FfmpegTokenRole::Unresolved) {
            found_unresolved = true;
            EXPECT_TRUE(token.warning.isEmpty());
            EXPECT_TRUE(!token.description.isEmpty());
        }
    }
    EXPECT_TRUE(found_unresolved);
    EXPECT_FALSE(result.structure_known);
}

TEST(FfmpegCommandExplainer, UnknownOptionIsNotInvented) {
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-zzz-unknown"), QStringLiteral("out.mp4")});
    bool found_unknown = false;
    for (const auto& token : result.tokens) {
        if (token.token == QStringLiteral("-zzz-unknown")) {
            found_unknown = true;
            EXPECT_EQ(token.role, FfmpegTokenRole::UnknownOption);
            EXPECT_FALSE(token.known);
            EXPECT_TRUE(token.description.contains(QStringLiteral("字典里没有收录")));
        }
    }
    EXPECT_TRUE(found_unknown);
}

TEST(FfmpegCommandExplainer, WarnsAboutStreamCopyConflict) {
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"), QStringLiteral("copy"),
         QStringLiteral("-crf"), QStringLiteral("20"), QStringLiteral("out.mp4")});
    bool warned = false;
    for (const QString& note : result.notes) {
        if (note.contains(QStringLiteral("copy"))) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(FfmpegCommandExplainer, NotesMissingOutput) {
    const auto result = FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4")});
    EXPECT_FALSE(result.has_output);
    EXPECT_FALSE(result.notes.empty());
}

TEST(FfmpegCommandExplainer, DoesNotClaimNoOutputWhenStructureIsUnknown) {
    // 未收录选项后面那个 token 被标成「未解析」。这时不能断言"没有输出文件":
    // 那个归属不明的 token 也可能就是输出文件，断言越肯定越会把人带偏。
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-zzz-unknown"), QStringLiteral("keyint=50")});

    EXPECT_FALSE(result.structure_known);
    for (const QString& note : result.notes) {
        EXPECT_FALSE(note.contains(QStringLiteral("没有输出文件"))) << note.toStdString();
    }
    bool hedged = false;
    for (const QString& note : result.notes) {
        if (note.contains(QStringLiteral("归属不明"))) {
            hedged = true;
        }
    }
    EXPECT_TRUE(hedged);
}

TEST(FfmpegCommandExplainer, InformationCommandsAreNotToldTheyLackIo) {
    // -version 这类命令天生没有输入也没有输出，挑它"缺输入/缺输出"是错的
    for (const QString& option : {QStringLiteral("-version"), QStringLiteral("-encoders"), QStringLiteral("-formats"),
                                  QStringLiteral("-filters"), QStringLiteral("-h")}) {
        const auto result = FfmpegCommandExplainer::Explain({option});
        for (const QString& note : result.notes) {
            EXPECT_FALSE(note.contains(QStringLiteral("没有 -i"))) << option.toStdString();
            EXPECT_FALSE(note.contains(QStringLiteral("没有输出文件"))) << option.toStdString();
        }
        EXPECT_TRUE(result.flow.contains(QStringLiteral("查询"))) << option.toStdString();
        EXPECT_FALSE(result.notes.empty());
    }
    // 而普通的转码命令仍然要照常提醒缺输出
    const auto transcode = FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4")});
    bool warned = false;
    for (const QString& note : transcode.notes) {
        if (note.contains(QStringLiteral("没有输出文件"))) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(FfmpegCommandExplainer, WarnsThatNostdinCannotBeStoppedGracefully) {
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-nostdin"), QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                         QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("out.mp4")});
    bool warned = false;
    for (const QString& note : result.notes) {
        if (note.contains(QStringLiteral("-nostdin"))) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(FfmpegCommandExplainer, EmptyCommandIsSafe) {
    const auto result = FfmpegCommandExplainer::Explain({});
    EXPECT_TRUE(result.tokens.empty());
    EXPECT_FALSE(result.notes.empty());
}

// ===================== 程序定位（"用户没装 ffmpeg"场景）=====================
//
// 这里不碰 QProcess，只验证: 找不到时给出的状态是否自洽、安装指引是否按平台给出
// 可执行的补救步骤。这是终端用户打开本页最可能撞上的第一个状态。

TEST(FfmpegToolLocator, EmptyAndBogusPathsAreNotExecutable) {
    EXPECT_FALSE(FfmpegToolLocator::IsExecutable(QString()));
    EXPECT_FALSE(FfmpegToolLocator::IsExecutable(QStringLiteral("/definitely/not/here/ffmpeg")));
    EXPECT_TRUE(FfmpegToolLocator::FindInDirectory(QString()).isEmpty());
    EXPECT_TRUE(FfmpegToolLocator::FindInDirectory(QStringLiteral("/definitely/not/here")).isEmpty());
}

TEST(FfmpegToolLocator, UserSettingWinsEvenWhenMissing) {
    // 用户填了路径就必须原样返回 —— 界面要靠它显示"你填的这个文件不存在"
    const FfmpegToolInfo info = FfmpegToolLocator::Resolve(QStringLiteral("/definitely/not/here/ffmpeg"));
    EXPECT_EQ(info.origin.toStdString(), std::string("用户设置"));
    EXPECT_EQ(info.path.toStdString(), std::string("/definitely/not/here/ffmpeg"));
    EXPECT_FALSE(info.exists);
}

TEST(FfmpegToolLocator, MissingToolIsReportedAsMissing) {
    const FfmpegToolInfo info = FfmpegToolLocator::Resolve(QString());
    if (info.exists) {
        // 这台机器装了 ffmpeg: 至少要能说清它从哪来
        EXPECT_TRUE(info.origin == QStringLiteral("随包分发") || info.origin == QStringLiteral("构建期探测") ||
                    info.origin == QStringLiteral("PATH") || info.origin == QStringLiteral("常见安装位置"));
    } else {
        EXPECT_EQ(info.origin.toStdString(), std::string("未找到"));
    }
}

TEST(FfmpegToolLocator, FindsEachToolUnderItsOwnName) {
    // 一个目录里只有 ffprobe.exe：找 ffprobe 要命中，找 ffmpeg 不能张冠李戴。
    // Windows 上 IsExecutable 只按后缀认，所以这里用空文件就能造出"可执行程序"。
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString probe_path = dir.filePath(QStringLiteral("ffprobe.exe"));
    QFile probe_file(probe_path);
    ASSERT_TRUE(probe_file.open(QIODevice::WriteOnly));
    probe_file.close();

    const QString found_probe = FfmpegToolLocator::FindInDirectory(dir.path(), FfmpegToolKind::Ffprobe);
    EXPECT_FALSE(found_probe.isEmpty());
    EXPECT_TRUE(found_probe.endsWith(QStringLiteral("ffprobe.exe"))) << found_probe.toStdString();

    // 默认重载找的是 ffmpeg —— 这个目录里没有它
    EXPECT_TRUE(FfmpegToolLocator::FindInDirectory(dir.path()).isEmpty());
    EXPECT_TRUE(FfmpegToolLocator::FindInDirectory(dir.path(), FfmpegToolKind::Ffplay).isEmpty());
}

TEST(FfmpegToolLocator, UnknownToolResolvesToNothing) {
    // Unknown 没有可找的东西：不能顺手退回 ffmpeg，否则界面会显示"找到了"却跑错程序
    const FfmpegToolInfo info = FfmpegToolLocator::Resolve(QString(), FfmpegToolKind::Unknown);
    EXPECT_FALSE(info.exists);
    EXPECT_TRUE(info.path.isEmpty());
}

TEST(FfmpegToolLocator, ParsesVersionLineForEveryTool) {
    EXPECT_EQ(FfmpegToolLocator::ParseVersionLine(
                  QStringLiteral("ffmpeg version 8.1.2-full_build-www.gyan.dev Copyright (c) 2000-2025")),
              QStringLiteral("8.1.2-full_build-www.gyan.dev"));
    EXPECT_EQ(
        FfmpegToolLocator::ParseVersionLine(QStringLiteral("ffprobe version 6.1.1-3ubuntu5 Copyright (c) 2007-2023")),
        QStringLiteral("6.1.1-3ubuntu5"));
    EXPECT_EQ(FfmpegToolLocator::ParseVersionLine(QStringLiteral("ffplay version 7.0 Copyright (c) 2003-2024")),
              QStringLiteral("7.0"));
}

TEST(FfmpegToolLocator, InstallGuideIsActionable) {
    const FfmpegInstallGuide guide = FfmpegToolLocator::InstallGuide();
    EXPECT_FALSE(guide.platform.isEmpty());
    EXPECT_FALSE(guide.headline.isEmpty());
    EXPECT_FALSE(guide.package_commands.isEmpty());
    EXPECT_FALSE(guide.manual_steps.isEmpty());
    EXPECT_TRUE(guide.download_url.startsWith(QStringLiteral("https://")));

    // 指引必须点明"只有这一页需要它"，否则用户会以为整个软件都废了
    EXPECT_FALSE(guide.note.isEmpty());
}

TEST(FfmpegToolLocator, CommonSearchDirsCoverPackageManagers) {
    const QStringList dirs = FfmpegToolLocator::CommonSearchDirs();
    EXPECT_FALSE(dirs.isEmpty());
#ifdef Q_OS_WIN
    EXPECT_TRUE(dirs.contains(QStringLiteral("%LOCALAPPDATA%\\Programs\\ffmpeg\\bin")));
    EXPECT_TRUE(dirs.contains(QStringLiteral("%ProgramData%\\chocolatey\\bin")));
    EXPECT_TRUE(dirs.contains(QStringLiteral("%USERPROFILE%\\scoop\\apps\\ffmpeg\\current\\bin")));
#elif defined(Q_OS_MACOS)
    EXPECT_TRUE(dirs.contains(QStringLiteral("/opt/homebrew/bin")));
    EXPECT_TRUE(dirs.contains(QStringLiteral("/usr/local/bin")));
#else
    EXPECT_TRUE(dirs.contains(QStringLiteral("/usr/bin")));
    EXPECT_TRUE(dirs.contains(QStringLiteral("/snap/bin")));
#endif
}

// ===================== 流拷贝判定：按选项-值配对，不做整串搜索 =====================
//
// copy 只作用于它自己那条流。用 arguments.contains("copy") 这种整串判断，
// `-c:v libx264 -c:a copy` 会被误判成"视频也拷贝了"，而 `-c copy` 又彻底漏报。

bool HasNoteContaining(const videoeye::ffmpeg::FfmpegCommandExplanation& result, const QString& needle) {
    for (const QString& note : result.notes) {
        if (note.contains(needle)) {
            return true;
        }
    }
    return false;
}

TEST(FfmpegCommandExplainer, GlobalOrPerStreamCopyIsResolvedPerStream) {
    // -c:v libx264 -c:a copy -crf 20: copy 属于音频，视频仍在重编码 → -crf 有效。
    // 以前会误报"-c:v copy 表示不重新编码，-crf 不会生效"。
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"),
                                         QStringLiteral("libx264"), QStringLiteral("-c:a"), QStringLiteral("copy"),
                                         QStringLiteral("-crf"), QStringLiteral("20"), QStringLiteral("out.mp4")});
    EXPECT_FALSE(HasNoteContaining(result, QStringLiteral("不会生效")));
}

TEST(FfmpegCommandExplainer, StreamCopyWarnsThatReencodeParamsAreIgnored) {
    // -c copy -crf 20: 以前一句不说（漏报），ffmpeg 会静默忽略 -crf。
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c"), QStringLiteral("copy"),
         QStringLiteral("-crf"), QStringLiteral("20"), QStringLiteral("out.mp4")});
    EXPECT_TRUE(HasNoteContaining(result, QStringLiteral("-crf")));
    EXPECT_TRUE(HasNoteContaining(result, QStringLiteral("不会生效")));
    // 已经指出参数无效，就不能同时说"这是一次干净的纯流拷贝"
    EXPECT_FALSE(HasNoteContaining(result, QStringLiteral("这是一次纯流拷贝")));
}

TEST(FfmpegCommandExplainer, StreamCopyPlusFilterIsReportedAsFatal) {
    // 实测 ffmpeg: "Filtering and streamcopy cannot be used together" → 直接退出，
    // 不是静默忽略。文案必须区分这两种后果。
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"), QStringLiteral("copy"),
         QStringLiteral("-vf"), QStringLiteral("scale=640:-2"), QStringLiteral("out.mp4")});
    EXPECT_TRUE(HasNoteContaining(result, QStringLiteral("Filtering and streamcopy")));
}

TEST(FfmpegCommandExplainer, PureStreamCopyIsExplainedExactlyOnce) {
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mkv"), QStringLiteral("-c"),
                                         QStringLiteral("copy"), QStringLiteral("out.mp4")});
    EXPECT_TRUE(HasNoteContaining(result, QStringLiteral("纯流拷贝")));
    EXPECT_FALSE(HasNoteContaining(result, QStringLiteral("不会生效")));
}

TEST(FfmpegCommandExplainer, PerStreamCodecOverridesGlobalCopy) {
    // -c copy -c:a aac：音频被重新编码，-b:a 有效 —— 后写的按流选项覆盖全局 copy。
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c"),
                                         QStringLiteral("copy"), QStringLiteral("-c:a"), QStringLiteral("aac"),
                                         QStringLiteral("-b:a"), QStringLiteral("128k"), QStringLiteral("out.mp4")});
    EXPECT_FALSE(HasNoteContaining(result, QStringLiteral("音频流不重新编码")));
    EXPECT_FALSE(HasNoteContaining(result, QStringLiteral("纯流拷贝")));
}

TEST(FfmpegCommandExplainer, OldCodecAliasesAreUnderstood) {
    // -vcodec / -acodec / -codec 是老教程里的主力写法，不能被标成"未知参数"
    // （那还会让后面的值变成归属不明，整条命令的结构判定失效）。
    const auto result =
        FfmpegCommandExplainer::Explain({QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-vcodec"),
                                         QStringLiteral("copy"), QStringLiteral("-acodec"), QStringLiteral("aac"),
                                         QStringLiteral("-ar"), QStringLiteral("44100"), QStringLiteral("out.mp4")});
    ASSERT_FALSE(result.tokens.empty());
    for (const auto& token : result.tokens) {
        EXPECT_NE(token.role, FfmpegTokenRole::UnknownOption) << token.token.toStdString();
    }
    // 视频 copy、音频重编码 + -ar 有效 → 没有任何"参数不会生效"
    EXPECT_FALSE(HasNoteContaining(result, QStringLiteral("不会生效")));
}

TEST(FfmpegCommandExplainer, InformationOptionsAreNotCalledUnknown) {
    // 查询命令列表里把 -encoders 标成「未知参数」，和下面"这是一条查询命令"自相矛盾。
    const auto result = FfmpegCommandExplainer::Explain({QStringLiteral("-encoders")});
    ASSERT_FALSE(result.tokens.empty());
    EXPECT_TRUE(result.tokens.front().known);
    EXPECT_NE(result.tokens.front().role, FfmpegTokenRole::UnknownOption);
}

TEST(FfmpegCommandExplainer, ExplainsFfprobeWithItsOwnStructure) {
    const auto result = FfmpegCommandExplainer::Explain({QStringLiteral("-show_streams"), QStringLiteral("in.mp4")},
                                                        FfmpegToolKind::Ffprobe);

    EXPECT_TRUE(result.has_input);
    EXPECT_EQ(result.input_count, 1);
    EXPECT_FALSE(result.has_output);
    // 关键：不能在提醒里说"没有输出文件" —— ffprobe 本来就不产出文件
    for (const QString& note : result.notes) {
        EXPECT_FALSE(note.contains(QStringLiteral("没有输出文件"))) << note.toStdString();
    }
    ASSERT_EQ(result.tokens.size(), 2u);
    EXPECT_EQ(result.tokens.at(0).role, FfmpegTokenRole::KnownOption);
    EXPECT_EQ(result.tokens.at(0).role_label, QStringLiteral("显示流信息"));
    EXPECT_EQ(result.tokens.at(1).role, FfmpegTokenRole::InputFile);
}

TEST(FfmpegCommandExplainer, FfplayYIsExplainedAsWindowHeight) {
    // 同名不同义：按 ffplay 解释 -y 必须说"窗口高度"，不能沿用 ffmpeg 的"覆盖输出"
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-y"), QStringLiteral("720"), QStringLiteral("in.mp4")}, FfmpegToolKind::Ffplay);
    ASSERT_FALSE(result.tokens.empty());
    EXPECT_EQ(result.tokens.front().role_label, QStringLiteral("窗口高度（ffplay）"));
    EXPECT_EQ(result.tokens.front().value, QStringLiteral("720"));
}

TEST(FfmpegCommandExplainer, FfprobeDoesNotBorrowFfmpegEncodingOptions) {
    // -crf 是 ffmpeg 的输出编码选项，ffprobe 里没有 —— 必须如实说"不认识"，
    // 而不是把 ffmpeg 的词条解释贴上来。
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-crf"), QStringLiteral("23"), QStringLiteral("in.mp4")}, FfmpegToolKind::Ffprobe);
    ASSERT_FALSE(result.tokens.empty());
    EXPECT_EQ(result.tokens.front().role, FfmpegTokenRole::UnknownOption);
}

TEST(FfmpegCommandCatalog, CoversStreamCopyAndMuxOptions) {
    // 这些选项在真实命令里很常见，但以前没收录 —— 一条命令里出现一个，
    // 整条命令就被判成"结构无法可靠判断"，值全显示「未解析」。
    const QStringList names = {
        QStringLiteral("-vcodec"),
        QStringLiteral("-acodec"),
        QStringLiteral("-codec"),
        QStringLiteral("-c:s"),
        QStringLiteral("-c:d"),
        QStringLiteral("-bsf:v"),
        QStringLiteral("-x264-params"),
        QStringLiteral("-fflags"),
        QStringLiteral("-err_detect"),
        QStringLiteral("-max_muxing_queue_size"),
        QStringLiteral("-avoid_negative_ts"),
        QStringLiteral("-vsync"),
        QStringLiteral("-tag:v"),
        QStringLiteral("-segment_time"),
        QStringLiteral("-hls_playlist_type"),
        QStringLiteral("-hls_segment_filename"),
        QStringLiteral("-dn"),
    };
    for (const QString& name : names) {
        const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(name);
        ASSERT_TRUE(entry != nullptr) << name.toStdString();
        EXPECT_FALSE(entry->summary.isEmpty()) << name.toStdString();
    }
}

// ===================== 命令草稿：插入片段 / 默认输出名 =====================
//
// 这两条规则以前散在 UI 里（`command_edit_->insert(snippet)` + 写死 "_out.mp4"），
// 于是"片段粘到上一个 token 上""-i "" 插完光标在引号外""mkv 素材默认出 mp4"三个问题
// 都没人管。抽出来之后就能在这里把边界钉住。

TEST(FfmpegCommandDraft, InsertionSeparatesAdjacentTokens) {
    const QString text = QStringLiteral("ffmpeg -i in.mp4");
    const auto result = ComposeInsertion(text, text.size(), QStringLiteral("-c:v"));
    // 以前直接 insert 会得到 "in.mp4-c:v"
    EXPECT_FALSE(result.text.contains(QStringLiteral("in.mp4-c:v")));
    EXPECT_TRUE(result.text.startsWith(QStringLiteral("ffmpeg -i in.mp4 -c:v")));
    // 光标停在片段之后、补的那个空格之前 —— 用户接着打的就是这个选项的值
    EXPECT_EQ(QStringLiteral(" "), result.text.mid(result.cursor, 1));
}

TEST(FfmpegCommandDraft, InsertionKeepsCursorInsideEmptyQuotes) {
    // `-i ""`：光标必须落在两个引号之间，否则用户一输入路径就写到引号外面
    const QString text = QStringLiteral("ffmpeg ");
    const auto result = ComposeInsertion(text, text.size(), QStringLiteral("-i \"\""));
    ASSERT_GT(result.cursor, 0);
    ASSERT_LT(result.cursor, result.text.size());
    EXPECT_EQ(QChar('"'), result.text.at(result.cursor - 1));
    EXPECT_EQ(QChar('"'), result.text.at(result.cursor));
}

TEST(FfmpegCommandDraft, InsertionInMiddleKeepsFollowingTokenSeparated) {
    const QString text = QStringLiteral("ffmpeg -crf 20 out.mp4");
    const auto result = ComposeInsertion(text, 7, QStringLiteral("-i"));
    EXPECT_EQ(QStringLiteral("ffmpeg -i -crf 20 out.mp4"), result.text);
}

TEST(FfmpegCommandDraft, InsertionIntoEmptyTextEndsWithOneSpace) {
    const auto result = ComposeInsertion(QString(), 0, QStringLiteral("-version"));
    EXPECT_EQ(QStringLiteral("-version "), result.text);
    EXPECT_EQ(8, result.cursor);
}

TEST(FfmpegCommandDraft, DefaultOutputPathKeepsSourceExtension) {
    // 写死 _out.mp4 会悄悄换容器：mkv 里的多音轨/字幕塞进 mp4 往往直接失败
    EXPECT_EQ(QStringLiteral("D:/m/in_out.mkv"), DefaultOutputPath(QStringLiteral("D:/m/in.mkv")));
    EXPECT_EQ(QStringLiteral("D:/m/a.b_out.mp4"), DefaultOutputPath(QStringLiteral("D:/m/a.b.mp4")));
    // 没有扩展名 → 退回 mp4
    EXPECT_EQ(QStringLiteral("D:/m/noext_out.mp4"), DefaultOutputPath(QStringLiteral("D:/m/noext")));
}

TEST(FfmpegCommandDraft, DefaultOutputPathIgnoresStreams) {
    // 网络流 / 设备没有"同目录"可言；给个看起来像本地路径的默认值只会误导
    EXPECT_TRUE(DefaultOutputPath(QStringLiteral("rtmp://host/live")).isEmpty());
    EXPECT_TRUE(DefaultOutputPath(QString()).isEmpty());
}

// ===================== 命令历史 =====================

TEST(FfmpegCommandHistory, PreviousWalksFromNewestAndNextReturnsDraft) {
    FfmpegCommandHistory history;
    history.Push(QStringLiteral("ffmpeg -i a.mp4 b.mp4"));
    history.Push(QStringLiteral("ffmpeg -i c.mp4 d.mp4"));

    // 最新在前
    EXPECT_EQ(QStringLiteral("ffmpeg -i c.mp4 d.mp4"), history.Entries().front());
    EXPECT_EQ(QStringLiteral("ffmpeg -i c.mp4 d.mp4"), history.Previous(QStringLiteral("半截输入")));
    EXPECT_EQ(QStringLiteral("ffmpeg -i a.mp4 b.mp4"), history.Previous(QStringLiteral("半截输入")));
    // 翻到最早那条就停住，不会转圈
    EXPECT_EQ(QStringLiteral("ffmpeg -i a.mp4 b.mp4"), history.Previous(QStringLiteral("半截输入")));

    EXPECT_EQ(QStringLiteral("ffmpeg -i c.mp4 d.mp4"), history.Next());
    // 越过最新一条 → 回到用户原来输入的内容
    EXPECT_EQ(QStringLiteral("半截输入"), history.Next());
    EXPECT_FALSE(history.browsing());
}

TEST(FfmpegCommandHistory, AdjacentDuplicateIsNotRecordedTwice) {
    FfmpegCommandHistory history;
    EXPECT_TRUE(history.Push(QStringLiteral("ffmpeg -i a.mp4 b.mp4")));
    // 同一条件又跑一次是常态，记两次只会让用户多按一下 ↑
    EXPECT_FALSE(history.Push(QStringLiteral("  ffmpeg -i a.mp4 b.mp4  ")));
    EXPECT_EQ(1, history.Entries().size());
    EXPECT_FALSE(history.Push(QStringLiteral("   ")));
    EXPECT_EQ(1, history.Entries().size());
}

TEST(FfmpegCommandHistory, EntriesAreCapped) {
    FfmpegCommandHistory history;
    for (int i = 0; i < FfmpegCommandHistory::kMaxEntries + 12; ++i) {
        history.Push(QStringLiteral("ffmpeg %1").arg(i));
    }
    EXPECT_EQ(FfmpegCommandHistory::kMaxEntries, history.Entries().size());
    // 留下的是最近的那些
    EXPECT_EQ(QStringLiteral("ffmpeg %1").arg(FfmpegCommandHistory::kMaxEntries + 11), history.Entries().front());
}

// ===================== 进度解析 =====================
//
// 取样自真实 stderr（本机 ffmpeg 7.0.2）：
//   Duration: 00:00:10.00, start: 0.000000, bitrate: 2456 kb/s
//   frame=  300 fps= 60 q=28.0 size=... time=00:00:10.00 bitrate=...

TEST(FfmpegProgressTracker, DurationAndTimeLineProducePercent) {
    FfmpegProgressTracker tracker;
    EXPECT_TRUE(tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 2456 kb/s")));
    EXPECT_TRUE(tracker.duration_known());
    EXPECT_EQ(QStringLiteral("00:10"), tracker.duration_text());

    EXPECT_TRUE(tracker.Feed(QStringLiteral("frame=  120 fps= 60 q=28.0 size=512kB time=00:00:04.00 bitrate=...")));
    EXPECT_NEAR(40.0, tracker.percent(), 0.001);
    EXPECT_EQ(QStringLiteral("00:04"), tracker.position_text());
}

TEST(FfmpegProgressTracker, WithoutDurationThereIsNoPercent) {
    // 流式输入打的是 "Duration: N/A"，绝不能编一个百分比出来
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: N/A, start: 0.000000, bitrate: N/A"));
    tracker.Feed(QStringLiteral("frame=   30 fps= 30 time=00:00:01.00 bitrate=..."));
    EXPECT_FALSE(tracker.duration_known());
    EXPECT_LT(tracker.percent(), 0.0);
    EXPECT_EQ(QStringLiteral("00:01"), tracker.position_text());
}

TEST(FfmpegProgressTracker, NegativeFirstTimestampClampsToZero) {
    // ffmpeg 开头偶尔吐 time=-00:00:00.02（第一帧时间戳为负）
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=    0 fps=0.0 q=0.0 size=0kB time=-00:00:00.02 bitrate=N/A"));
    EXPECT_EQ(0, tracker.position_ms());
    EXPECT_NEAR(0.0, tracker.percent(), 0.001);
}

TEST(FfmpegProgressTracker, OnlyFirstDurationIsUsed) {
    // 多输入（-i a -i b）后面那个 Duration 不是这次的总量
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("  Duration: 01:00:00.00, start: 0.000000, bitrate: 1 kb/s"));
    EXPECT_EQ(10000, tracker.duration_ms());
}

TEST(FfmpegProgressTracker, PositionIsClampedToHundredPercent) {
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=1 time=00:00:12.00 bitrate=..."));
    EXPECT_NEAR(100.0, tracker.percent(), 0.001);
}

TEST(FfmpegProgressTracker, DurationNotRetriedAfterNA) {
    // 第一个 Duration 是 N/A（流式输入）时，第二个输入的 Duration 不能顶上来当分母：
    // position 是整体输出的位置，两个口径对不上，宁可不给百分比。
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: N/A, start: 0.000000, bitrate: N/A"));
    tracker.Feed(QStringLiteral("  Duration: 01:00:00.00, start: 0.000000, bitrate: 1 kb/s"));
    EXPECT_FALSE(tracker.duration_known());
    EXPECT_LT(tracker.percent(), 0.0);
}

// 本机 ffmpeg 6.1 实测原样（-preset slow 转 30 秒 720p）：
//   frame=  579 fps=128 q=20.0 size=   14080kB time=00:00:19.23 bitrate=5997.1kbits/s speed=4.24x
TEST(FfmpegProgressTracker, SpeedSizeAndFpsFromRealProgressLine) {
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:30.00, start: 0.000000, bitrate: 5997 kb/s"));
    tracker.Feed(QStringLiteral(
        "frame=  579 fps=128 q=20.0 size=   14080kB time=00:00:19.23 bitrate=5997.1kbits/s speed=4.24x    "));

    EXPECT_NEAR(128.0, tracker.fps(), 0.001);
    EXPECT_EQ(14080000, tracker.size_bytes()); // kB 是十进制（1000），不是 KiB
    EXPECT_NEAR(4.24, tracker.speed(), 0.001);
    EXPECT_NEAR(64.1, tracker.percent(), 0.01);

    // 剩余媒体时长 30 - 19.23 = 10.77 秒，按 4.24 倍速约 2.54 秒
    EXPECT_EQ(2540, tracker.eta_ms());
}

TEST(FfmpegProgressTracker, SpeedAveragesRecentSamples) {
    // 收尾时 speed 会突然飙高，单点采样算出来的剩余时间会跳；取窗口平均压一压
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=1 fps=25 time=00:00:01.00 speed=2.0x"));
    tracker.Feed(QStringLiteral("frame=2 fps=25 time=00:00:02.00 speed=6.0x"));
    EXPECT_NEAR(4.0, tracker.speed(), 0.001);
}

TEST(FfmpegProgressTracker, FirstLinePlaceholdersAreNotFacts) {
    // 开头那行是占位：fps=0.0 / size=0kB / speed=N/A / time=N/A 都不能当真值
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=    0 fps=0.0 q=0.0 size=       0kB time=N/A bitrate=N/A speed=N/A"));
    EXPECT_LT(tracker.fps(), 0.0);
    EXPECT_LT(tracker.size_bytes(), 0);
    EXPECT_LT(tracker.speed(), 0.0);
    EXPECT_LT(tracker.eta_ms(), 0);
}

TEST(FfmpegProgressTracker, EtaDividesRemainingBySpeed) {
    // 剩余媒体时长要除以处理速度才是墙钟时间：剩 10 秒、2 倍速 → 约 5 秒
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:20.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=1 fps=25 time=00:00:10.00 speed=2.0x"));
    EXPECT_EQ(5000, tracker.eta_ms());
}

TEST(FfmpegProgressTracker, EtaUnknownBeforePositionMoves) {
    // position 还是 0 时算出来的是"满额剩余"，没有参考价值
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=0 fps=25 size=0kB time=00:00:00.00 speed=1.0x"));
    EXPECT_LT(tracker.eta_ms(), 0);
}

TEST(FfmpegProgressTracker, SizeUnitsFollowSuffix) {
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("frame=1 size=  1.5MiB time=00:00:01.00 speed=1.0x"));
    EXPECT_EQ(1572864, tracker.size_bytes()); // 1.5 * 1024 * 1024
}

TEST(FfmpegProgressTracker, ResetClearsDetailFields) {
    FfmpegProgressTracker tracker;
    tracker.Feed(QStringLiteral("  Duration: 00:00:10.00, start: 0.000000, bitrate: 1 kb/s"));
    tracker.Feed(QStringLiteral("frame=10 fps=25 size=512kB time=00:00:01.00 speed=2.0x"));
    tracker.Reset();

    EXPECT_FALSE(tracker.duration_known());
    EXPECT_LT(tracker.percent(), 0.0);
    EXPECT_LT(tracker.speed(), 0.0);
    EXPECT_LT(tracker.fps(), 0.0);
    EXPECT_LT(tracker.size_bytes(), 0);
    EXPECT_LT(tracker.eta_ms(), 0);
    // Reset 之后要能重新吃下一条命令的 Duration
    tracker.Feed(QStringLiteral("  Duration: 00:00:04.00, start: 0.000000, bitrate: 1 kb/s"));
    EXPECT_EQ(4000, tracker.duration_ms());
}

TEST(FfmpegProgressTracker, SummaryShowsSpeedAndRemaining) {
    FfmpegProgressSnapshot snapshot;
    snapshot.percent = 64.1;
    snapshot.position_ms = 19230;
    snapshot.duration_ms = 30000;
    snapshot.speed = 4.24;
    snapshot.eta_ms = 2540;

    const QString text = FormatProgressSummary(snapshot);
    EXPECT_TRUE(text.contains(QStringLiteral("64.1%"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("00:19 / 00:30"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("4.2×"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("约剩 00:02"))) << text.toStdString();
}

TEST(FfmpegProgressTracker, SummaryWithoutDurationSkipsPercentAndEta) {
    FfmpegProgressSnapshot snapshot;
    snapshot.position_ms = 7000;
    snapshot.speed = 2.0;

    const QString text = FormatProgressSummary(snapshot);
    EXPECT_TRUE(text.contains(QStringLiteral("总时长未知"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("2.0×"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QLatin1Char('%'))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("约剩"))) << text.toStdString();
}

TEST(FfmpegProgressTracker, DetailOmitsUnknownFields) {
    FfmpegProgressSnapshot snapshot;
    snapshot.position_ms = 7000;

    const QString text = FormatProgressDetail(snapshot);
    EXPECT_TRUE(text.contains(QStringLiteral("总时长未知"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("速度"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("帧率"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("已写入"))) << text.toStdString();
}

TEST(FfmpegProgressTracker, ByteSizeFormatting) {
    EXPECT_EQ(QStringLiteral("512 B"), FormatByteSize(512));
    EXPECT_EQ(QStringLiteral("1.0 KiB"), FormatByteSize(1024));
    EXPECT_EQ(QStringLiteral("14.1 MiB"), FormatByteSize(14800000));
    EXPECT_EQ(QString(), FormatByteSize(-1)); // 未知不编成 0 B
}

TEST(FfmpegProgressTracker, ClockFormatSwitchesAtOneHour) {
    EXPECT_EQ(QStringLiteral("00:05"), FormatProgressClock(5000));
    EXPECT_EQ(QStringLiteral("59:59"), FormatProgressClock(59 * 60 * 1000 + 59000));
    EXPECT_EQ(QStringLiteral("1:00:00"), FormatProgressClock(3600 * 1000));
    EXPECT_EQ(QStringLiteral("00:00"), FormatProgressClock(-5)); // 负值按 0
}

} // namespace
