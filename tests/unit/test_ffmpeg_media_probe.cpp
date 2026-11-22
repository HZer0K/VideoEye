// 「ffmpeg -i 的 stderr → 流参数快照」的纯文本解析测试。
//
// 这一层只做一件事：把 ffmpeg 打印的那几行变成结构化数字。它最容易在两处退化：
//   1) 切段切错 —— 像素格式的 `(tv, bt709, progressive)` 里有逗号，直接 split 会把
//      后两段当成声道布局之类的字段，于是"分辨率正确但帧率错位"这类静默错误；
//   2) 把"读不到"当成 0 —— `N/A kb/s` 记成 0 会让对比图读成"码率归零"。
// 所以每个用例的输入都是从真实 ffmpeg 输出里抄的原文（含 banner、Metadata 块、
// 尾部 disposition），并专门留了流映射行、封面图、探测失败三条负向用例。
//
// 另：`ffmpeg -i <file>` 恒定以退出码 1 结束（"At least one output file must be
// specified"）。这个用例把这句话也一起放进样本里，钉住"退出码 1 不影响解析"。

#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegMediaProbe.h"

namespace {

using videoeye::ffmpeg::BuildMediaProbeArguments;
using videoeye::ffmpeg::DescribeStreamSummary;
using videoeye::ffmpeg::FillLocalFileFacts;
using videoeye::ffmpeg::FormatBitrateHuman;
using videoeye::ffmpeg::FormatDurationHuman;
using videoeye::ffmpeg::FormatFpsHuman;
using videoeye::ffmpeg::FormatResolutionHuman;
using videoeye::ffmpeg::MediaProbeSnapshot;
using videoeye::ffmpeg::MediaStreamKind;
using videoeye::ffmpeg::MediaStreamSnapshot;
using videoeye::ffmpeg::ParseMediaProbe;

const char* const kMp4WithSubtitle =
    "ffmpeg version 8.1.3 Copyright (c) 2000-2025 the FFmpeg developers\n"
    "  built with gcc 14.2.0 (Rev3, Built by MSYS2 project)\n"
    "Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'in.mp4':\n"
    "  Metadata:\n"
    "    major_brand     : isom\n"
    "    minor_version   : 512\n"
    "    compatible_brands: isomiso2avc1mp41\n"
    "    encoder         : Lavf61.7.100\n"
    "  Duration: 00:01:23.45, start: 0.000000, bitrate: 5940 kb/s\n"
    "  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), "
    "yuv420p(tv, bt709, progressive), 1920x1080 [SAR 1:1 DAR 16:9], 5874 kb/s, "
    "29.97 fps, 29.97 tbr, 30k tbn (default)\n"
    "  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 48000 Hz, stereo, fltp, "
    "127 kb/s (default)\n"
    "  Stream #0:2[0x3](und): Subtitle: mov_text (tx3g / 0x67337874), 0 kb/s (default)\n"
    "At least one output file must be specified\n";

TEST(FfmpegMediaProbeTests, ParsesContainerAndDuration) {
    const MediaProbeSnapshot snapshot = ParseMediaProbe(QString::fromUtf8(kMp4WithSubtitle), QStringLiteral("in.mp4"));
    ASSERT_TRUE(snapshot.parsed);
    EXPECT_TRUE(snapshot.error.isEmpty());
    EXPECT_EQ(snapshot.container, QStringLiteral("mov"));
    EXPECT_EQ(snapshot.container_aliases, QStringLiteral("mov,mp4,m4a,3gp,3g2,mj2"));
    EXPECT_NEAR(snapshot.duration_seconds, 83.45, 0.001);
    EXPECT_DOUBLE_EQ(snapshot.start_seconds, 0.0);
    EXPECT_EQ(snapshot.bitrate_bps, 5940000);
}

TEST(FfmpegMediaProbeTests, ParsesVideoStreamFields) {
    const MediaProbeSnapshot snapshot = ParseMediaProbe(QString::fromUtf8(kMp4WithSubtitle));
    ASSERT_EQ(snapshot.streams.size(), 3u);

    const MediaStreamSnapshot& video = snapshot.streams.at(0);
    EXPECT_EQ(video.kind, MediaStreamKind::Video);
    EXPECT_EQ(video.index, 0);
    EXPECT_EQ(video.input_label, QStringLiteral("0:0"));
    EXPECT_EQ(video.language, QStringLiteral("und"));
    EXPECT_EQ(video.codec, QStringLiteral("h264"));
    EXPECT_EQ(video.profile, QStringLiteral("High"));
    EXPECT_EQ(video.pix_fmt, QStringLiteral("yuv420p"));
    EXPECT_EQ(video.width, 1920);
    EXPECT_EQ(video.height, 1080);
    EXPECT_EQ(video.sample_aspect, QStringLiteral("1:1"));
    EXPECT_EQ(video.display_aspect, QStringLiteral("16:9"));
    EXPECT_NEAR(video.fps, 29.97, 0.001);
    EXPECT_EQ(video.bitrate_bps, 5874000);
    EXPECT_FALSE(video.attached_pic);
    EXPECT_TRUE(video.dispositions.contains(QStringLiteral("default")));
    // 原始行要留着给详情区核对，不能只剩解析结果。
    EXPECT_TRUE(video.raw.contains(QStringLiteral("Stream #0:0")));
}

TEST(FfmpegMediaProbeTests, ParsesAudioStreamFields) {
    const MediaProbeSnapshot snapshot = ParseMediaProbe(QString::fromUtf8(kMp4WithSubtitle));
    ASSERT_EQ(snapshot.streams.size(), 3u);

    const MediaStreamSnapshot& audio = snapshot.streams.at(1);
    EXPECT_EQ(audio.kind, MediaStreamKind::Audio);
    EXPECT_EQ(audio.index, 1);
    EXPECT_EQ(audio.codec, QStringLiteral("aac"));
    EXPECT_EQ(audio.profile, QStringLiteral("LC"));
    EXPECT_EQ(audio.sample_rate, 48000);
    EXPECT_EQ(audio.channel_layout, QStringLiteral("stereo"));
    EXPECT_EQ(audio.sample_fmt, QStringLiteral("fltp"));
    EXPECT_EQ(audio.bitrate_bps, 127000);
    EXPECT_EQ(audio.width, 0); // 音频不该被塞进分辨率
}

TEST(FfmpegMediaProbeTests, ParsesSubtitleStream) {
    const MediaProbeSnapshot snapshot = ParseMediaProbe(QString::fromUtf8(kMp4WithSubtitle));
    const MediaStreamSnapshot& subtitle = snapshot.streams.at(2);
    EXPECT_EQ(subtitle.kind, MediaStreamKind::Subtitle);
    EXPECT_EQ(subtitle.codec, QStringLiteral("mov_text"));
    // `(tx3g / 0x67337874)` 是 4CC 标签，不是 profile —— 不能当成 profile = "tx3g / ..."。
    EXPECT_TRUE(subtitle.profile.isEmpty());
    EXPECT_EQ(snapshot.CountStreams(MediaStreamKind::Video), 1);
    EXPECT_EQ(snapshot.CountStreams(MediaStreamKind::Subtitle), 1);
}

TEST(FfmpegMediaProbeTests, NaDurationStaysUnknownInsteadOfZero) {
    const QString text =
        QString::fromUtf8("Input #0, matroska,webm, from 'in.mkv':\n"
                          "  Duration: N/A, start: 0.000000, bitrate: N/A\n"
                          "  Stream #0:0(eng): Video: hevc (Main), yuv420p(tv), 3840x2160, 25 fps, 25 tbr, 1k tbn\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text);
    ASSERT_TRUE(snapshot.parsed);
    EXPECT_EQ(snapshot.duration_seconds, -1.0);
    EXPECT_EQ(snapshot.bitrate_bps, -1);
    EXPECT_EQ(snapshot.streams.size(), 1u);
    EXPECT_EQ(snapshot.streams.at(0).width, 3840);
    EXPECT_NEAR(snapshot.streams.at(0).fps, 25.0, 0.001);
    EXPECT_EQ(snapshot.streams.at(0).language, QStringLiteral("eng"));
}

TEST(FfmpegMediaProbeTests, StreamMappingLineIsNotAStream) {
    // `Stream #0:1 -> #0:0 (...)` 是映射日志。把它当成流定义，一条命令就会凭空多出
    // 几条流，"视频流 1 → 3" 这种假变化全从这来。
    const QString text =
        QString::fromUtf8("Input #0, matroska,webm, from 'in.mkv':\n"
                          "  Duration: 00:00:10.00, start: 0.000000, bitrate: 2000 kb/s\n"
                          "  Stream #0:0(eng): Video: hevc (Main), yuv420p(tv), 3840x2160, 25 fps, 25 tbr, 1k tbn\n"
                          "  Stream #0:1 -> #0:0 (hevc (native) -> h264 (libx264))\n"
                          "  Stream #0:2(eng): Audio: opus, 48000 Hz, 5.1(side), fltp, 320 kb/s\n"
                          "Stream mapping:\n"
                          "  Stream #0:1 -> #0:0 (hevc (native) -> h264 (libx264))\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text);
    ASSERT_TRUE(snapshot.parsed);
    ASSERT_EQ(snapshot.streams.size(), 2u);
    EXPECT_EQ(snapshot.streams.at(1).kind, MediaStreamKind::Audio);
    EXPECT_EQ(snapshot.streams.at(1).channel_layout, QStringLiteral("5.1(side)"));
    EXPECT_EQ(snapshot.streams.at(1).bitrate_bps, 320000);
}

TEST(FfmpegMediaProbeTests, AttachedPicIsMarkedAndExcludedFromPlayableCount) {
    // mp4 的封面图是**一条 video 流**。不单独标出来，"视频流 2 → 1" 会被读成丢了一路视频。
    const QString text =
        QString::fromUtf8("Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'song.mp4':\n"
                          "  Duration: 00:03:20.00, start: 0.000000, bitrate: 320 kb/s\n"
                          "  Stream #0:0(und): Audio: mp3, 44100 Hz, stereo, fltp, 128 kb/s (default)\n"
                          "  Stream #0:1(und): Video: mjpeg (Baseline) (jpeg / 0x6765706a), "
                          "yuvj420p(pc, bt470bg/unknown/unknown), 600x600 [SAR 1:1 DAR 1:1], 90k tbr, 90k tbn "
                          "(attached pic)\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text);
    ASSERT_EQ(snapshot.streams.size(), 2u);
    EXPECT_TRUE(snapshot.streams.at(1).attached_pic);
    EXPECT_EQ(snapshot.CountStreams(MediaStreamKind::Video), 1);
    EXPECT_EQ(snapshot.CountPlayableStreams(MediaStreamKind::Video), 0);
    EXPECT_EQ(snapshot.streams.at(1).pix_fmt, QStringLiteral("yuvj420p"));
}

TEST(FfmpegMediaProbeTests, PcmCodecTagIsNotMistakenForProfile) {
    const QString text =
        QString::fromUtf8("Input #0, wav, from 'a.wav':\n"
                          "  Duration: 00:00:05.00, start: 0.000000, bitrate: 1536 kb/s\n"
                          "  Stream #0:0: Audio: pcm_s16le ([1][0][0][0] / 0x0001), 48000 Hz, stereo, s16, "
                          "1536 kb/s\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text);
    ASSERT_EQ(snapshot.streams.size(), 1u);
    EXPECT_EQ(snapshot.streams.at(0).codec, QStringLiteral("pcm_s16le"));
    EXPECT_TRUE(snapshot.streams.at(0).profile.isEmpty());
    EXPECT_EQ(snapshot.streams.at(0).sample_fmt, QStringLiteral("s16"));
}

TEST(FfmpegMediaProbeTests, UnknownFpsIsZeroNotZeroFrames) {
    const QString text =
        QString::fromUtf8("Input #0, flv, from 'x.flv':\n"
                          "  Duration: 00:00:02.00, start: 0.000000, bitrate: 900 kb/s\n"
                          "  Stream #0:0: Video: h264 (High), yuv420p, 1280x720, 0.00 fps, 0.00 tbr\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text);
    ASSERT_EQ(snapshot.streams.size(), 1u);
    EXPECT_DOUBLE_EQ(snapshot.streams.at(0).fps, 0.0);
    EXPECT_EQ(snapshot.streams.at(0).width, 1280);
}

TEST(FfmpegMediaProbeTests, FailedProbeExplainsWhy) {
    const QString text = QString::fromUtf8("ffmpeg version 8.1.3 Copyright (c) 2000-2025 the FFmpeg developers\n"
                                           "in.mp4: No such file or directory\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text, QStringLiteral("in.mp4"));
    EXPECT_FALSE(snapshot.parsed);
    EXPECT_FALSE(snapshot.error.isEmpty());
    EXPECT_TRUE(snapshot.error.contains(QStringLiteral("不存在")));
    EXPECT_TRUE(snapshot.streams.empty());
}

TEST(FfmpegMediaProbeTests, OnlyFirstInputBlockIsUsed) {
    const QString text = QString::fromUtf8("Input #0, mp3, from 'a.mp3':\n"
                                           "  Duration: 00:00:03.00, start: 0.000000, bitrate: 128 kb/s\n"
                                           "  Stream #0:0: Audio: mp3, 44100 Hz, stereo, fltp, 128 kb/s\n"
                                           "Input #1, h264, from 'b.h264':\n"
                                           "  Duration: N/A, bitrate: N/A\n"
                                           "  Stream #1:0: Video: h264, yuv420p, 640x480, 25 fps\n");
    const MediaProbeSnapshot snapshot = ParseMediaProbe(text);
    ASSERT_TRUE(snapshot.parsed);
    EXPECT_EQ(snapshot.container, QStringLiteral("mp3"));
    ASSERT_EQ(snapshot.streams.size(), 1u);
    EXPECT_EQ(snapshot.streams.at(0).kind, MediaStreamKind::Audio);
}

TEST(FfmpegMediaProbeTests, LocalFileSizeIsMeasuredNotGuessed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("probe.mp4"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(2048, 'x'));
    file.close();

    MediaProbeSnapshot snapshot;
    snapshot.path = path;
    FillLocalFileFacts(&snapshot);
    EXPECT_EQ(snapshot.file_size_bytes, 2048);

    MediaProbeSnapshot url;
    url.path = QStringLiteral("http://example.com/a.mp4");
    FillLocalFileFacts(&url);
    EXPECT_EQ(url.file_size_bytes, -1); // URL 没有"磁盘大小"，不能算成 0

    MediaProbeSnapshot missing;
    missing.path = dir.filePath(QStringLiteral("nope.mp4"));
    FillLocalFileFacts(&missing);
    EXPECT_EQ(missing.file_size_bytes, -1);
}

TEST(FfmpegMediaProbeTests, FormatsAreHumanReadable) {
    EXPECT_EQ(FormatDurationHuman(83.5), QStringLiteral("1:23.5"));
    EXPECT_EQ(FormatDurationHuman(-1.0), QStringLiteral("—"));
    EXPECT_EQ(FormatDurationHuman(3723.0), QStringLiteral("1:02:03.0"));
    EXPECT_EQ(FormatBitrateHuman(5940000), QStringLiteral("5.94 Mbps"));
    EXPECT_EQ(FormatBitrateHuman(127000), QStringLiteral("127 kbps"));
    EXPECT_EQ(FormatBitrateHuman(-1), QStringLiteral("—"));
    EXPECT_EQ(FormatFpsHuman(29.97), QStringLiteral("29.97 fps"));
    EXPECT_EQ(FormatFpsHuman(25.0), QStringLiteral("25 fps"));
    EXPECT_EQ(FormatFpsHuman(0.0), QStringLiteral("—"));
    EXPECT_EQ(FormatResolutionHuman(1920, 1080), QStringLiteral("1920×1080"));
    EXPECT_EQ(FormatResolutionHuman(0, 0), QStringLiteral("—"));
}

TEST(FfmpegMediaProbeTests, StreamSummaryListsCodecAndGeometry) {
    MediaStreamSnapshot video;
    video.kind = MediaStreamKind::Video;
    video.codec = QStringLiteral("h264");
    video.profile = QStringLiteral("High");
    video.width = 1920;
    video.height = 1080;
    video.fps = 30.0;
    video.bitrate_bps = 5000000;
    const QString summary = DescribeStreamSummary(video);
    EXPECT_TRUE(summary.contains(QStringLiteral("h264 High")));
    EXPECT_TRUE(summary.contains(QStringLiteral("1920×1080")));
    EXPECT_TRUE(summary.contains(QStringLiteral("30 fps")));
    EXPECT_TRUE(summary.contains(QStringLiteral("5.00 Mbps")));

    MediaStreamSnapshot empty;
    EXPECT_EQ(DescribeStreamSummary(empty), QStringLiteral("未读到参数"));
}

TEST(FfmpegMediaProbeTests, ProbeArgumentsKeepPathAsSingleToken) {
    // 路径带空格 / 中文也必须是一个 token —— 走参数数组，不经 shell。
    const QStringList args = BuildMediaProbeArguments(QStringLiteral("D:/素材/我的 视频.mp4"));
    ASSERT_EQ(args.size(), 4);
    EXPECT_EQ(args.at(3), QStringLiteral("D:/素材/我的 视频.mp4"));
    EXPECT_TRUE(args.contains(QStringLiteral("-nostdin")));
}

} // namespace
