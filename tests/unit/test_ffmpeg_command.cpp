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

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegToolLocator.h"

namespace {

using videoeye::ffmpegtool::AnalyzeCommandStructure;
using videoeye::ffmpegtool::CommandParseStatus;
using videoeye::ffmpegtool::DetectStdoutMediaOutput;
using videoeye::ffmpegtool::FfmpegArgRole;
using videoeye::ffmpegtool::FfmpegCapabilityCache;
using videoeye::ffmpegtool::FfmpegCatalogEntry;
using videoeye::ffmpegtool::FfmpegCommandCatalog;
using videoeye::ffmpegtool::FfmpegCommandExplainer;
using videoeye::ffmpegtool::FfmpegEntryKind;
using videoeye::ffmpegtool::FfmpegFormatLists;
using videoeye::ffmpegtool::FfmpegInstallGuide;
using videoeye::ffmpegtool::FfmpegTokenRole;
using videoeye::ffmpegtool::FfmpegToolInfo;
using videoeye::ffmpegtool::FfmpegToolLocator;
using videoeye::ffmpegtool::InformationOptionDescription;
using videoeye::ffmpegtool::IsExplicitProgramPath;
using videoeye::ffmpegtool::IsInformationCommand;
using videoeye::ffmpegtool::LooksLikeFfmpegProgram;
using videoeye::ffmpegtool::ParseCommandLine;

std::vector<std::string> ToStdList(const QStringList& list) {
    std::vector<std::string> out;
    out.reserve(list.size());
    for (const QString& item : list) {
        out.push_back(item.toStdString());
    }
    return out;
}

// ===================== 解析 =====================

TEST(FfmpegCommandParser, SplitsPlainCommand) {
    const auto parsed = ParseCommandLine(QStringLiteral("ffmpeg -i in.mp4 -c:v libx264 out.mp4"));
    ASSERT_EQ(parsed.status, CommandParseStatus::Ok);
    EXPECT_EQ(ToStdList(parsed.arguments),
              std::vector<std::string>({"-i", "in.mp4", "-c:v", "libx264", "out.mp4"}));
    EXPECT_EQ(parsed.program_token, QStringLiteral("ffmpeg"));
}

TEST(FfmpegCommandParser, KeepsQuotedPathWithSpaces) {
    const auto parsed = ParseCommandLine(
        QStringLiteral("ffmpeg -i \"D:/my media/源 文件.mp4\" -c copy \"out file.mp4\""));
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
             QStringLiteral("-version"), QStringLiteral("-buildconf"), QStringLiteral("-formats"),
             QStringLiteral("-demuxers"), QStringLiteral("-muxers"), QStringLiteral("-encoders"),
             QStringLiteral("-decoders"), QStringLiteral("-filters"), QStringLiteral("-pix_fmts"),
             QStringLiteral("-layouts"), QStringLiteral("-sample_fmts"), QStringLiteral("-protocols"),
             QStringLiteral("-devices"), QStringLiteral("-hwaccels"), QStringLiteral("-bsfs"),
             QStringLiteral("-h"),
         }) {
        EXPECT_TRUE(IsInformationCommand({option})) << option.toStdString();
        EXPECT_FALSE(InformationOptionDescription(option).isEmpty()) << option.toStdString();
    }
    // 转码命令不能因为参数里恰好有个 -h 之类就被当成查询命令
    EXPECT_FALSE(IsInformationCommand({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                       QStringLiteral("out.mp4")}));
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
    const auto other = ParseCommandLine(QStringLiteral("ffprobe -i in.mp4"));
    EXPECT_EQ(other.status, CommandParseStatus::UnsupportedProgram);

    const auto empty = ParseCommandLine(QStringLiteral("   "));
    EXPECT_EQ(empty.status, CommandParseStatus::Empty);
}

TEST(FfmpegCommandParser, DetectsPipeOutput) {
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                       QStringLiteral("pipe:1")}),
              QStringLiteral("pipe:1"));
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                       QStringLiteral("-f"), QStringLiteral("matroska"),
                                       QStringLiteral("-")}),
              QStringLiteral("-"));
    // -f null - 是"只解码不出片"的基准测试写法，不往 stdout 写数据，不能误判
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                         QStringLiteral("-f"), QStringLiteral("null"),
                                         QStringLiteral("-")})
                    .isEmpty());
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                         QStringLiteral("-c"), QStringLiteral("copy"),
                                         QStringLiteral("out.mp4")})
                    .isEmpty());
    // 只产出文本的 muxer 写到管道上也只是几行字，不必拦
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                         QStringLiteral("-f"), QStringLiteral("md5"),
                                         QStringLiteral("-")})
                    .isEmpty());
}

TEST(FfmpegCommandParser, DetectsPipeWriteToStderr) {
    // pipe:2 写的是标准错误，一样会把二进制灌进日志区
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                       QStringLiteral("-c:v"), QStringLiteral("copy"),
                                       QStringLiteral("pipe:2")}),
              QStringLiteral("pipe:2"));
}

TEST(FfmpegCommandParser, DetectsLaterPipeOutputInMultiOutputCommand) {
    // 这正是"-f null 被记成全命令状态"漏掉的情形: 前一个输出是基准测试用的 -f null -，
    // 后一个输出才是真的往通道写 —— 必须各自按自己生效的 -f 判断。
    EXPECT_EQ(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                       QStringLiteral("-f"), QStringLiteral("null"),
                                       QStringLiteral("-"),
                                       QStringLiteral("-c"), QStringLiteral("copy"),
                                       QStringLiteral("-f"), QStringLiteral("matroska"),
                                       QStringLiteral("pipe:2")}),
              QStringLiteral("pipe:2"));
    // 反过来：两个输出都是文件，不该因为命令里有管道名而误报
    EXPECT_TRUE(DetectStdoutMediaOutput({QStringLiteral("-i"), QStringLiteral("in.mp4"),
                                         QStringLiteral("-f"), QStringLiteral("null"),
                                         QStringLiteral("-"),
                                         QStringLiteral("-f"), QStringLiteral("mp4"),
                                         QStringLiteral("out.mp4")})
                    .isEmpty());
}

// ===================== 命令结构（输入组 / 输出组）=====================

TEST(FfmpegCommandStructure, SplitsInputsAndOutputs) {
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("a.mp4"), QStringLiteral("-i"), QStringLiteral("b.mp4"),
         QStringLiteral("-c:v"), QStringLiteral("copy"), QStringLiteral("out1.mp4"),
         QStringLiteral("out2.mkv")});
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
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-f"), QStringLiteral("null"),
         QStringLiteral("-"), QStringLiteral("-c"), QStringLiteral("copy"),
         QStringLiteral("-f"), QStringLiteral("matroska"), QStringLiteral("-")});
    ASSERT_EQ(structure.output_count, 2);
    EXPECT_EQ(structure.args.at(4).output_format, QStringLiteral("null"));
    EXPECT_EQ(structure.args.at(9).output_format, QStringLiteral("matroska"));
}

TEST(FfmpegCommandStructure, InputSideFormatIsNotTreatedAsOutputFormat) {
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
         QStringLiteral("testsrc"), QStringLiteral("out.mp4")});
    EXPECT_EQ(structure.input_count, 1);
    EXPECT_EQ(structure.output_count, 1);
    EXPECT_TRUE(structure.args.at(0).applies_to_input);
    EXPECT_TRUE(structure.args.at(4).output_format.isEmpty());
}

TEST(FfmpegCommandStructure, FormatAfterAnOutputStillBelongsToTheNextInput) {
    // 回归: 判断 -f 方向不能看"之前出过几个输出"。合法命令可以在已有输出之后
    // 继续添加输入组 —— 这里的 -f lavfi 依然是**输入**格式。
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("a.mp4"), QStringLiteral("out1.mp4"),
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
        {QStringLiteral("-i"), QStringLiteral("a.mp4"), QStringLiteral("out1.mp4"),
         QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
         QStringLiteral("testsrc"), QStringLiteral("-f"), QStringLiteral("mp4"),
         QStringLiteral("out2.mp4")});
    EXPECT_TRUE(with_output_format.args.at(3).applies_to_input);
    EXPECT_FALSE(with_output_format.args.at(7).applies_to_input);
    EXPECT_EQ(with_output_format.args.at(9).output_format, QStringLiteral("mp4"));
}

TEST(FfmpegCommandStructure, UnknownOptionLeavesNextTokenUnresolved) {
    // 字典没有这个选项 → 后面那个 token 是它的值还是输出文件，不能猜
    const auto structure = AnalyzeCommandStructure(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-x264-params"),
         QStringLiteral("out.mp4")});
    EXPECT_FALSE(structure.complete);
    EXPECT_EQ(structure.args.at(3).role, FfmpegArgRole::Uncertain);
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
        QStringLiteral("-i"),   QStringLiteral("-c:v"),  QStringLiteral("-c:a"),
        QStringLiteral("-crf"), QStringLiteral("-b:v"),  QStringLiteral("-b:a"),
        QStringLiteral("-preset"), QStringLiteral("-pix_fmt"), QStringLiteral("-vf"),
        QStringLiteral("-af"),  QStringLiteral("-map"),  QStringLiteral("-ss"),
        QStringLiteral("-t"),   QStringLiteral("-y"),    QStringLiteral("-movflags"),
        QStringLiteral("-filter_complex"), QStringLiteral("-g"), QStringLiteral("-ar"),
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
        QString(), FfmpegCommandCatalog::CategoryName(
                       videoeye::ffmpegtool::FfmpegEntryCategory::Encoding));
    ASSERT_FALSE(in_encoding.empty());
    for (const auto* entry : in_encoding) {
        EXPECT_EQ(FfmpegCommandCatalog::CategoryName(entry->category),
                  FfmpegCommandCatalog::CategoryName(
                      videoeye::ffmpegtool::FfmpegEntryCategory::Encoding));
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
    const QString formats = QStringLiteral(
        "File formats:\n"
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
    FfmpegCapabilityCache::Instance().SetFormats({QStringLiteral("lavfi")},
                                                 {QStringLiteral("ffmetadata")});
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
    const QString encoders = QStringLiteral(
        "Encoders:\n"
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
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"),
         QStringLiteral("libx264"), QStringLiteral("-crf"), QStringLiteral("23"),
         QStringLiteral("out.mp4")});

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

TEST(FfmpegCommandExplainer, RecognizesMultipleOutputs) {
    // 一条命令可以有两个输出：写在两个输出之间的参数归属后一个输出
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"),
         QStringLiteral("-s"), QStringLiteral("hd480"), QStringLiteral("out1.mp4"),
         QStringLiteral("-s"), QStringLiteral("hd720"), QStringLiteral("out2.mp4")});

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
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("out1.mp4"),
         QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("out2.mp4")});
    for (const auto& token : result.tokens) {
        if (token.token == QStringLiteral("out1.mp4") || token.token == QStringLiteral("out2.mp4")) {
            EXPECT_EQ(token.role, FfmpegTokenRole::OutputFile) << token.token.toStdString();
            EXPECT_TRUE(token.warning.isEmpty()) << token.token.toStdString();
        }
    }
}

TEST(FfmpegCommandExplainer, UnresolvedTokensCarryNoAdvice) {
    // 认不出来的结构必须说"未解析"，而不是给一条可能错的纠正建议
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-x264-params"),
         QStringLiteral("x"), QStringLiteral("out.mp4")});
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
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-x264-params"),
         QStringLiteral("out.mp4")});
    bool found_unknown = false;
    for (const auto& token : result.tokens) {
        if (token.token == QStringLiteral("-x264-params")) {
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
        {QStringLiteral("-i"), QStringLiteral("in.mp4"), QStringLiteral("-c:v"),
         QStringLiteral("copy"), QStringLiteral("-crf"), QStringLiteral("20"),
         QStringLiteral("out.mp4")});
    bool warned = false;
    for (const QString& note : result.notes) {
        if (note.contains(QStringLiteral("copy"))) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(FfmpegCommandExplainer, NotesMissingOutput) {
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4")});
    EXPECT_FALSE(result.has_output);
    EXPECT_FALSE(result.notes.empty());
}

TEST(FfmpegCommandExplainer, InformationCommandsAreNotToldTheyLackIo) {
    // -version 这类命令天生没有输入也没有输出，挑它"缺输入/缺输出"是错的
    for (const QString& option : {QStringLiteral("-version"), QStringLiteral("-encoders"),
                                  QStringLiteral("-formats"), QStringLiteral("-filters"),
                                  QStringLiteral("-h")}) {
        const auto result = FfmpegCommandExplainer::Explain({option});
        for (const QString& note : result.notes) {
            EXPECT_FALSE(note.contains(QStringLiteral("没有 -i"))) << option.toStdString();
            EXPECT_FALSE(note.contains(QStringLiteral("没有输出文件"))) << option.toStdString();
        }
        EXPECT_TRUE(result.flow.contains(QStringLiteral("查询"))) << option.toStdString();
        EXPECT_FALSE(result.notes.empty());
    }
    // 而普通的转码命令仍然要照常提醒缺输出
    const auto transcode = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-i"), QStringLiteral("in.mp4")});
    bool warned = false;
    for (const QString& note : transcode.notes) {
        if (note.contains(QStringLiteral("没有输出文件"))) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
}

TEST(FfmpegCommandExplainer, WarnsThatNostdinCannotBeStoppedGracefully) {
    const auto result = FfmpegCommandExplainer::Explain(
        {QStringLiteral("-nostdin"), QStringLiteral("-i"), QStringLiteral("in.mp4"),
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
    const FfmpegToolInfo info = FfmpegToolLocator::Resolve(
        QStringLiteral("/definitely/not/here/ffmpeg"));
    EXPECT_EQ(info.origin.toStdString(), std::string("用户设置"));
    EXPECT_EQ(info.path.toStdString(), std::string("/definitely/not/here/ffmpeg"));
    EXPECT_FALSE(info.exists);
}

TEST(FfmpegToolLocator, MissingToolIsReportedAsMissing) {
    const FfmpegToolInfo info = FfmpegToolLocator::Resolve(QString());
    if (info.exists) {
        // 这台机器装了 ffmpeg: 至少要能说清它从哪来
        EXPECT_TRUE(info.origin == QStringLiteral("随包分发") ||
                    info.origin == QStringLiteral("构建期探测") ||
                    info.origin == QStringLiteral("PATH") ||
                    info.origin == QStringLiteral("常见安装位置"));
    } else {
        EXPECT_EQ(info.origin.toStdString(), std::string("未找到"));
    }
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

}  // namespace
