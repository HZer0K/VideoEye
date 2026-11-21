// 「运行前路径预检」的纯逻辑测试。
//
// 预检看的是磁盘现状，所以不能用假路径编：临时目录里真造一个 in.mp4，再拿它做输入。
// 两个方向都要盯住 ——
//   * 漏报：真错了却不吱声，用户还是得等 ffmpeg 跑一遍；
//   * 误报：本来能跑的命令被拦下来，比漏报更烦人。URL / 管道 / `-f lavfi` 的虚拟输入
//     都属于这一类，各留了用例。

#include <vector>

#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegCommandPreflight.h"

namespace {

using videoeye::ffmpeg::IsLocalPathTarget;
using videoeye::ffmpeg::PreflightCommand;
using videoeye::ffmpeg::PreflightIssue;
using videoeye::ffmpeg::PreflightSeverity;

bool HasIssue(const std::vector<PreflightIssue>& issues, const QString& id) {
    for (const auto& issue : issues) {
        if (issue.id == id) {
            return true;
        }
    }
    return false;
}

QString IdsOf(const std::vector<PreflightIssue>& issues) {
    QStringList ids;
    for (const auto& issue : issues) {
        ids << issue.id;
    }
    return ids.join(QLatin1Char(','));
}

class FfmpegCommandPreflight : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        in_path_ = dir_.filePath(QStringLiteral("in.mp4"));
        QFile file(in_path_);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("not really a video", 18);
        file.close();
    }

    QString Path(const QString& name) const {
        return dir_.filePath(name);
    }

    QTemporaryDir dir_;
    QString in_path_;
};

// ---- 正常命令不该被拦 ----

TEST_F(FfmpegCommandPreflight, HealthyCommandPasses) {
    const auto issues = PreflightCommand({QStringLiteral("-i"), in_path_, Path(QStringLiteral("out.mp4"))});
    EXPECT_TRUE(issues.empty()) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, HealthyCommandWithOptionsPasses) {
    // 带常见编码选项的真实形状：解释器有没有把这些选项的值吃对，直接决定预检能不能用
    const auto issues =
        PreflightCommand({QStringLiteral("-i"), in_path_, QStringLiteral("-c:v"), QStringLiteral("libx264"),
                          QStringLiteral("-crf"), QStringLiteral("23"), Path(QStringLiteral("out.mp4"))});
    EXPECT_TRUE(issues.empty()) << IdsOf(issues).toStdString();
}

// ---- 漏报检查 ----

TEST_F(FfmpegCommandPreflight, MissingInputReported) {
    const QString missing = Path(QStringLiteral("nope.mp4"));
    const auto issues = PreflightCommand({QStringLiteral("-i"), missing, Path(QStringLiteral("out.mp4"))});
    ASSERT_EQ(1u, issues.size());
    EXPECT_EQ(QStringLiteral("input-missing"), issues.front().id);
    EXPECT_EQ(PreflightSeverity::Error, issues.front().severity);
    EXPECT_TRUE(issues.front().message.contains(QStringLiteral("nope.mp4")));
    EXPECT_EQ(missing, issues.front().path);
}

TEST_F(FfmpegCommandPreflight, MissingInputReportedOnce) {
    // 同一个坏路径被两个 -i 引用，只报一条
    const QString missing = Path(QStringLiteral("nope.mp4"));
    const auto issues = PreflightCommand(
        {QStringLiteral("-i"), missing, QStringLiteral("-i"), missing, Path(QStringLiteral("out.mp4"))});
    ASSERT_EQ(1u, issues.size());
    EXPECT_EQ(QStringLiteral("input-missing"), issues.front().id);
}

TEST_F(FfmpegCommandPreflight, OutputDirectoryMissingReported) {
    // 输出到一个还没建的子目录：ffmpeg 不会替你建，最后只报一句"文件不存在"
    const auto issues = PreflightCommand({QStringLiteral("-i"), in_path_, Path(QStringLiteral("sub/dir/out.mp4"))});
    ASSERT_EQ(1u, issues.size());
    EXPECT_EQ(QStringLiteral("output-dir-missing"), issues.front().id);
    EXPECT_TRUE(issues.front().message.contains(QStringLiteral("sub")));
}

TEST_F(FfmpegCommandPreflight, SameAsInputReported) {
    // 同一文件的不同写法：`<tmp>/./in.mp4` 与 `<tmp>/in.mp4`
    const QString same = dir_.path() + QStringLiteral("/./in.mp4");
    const auto issues = PreflightCommand({QStringLiteral("-i"), in_path_, same});
    EXPECT_TRUE(HasIssue(issues, QStringLiteral("same-as-input"))) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, OutputPointingAtDirectoryReported) {
    const auto issues = PreflightCommand({QStringLiteral("-i"), in_path_, dir_.path()});
    EXPECT_TRUE(HasIssue(issues, QStringLiteral("output-is-directory"))) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, InputBeingDirectoryReported) {
    const auto issues = PreflightCommand({QStringLiteral("-i"), dir_.path(), Path(QStringLiteral("out.mp4"))});
    EXPECT_TRUE(HasIssue(issues, QStringLiteral("input-is-directory"))) << IdsOf(issues).toStdString();
}

#ifdef Q_OS_WIN
TEST_F(FfmpegCommandPreflight, CaseOnlyDifferenceStillSameFile) {
    // NTFS 大小写不敏感：A.MP4 与 a.mp4 是同一个文件，不能当成两个
    const QString upper = in_path_.toUpper();
    const auto issues = PreflightCommand({QStringLiteral("-i"), in_path_, upper});
    EXPECT_TRUE(HasIssue(issues, QStringLiteral("same-as-input"))) << IdsOf(issues).toStdString();
}
#endif

// ---- 误报检查：这些"虚拟输入"本来就不在磁盘上 ----

TEST_F(FfmpegCommandPreflight, LavfiVirtualInputNotReported) {
    // `-f lavfi -i testsrc2` 是最常用的造测试画面写法，没有扩展名也没有分隔符
    const auto issues = PreflightCommand({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                                          QStringLiteral("testsrc2=s=320x240"), QStringLiteral("-t"),
                                          QStringLiteral("1"), Path(QStringLiteral("out.mp4"))});
    EXPECT_TRUE(issues.empty()) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, UrlInputNotReported) {
    const auto issues = PreflightCommand(
        {QStringLiteral("-i"), QStringLiteral("http://example.com/a.mp4"), Path(QStringLiteral("out.mp4"))});
    EXPECT_TRUE(issues.empty()) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, PipeTargetsNotReported) {
    // `-i -` 读 stdin、`-f md5 -` 把校验和写到 stdout：都不是磁盘路径
    const auto issues = PreflightCommand(
        {QStringLiteral("-i"), QStringLiteral("-"), QStringLiteral("-f"), QStringLiteral("md5"), QStringLiteral("-")});
    EXPECT_TRUE(issues.empty()) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, InformationCommandNotReported) {
    EXPECT_TRUE(PreflightCommand({QStringLiteral("-version")}).empty());
    EXPECT_TRUE(PreflightCommand({QStringLiteral("-encoders")}).empty());
}

TEST_F(FfmpegCommandPreflight, IncompleteStructureNotReported) {
    // 字典里没有的选项，后面紧跟的那个 token 又是最后一个裸参数 —— 它到底是这个选项的
    // 值还是输出文件，判断不了。这时候连"有没有输出"都不确定，宁可整体不检查。
    const auto issues = PreflightCommand({QStringLiteral("-i"), Path(QStringLiteral("nope.mp4")),
                                          QStringLiteral("-zzz-unknown"), QStringLiteral("value")});
    EXPECT_TRUE(issues.empty()) << IdsOf(issues).toStdString();
}

TEST_F(FfmpegCommandPreflight, UnrecognizedOptionWithTrailingOutputStillChecked) {
    // 同一条命令后面再跟一个明确的输出，结构就确定了（value 只能是选项值）——
    // 别因为有个不认识的选项就把检查整体放弃。
    const auto issues =
        PreflightCommand({QStringLiteral("-i"), Path(QStringLiteral("nope.mp4")), QStringLiteral("-zzz-unknown"),
                          QStringLiteral("value"), Path(QStringLiteral("out.mp4"))});
    ASSERT_EQ(1u, issues.size()) << IdsOf(issues).toStdString();
    EXPECT_EQ(QStringLiteral("input-missing"), issues.front().id);
}

// ---- 目标分类 ----

TEST(FfmpegPreflightTargets, DistinguishesLocalFilesFromStreamsAndUrls) {
    EXPECT_TRUE(IsLocalPathTarget(QStringLiteral("out.mp4")));
    EXPECT_TRUE(IsLocalPathTarget(QStringLiteral("C:\\media\\in.mp4")));
    EXPECT_TRUE(IsLocalPathTarget(QStringLiteral("D:/media/in.mp4")));
    EXPECT_TRUE(IsLocalPathTarget(QStringLiteral("相对/路径.mkv")));

    EXPECT_FALSE(IsLocalPathTarget(QStringLiteral("-")));
    EXPECT_FALSE(IsLocalPathTarget(QStringLiteral("pipe:1")));
    EXPECT_FALSE(IsLocalPathTarget(QStringLiteral("http://example.com/a.mp4")));
    EXPECT_FALSE(IsLocalPathTarget(QStringLiteral("rtmp://live/stream")));
    EXPECT_FALSE(IsLocalPathTarget(QStringLiteral("concat:a.txt|b.txt")));
    EXPECT_FALSE(IsLocalPathTarget(QString()));
}

} // namespace
