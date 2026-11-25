// 「格式转换」参数 → argv 的纯逻辑测试。
//
// 这一层的价值全在"错的组合能被提前挡住"，所以用例分三类：
//   * 正常拼装：每个容器的默认编码、CRF/码率/滤镜/快启动该出现在哪儿；
//   * 拒绝：容器不收的编码、输入输出同一路径、流复制配滤镜；
//   * 一致性：SupportedVideoCodecs / SupportedAudioCodecs 列出来的每一项都必须真能
//     拼出命令，没列出来的必须被拒 —— 这条防止能力表与实际校验慢慢漂移。

#include <string>
#include <vector>

#include <QDir>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/ffmpeg/FormatConversionPlan.h"

namespace {

using videoeye::ffmpeg::BuildConversionPlan;
using videoeye::ffmpeg::ConversionAudioCodec;
using videoeye::ffmpeg::ConversionContainer;
using videoeye::ffmpeg::ConversionFrameRate;
using videoeye::ffmpeg::ConversionPixelFormat;
using videoeye::ffmpeg::ConversionPlan;
using videoeye::ffmpeg::ConversionPreset;
using videoeye::ffmpeg::ConversionProfile;
using videoeye::ffmpeg::ConversionQuality;
using videoeye::ffmpeg::ConversionScale;
using videoeye::ffmpeg::ConversionSourceFacts;
using videoeye::ffmpeg::ConversionVideoCodec;
using videoeye::ffmpeg::ConversionVideoRateMode;
using videoeye::ffmpeg::ResolveAudioCodec;
using videoeye::ffmpeg::ResolveVideoCodec;

std::string S(const QString& text) {
    return text.toUtf8().toStdString();
}

/// 取 `-key value` 里的 value；不存在返回空。
QString ValueAfter(const QStringList& args, const QString& key) {
    const int index = args.indexOf(key);
    if (index < 0 || index + 1 >= args.size()) {
        return QString();
    }
    return args.at(index + 1);
}

bool HasWarning(const QStringList& warnings, const QString& needle) {
    for (const QString& warning : warnings) {
        if (warning.contains(needle)) {
            return true;
        }
    }
    return false;
}

ConversionSourceFacts KnownSource(const QString& video, const QString& audio) {
    ConversionSourceFacts source;
    source.known = true;
    source.has_video = !video.isEmpty();
    source.has_audio = !audio.isEmpty();
    source.video_codec = video;
    source.audio_codec = audio;
    return source;
}

class FormatConversionPlanTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        input_ = dir_.filePath(QStringLiteral("source.mp4"));
    }

    QString Output(const QString& name) const {
        return dir_.filePath(name);
    }

    QTemporaryDir dir_;
    QString input_;
};

// ---------- 正常拼装 ----------

TEST_F(FormatConversionPlanTest, Mp4DefaultsToH264AacWithFaststart) {
    ConversionProfile profile; // mp4 + Auto + Auto + Balanced
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(plan.warnings.isEmpty()) << S(plan.warnings.join(QLatin1Char('|')));
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:v"))), "libx264");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-crf"))), "23");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-preset"))), "medium");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-pix_fmt"))), "yuv420p");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:a"))), "aac");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-b:a"))), "128k");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-movflags"))), "+faststart");
    // 输出必须是最后一个 token（多一个尾部参数就会被 ffmpeg 当成第二个输出）。
    EXPECT_EQ(S(plan.arguments.back()), S(Output(QStringLiteral("out.mp4"))));
    EXPECT_EQ(S(plan.extension), "mp4");
}

TEST_F(FormatConversionPlanTest, AutoResolvesPerContainer) {
    EXPECT_EQ(ResolveVideoCodec(ConversionContainer::Webm, ConversionVideoCodec::Auto), ConversionVideoCodec::Vp9);
    EXPECT_EQ(ResolveAudioCodec(ConversionContainer::Webm, ConversionAudioCodec::Auto), ConversionAudioCodec::Opus);
    EXPECT_EQ(ResolveVideoCodec(ConversionContainer::Avi, ConversionVideoCodec::Auto), ConversionVideoCodec::Mpeg4);
    EXPECT_EQ(ResolveAudioCodec(ConversionContainer::Avi, ConversionAudioCodec::Auto), ConversionAudioCodec::Mp3);
    EXPECT_EQ(ResolveVideoCodec(ConversionContainer::Wav, ConversionVideoCodec::Auto), ConversionVideoCodec::None);

    ConversionProfile profile;
    profile.container = ConversionContainer::Webm;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.webm")));
    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:v"))), "libvpx-vp9");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:a"))), "libopus");
    // VP9 的恒定质量必须配 -b:v 0，否则默认码率会盖掉 -crf。
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-b:v"))), "0");
}

TEST_F(FormatConversionPlanTest, Mpeg4UsesQscaleInsteadOfCrf) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Avi;
    profile.video = ConversionVideoCodec::Mpeg4;
    profile.audio = ConversionAudioCodec::Mp3;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.avi")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-q:v"))), "4");
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-crf")));
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:a"))), "libmp3lame");
    // avi 不是 mp4 家族，加 -movflags 会被 ffmpeg 拒绝。
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-movflags")));
}

TEST_F(FormatConversionPlanTest, AudioOnlyContainerDropsVideoTrack) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Mp3;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp3")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("-vn")));
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-c:v")));
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:a"))), "libmp3lame");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-b:a"))), "192k");
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-movflags")));
}

TEST_F(FormatConversionPlanTest, WavUsesPcmWithoutBitrateFlag) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Wav;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.wav")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:a"))), "pcm_s16le");
    // 无损编码没有"码率"这个旋钮，写 -b:a 是错的。
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-b:a")));
}

TEST_F(FormatConversionPlanTest, ScaleAndFrameRateBecomeOneVideoFilter) {
    ConversionProfile profile;
    profile.scale = ConversionScale::P720;
    profile.fps = ConversionFrameRate::Fps30;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-vf"))), "scale=-2:720,fps=30");
}

TEST_F(FormatConversionPlanTest, CustomQualityAndBitrateAreHonored) {
    ConversionProfile profile;
    profile.quality = ConversionQuality::Custom;
    profile.crf = 30;
    profile.audio_bitrate_kbps = 320;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-crf"))), "30");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-b:a"))), "320k");
}

TEST_F(FormatConversionPlanTest, CustomCrfIsClampedToCodecRange) {
    ConversionProfile profile;
    profile.quality = ConversionQuality::Custom;
    profile.crf = 99;
    const ConversionPlan h264 = BuildConversionPlan(profile, input_, Output(QStringLiteral("a.mp4")));
    ASSERT_TRUE(h264.ok);
    EXPECT_EQ(S(ValueAfter(h264.arguments, QStringLiteral("-crf"))), "51");

    profile.container = ConversionContainer::Webm;
    const ConversionPlan vp9 = BuildConversionPlan(profile, input_, Output(QStringLiteral("a.webm")));
    ASSERT_TRUE(vp9.ok) << S(vp9.error);
    // VP9 的上限是 63，不能被 h264 的 51 夹住。
    EXPECT_EQ(S(ValueAfter(vp9.arguments, QStringLiteral("-crf"))), "63");
}

TEST_F(FormatConversionPlanTest, BitrateModeUsesBvAndDropsCrf) {
    ConversionProfile profile;
    profile.video_rate_mode = ConversionVideoRateMode::Bitrate;
    profile.video_bitrate_kbps = 5000;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-b:v"))), "5000k");
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-crf")));
    // 预设仍然对 H.264 生效：码率模式只替换码率控制方式。
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-preset"))), "medium");
}

TEST_F(FormatConversionPlanTest, BitrateModeRequiresPositiveBitrate) {
    ConversionProfile profile;
    profile.video_rate_mode = ConversionVideoRateMode::Bitrate; // video_bitrate_kbps 默认 0
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("码率"))) << S(plan.error);
    EXPECT_TRUE(plan.arguments.isEmpty());
}

TEST_F(FormatConversionPlanTest, BitrateModeDoesNotWriteZeroBufferForVp9) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Webm;
    profile.video_rate_mode = ConversionVideoRateMode::Bitrate;
    profile.video_bitrate_kbps = 2000;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.webm")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-b:v"))), "2000k");
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-crf")));
}

TEST_F(FormatConversionPlanTest, PresetIsHonoredForH264AndH265) {
    ConversionProfile h264;
    h264.preset = ConversionPreset::VerySlow;
    const ConversionPlan h264_plan = BuildConversionPlan(h264, input_, Output(QStringLiteral("a.mp4")));
    ASSERT_TRUE(h264_plan.ok) << S(h264_plan.error);
    EXPECT_EQ(S(ValueAfter(h264_plan.arguments, QStringLiteral("-preset"))), "veryslow");

    ConversionProfile h265;
    h265.container = ConversionContainer::Mkv;
    h265.video = ConversionVideoCodec::H265;
    h265.preset = ConversionPreset::Faster;
    const ConversionPlan h265_plan = BuildConversionPlan(h265, input_, Output(QStringLiteral("a.mkv")));
    ASSERT_TRUE(h265_plan.ok) << S(h265_plan.error);
    EXPECT_EQ(S(ValueAfter(h265_plan.arguments, QStringLiteral("-preset"))), "faster");
}

TEST_F(FormatConversionPlanTest, PresetIsIgnoredForCodecsWithoutPreset) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Webm; // 自动 → VP9
    profile.preset = ConversionPreset::VerySlow;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.webm")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-preset")));
}

TEST_F(FormatConversionPlanTest, TenBitPixelFormatAppliedForSupportedCodecs) {
    ConversionProfile profile;
    profile.pixel_format = ConversionPixelFormat::Yuv420p10;
    const ConversionPlan h264 = BuildConversionPlan(profile, input_, Output(QStringLiteral("a.mp4")));
    ASSERT_TRUE(h264.ok) << S(h264.error);
    EXPECT_EQ(S(ValueAfter(h264.arguments, QStringLiteral("-pix_fmt"))), "yuv420p10le");

    ConversionProfile av1;
    av1.container = ConversionContainer::Mkv;
    av1.video = ConversionVideoCodec::Av1;
    av1.pixel_format = ConversionPixelFormat::Yuv420p10;
    const ConversionPlan av1_plan = BuildConversionPlan(av1, input_, Output(QStringLiteral("a.mkv")));
    ASSERT_TRUE(av1_plan.ok) << S(av1_plan.error);
    EXPECT_EQ(S(ValueAfter(av1_plan.arguments, QStringLiteral("-pix_fmt"))), "yuv420p10le");
}

TEST_F(FormatConversionPlanTest, TenBitIsRejectedForMpeg4) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Avi;
    profile.video = ConversionVideoCodec::Mpeg4;
    profile.pixel_format = ConversionPixelFormat::Yuv420p10;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.avi")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("10-bit"))) << S(plan.error);
    EXPECT_TRUE(plan.arguments.isEmpty());
}

TEST_F(FormatConversionPlanTest, ExtraArgumentsAreAppendedBeforeOutput) {
    ConversionProfile profile;
    profile.extra_arguments = QStringLiteral("-metadata title=\"hello world\" -map 0:s?");
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("-metadata")));
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("title=hello world")));
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("-map")));
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("0:s?")));
    // 输出必须仍然是最后一个 token，否则 ffmpeg 会把附加参数当成第二个输出。
    EXPECT_EQ(S(plan.arguments.back()), S(Output(QStringLiteral("out.mp4"))));
    EXPECT_LT(plan.arguments.indexOf(QStringLiteral("-metadata")), plan.arguments.size() - 1);
}

TEST_F(FormatConversionPlanTest, ExtraArgumentsAreRejectedWhenDangerous) {
    const QStringList cases = {QStringLiteral("-i other.mp4"), QStringLiteral("-y"), QStringLiteral("-n"),
                               QStringLiteral("-nostdin")};
    for (const QString& extra : cases) {
        ConversionProfile profile;
        profile.extra_arguments = extra;
        const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));
        EXPECT_FALSE(plan.ok) << S(extra);
        EXPECT_TRUE(plan.arguments.isEmpty()) << S(extra);
    }
}

TEST_F(FormatConversionPlanTest, ExtraArgumentsMustStartWithAnOption) {
    ConversionProfile profile;
    profile.extra_arguments = QStringLiteral("out2.mp4");
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("以 - 开头"))) << S(plan.error);
}

TEST_F(FormatConversionPlanTest, ExtraArgumentsCannotContainInputOrOutputPath) {
    ConversionProfile profile;
    profile.extra_arguments = QStringLiteral("-metadata ") + input_;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("路径"))) << S(plan.error);
}

TEST_F(FormatConversionPlanTest, ExtraArgumentsWithUnclosedQuoteAreRejected) {
    ConversionProfile profile;
    profile.extra_arguments = QStringLiteral("-metadata title=\"oops");
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("引号"))) << S(plan.error);
}

TEST_F(FormatConversionPlanTest, ExtraArgumentsOverrideManagedOptionWarns) {
    ConversionProfile profile;
    profile.extra_arguments = QStringLiteral("-crf 18");
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("-crf")));
    // 用户写的 -crf 18 在页面生成的 -crf 23 之后，ffmpeg 以最后一个为准。
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-crf"))), "23");
    EXPECT_EQ(plan.arguments.lastIndexOf(QStringLiteral("-crf")), plan.arguments.size() - 3);
}

TEST_F(FormatConversionPlanTest, GifKeepsOutputLastWithExtraArguments) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Gif;
    profile.extra_arguments = QStringLiteral("-metadata title=demo");
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.gif")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("-metadata")));
    EXPECT_EQ(S(plan.arguments.back()), S(Output(QStringLiteral("out.gif"))));
}

TEST_F(FormatConversionPlanTest, GifUsesPaletteGraphAndIgnoresCodecChoices) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Gif;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.gif")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    const QString filter = ValueAfter(plan.arguments, QStringLiteral("-filter_complex"));
    EXPECT_TRUE(filter.contains(QStringLiteral("palettegen"))) << S(filter);
    EXPECT_TRUE(filter.contains(QStringLiteral("paletteuse"))) << S(filter);
    EXPECT_TRUE(filter.contains(QStringLiteral("fps=15"))) << S(filter);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-loop"))), "0");
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-c:v")));
    EXPECT_FALSE(plan.arguments.contains(QStringLiteral("-c:a")));
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("调色板")));
}

// ---------- 拒绝 ----------

TEST_F(FormatConversionPlanTest, WebmRejectsH264ForReencode) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Webm;
    profile.video = ConversionVideoCodec::H264;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.webm")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("WebM")) || plan.error.contains(QStringLiteral("不接受")))
        << S(plan.error);
    // 失败时不能留下半条命令，否则调用方一疏忽就真去跑了。
    EXPECT_TRUE(plan.arguments.isEmpty());
}

TEST_F(FormatConversionPlanTest, WebmCopyRejectedWhenSourceIsH264) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Webm;
    profile.video = ConversionVideoCodec::Copy;
    profile.audio = ConversionAudioCodec::Opus;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.webm")),
                                                    KnownSource(QStringLiteral("h264"), QStringLiteral("aac")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("h264"))) << S(plan.error);
}

TEST_F(FormatConversionPlanTest, WebmCopyAcceptedWhenSourceAlreadyVp9) {
    ConversionProfile profile;
    profile.container = ConversionContainer::Webm;
    profile.video = ConversionVideoCodec::Copy;
    profile.audio = ConversionAudioCodec::Copy;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.webm")),
                                                    KnownSource(QStringLiteral("vp9"), QStringLiteral("opus")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:v"))), "copy");
    EXPECT_EQ(S(ValueAfter(plan.arguments, QStringLiteral("-c:a"))), "copy");
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("流复制不重新编码")));
}

TEST_F(FormatConversionPlanTest, CopyToUnknownContainerOnlyWarns) {
    // 源信息未知时不能断言必然失败：同样的命令在源编码恰好兼容时是能跑的。
    ConversionProfile profile;
    profile.container = ConversionContainer::Mp4;
    profile.video = ConversionVideoCodec::Copy;
    profile.audio = ConversionAudioCodec::Copy;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("只有源编码被目标容器接受")));
}

TEST_F(FormatConversionPlanTest, CopyCannotBeCombinedWithScale) {
    ConversionProfile profile;
    profile.video = ConversionVideoCodec::Copy;
    profile.scale = ConversionScale::P1080;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("流复制")));
}

TEST_F(FormatConversionPlanTest, SameInputAndOutputIsRejected) {
    ConversionProfile profile;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, input_);

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("同一个文件"))) << S(plan.error);
}

TEST_F(FormatConversionPlanTest, EmptyInputIsRejected) {
    const ConversionPlan plan = BuildConversionPlan(ConversionProfile(), QString(), QString());
    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("输入")));
}

TEST_F(FormatConversionPlanTest, NothingToWriteIsRejected) {
    ConversionProfile profile;
    profile.video = ConversionVideoCodec::None;
    profile.audio = ConversionAudioCodec::None;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")));

    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.error.contains(QStringLiteral("没有可写出的内容"))) << S(plan.error);
}

TEST_F(FormatConversionPlanTest, SourceWithoutAudioWarnsOnAudioCopy) {
    ConversionProfile profile;
    profile.video = ConversionVideoCodec::Copy;
    profile.audio = ConversionAudioCodec::Copy;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mkv")),
                                                    KnownSource(QStringLiteral("h264"), QString()));

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("没有音频轨")));
}

// ---------- 默认输出路径 ----------

TEST_F(FormatConversionPlanTest, EmptyOutputFallsBackToSiblingWithNewExtension) {
    const ConversionPlan plan = BuildConversionPlan(ConversionProfile(), input_, QString(), ConversionSourceFacts());

    ASSERT_TRUE(plan.ok) << S(plan.error);
    const QString output = plan.arguments.back();
    // 源本身是 .mp4，目标容器也是 mp4：默认名必须加后缀，不能等于输入。
    EXPECT_TRUE(output.endsWith(QStringLiteral("source_converted.mp4"))) << S(output);
    EXPECT_FALSE(videoeye::ffmpeg::IsSameLocalFile(input_, output));
}

TEST_F(FormatConversionPlanTest, DefaultOutputAvoidsOverwritingSameExtensionSource) {
    // 源是 .mkv、目标容器也是 mkv：只改扩展名会得到和输入一模一样的路径。
    const QString mkv_input = dir_.filePath(QStringLiteral("clip.mkv"));
    const QString output = videoeye::ffmpeg::DefaultConversionOutputPath(mkv_input, ConversionContainer::Mkv);

    EXPECT_FALSE(output.isEmpty());
    EXPECT_FALSE(videoeye::ffmpeg::IsSameLocalFile(mkv_input, output));
    EXPECT_TRUE(output.contains(QStringLiteral("_converted"))) << S(output);
}

TEST_F(FormatConversionPlanTest, DefaultOutputIsEmptyForNetworkStream) {
    EXPECT_TRUE(
        videoeye::ffmpeg::DefaultConversionOutputPath(QStringLiteral("rtmp://host/live"), ConversionContainer::Mp4)
            .isEmpty());
}

// ---------- 能力表与校验的一致性 ----------

TEST_F(FormatConversionPlanTest, EveryAdvertisedCodecCanBuildACommand) {
    for (const ConversionContainer container : videoeye::ffmpeg::AllConversionContainers()) {
        if (container == ConversionContainer::Gif) {
            continue; // GIF 的命令由调色板分支单独生成，不看编码枚举
        }
        for (const ConversionVideoCodec video : videoeye::ffmpeg::SupportedVideoCodecs(container)) {
            for (const ConversionAudioCodec audio : videoeye::ffmpeg::SupportedAudioCodecs(container)) {
                // 「不要视频 + 不要音频」本来就被拒绝；纯音频容器上的 Auto 也会解析成
                // 不要视频，所以要先 Resolve 再判，否则会把合法项误当成空输出组合。
                if (ResolveVideoCodec(container, video) == ConversionVideoCodec::None &&
                    ResolveAudioCodec(container, audio) == ConversionAudioCodec::None) {
                    continue;
                }
                ConversionProfile profile;
                profile.container = container;
                profile.video = video;
                profile.audio = audio;
                // 流复制在源未知时只给警告，不会失败；这里专门验证"表里列的都能拼出命令"。
                const ConversionPlan plan = BuildConversionPlan(
                    profile, input_,
                    dir_.filePath(QStringLiteral("out.") + videoeye::ffmpeg::ContainerExtension(container)));
                EXPECT_TRUE(plan.ok) << "容器 " << S(videoeye::ffmpeg::ContainerLabel(container)) << " 编码 "
                                     << S(videoeye::ffmpeg::VideoCodecLabel(video)) << " / "
                                     << S(videoeye::ffmpeg::AudioCodecLabel(audio)) << " 被拒: " << S(plan.error);
            }
        }
    }
}

TEST_F(FormatConversionPlanTest, CodecNotInAdvertisedListIsRejected) {
    // 反向：表里没有的编码必须被拒，否则"过滤下拉项"这件事就没有意义。
    ConversionProfile flv_vp9;
    flv_vp9.container = ConversionContainer::Flv;
    flv_vp9.video = ConversionVideoCodec::Vp9;
    EXPECT_FALSE(BuildConversionPlan(flv_vp9, input_, dir_.filePath(QStringLiteral("out.flv"))).ok);

    ConversionProfile mp3_flac;
    mp3_flac.container = ConversionContainer::Mp3;
    mp3_flac.audio = ConversionAudioCodec::Flac;
    EXPECT_FALSE(BuildConversionPlan(mp3_flac, input_, dir_.filePath(QStringLiteral("out.mp3"))).ok);
}

} // namespace
