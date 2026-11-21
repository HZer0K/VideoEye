// 「ffmpeg 失败诊断」的纯逻辑测试。
//
// 输入是 stderr 全文，输出是"人话结论"。这一层最容易退化：随手补一条 needle 就能让
// 某个用例变绿，但真实日志措辞稍变就漏；反过来 needle 定得太宽又会把正常日志误报成
// 故障。所以每个用例用的都是从真实 ffmpeg 输出里抄下来的原话（注释标了上下文），
// 并且专门留了两条负向用例：只有兜底行、以及完全正常的 banner+进度。

#include <vector>

#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegFailureDiagnostics.h"

namespace {

using videoeye::ffmpeg::DiagnoseFailure;
using videoeye::ffmpeg::FfmpegDiagnosis;

bool HasDiagnosis(const std::vector<FfmpegDiagnosis>& list, const QString& id) {
    for (const auto& item : list) {
        if (item.id == id) {
            return true;
        }
    }
    return false;
}

QString IdsOf(const std::vector<FfmpegDiagnosis>& list) {
    QStringList ids;
    for (const auto& item : list) {
        ids << item.id;
    }
    return ids.join(QLatin1Char(','));
}

// ---- 正向：每条规则认得自己的原话 ----

TEST(FfmpegFailureDiagnostics, DetectsMissingFile) {
    // 输入路径写错时的原话（stderr 上是 `<path>: No such file or directory`）
    const auto list = DiagnoseFailure(QStringLiteral("in.mp4: No such file or directory\nConversion failed!"));
    ASSERT_EQ(1u, list.size());
    EXPECT_EQ(QStringLiteral("missing-file"), list.front().id);
    EXPECT_FALSE(list.front().title.isEmpty());
    EXPECT_FALSE(list.front().suggestion.isEmpty());
    EXPECT_TRUE(list.front().evidence.contains(QStringLiteral("No such file")));
}

TEST(FfmpegFailureDiagnostics, DetectsSameAsInput) {
    // 输出写成了输入：`Output out.mp4 same as Input #0 - exiting`
    const auto list = DiagnoseFailure(QStringLiteral("Output out.mp4 same as Input #0 - exiting"));
    ASSERT_EQ(1u, list.size());
    EXPECT_EQ(QStringLiteral("same-as-input"), list.front().id);
}

TEST(FfmpegFailureDiagnostics, DetectsUnknownEncoder) {
    const auto list = DiagnoseFailure(QStringLiteral("[mp4 @ 000001] Unknown encoder 'libsvtav1'"));
    EXPECT_TRUE(HasDiagnosis(list, QStringLiteral("unknown-encoder")));
}

TEST(FfmpegFailureDiagnostics, DetectsUnknownOption) {
    // 抄来的命令在别的版本上不存在这个选项
    const auto list = DiagnoseFailure(
        QStringLiteral("Unrecognized option 'zzz'.\nError splitting the argument list: Option not found"));
    ASSERT_EQ(1u, list.size()); // 两条 needle 属于同一条规则，不能出两遍
    EXPECT_EQ(QStringLiteral("unknown-option"), list.front().id);
}

TEST(FfmpegFailureDiagnostics, DetectsFilterWithStreamCopy) {
    // 探针实测：`-vf scale=... -c copy` 的原话
    const auto list = DiagnoseFailure(QStringLiteral("Filtering and streamcopy cannot be used together"));
    ASSERT_EQ(1u, list.size());
    EXPECT_EQ(QStringLiteral("filter-copy-conflict"), list.front().id);
}

TEST(FfmpegFailureDiagnostics, DetectsMuxingQueueOverflow) {
    const auto list = DiagnoseFailure(QStringLiteral("Too many packets buffered for output stream 0:1."));
    EXPECT_TRUE(HasDiagnosis(list, QStringLiteral("mux-queue-overflow")));
}

TEST(FfmpegFailureDiagnostics, DetectsImageSequenceMissingPattern) {
    // 实测原话（ffmpeg 6.1，`-vf fps=1 out.png` 这种多帧写同一个名字的写法）：
    //   三句连着打，前两句是 hint，最后一句才是硬错误
    const auto list = DiagnoseFailure(
        QStringLiteral("[image2 @ 000002] The specified filename 'out.png' does not contain an image sequence "
                       "pattern or a pattern is invalid.\n"
                       "[image2 @ 000002] Cannot write more than one file with the same name. Are you missing "
                       "the -update option or a sequence pattern?\n"
                       "[vost#0:0/png @ 000003] Error submitting a packet to the muxer: Invalid argument"));
    ASSERT_EQ(1u, list.size()) << IdsOf(list).toStdString(); // 同一件事只出一条
    EXPECT_EQ(QStringLiteral("image2-sequence"), list.front().id);
    EXPECT_TRUE(list.front().suggestion.contains(QStringLiteral("%03d")));
    EXPECT_TRUE(list.front().suggestion.contains(QStringLiteral("-frames:v 1")));

    // 两句各自单独出现也要认：版本不同不一定两句都打（6.1 两句都打，
    // 更老的版本只有第一句的 hint）。只认其中一句 = 换了版本就漏。
    const auto hint_only = DiagnoseFailure(QStringLiteral(
        "[image2 @ 000002] The specified filename 'out.png' does not contain an image sequence pattern or a "
        "pattern is invalid."));
    EXPECT_TRUE(HasDiagnosis(hint_only, QStringLiteral("image2-sequence"))) << IdsOf(hint_only).toStdString();

    const auto error_only = DiagnoseFailure(QStringLiteral(
        "[image2 @ 000002] Cannot write more than one file with the same name. Are you missing the -update option "
        "or a sequence pattern?"));
    EXPECT_TRUE(HasDiagnosis(error_only, QStringLiteral("image2-sequence"))) << IdsOf(error_only).toStdString();
}

TEST(FfmpegFailureDiagnostics, DetectsCorruptMp4) {
    const auto list =
        DiagnoseFailure(QStringLiteral("moov atom not found\nin.mp4: Invalid data found when processing input"));
    EXPECT_TRUE(HasDiagnosis(list, QStringLiteral("corrupt-moov")));
    EXPECT_TRUE(HasDiagnosis(list, QStringLiteral("invalid-data")));
}

TEST(FfmpegFailureDiagnostics, DetectsEvenDimensionRequirement) {
    const auto list = DiagnoseFailure(QStringLiteral("[libx264 @ 000002] height not divisible by 2 (853x481)"));
    EXPECT_TRUE(HasDiagnosis(list, QStringLiteral("even-dimension")));
}

// ---- 负向：不该报的不能报 ----

TEST(FfmpegFailureDiagnostics, GenericTrailerAloneIsNotADiagnosis) {
    // `Conversion failed!` 任何失败都会打，本身推不出原因 —— 只凭它下结论等于瞎猜。
    EXPECT_TRUE(DiagnoseFailure(QStringLiteral("Conversion failed!")).empty());
}

TEST(FfmpegFailureDiagnostics, HealthyLogProducesNothing) {
    const QString healthy =
        QStringLiteral("ffmpeg version 7.0.2-full_build Copyright (c) 2000-2024\n"
                       "  Duration: 00:00:10.13, start: 0.000000, bitrate: 1420 kb/s\n"
                       "  Stream #0:0: Video: h264, yuv420p, 1920x1080, 30 fps\n"
                       "frame=  120 fps= 60 q=28.0 size=     512kB time=00:00:04.00 bitrate=1048.6kbits/s\n"
                       "frame=  304 fps= 61 q=-1.0 Lsize=    1302kB time=00:00:10.10 bitrate=1056.1kbits/s");
    EXPECT_TRUE(DiagnoseFailure(healthy).empty());
}

TEST(FfmpegFailureDiagnostics, BlankTextProducesNothing) {
    EXPECT_TRUE(DiagnoseFailure(QString()).empty());
    EXPECT_TRUE(DiagnoseFailure(QStringLiteral("   \n\n \t ")).empty());
}

// ---- 合并、去重、截断 ----

TEST(FfmpegFailureDiagnostics, SameRuleReportedOnce) {
    // ffmpeg 会把同一个错按输出流重复打好几遍
    const QString text = QStringLiteral("in.mp4: No such file or directory\n"
                                        "in.mp4: No such file or directory\n"
                                        "in.mp4: No such file or directory\n");
    const auto list = DiagnoseFailure(text);
    ASSERT_EQ(1u, list.size());
    EXPECT_EQ(QStringLiteral("missing-file"), list.front().id);
}

TEST(FfmpegFailureDiagnostics, OrderFollowsSeverityNotTextOrder) {
    // 文本里 encoder 的错在前，但"路径不存在"更基本，应该排前面
    const QString text = QStringLiteral("Unknown encoder 'libfoo'\n"
                                        "in.mp4: No such file or directory\n");
    const auto list = DiagnoseFailure(text);
    ASSERT_GE(list.size(), 2u);
    EXPECT_EQ(QStringLiteral("missing-file"), list.front().id);
    EXPECT_TRUE(HasDiagnosis(list, QStringLiteral("unknown-encoder")));
}

TEST(FfmpegFailureDiagnostics, ReportsAtMostFour) {
    // 六类不同的错，只留最需要先看的四条，顺序同样是规则表顺序
    const QString text = QStringLiteral("Output out.mp4 same as Input #0 - exiting\n"
                                        "in.mp4: No such file or directory\n"
                                        "out.mp4: Permission denied\n"
                                        "No space left on device\n"
                                        "Unknown encoder 'libfoo'\n"
                                        "Unrecognized option 'zzz'.\n");
    const auto list = DiagnoseFailure(text);
    ASSERT_EQ(4u, list.size());
    EXPECT_EQ(QStringLiteral("same-as-input,missing-file,permission-denied,no-space"), IdsOf(list));
}

TEST(FfmpegFailureDiagnostics, EvidenceIsClipped) {
    const QString long_path = QString(400, QLatin1Char('a'));
    const QString text = long_path + QStringLiteral(".mp4: No such file or directory");
    const auto list = DiagnoseFailure(text);
    ASSERT_EQ(1u, list.size());
    EXPECT_LE(list.front().evidence.size(), 160);
    EXPECT_TRUE(list.front().evidence.endsWith(QStringLiteral("…")));
}

TEST(FfmpegFailureDiagnostics, MatchesDespiteCaseAndCrLf) {
    // 用户从别处粘进来的日志可能是 CRLF，措辞大小写也不一定统一
    const QString text = QStringLiteral("in.mp4: NO SUCH FILE OR DIRECTORY\r\n");
    const auto list = DiagnoseFailure(text);
    ASSERT_EQ(1u, list.size());
    EXPECT_EQ(QStringLiteral("missing-file"), list.front().id);
}

} // namespace
