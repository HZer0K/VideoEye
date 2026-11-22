// 「命令把素材改成了什么」的纯逻辑测试。
//
// 这一层最容易出的错不是崩，而是**悄悄说错**：
//   * 把声明当成实测（"命令写了 scale=1280:-2，所以分辨率变成 1280×720"——高度是 -2 推出来的，
//     命令里根本没写）；
//   * 配对配错（没写 -map 时按类型顺序硬配，结果是 aac → aac "未变"）；
//   * 把被 -map 丢掉的流从图上抹掉（用户最该看到的恰好是"视频没了"）。
// 所以用例的重点是这些边界，而不是"能画出来"。
//
// 样本参数取自真实 ffmpeg 输出（1.6 秒的 192x108 x265 小样、常见 1080p h264 片源），
// 不用随手编的数字 —— 编出来的数字会让"断言写错但测试仍绿"。

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/ffmpeg/FfmpegChangeGraph.h"
#include "core/ffmpeg/FfmpegMediaProbe.h"

namespace {

using videoeye::ffmpeg::BuildChangeGraphs;
using videoeye::ffmpeg::ChangeGraph;
using videoeye::ffmpeg::ChangeState;
using videoeye::ffmpeg::DescribeChange;
using videoeye::ffmpeg::DescribeChangeSummary;
using videoeye::ffmpeg::FieldChange;
using videoeye::ffmpeg::GraphRow;
using videoeye::ffmpeg::MediaProbeSnapshot;
using videoeye::ffmpeg::MediaStreamKind;
using videoeye::ffmpeg::MediaStreamSnapshot;
using videoeye::ffmpeg::ProcessKind;

QStringList Args(const QString& text) {
    return text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

MediaStreamSnapshot VideoStream(int index, const QString& codec, const QString& profile, int width, int height,
                                double fps, qint64 bitrate) {
    MediaStreamSnapshot stream;
    stream.kind = MediaStreamKind::Video;
    stream.kind_label = QStringLiteral("Video");
    stream.index = index;
    stream.input_label = QStringLiteral("0:%1").arg(index);
    stream.codec = codec;
    stream.profile = profile;
    stream.width = width;
    stream.height = height;
    stream.fps = fps;
    stream.bitrate_bps = bitrate;
    stream.pix_fmt = QStringLiteral("yuv420p");
    return stream;
}

MediaStreamSnapshot AudioStream(int index, const QString& codec, const QString& profile, int sample_rate,
                                const QString& layout, qint64 bitrate) {
    MediaStreamSnapshot stream;
    stream.kind = MediaStreamKind::Audio;
    stream.kind_label = QStringLiteral("Audio");
    stream.index = index;
    stream.input_label = QStringLiteral("0:%1").arg(index);
    stream.codec = codec;
    stream.profile = profile;
    stream.sample_rate = sample_rate;
    stream.channel_layout = layout;
    stream.bitrate_bps = bitrate;
    return stream;
}

MediaProbeSnapshot Snapshot(const QString& container, double duration, qint64 bitrate,
                            std::vector<MediaStreamSnapshot> streams) {
    MediaProbeSnapshot snapshot;
    snapshot.parsed = true;
    snapshot.container = container;
    snapshot.container_aliases = container;
    snapshot.duration_seconds = duration;
    snapshot.bitrate_bps = bitrate;
    snapshot.streams = std::move(streams);
    return snapshot;
}

MediaProbeSnapshot Input1080p() {
    MediaProbeSnapshot snapshot = Snapshot(
        QStringLiteral("mov"), 83.45, 5940000,
        {VideoStream(0, QStringLiteral("h264"), QStringLiteral("High"), 1920, 1080, 29.97, 5874000),
         AudioStream(1, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    snapshot.path = QStringLiteral("in.mp4"); // 扩展名对比要用它
    return snapshot;
}

const FieldChange* FindChange(const std::vector<FieldChange>& changes, const QString& key) {
    for (const FieldChange& change : changes) {
        if (change.key == key) {
            return &change;
        }
    }
    return nullptr;
}

const GraphRow* FindRow(const ChangeGraph& graph, MediaStreamKind kind, int ordinal) {
    int seen = 0;
    for (const GraphRow& row : graph.rows) {
        if (row.kind != kind) {
            continue;
        }
        if (seen == ordinal) {
            return &row;
        }
        ++seen;
    }
    return nullptr;
}

// ---- 声明 + 实测：正常重编码 ----

TEST(FfmpegChangeGraphTests, TranscodeShowsDeclaredPipeAndMeasuredDiff) {
    const MediaProbeSnapshot output = Snapshot(
        QStringLiteral("mov"), 83.45, 1300000,
        {VideoStream(0, QStringLiteral("hevc"), QStringLiteral("Main"), 1280, 720, 29.97, 1100000),
         AudioStream(1, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -vf scale=1280:-2 -c:v libx265 -crf 28 -c:a copy out.mp4"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {output});
    ASSERT_EQ(graphs.size(), 1u);
    const ChangeGraph& graph = graphs.at(0);
    ASSERT_TRUE(graph.ok);
    EXPECT_TRUE(graph.output_measured);
    EXPECT_EQ(graph.target, QStringLiteral("out.mp4"));
    EXPECT_EQ(graph.container_before, QStringLiteral("mov"));
    ASSERT_EQ(graph.rows.size(), 2u);

    const GraphRow* video = FindRow(graph, MediaStreamKind::Video, 0);
    ASSERT_NE(video, nullptr);
    // 工序来自声明：滤镜链与编码器都能说清
    EXPECT_EQ(video->filter_labels, QStringList{QStringLiteral("scale 1280:-2")});
    EXPECT_EQ(video->encoder_label, QStringLiteral("libx265"));
    EXPECT_EQ(video->encoder_kind, ProcessKind::Encoder);
    EXPECT_TRUE(video->encoder_detail.contains(QStringLiteral("crf 28")));
    EXPECT_EQ(video->state, ChangeState::Changed);

    // 字段级对比只来自两侧实测
    ASSERT_NE(FindChange(video->changes, QStringLiteral("codec")), nullptr);
    const FieldChange* codec = FindChange(video->changes, QStringLiteral("codec"));
    EXPECT_EQ(codec->state, ChangeState::Changed);
    EXPECT_EQ(codec->before, QStringLiteral("h264 High"));
    EXPECT_EQ(codec->after, QStringLiteral("hevc Main"));
    const FieldChange* size = FindChange(video->changes, QStringLiteral("size"));
    ASSERT_NE(size, nullptr);
    EXPECT_EQ(size->state, ChangeState::Changed);
    EXPECT_EQ(size->before, QStringLiteral("1920×1080"));
    EXPECT_EQ(size->after, QStringLiteral("1280×720"));
    const FieldChange* fps = FindChange(video->changes, QStringLiteral("fps"));
    ASSERT_NE(fps, nullptr);
    EXPECT_EQ(fps->state, ChangeState::Unchanged); // 29.97 → 29.97 不该亮成"变了"

    const GraphRow* audio = FindRow(graph, MediaStreamKind::Audio, 0);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->encoder_label, QStringLiteral("copy"));
    EXPECT_EQ(audio->encoder_kind, ProcessKind::Copy);
    EXPECT_TRUE(audio->filter_labels.isEmpty());
    EXPECT_EQ(audio->state, ChangeState::Unchanged); // copy：参数原样
}

TEST(FfmpegChangeGraphTests, ContainerLevelChangesAreReported) {
    const MediaProbeSnapshot output =
        Snapshot(QStringLiteral("matroska,webm"), 63.45, 1200000,
                 {VideoStream(0, QStringLiteral("hevc"), QStringLiteral("Main"), 1280, 720, 29.97, 1000000),
                  AudioStream(1, QStringLiteral("opus"), QString(), 48000, QStringLiteral("stereo"), 96000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -c:v libx265 -c:a libopus out.mkv"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {output});
    ASSERT_EQ(graphs.size(), 1u);
    const ChangeGraph& graph = graphs.at(0);
    ASSERT_TRUE(graph.output_measured);

    const FieldChange* container = FindChange(graph.container_changes, QStringLiteral("container"));
    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->state, ChangeState::Changed); // demuxer 换了
    const FieldChange* duration = FindChange(graph.container_changes, QStringLiteral("duration"));
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(duration->state, ChangeState::Changed);
    const FieldChange* counts = FindChange(graph.container_changes, QStringLiteral("stream_count"));
    ASSERT_NE(counts, nullptr);
    EXPECT_EQ(counts->state, ChangeState::Unchanged);
    EXPECT_EQ(counts->after, QStringLiteral("1 视频 / 1 音频 / 0 字幕"));
}

TEST(FfmpegChangeGraphTests, SmallDurationDriftIsNotReportedAsChanged) {
    // 重编码会带来几十毫秒的尾音 / 填充差。把 83.45 → 83.47 报成"时长变了"，
    // 每次重编码都会亮一片无意义的高亮，用户很快就学会忽略整张图。
    const MediaProbeSnapshot output = Snapshot(
        QStringLiteral("mov"), 83.47, 5900000,
        {VideoStream(0, QStringLiteral("h264"), QStringLiteral("High"), 1920, 1080, 29.97, 5874000),
         AudioStream(1, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -c copy out.mp4"));
    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {output});
    ASSERT_EQ(graphs.size(), 1u);
    const FieldChange* duration = FindChange(graphs.at(0).container_changes, QStringLiteral("duration"));
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(duration->state, ChangeState::Unchanged);
    // 但真被裁掉一段（-ss/-t）时必须报出来
    const MediaProbeSnapshot trimmed = Snapshot(
        QStringLiteral("mov"), 20.05, 5900000,
        {VideoStream(0, QStringLiteral("h264"), QStringLiteral("High"), 1920, 1080, 29.97, 5874000),
         AudioStream(1, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const std::vector<ChangeGraph> trimmed_graphs =
        BuildChangeGraphs(Input1080p(), Args(QStringLiteral("-i in.mp4 -ss 10 -t 20 -c copy out.mp4")), {trimmed});
    ASSERT_EQ(trimmed_graphs.size(), 1u);
    const FieldChange* trimmed_duration =
        FindChange(trimmed_graphs.at(0).container_changes, QStringLiteral("duration"));
    ASSERT_NE(trimmed_duration, nullptr);
    EXPECT_EQ(trimmed_duration->state, ChangeState::Changed);
}

TEST(FfmpegChangeGraphTests, ValuesThatPrintIdenticallyAreNotReportedAsChanged) {
    // 时长 1.00 → 1.04 在 0.1 秒的显示精度下是同一串 "0:01.0"。判成"已变"会让用户
    // 看到一条 "0:01.0 → 0:01.0" 的高亮 —— 从此这张图上的高亮都要打折扣。
    MediaProbeSnapshot input =
        Snapshot(QStringLiteral("mov"), 1.00, 483000,
                 {VideoStream(0, QStringLiteral("mpeg4"), QStringLiteral("Simple Profile"), 320, 240, 25.0, 395000)});
    input.path = QStringLiteral("in.mp4");
    const MediaProbeSnapshot output =
        Snapshot(QStringLiteral("mov"), 1.04, 483000,
                 {VideoStream(0, QStringLiteral("mpeg4"), QStringLiteral("Simple Profile"), 320, 240, 25.0, 395000)});
    const std::vector<ChangeGraph> graphs =
        BuildChangeGraphs(input, Args(QStringLiteral("-i in.mp4 -c copy out.mp4")), {output});
    ASSERT_EQ(graphs.size(), 1u);
    const FieldChange* duration = FindChange(graphs.at(0).container_changes, QStringLiteral("duration"));
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(duration->before, duration->after);
    EXPECT_EQ(duration->state, ChangeState::Unchanged);
}

TEST(FfmpegChangeGraphTests, SameDemuxerDifferentExtensionIsCalledOut) {
    // mp4 与 mov 是同一个 demuxer：容器"没变"是对的，但扩展名变了要说明白，
    // 否则用户会以为命令根本没生效。
    const MediaProbeSnapshot output = Snapshot(
        QStringLiteral("mov"), 83.45, 5900000,
        {VideoStream(0, QStringLiteral("h264"), QStringLiteral("High"), 1920, 1080, 29.97, 5874000),
         AudioStream(1, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -c copy out.mov"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {output});
    ASSERT_EQ(graphs.size(), 1u);
    const FieldChange* container = FindChange(graphs.at(0).container_changes, QStringLiteral("container"));
    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->state, ChangeState::Unchanged);
    EXPECT_TRUE(container->note.contains(QStringLiteral("扩展名")));
    EXPECT_TRUE(container->note.contains(QStringLiteral("mp4")));
    EXPECT_TRUE(container->note.contains(QStringLiteral("mov")));
}

// ---- 没探测到输出：只有工序预览，不许编对比 ----

TEST(FfmpegChangeGraphTests, WithoutOutputProbeThereAreNoFieldChanges) {
    const QStringList args = Args(QStringLiteral("-i in.mp4 -c:v libx265 out.mp4"));
    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {});
    ASSERT_EQ(graphs.size(), 1u);
    const ChangeGraph& graph = graphs.at(0);
    ASSERT_TRUE(graph.ok);
    EXPECT_FALSE(graph.output_measured);
    EXPECT_TRUE(graph.container_changes.empty());

    ASSERT_EQ(graph.rows.size(), 2u);
    for (const GraphRow& row : graph.rows) {
        EXPECT_TRUE(row.changes.empty()); // 没有实测就没有字段对比
        EXPECT_TRUE(row.output_summary.isEmpty());
        EXPECT_TRUE(row.note.contains(QStringLiteral("未探测")));
    }
    const GraphRow* video = FindRow(graph, MediaStreamKind::Video, 0);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->encoder_label, QStringLiteral("libx265")); // 声明仍然画得出来
    EXPECT_EQ(video->state, ChangeState::Unknown);              // 但状态是"不知道"
    EXPECT_TRUE(DescribeChangeSummary(graph).contains(QStringLiteral("未探测")));
    EXPECT_TRUE(graph.pairing_note.isEmpty()); // 没输出就别提"配对"
}

TEST(FfmpegChangeGraphTests, FailedOutputProbeSurfacesTheReason) {
    MediaProbeSnapshot failed;
    failed.parsed = false;
    failed.error = QStringLiteral("文件不存在，或路径写错了");
    const QStringList args = Args(QStringLiteral("-i in.mp4 out.mp4"));
    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {failed});
    ASSERT_EQ(graphs.size(), 1u);
    EXPECT_FALSE(graphs.at(0).output_measured);
    EXPECT_EQ(graphs.at(0).output_error, QStringLiteral("文件不存在，或路径写错了"));
    EXPECT_TRUE(DescribeChangeSummary(graphs.at(0)).contains(QStringLiteral("探测失败")));
}

// ---- 丢弃 / 只映射部分流 ----

TEST(FfmpegChangeGraphTests, DeclaredDropShowsAsRemovedRow) {
    const MediaProbeSnapshot audio_only = Snapshot(
        QStringLiteral("ipod"), 83.45, 127000,
        {AudioStream(0, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -vn -c:a copy out.m4a"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {audio_only});
    ASSERT_EQ(graphs.size(), 1u);
    const ChangeGraph& graph = graphs.at(0);
    const GraphRow* video = FindRow(graph, MediaStreamKind::Video, 0);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->state, ChangeState::Removed);
    EXPECT_EQ(video->encoder_kind, ProcessKind::Drop);
    EXPECT_EQ(video->encoder_label, QStringLiteral("丢弃"));
    EXPECT_TRUE(video->note.contains(QStringLiteral("丢弃")));
    EXPECT_EQ(graph.CountRows(ChangeState::Removed), 1);

    const GraphRow* audio = FindRow(graph, MediaStreamKind::Audio, 0);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->state, ChangeState::Unchanged);
}

TEST(FfmpegChangeGraphTests, MapSelectingOnlyAudioKeepsVideoRowAsRemoved) {
    // `-map 0:a` 只带走音频。被丢掉的视频必须仍在图上（标"未被 -map 选中"），
    // 否则用户看到的是一张"什么都没少"的图。
    const MediaProbeSnapshot audio_only = Snapshot(
        QStringLiteral("ipod"), 83.45, 127000,
        {AudioStream(0, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -map 0:a -c:a copy out.m4a"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {audio_only});
    ASSERT_EQ(graphs.size(), 1u);
    const ChangeGraph& graph = graphs.at(0);
    EXPECT_EQ(graph.pairing_note, QStringLiteral("按 -map 配对"));

    const GraphRow* video = FindRow(graph, MediaStreamKind::Video, 0);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->state, ChangeState::Removed);
    EXPECT_TRUE(video->note.contains(QStringLiteral("-map")));
    EXPECT_EQ(video->output_index, -1);

    const GraphRow* audio = FindRow(graph, MediaStreamKind::Audio, 0);
    ASSERT_NE(audio, nullptr);
    EXPECT_GE(audio->output_index, 0);
}

TEST(FfmpegChangeGraphTests, UninterpretableMapFallsBackWithAnHonestNote) {
    const MediaProbeSnapshot output =
        Snapshot(QStringLiteral("mov"), 83.45, 5900000,
                 {VideoStream(0, QStringLiteral("h264"), QStringLiteral("High"), 1920, 1080, 29.97, 5874000)});
    // `-map [v]` 是滤镜输出标签，无法反推输入流 —— 必须退化成"按类型配对"并说明白。
    const QStringList args = Args(QStringLiteral("-i in.mp4 -filter_complex [0:v]scale=1280:-2[v] -map [v] out.mp4"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {output});
    ASSERT_EQ(graphs.size(), 1u);
    const ChangeGraph& graph = graphs.at(0);
    EXPECT_TRUE(graph.pairing_note.contains(QStringLiteral("按类型顺序配对")));
    EXPECT_TRUE(graph.has_complex_filter);
}

// ---- 多输出 / 非转码命令 / 坏输入 ----

TEST(FfmpegChangeGraphTests, MultiOutputGetsOneGraphPerOutputWithCaveat) {
    const MediaProbeSnapshot first =
        Snapshot(QStringLiteral("mov"), 83.45, 5900000,
                 {VideoStream(0, QStringLiteral("h264"), QStringLiteral("High"), 1920, 1080, 29.97, 5874000)});
    const MediaProbeSnapshot second = first;
    const QStringList args = Args(QStringLiteral("-i in.mp4 out1.mp4 out2.mp4"));

    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {first, second});
    ASSERT_EQ(graphs.size(), 2u);
    EXPECT_EQ(graphs.at(0).target, QStringLiteral("out1.mp4"));
    EXPECT_EQ(graphs.at(1).target, QStringLiteral("out2.mp4"));
    for (const ChangeGraph& graph : graphs) {
        EXPECT_TRUE(graph.note.contains(QStringLiteral("多输出")));
    }
}

TEST(FfmpegChangeGraphTests, InformationCommandHasNoGraph) {
    EXPECT_TRUE(BuildChangeGraphs(Input1080p(), Args(QStringLiteral("-version")), {}).empty());
    EXPECT_TRUE(BuildChangeGraphs(Input1080p(), Args(QStringLiteral("-encoders")), {}).empty());
}

TEST(FfmpegChangeGraphTests, CommandWithoutOutputExplainsItself) {
    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), Args(QStringLiteral("-i in.mp4")), {});
    ASSERT_EQ(graphs.size(), 1u);
    EXPECT_FALSE(graphs.at(0).ok);
    EXPECT_TRUE(graphs.at(0).note.contains(QStringLiteral("没有输出")));
}

TEST(FfmpegChangeGraphTests, UnprobedInputExplainsItself) {
    MediaProbeSnapshot input;
    input.parsed = false;
    input.error = QStringLiteral("文件不存在，或路径写错了");
    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(input, Args(QStringLiteral("-i in.mp4 out.mp4")), {});
    ASSERT_EQ(graphs.size(), 1u);
    EXPECT_FALSE(graphs.at(0).ok);
    EXPECT_EQ(graphs.at(0).note, QStringLiteral("文件不存在，或路径写错了"));
}

// ---- 文案 ----

TEST(FfmpegChangeGraphTests, ChangeTextMarksMeasuredAndUnknown) {
    FieldChange changed;
    changed.label = QStringLiteral("编码");
    changed.before = QStringLiteral("h264");
    changed.after = QStringLiteral("hevc");
    changed.state = ChangeState::Changed;
    EXPECT_EQ(DescribeChange(changed), QStringLiteral("编码：h264 → hevc"));

    FieldChange unknown;
    unknown.label = QStringLiteral("码率");
    unknown.after = QStringLiteral("1.10 Mbps");
    unknown.state = ChangeState::Unknown;
    EXPECT_TRUE(DescribeChange(unknown).contains(QStringLiteral("未读到")));
}

TEST(FfmpegChangeGraphTests, SummaryCountsChangedUnchangedAndUnknownSeparately) {
    const MediaProbeSnapshot output = Snapshot(
        QStringLiteral("mov"), 83.45, 1300000,
        {VideoStream(0, QStringLiteral("hevc"), QStringLiteral("Main"), 1280, 720, 29.97, 1100000),
         AudioStream(1, QStringLiteral("aac"), QStringLiteral("LC"), 48000, QStringLiteral("stereo"), 127000)});
    const QStringList args = Args(QStringLiteral("-i in.mp4 -c:v libx265 -c:a copy out.mp4"));
    const std::vector<ChangeGraph> graphs = BuildChangeGraphs(Input1080p(), args, {output});
    ASSERT_EQ(graphs.size(), 1u);
    const QString summary = DescribeChangeSummary(graphs.at(0));
    // 不能把"未变"和"已变"合成一个总数 —— 那会让"3 项都变了"和"1 变 2 未变"看着一样。
    EXPECT_TRUE(summary.contains(QStringLiteral("处变化")));
    EXPECT_TRUE(summary.contains(QStringLiteral("处未变")));
}

} // namespace
