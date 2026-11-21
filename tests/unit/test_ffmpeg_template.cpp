// 场景模板用例。
//
// 模板本身是"数据"，但错的模板会把用户带沟里，所以这里钉三类性质：
//   1. 表结构自洽：id 唯一、每条都有 {input}/{output}、扩展名与序号声明一致；
//   2. 占位符替换：路径带空格时仍是一个 token、缺输入时保留占位符而不是留空；
//   3. **能过自己的解析器**：拼出来的行交给 ParseCommandLine + AnalyzeCommandStructure，
//      结构必须完整、输入输出都要被认出来。这一条最容易翻车 —— 少写一个引号，
//      路径就被切成两个参数，而单看字符串是看不出来的。
//   4. **能过自己的字典**：模板是我们自己发出去的，若里面用了字典没收录的选项，
//      用户点开解释区就会看到「未知参数」—— 等于自曝短板。

#include <gtest/gtest.h>

#include <QSet>

#include "core/ffmpeg/FfmpegCommandCatalog.h"
#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "core/ffmpeg/FfmpegCommandTemplate.h"

using videoeye::ffmpeg::AnalyzeCommandStructure;
using videoeye::ffmpeg::ComposeTemplateCommand;
using videoeye::ffmpeg::FfmpegArgRole;
using videoeye::ffmpeg::FfmpegCapabilityCache;
using videoeye::ffmpeg::FfmpegCommandExplainer;
using videoeye::ffmpeg::FfmpegCommandTemplate;
using videoeye::ffmpeg::FfmpegTemplateLibrary;
using videoeye::ffmpeg::FfmpegTokenRole;
using videoeye::ffmpeg::FindFfmpegTemplate;
using videoeye::ffmpeg::FindFocusToken;
using videoeye::ffmpeg::InstantiateTemplate;
using videoeye::ffmpeg::MissingTemplateSlots;
using videoeye::ffmpeg::ParseCommandLine;
using videoeye::ffmpeg::TemplateOutputPath;

namespace {

std::string Joined(const QStringList& list) {
    return list.join(QLatin1Char('|')).toStdString();
}

/// 去掉一层引号：模板给带空格的路径加引号，解析回来时要能还原才说明没切错。
QString Unquote(const QString& text) {
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"'))) {
        return text.mid(1, text.size() - 2);
    }
    return text;
}

} // namespace

TEST(FfmpegCommandTemplateTests, LibraryIsSelfConsistent) {
    const auto& library = FfmpegTemplateLibrary();
    ASSERT_FALSE(library.empty());

    QSet<QString> seen;
    for (const FfmpegCommandTemplate& tmpl : library) {
        EXPECT_FALSE(tmpl.id.isEmpty()) << Joined(tmpl.arguments);
        EXPECT_FALSE(seen.contains(tmpl.id)) << "重复的 id: " << tmpl.id.toStdString();
        seen.insert(tmpl.id);

        EXPECT_FALSE(tmpl.title.isEmpty());
        EXPECT_FALSE(tmpl.summary.isEmpty());
        EXPECT_FALSE(tmpl.output_extension.isEmpty()) << tmpl.id.toStdString();

        // 占位符必须是整个 token，不能"拼在别的 token 里"—— 否则路径带空格时
        // 无法决定引号加在哪
        EXPECT_TRUE(tmpl.arguments.contains(QStringLiteral("{input}"))) << tmpl.id.toStdString();
        EXPECT_TRUE(tmpl.arguments.contains(QStringLiteral("{output}"))) << tmpl.id.toStdString();
        for (const QString& arg : tmpl.arguments) {
            if (arg.contains(QLatin1String("{input}")) || arg.contains(QLatin1String("{output}"))) {
                EXPECT_TRUE(arg == QLatin1String("{input}") || arg == QLatin1String("{output}"))
                    << "占位符被拼进了别的 token: " << arg.toStdString();
            }
        }
    }
}

TEST(FfmpegCommandTemplateTests, FindByIdReturnsSameObject) {
    const auto* found = FindFfmpegTemplate(QStringLiteral("gif"));
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(QStringLiteral("gif"), found->id);
    EXPECT_TRUE(FindFfmpegTemplate(QStringLiteral("no-such-template")) == nullptr);
}

TEST(FfmpegCommandTemplateTests, OutputPathKeepsDirectoryAndUsesTemplateExtension) {
    const auto* h264 = FindFfmpegTemplate(QStringLiteral("h264"));
    ASSERT_NE(nullptr, h264);

    // in.mkv 走 h264 模板 → in_h264.mp4（容器跟着模板走，不跟着源后缀走）
    QString out = TemplateOutputPath(*h264, QStringLiteral("D:/media/in.mkv"));
    out.replace(QLatin1Char('\\'), QLatin1Char('/')); // 平台分隔符统一后再比
    EXPECT_EQ(QStringLiteral("D:/media/in_h264.mp4"), out);
}

TEST(FfmpegCommandTemplateTests, SequenceOutputGetsFramePattern) {
    const auto* frames = FindFfmpegTemplate(QStringLiteral("frames"));
    ASSERT_NE(nullptr, frames);

    const QString out = TemplateOutputPath(*frames, QStringLiteral("/tmp/clip.mp4"));
    // 多帧写同一个名字 ffmpeg 会直接报错，序号是硬要求
    EXPECT_EQ(QStringLiteral("/tmp/clip_frames_%03d.png"), out);
}

TEST(FfmpegCommandTemplateTests, UrlAndEmptyInputGiveNoOutputPath) {
    const auto* gif = FindFfmpegTemplate(QStringLiteral("gif"));
    ASSERT_NE(nullptr, gif);

    EXPECT_TRUE(TemplateOutputPath(*gif, QString()).isEmpty());
    EXPECT_TRUE(TemplateOutputPath(*gif, QStringLiteral("rtmp://live/x")).isEmpty());
}

TEST(FfmpegCommandTemplateTests, InstantiateQuotesPathWithSpace) {
    const auto* audio = FindFfmpegTemplate(QStringLiteral("audio"));
    ASSERT_NE(nullptr, audio);

    const auto args = InstantiateTemplate(*audio, QStringLiteral("C:/a b/in.mp4"), QStringLiteral("C:/a b/out.m4a"));
    EXPECT_TRUE(args.contains(QStringLiteral("\"C:/a b/in.mp4\""))) << Joined(args);
    EXPECT_TRUE(args.contains(QStringLiteral("\"C:/a b/out.m4a\""))) << Joined(args);

    // 没有空格就不加引号：少一层噪音，粘贴到终端也原样可用
    const auto plain = InstantiateTemplate(*audio, QStringLiteral("C:/in.mp4"), QStringLiteral("C:/out.m4a"));
    EXPECT_TRUE(plain.contains(QStringLiteral("C:/in.mp4"))) << Joined(plain);
}

TEST(FfmpegCommandTemplateTests, MissingInputKeepsPlaceholder) {
    const auto* h264 = FindFfmpegTemplate(QStringLiteral("h264"));
    ASSERT_NE(nullptr, h264);

    // 没加载素材：占位符留着，用户自己填 —— 比插一条缺路径的残命令更好懂
    const auto args = InstantiateTemplate(*h264, QString(), QString());
    const QString command = ComposeTemplateCommand(args);
    EXPECT_TRUE(command.contains(QStringLiteral("{input}"))) << command.toStdString();
    EXPECT_TRUE(command.contains(QStringLiteral("{output}"))) << command.toStdString();

    const auto missing = MissingTemplateSlots(args);
    ASSERT_EQ(2, missing.size());
    EXPECT_EQ(QStringLiteral("{input}"), missing.at(0));
    EXPECT_EQ(QStringLiteral("{output}"), missing.at(1));
}

TEST(FfmpegCommandTemplateTests, CompleteCommandHasNoMissingSlot) {
    const auto* clip = FindFfmpegTemplate(QStringLiteral("clip"));
    ASSERT_NE(nullptr, clip);

    const auto args = InstantiateTemplate(*clip, QStringLiteral("/tmp/in.mp4"), QStringLiteral("/tmp/in_clip.mp4"));
    EXPECT_TRUE(MissingTemplateSlots(args).empty()) << Joined(MissingTemplateSlots(args));
}

TEST(FfmpegCommandTemplateTests, EveryTemplateSurvivesItsOwnParser) {
    // 这是最关键的一条：模板拼出来必须能被本页的解析器认成"有输入、有输出、结构完整"。
    // 少一个引号、把 `-vf` 的值写成两个 token，都会在这里翻出来。
    //
    // 路径特意带空格：只有这样才能同时验到"加引号 → 解析回一个 token"这条路，
    // 用无空格路径的话，引号逻辑写错也发现不了。
    for (const FfmpegCommandTemplate& tmpl : FfmpegTemplateLibrary()) {
        const QString output =
            QStringLiteral("/tmp/ve t/ve_t_%1.%2")
                .arg(tmpl.id)
                .arg(tmpl.output_is_sequence ? QStringLiteral("%03d.") + tmpl.output_extension : tmpl.output_extension);
        const auto args = InstantiateTemplate(tmpl, QStringLiteral("/tmp/ve t/in.mp4"), output);
        const QString command = ComposeTemplateCommand(args);

        const auto parsed = ParseCommandLine(command);
        ASSERT_EQ(videoeye::ffmpeg::CommandParseStatus::Ok, parsed.status) << tmpl.id.toStdString();
        ASSERT_EQ(args.size(), parsed.arguments.size()) << tmpl.id.toStdString() << " args lost while re-parsing";

        const auto structure = AnalyzeCommandStructure(parsed.arguments);
        EXPECT_TRUE(structure.complete) << tmpl.id.toStdString();
        EXPECT_TRUE(structure.has_input) << tmpl.id.toStdString() << " 没认出输入";
        ASSERT_EQ(1, structure.input_count) << tmpl.id.toStdString();
        ASSERT_EQ(1, structure.output_count) << tmpl.id.toStdString();

        // 输出那个 token 必须真的是 OutputUrl，而不是"归属不明"
        int output_tokens = 0;
        for (const auto& placement : structure.args) {
            if (placement.role == FfmpegArgRole::OutputUrl) {
                ++output_tokens;
                EXPECT_EQ(output.toStdString(), Unquote(parsed.arguments.at(placement.index)).toStdString());
            }
        }
        EXPECT_EQ(1, output_tokens) << tmpl.id.toStdString();
    }
}

TEST(FfmpegCommandTemplateTests, FocusTokenFindsValueToEditFirst) {
    const auto* clip = FindFfmpegTemplate(QStringLiteral("clip"));
    ASSERT_NE(nullptr, clip);

    const auto args = InstantiateTemplate(*clip, QStringLiteral("/tmp/in.mp4"), QStringLiteral("/tmp/out.mp4"));
    const QString command = ComposeTemplateCommand(args);
    const int at = FindFocusToken(command, clip->focus_literal);
    ASSERT_GE(at, 0) << command.toStdString();
    // 选中的应该是 -ss 的值，而不是时间在别处偶然出现的位置
    EXPECT_EQ(QStringLiteral("00:00:00"), command.mid(at, clip->focus_literal.size()));
    EXPECT_TRUE(command.left(at).endsWith(QStringLiteral("-ss "))) << command.toStdString();

    EXPECT_EQ(-1, FindFocusToken(command, QString()));
    EXPECT_EQ(-1, FindFocusToken(command, QStringLiteral("不存在的值")));
}

TEST(FfmpegCommandTemplateTests, EveryTemplateIsFullyCoveredByCatalog) {
    // 能力缓存先清空：未探测时 HasEncoder / HasFilter 一律返回 true，
    // 这样这里只验"字典认不认这些选项"，不会因为本机没装某个编码器而误红。
    FfmpegCapabilityCache::Instance().Clear();

    for (const FfmpegCommandTemplate& tmpl : FfmpegTemplateLibrary()) {
        const auto args = InstantiateTemplate(tmpl, QStringLiteral("/tmp/in.mp4"),
                                              QStringLiteral("/tmp/out.") + tmpl.output_extension);
        const auto explained = FfmpegCommandExplainer::Explain(args);

        for (const auto& token : explained.tokens) {
            EXPECT_NE(FfmpegTokenRole::UnknownOption, token.role)
                << tmpl.id.toStdString() << " 里的 " << token.token.toStdString() << " 字典没收录";
            EXPECT_TRUE(token.warning.isEmpty())
                << tmpl.id.toStdString() << " / " << token.token.toStdString() << ": " << token.warning.toStdString();
        }
    }
}
