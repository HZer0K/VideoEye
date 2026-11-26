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

#include "core/ffmpeg/FfmpegMediaProbe.h"
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
using videoeye::ffmpeg::ConversionVerification;
using videoeye::ffmpeg::ConversionVideoCodec;
using videoeye::ffmpeg::ConversionVideoRateMode;
using videoeye::ffmpeg::MediaProbeSnapshot;
using videoeye::ffmpeg::MediaStreamKind;
using videoeye::ffmpeg::MediaStreamSnapshot;
using videoeye::ffmpeg::RequiredEncoders;
using videoeye::ffmpeg::ResolveAudioCodec;
using videoeye::ffmpeg::ResolveVideoCodec;
using videoeye::ffmpeg::VerifyConversionOutput;

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
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")),
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

TEST_F(FormatConversionPlanTest, OutputExtensionMustMatchContainer) {
    // ffmpeg 按扩展名选 muxer：容器选 MP4、名字写 .mkv，盘上出来的就是 Matroska。
    ConversionProfile mp4;
    const ConversionPlan mismatch = BuildConversionPlan(mp4, input_, Output(QStringLiteral("out.mkv")));
    EXPECT_FALSE(mismatch.ok);
    EXPECT_TRUE(mismatch.error.contains(QStringLiteral("扩展名"))) << S(mismatch.error);
    EXPECT_TRUE(mismatch.arguments.isEmpty());

    const ConversionPlan missing = BuildConversionPlan(mp4, input_, Output(QStringLiteral("out")));
    EXPECT_FALSE(missing.ok);
    EXPECT_TRUE(missing.error.contains(QStringLiteral("扩展名"))) << S(missing.error);

    // 等价别名照收：.m2ts 就是 TS。
    ConversionProfile ts;
    ts.container = ConversionContainer::Ts;
    const ConversionPlan alias = BuildConversionPlan(ts, input_, Output(QStringLiteral("out.m2ts")));
    EXPECT_TRUE(alias.ok) << S(alias.error);
}

TEST_F(FormatConversionPlanTest, StreamMappingExcludesCoverArtAndLimitsAudio) {
    ConversionProfile mp4;
    const ConversionPlan video_plan = BuildConversionPlan(mp4, input_, Output(QStringLiteral("out.mp4")));
    ASSERT_TRUE(video_plan.ok) << S(video_plan.error);
    // 大写 V 排除封面图（attached pic），小写 v 会把封面当第二路视频重编码。
    EXPECT_TRUE(video_plan.arguments.contains(QStringLiteral("0:V?")));
    EXPECT_FALSE(video_plan.arguments.contains(QStringLiteral("0:v?")));
    EXPECT_TRUE(video_plan.arguments.contains(QStringLiteral("0:a?")));

    // 纯音频容器的 muxer 收不下多条音轨：显式只映射第一条。
    ConversionProfile mp3;
    mp3.container = ConversionContainer::Mp3;
    const ConversionPlan audio_plan = BuildConversionPlan(mp3, input_, Output(QStringLiteral("out.mp3")));
    ASSERT_TRUE(audio_plan.ok) << S(audio_plan.error);
    EXPECT_TRUE(audio_plan.arguments.contains(QStringLiteral("0:a:0?")));
    EXPECT_FALSE(audio_plan.arguments.contains(QStringLiteral("0:a?")));
}

TEST_F(FormatConversionPlanTest, SourceSubtitlesWarnButDoNotBlock) {
    ConversionSourceFacts source = KnownSource(QStringLiteral("h264"), QStringLiteral("aac"));
    source.audio_track_count = 1;
    source.subtitle_track_count = 2;
    ConversionProfile profile;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp4")), source);

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("字幕"))) << S(plan.warnings.join(QLatin1Char('|')));

    // 用户已经在附加参数里自己映射字幕时，不再重复提醒"字幕会丢"。
    ConversionProfile mapped_profile;
    mapped_profile.extra_arguments = QStringLiteral("-map 0:s?");
    const ConversionPlan mapped =
        BuildConversionPlan(mapped_profile, input_, Output(QStringLiteral("out.mp4")), source);
    ASSERT_TRUE(mapped.ok) << S(mapped.error);
    EXPECT_FALSE(HasWarning(mapped.warnings, QStringLiteral("字幕"))) << S(mapped.warnings.join(QLatin1Char('|')));
}

TEST_F(FormatConversionPlanTest, AudioOnlyContainerWarnsOnMultipleAudioTracks) {
    ConversionSourceFacts source = KnownSource(QStringLiteral("h264"), QStringLiteral("aac"));
    source.audio_track_count = 3;
    ConversionProfile profile;
    profile.container = ConversionContainer::Mp3;
    const ConversionPlan plan = BuildConversionPlan(profile, input_, Output(QStringLiteral("out.mp3")), source);

    ASSERT_TRUE(plan.ok) << S(plan.error);
    EXPECT_TRUE(HasWarning(plan.warnings, QStringLiteral("第一条"))) << S(plan.warnings.join(QLatin1Char('|')));
}

TEST_F(FormatConversionPlanTest, Vp9AndAv1UseSpeedControlInsteadOfPreset) {
    ConversionProfile webm;
    webm.container = ConversionContainer::Webm; // Auto → VP9
    webm.preset = ConversionPreset::VerySlow;
    const ConversionPlan vp9 = BuildConversionPlan(webm, input_, Output(QStringLiteral("out.webm")));
    ASSERT_TRUE(vp9.ok) << S(vp9.error);
    EXPECT_FALSE(vp9.arguments.contains(QStringLiteral("-preset")));
    EXPECT_EQ(S(ValueAfter(vp9.arguments, QStringLiteral("-row-mt"))), "1");
    EXPECT_EQ(S(ValueAfter(vp9.arguments, QStringLiteral("-cpu-used"))), "0"); // veryslow → 0

    ConversionProfile av1;
    av1.container = ConversionContainer::Mkv;
    av1.video = ConversionVideoCodec::Av1;
    av1.preset = ConversionPreset::UltraFast;
    const ConversionPlan av1_plan = BuildConversionPlan(av1, input_, Output(QStringLiteral("out.mkv")));
    ASSERT_TRUE(av1_plan.ok) << S(av1_plan.error);
    EXPECT_EQ(S(ValueAfter(av1_plan.arguments, QStringLiteral("-cpu-used"))), "8");
}

TEST_F(FormatConversionPlanTest, RequiredEncodersFollowResolvedTargets) {
    const auto names = [](const QStringList& values) { return values.join(QLatin1Char(',')).toStdString(); };

    ConversionProfile mp4;
    EXPECT_EQ(names(RequiredEncoders(mp4)), "libx264,aac");

    ConversionProfile webm;
    webm.container = ConversionContainer::Webm;
    EXPECT_EQ(names(RequiredEncoders(webm)), "libvpx-vp9,libopus");

    ConversionProfile mp3;
    mp3.container = ConversionContainer::Mp3;
    EXPECT_EQ(names(RequiredEncoders(mp3)), "libmp3lame");

    ConversionProfile wav;
    wav.container = ConversionContainer::Wav;
    EXPECT_EQ(names(RequiredEncoders(wav)), "pcm_s16le");

    ConversionProfile gif;
    gif.container = ConversionContainer::Gif;
    EXPECT_EQ(names(RequiredEncoders(gif)), "gif");

    // 流复制不产生编码器依赖；两端都复制时清单为空。
    ConversionProfile copy;
    copy.video = ConversionVideoCodec::Copy;
    copy.audio = ConversionAudioCodec::Copy;
    EXPECT_TRUE(RequiredEncoders(copy).isEmpty());
}

namespace {

MediaStreamSnapshot MakeStream(MediaStreamKind kind, const QString& codec, int width = 0, int height = 0) {
    MediaStreamSnapshot stream;
    stream.kind = kind;
    stream.codec = codec;
    stream.width = width;
    stream.height = height;
    return stream;
}

MediaProbeSnapshot MakeOutput(const QString& container, double duration) {
    MediaProbeSnapshot snapshot;
    snapshot.parsed = true;
    snapshot.container = container;
    snapshot.duration_seconds = duration;
    return snapshot;
}

} // namespace

TEST_F(FormatConversionPlanTest, VerificationAcceptsMatchingOutput) {
    ConversionProfile profile; // mp4 + Auto → h264 / aac
    ConversionSourceFacts source = KnownSource(QStringLiteral("h264"), QStringLiteral("aac"));
    source.duration_seconds = 10.0;

    MediaProbeSnapshot output = MakeOutput(QStringLiteral("mov,mp4"), 10.05);
    output.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("h264"), 1920, 1080));
    output.streams.push_back(MakeStream(MediaStreamKind::Audio, QStringLiteral("aac")));

    const ConversionVerification verification = VerifyConversionOutput(profile, source, output);
    EXPECT_TRUE(verification.parsed);
    EXPECT_FALSE(verification.mismatch) << S(verification.notes.join(QLatin1Char('|')));
}

TEST_F(FormatConversionPlanTest, VerificationFlagsCodecResolutionAudioAndDuration) {
    ConversionSourceFacts source = KnownSource(QStringLiteral("h264"), QStringLiteral("aac"));
    source.duration_seconds = 60.0;

    // 编码不对：目标 h264，实际 hevc。
    {
        ConversionProfile profile;
        MediaProbeSnapshot output = MakeOutput(QStringLiteral("matroska"), 60.0);
        output.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("hevc"), 1920, 1080));
        output.streams.push_back(MakeStream(MediaStreamKind::Audio, QStringLiteral("aac")));
        EXPECT_TRUE(VerifyConversionOutput(profile, source, output).mismatch);
    }

    // 缩放没生效：目标 720p，实际还是 1080p。
    {
        ConversionProfile profile;
        profile.scale = ConversionScale::P720;
        MediaProbeSnapshot output = MakeOutput(QStringLiteral("mp4"), 60.0);
        output.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("h264"), 1920, 1080));
        output.streams.push_back(MakeStream(MediaStreamKind::Audio, QStringLiteral("aac")));
        const ConversionVerification verification = VerifyConversionOutput(profile, source, output);
        EXPECT_TRUE(verification.mismatch);
        EXPECT_TRUE(verification.notes.join(QString()).contains(QStringLiteral("分辨率")));
    }

    // 音轨丢了。
    {
        ConversionProfile profile;
        MediaProbeSnapshot output = MakeOutput(QStringLiteral("mp4"), 60.0);
        output.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("h264"), 1920, 1080));
        const ConversionVerification verification = VerifyConversionOutput(profile, source, output);
        EXPECT_TRUE(verification.mismatch);
        EXPECT_TRUE(verification.notes.join(QString()).contains(QStringLiteral("音频")));
    }

    // 时长被截短：60s 只剩 30s。
    {
        ConversionProfile profile;
        MediaProbeSnapshot output = MakeOutput(QStringLiteral("mp4"), 30.0);
        output.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("h264"), 1920, 1080));
        output.streams.push_back(MakeStream(MediaStreamKind::Audio, QStringLiteral("aac")));
        const ConversionVerification verification = VerifyConversionOutput(profile, source, output);
        EXPECT_TRUE(verification.mismatch);
        EXPECT_TRUE(verification.notes.join(QString()).contains(QStringLiteral("时长")));
    }

    // 读不出流信息：直接判差异（不能把"没核对"说成通过）。
    {
        ConversionProfile profile;
        MediaProbeSnapshot output;
        const ConversionVerification verification = VerifyConversionOutput(profile, source, output);
        EXPECT_FALSE(verification.parsed);
        EXPECT_TRUE(verification.mismatch);
    }
}

TEST_F(FormatConversionPlanTest, VerificationHandlesGifAndNoVideoTargets) {
    ConversionSourceFacts source;
    source.known = true;

    ConversionProfile gif;
    gif.container = ConversionContainer::Gif;
    MediaProbeSnapshot gif_output = MakeOutput(QStringLiteral("gif"), 5.0);
    gif_output.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("gif"), 320, 480));
    EXPECT_FALSE(VerifyConversionOutput(gif, source, gif_output).mismatch);

    // 目标是不要视频，输出却还有视频轨。
    ConversionProfile audio_only;
    audio_only.container = ConversionContainer::Mp3;
    MediaProbeSnapshot wrong = MakeOutput(QStringLiteral("mp3"), 5.0);
    wrong.streams.push_back(MakeStream(MediaStreamKind::Video, QStringLiteral("h264"), 320, 240));
    wrong.streams.push_back(MakeStream(MediaStreamKind::Audio, QStringLiteral("mp3")));
    EXPECT_TRUE(VerifyConversionOutput(audio_only, source, wrong).mismatch);
}

} // namespace
