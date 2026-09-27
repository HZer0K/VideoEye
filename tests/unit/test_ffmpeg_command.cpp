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

using videoeye::ffmpegtool::CommandParseStatus;
using videoeye::ffmpegtool::DetectStdoutMediaOutput;
using videoeye::ffmpegtool::FfmpegCatalogEntry;
using videoeye::ffmpegtool::FfmpegCommandCatalog;
using videoeye::ffmpegtool::FfmpegCommandExplainer;
using videoeye::ffmpegtool::FfmpegEntryKind;
using videoeye::ffmpegtool::FfmpegInstallGuide;
using videoeye::ffmpegtool::FfmpegTokenRole;
using videoeye::ffmpegtool::FfmpegToolInfo;
using videoeye::ffmpegtool::FfmpegToolLocator;
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
