// 「这次跑出了哪些文件」的用例。
//
// 这一层的价值全在**别骗人**：命令里写了输出名不等于文件真的落盘了（覆盖被拒、
// 失败早退、`-f null -` 压根不产出）。所以用例覆盖三类：
//   1. 序号模板的识别与展开（%d / %03d 的补零宽度，别把 1 写成 1 而不是 001）；
//   2. 产出判定只认磁盘实证，且把"声明了但没产出"单独列出来；
//   3. 非本地目标（pipe: / - / rtmp://）一律不算产出。

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegRunOutputs.h"

using videoeye::ffmpeg::CollectRunOutputs;
using videoeye::ffmpeg::ExpandSequencePattern;
using videoeye::ffmpeg::FfmpegRunOutputs;
using videoeye::ffmpeg::HasSequencePattern;
using videoeye::ffmpeg::RevealDirectory;

namespace {

/// 造一个空文件（存在与否就够判定了，内容无所谓）
QString Touch(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.close();
    }
    return path;
}

std::string Joined(const QStringList& list) {
    return list.join(QLatin1Char('|')).toStdString();
}

} // namespace

TEST(FfmpegRunOutputs, SequencePatternDetection) {
    EXPECT_TRUE(HasSequencePattern(QStringLiteral("out_%03d.png")));
    EXPECT_TRUE(HasSequencePattern(QStringLiteral("out_%d.png")));
    EXPECT_TRUE(HasSequencePattern(QStringLiteral("out_%3d.png")));
    // 文件名里出现 %d 就是序号模式 —— 这是 ffmpeg 自己的规则（它会把 100%done.png
    // 展开成 100<帧号>one.png）。检测不替用户"猜意图"，照规则来。
    EXPECT_TRUE(HasSequencePattern(QStringLiteral("100%done.png")));

    EXPECT_FALSE(HasSequencePattern(QStringLiteral("out.png")));
    EXPECT_FALSE(HasSequencePattern(QStringLiteral("a%b.png")));
    EXPECT_FALSE(HasSequencePattern(QStringLiteral("100%x.png")));
    EXPECT_FALSE(HasSequencePattern(QString()));
}

TEST(FfmpegRunOutputs, ExpandPadsToDeclaredWidth) {
    EXPECT_EQ(QStringLiteral("f_007.png"), ExpandSequencePattern(QStringLiteral("f_%03d.png"), 7));
    EXPECT_EQ(QStringLiteral("f_1000.png"), ExpandSequencePattern(QStringLiteral("f_%03d.png"), 1000));
    // %d 不补零
    EXPECT_EQ(QStringLiteral("f_7.png"), ExpandSequencePattern(QStringLiteral("f_%d.png"), 7));
    // 没有模式就原样返回（别自作主张改路径）
    EXPECT_EQ(QStringLiteral("f.png"), ExpandSequencePattern(QStringLiteral("f.png"), 7));
    // 非序号的 % 也不能被吃掉
    EXPECT_EQ(QStringLiteral("100%x.png"), ExpandSequencePattern(QStringLiteral("100%x.png"), 3));
}

TEST(FfmpegRunOutputs, DetectsFileActuallyOnDisk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = Touch(dir, QStringLiteral("out.mp4"));

    const auto outputs = CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), out});
    ASSERT_EQ(1, outputs.files.size()) << Joined(outputs.files);
    EXPECT_EQ(out, outputs.files.front());
    EXPECT_TRUE(outputs.missing.empty()) << Joined(outputs.missing);
}

TEST(FfmpegRunOutputs, ReportsDeclaredButMissingOutput) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("never_written.mp4"));

    const auto outputs = CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), out});
    EXPECT_TRUE(outputs.files.empty()) << Joined(outputs.files);
    ASSERT_EQ(1, outputs.missing.size()) << Joined(outputs.missing);
    EXPECT_EQ(out, outputs.missing.front());
}

TEST(FfmpegRunOutputs, IgnoresNonLocalTargets) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // -f null - ：只解码不出片，"-" 不是文件
    auto null_out = CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")),
                                       QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
    EXPECT_TRUE(null_out.files.empty()) << Joined(null_out.files);
    EXPECT_TRUE(null_out.missing.empty()) << Joined(null_out.missing);

    // 网络推流
    auto rtmp = CollectRunOutputs(
        {QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), QStringLiteral("rtmp://live/x")});
    EXPECT_TRUE(rtmp.files.empty()) << Joined(rtmp.files);
    EXPECT_TRUE(rtmp.missing.empty()) << Joined(rtmp.missing);

    // 显式管道
    auto pipe =
        CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), QStringLiteral("pipe:1")});
    EXPECT_TRUE(pipe.files.empty()) << Joined(pipe.files);
    EXPECT_TRUE(pipe.missing.empty()) << Joined(pipe.missing);
}

TEST(FfmpegRunOutputs, SequenceReportsFirstExistingFrame) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pattern = dir.filePath(QStringLiteral("frame_%03d.png"));
    const QString first = Touch(dir, QStringLiteral("frame_001.png"));
    Touch(dir, QStringLiteral("frame_002.png"));

    const auto outputs = CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), pattern});
    ASSERT_EQ(1, outputs.files.size()) << Joined(outputs.files);
    EXPECT_EQ(first, outputs.files.front());
    EXPECT_TRUE(outputs.has_sequence);
    EXPECT_TRUE(outputs.missing.empty()) << Joined(outputs.missing);
}

TEST(FfmpegRunOutputs, SequenceWithoutAnyFrameIsMissing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pattern = dir.filePath(QStringLiteral("frame_%03d.png"));

    const auto outputs = CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), pattern});
    EXPECT_TRUE(outputs.files.empty()) << Joined(outputs.files);
    // 报的是模板原文：用户要对照的是自己写的那个模式
    ASSERT_EQ(1, outputs.missing.size()) << Joined(outputs.missing);
    EXPECT_EQ(pattern, outputs.missing.front());
    EXPECT_TRUE(outputs.has_sequence);
}

TEST(FfmpegRunOutputs, EveryOutputIsListed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString a = Touch(dir, QStringLiteral("a.mp4"));
    const QString b = Touch(dir, QStringLiteral("b.mp4"));

    const auto outputs = CollectRunOutputs({QStringLiteral("-i"), dir.filePath(QStringLiteral("in.mp4")), a,
                                            QStringLiteral("-map"), QStringLiteral("0"), b});
    ASSERT_EQ(2, outputs.files.size()) << Joined(outputs.files);
    EXPECT_EQ(a, outputs.files.at(0));
    EXPECT_EQ(b, outputs.files.at(1));
}

TEST(FfmpegRunOutputs, RevealDirectoryPrefersProducedFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = Touch(dir, QStringLiteral("out.mp4"));

    FfmpegRunOutputs outputs;
    outputs.files.append(out);
    EXPECT_EQ(QDir(dir.path()).absolutePath(), RevealDirectory(outputs));
}

TEST(FfmpegRunOutputs, RevealDirectoryFallsBackToDeclaredOutput) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // 没产出也要能去"为什么没有"的地方看看
    FfmpegRunOutputs outputs;
    outputs.missing.append(dir.filePath(QStringLiteral("never.mp4")));
    EXPECT_EQ(QDir(dir.path()).absolutePath(), RevealDirectory(outputs));
}

TEST(FfmpegRunOutputs, RevealDirectoryEmptyWhenNothingLocal) {
    FfmpegRunOutputs outputs;
    EXPECT_TRUE(RevealDirectory(outputs).isEmpty());
}
