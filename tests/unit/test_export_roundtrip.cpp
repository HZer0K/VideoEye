// 导出 round-trip 回归：**真实语料 + 序列级取证**
//
// 现有 test_media_exporter.cpp 的 18 个用例全部跑在 128x72 / 40 帧的合成小样本上，
// 产物的断言粒度是「能打开、帧数在 ±1、时长在 0.7~1.4 倍」。这挡不住三类错误：
//   * 包顺序被改写、PTS 回退，帧数却一模一样
//   * 区间裁剪没真的裁，或者裁出一段不连续的包
//   * 包是对的、解出来的画面不是同一批（流映射 / 时基写错）
//
// 这里用仓库自带的真实语料（tests/corpus/container/real_*.mp4）跑 remux 与区间
// 裁剪，再用 MediaSequenceFingerprint 逐包 / 逐帧配对。可选的外部语料批量用例
// 由 VE_EXPORT_CORPUS_DIR 打开，CI 没这个目录时跳过，不影响门禁。
//
// 守护原则与项目其它门禁一致：没有事实时不得报「一致」，所以取证模块自己和
// 下面 CompareGuards* 用例都钉死了「空输入 / 换序 / 回退必须判红」。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "core/exporter/MediaExporter.h"
#include "tests/support/MediaSequenceFingerprint.h"

#ifndef VIDEOEYE_CONTAINER_CORPUS_DIR
#define VIDEOEYE_CONTAINER_CORPUS_DIR ""
#endif

namespace {

using namespace videoeye;

struct AppScope {
    QCoreApplication* app = nullptr;
    bool owned = false;

    AppScope() {
        static int argc = 1;
        static char arg0[] = "test_export_roundtrip";
        static char* argv[] = {arg0, nullptr};
        app = QCoreApplication::instance();
        if (!app) {
            app = new QCoreApplication(argc, argv);
            owned = true;
        }
    }
    ~AppScope() {
        if (owned)
            delete app;
    }
    AppScope(const AppScope&) = delete;
    AppScope& operator=(const AppScope&) = delete;
};

struct ExportRun {
    bool finished = false;
    bool error = false;
    bool canceled = false;
    QString error_msg;
};

ExportRun RunExport(const exporter::ExportOptions& opt) {
    ExportRun run;
    exporter::MediaExporter exporter;
    QObject::connect(&exporter, &exporter::MediaExporter::ExportFinished,
                     [&run](const QString&) { run.finished = true; });
    QObject::connect(&exporter, &exporter::MediaExporter::ExportCanceled,
                     [&run](const QString&) { run.canceled = true; });
    QObject::connect(&exporter, &exporter::MediaExporter::ExportError, [&run](const QString& message) {
        run.error = true;
        run.error_msg = message;
    });
    exporter.Export(opt); // 同步执行，信号直连
    return run;
}

struct WorkDir {
    QTemporaryDir dir;
    bool valid() const {
        return dir.isValid();
    }
    QString path() const {
        return dir.path();
    }
    std::string stdPath() const {
        return dir.path().toStdString();
    }
    QStringList TempResidue() const {
        QStringList residue;
        for (const QString& name : QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot)) {
            if (name.contains(".part-") || name.contains(".bak-"))
                residue << name;
        }
        return residue;
    }
};

exporter::ExportOptions MakeOptions(const QString& input, const QString& output, const QString& format = "mp4") {
    exporter::ExportOptions opt;
    opt.input_path = input;
    opt.output_path = output;
    opt.format = format;
    opt.kind = exporter::ExportKind::Video;
    return opt;
}

std::string CorpusPath(const char* name) {
    return std::string(VIDEOEYE_CONTAINER_CORPUS_DIR) + "/" + name;
}

bool CorpusAvailable(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 1024;
}

QString FileDigest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        if (!out.empty())
            out += "\n";
        out += line;
    }
    return out;
}

void ExpectSequenceClean(const char* context, const std::vector<std::string>& diffs) {
    EXPECT_TRUE(diffs.empty()) << context << "\n" << Join(diffs);
}

// 只用于守护用例：造一条有 3 个包、PTS 递增、带帧哈希的视频流 + 1 条音频流。
videoeye_test::MediaSequence MakeSyntheticSequence(int first_pts, bool pts_regression, bool swap_packets,
                                                   const std::string& frame_hash) {
    videoeye_test::MediaSequence seq;
    seq.opened = true;
    seq.streams.resize(2);
    seq.streams[0].index = 0;
    seq.streams[0].codec_type = "video";
    seq.streams[0].codec_name = "h264";
    seq.streams[0].decoded_frames = 3;
    seq.streams[0].display_pts_samples = 3;
    seq.streams[0].frame_sequence_hash = frame_hash;
    seq.streams[1].index = 1;
    seq.streams[1].codec_type = "audio";
    seq.streams[1].codec_name = "aac";

    for (int i = 0; i < 3; ++i) {
        videoeye_test::PacketFacts p;
        p.has_pts = true;
        p.has_dts = true;
        p.pts = first_pts + i * 40;
        p.dts = first_pts + i * 40;
        p.size = 100 + i;
        p.payload_hash = 1000 + static_cast<uint64_t>(i);
        seq.streams[0].packets.push_back(p);
    }
    if (pts_regression) {
        seq.streams[0].packets[2].dts -= 100;
        seq.streams[0].dts_monotonic = false;
        seq.streams[0].display_pts_monotonic = false;
    }
    if (swap_packets)
        std::swap(seq.streams[0].packets[0], seq.streams[0].packets[1]);

    uint64_t rolling = videoeye_test::Fnv1a64(14695981039346656037ULL, nullptr, 0);
    for (const auto& p : seq.streams[0].packets)
        rolling = videoeye_test::Fnv1a64(rolling, &p.payload_hash, sizeof(p.payload_hash));
    seq.streams[0].payload_sequence_hash = videoeye_test::HexHash(rolling);
    for (int i = 0; i < 2; ++i) {
        videoeye_test::PacketFacts a;
        a.has_pts = true;
        a.has_dts = true;
        a.pts = first_pts + i * 1024;
        a.dts = a.pts;
        a.size = 200 + i;
        a.payload_hash = 5000 + static_cast<uint64_t>(i);
        seq.streams[1].packets.push_back(a);
    }
    uint64_t audio_rolling = videoeye_test::Fnv1a64(14695981039346656037ULL, nullptr, 0);
    for (const auto& p : seq.streams[1].packets)
        audio_rolling = videoeye_test::Fnv1a64(audio_rolling, &p.payload_hash, sizeof(p.payload_hash));
    seq.streams[1].payload_sequence_hash = videoeye_test::HexHash(audio_rolling);
    seq.total_packets = 5;
    return seq;
}

quint32 ReadBe32(const QByteArray& bytes, int offset) {
    return (static_cast<quint32>(static_cast<uchar>(bytes.at(offset))) << 24) |
           (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 1))) << 16) |
           (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 2))) << 8) |
           static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 3)));
}

void WriteBe32(QByteArray& bytes, int offset, quint32 value) {
    bytes[offset] = static_cast<char>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<char>(value & 0xFF);
}

// 把 mvhd / mdhd / tkhd 的声明时长统一改成 1/3，制造「元数据少报」样本。
// 只动元数据，码流和样本表原样保留：FFmpeg 仍读得出全部 30 个视频包
// （最大 PTS 1.16 s），但容器只声明 0.407 s。这正是外部语料
// fragmented_captions_h265.mp4 触发尾包丢失的形态，用仓内样本即可自包含复现。
QString MakeUnderReportedDurationSample(const QString& source, const QString& target) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly))
        return QString();
    QByteArray bytes = in.readAll();
    in.close();

    const auto patch = [&bytes](const char* tag, int duration_offset) {
        const QByteArray needle(tag);
        int from = 0;
        int patched = 0;
        while (true) {
            const int i = bytes.indexOf(needle, from);
            if (i < 0)
                break;
            const int box = i - 4;
            from = i + 4;
            if (box < 0 || box + duration_offset + 4 > bytes.size() ||
                static_cast<uchar>(bytes.at(box + 8)) != 0) // version 0 才是 32 位时长
                continue;
            const quint32 original = ReadBe32(bytes, box + duration_offset);
            WriteBe32(bytes, box + duration_offset, original / 3);
            ++patched;
        }
        return patched;
    };

    const int mvhd = patch("mvhd", 24);
    const int mdhd = patch("mdhd", 24);
    const int tkhd = patch("tkhd", 28);
    if (mvhd == 0 || mdhd == 0 || tkhd == 0)
        return QString();

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size())
        return QString();
    return target;
}

} // namespace

// ---------------------------------------------------------------------------
// 1) 真实语料 remux：逐包载荷序列、PTS/DTS 间隔、逐帧亮度序列必须全部一致。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, RealCorpusRemuxPreservesPacketAndFrameSequences) {
    AppScope app;
    const std::string source = CorpusPath("real_h264_aac_faststart.mp4");
    if (!CorpusAvailable(source))
        GTEST_SKIP() << "仓内真实语料缺失: " << source;

    const auto expected = videoeye_test::CaptureMediaSequence(source);
    ASSERT_TRUE(videoeye_test::HasDecodableVideo(expected)) << "源样本自身必须能全帧解码: " << expected.error;
    ASSERT_GE(expected.total_packets, 50u) << "源样本包数过少，覆盖守护失效";
    ASSERT_EQ(expected.streams.size(), 2u);
    ASSERT_FALSE(expected.streams[0].payload_sequence_hash.empty());

    WorkDir work;
    ASSERT_TRUE(work.valid());
    const QString output = work.path() + "/remux.mp4";
    const auto run = RunExport(MakeOptions(QString::fromStdString(source), output));
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();
    ASSERT_FALSE(run.error) << run.error_msg.toStdString();

    const auto actual = videoeye_test::CaptureMediaSequence(output.toStdString());
    ExpectSequenceClean("真实语料 remux 序列对比", videoeye_test::CompareExactSequence(expected, actual));
    EXPECT_TRUE(videoeye_test::HasDecodableVideo(actual)) << actual.error;
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// ---------------------------------------------------------------------------
// 2) 真实语料区间裁剪：产物必须是源包序列的连续子序列，不是「随便少几帧」。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, RealCorpusRangeTrimIsContiguousSubsequence) {
    AppScope app;
    const std::string source = CorpusPath("real_h264_aac_faststart.mp4");
    if (!CorpusAvailable(source))
        GTEST_SKIP() << "仓内真实语料缺失: " << source;

    const auto expected = videoeye_test::CaptureMediaSequence(source);
    ASSERT_TRUE(videoeye_test::HasDecodableVideo(expected)) << expected.error;

    WorkDir work;
    ASSERT_TRUE(work.valid());
    const QString output = work.path() + "/trim.mp4";
    exporter::ExportOptions opt = MakeOptions(QString::fromStdString(source), output);
    opt.start_ms = 200;
    opt.end_ms = 800;
    const auto run = RunExport(opt);
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();

    const auto actual = videoeye_test::CaptureMediaSequence(output.toStdString());
    ExpectSequenceClean("真实语料区间裁剪", videoeye_test::CompareSubsequence(expected, actual));
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// ---------------------------------------------------------------------------
// 3) fMP4 真实语料也走一遍 remux，覆盖分片容器的导出路径。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, FragmentedRealCorpusRemuxPreservesSequences) {
    AppScope app;
    const std::string source = CorpusPath("real_h264_aac_frag.mp4");
    if (!CorpusAvailable(source))
        GTEST_SKIP() << "仓内 fMP4 语料缺失: " << source;

    const auto expected = videoeye_test::CaptureMediaSequence(source);
    ASSERT_TRUE(videoeye_test::HasDecodableVideo(expected)) << expected.error;
    ASSERT_GE(expected.total_packets, 50u);

    WorkDir work;
    ASSERT_TRUE(work.valid());
    const QString output = work.path() + "/frag_remux.mp4";
    const auto run = RunExport(MakeOptions(QString::fromStdString(source), output));
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();

    const auto actual = videoeye_test::CaptureMediaSequence(output.toStdString());
    ExpectSequenceClean("fMP4 remux 序列对比", videoeye_test::CompareExactSequence(expected, actual));
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// ---------------------------------------------------------------------------
// 4) 源片不可被改写：output==input 必须拒绝，正常导出前后源字节完全一致。
//    历史事故是「原子替换方向写反，原片被永久替换还报成功」，合成小样本那
//    条用例挡不住真实语料上的路径别名。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, RealCorpusSourceBytesNeverChange) {
    AppScope app;
    const std::string source = CorpusPath("real_h264_aac_faststart.mp4");
    if (!CorpusAvailable(source))
        GTEST_SKIP() << "仓内真实语料缺失: " << source;

    WorkDir work;
    ASSERT_TRUE(work.valid());
    // 复制一份到临时目录再验，绝不拿仓内语料冒险。
    const QString input = work.path() + "/source.mp4";
    ASSERT_TRUE(QFile::copy(QString::fromStdString(source), input));
    const QString before = FileDigest(input);
    ASSERT_FALSE(before.isEmpty());

    const auto refused = RunExport(MakeOptions(input, input));
    EXPECT_FALSE(refused.finished);
    EXPECT_TRUE(refused.error) << "输出路径 == 输入路径必须被拒绝";
    EXPECT_EQ(FileDigest(input), before) << "被拒后源片字节不得变化";

    const QString output = work.path() + "/copy.mp4";
    const auto run = RunExport(MakeOptions(input, output));
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();
    EXPECT_EQ(FileDigest(input), before) << "导出成功后源片字节不得变化";
    EXPECT_FALSE(FileDigest(output).isEmpty());
    EXPECT_NE(FileDigest(output), before);
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// ---------------------------------------------------------------------------
// 5) 元数据少报时长时，全片导出不得丢尾包。
//    拷贝模式曾无条件拿「容器声明时长」当终点阈值，尾包 PTS 略超就被整包丢掉，
//    还提前终止读取循环；编码模式的 FrameOutside 早有 requested_end_ms 守护。
//    这里用上面那份字节补丁样本自包含复现，不依赖外部语料。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, ShortDeclaredDurationDoesNotDropTailPackets) {
    AppScope app;
    const std::string source = CorpusPath("real_h264_aac_faststart.mp4");
    if (!CorpusAvailable(source))
        GTEST_SKIP() << "仓内真实语料缺失: " << source;

    WorkDir work;
    ASSERT_TRUE(work.valid());
    const QString patched = work.path() + "/short_duration.mp4";
    ASSERT_FALSE(MakeUnderReportedDurationSample(QString::fromStdString(source), patched).isEmpty());

    const auto expected = videoeye_test::CaptureMediaSequence(patched.toStdString());
    ASSERT_TRUE(videoeye_test::HasDecodableVideo(expected)) << expected.error;
    ASSERT_GE(expected.total_packets, 50u);
    // 前置守护：补丁必须真的造成「声明时长 < 尾包 PTS」，否则用例没有判别力。
    ASSERT_LT(expected.duration_seconds, 0.6) << "mvhd/mdhd/tkhd 补丁未生效";
    double max_pts_seconds = 0.0;
    for (const auto& stream : expected.streams) {
        if (stream.time_base_den <= 0)
            continue;
        for (const auto& packet : stream.packets) {
            if (packet.has_pts)
                max_pts_seconds = std::max(max_pts_seconds, packet.pts * static_cast<double>(stream.time_base_num) /
                                                                stream.time_base_den);
        }
    }
    ASSERT_GT(max_pts_seconds, expected.duration_seconds + 0.2)
        << "尾包 PTS 必须明显超出声明时长（实测 " << max_pts_seconds << " vs " << expected.duration_seconds << "）";

    const QString output = work.path() + "/whole.mp4";
    const auto run = RunExport(MakeOptions(patched, output)); // 不指定 start / end
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();
    const auto actual = videoeye_test::CaptureMediaSequence(output.toStdString());
    ExpectSequenceClean("元数据少报时长的全片导出", videoeye_test::CompareExactSequence(expected, actual));
    EXPECT_EQ(actual.total_packets, expected.total_packets) << "尾包不得被裁掉";
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// ---------------------------------------------------------------------------
// 6) 反向守护：取证/对比机制本身「坏了看不出来」是最危险的。空事实、换序、
//    PTS 回退、画面被改写、裁剪不连续，每一种都必须精确报错。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, CompareGuardsRejectEmptyReorderedAndRegressedFacts) {
    const auto base = MakeSyntheticSequence(0, false, false, "abc123");

    ExpectSequenceClean("自比对应无差异", videoeye_test::CompareExactSequence(base, base));

    videoeye_test::MediaSequence empty;
    EXPECT_FALSE(videoeye_test::CompareExactSequence(base, empty).empty()) << "空产物必须判红";
    EXPECT_FALSE(videoeye_test::CompareExactSequence(empty, base).empty()) << "空源必须判红";

    const auto swapped = MakeSyntheticSequence(0, false, true, "abc123");
    const auto swap_diffs = videoeye_test::CompareExactSequence(base, swapped);
    EXPECT_FALSE(swap_diffs.empty()) << "换序必须判红";
    EXPECT_NE(Join(swap_diffs).find("逐包载荷序列"), std::string::npos) << Join(swap_diffs);

    const auto regressed = MakeSyntheticSequence(0, true, false, "abc123");
    const auto regression_diffs = videoeye_test::CompareExactSequence(base, regressed);
    EXPECT_FALSE(regression_diffs.empty()) << "显示顺序 / DTS 回退必须判红";
    EXPECT_NE(Join(regression_diffs).find("显示顺序"), std::string::npos) << Join(regression_diffs);

    const auto wrong_picture = MakeSyntheticSequence(0, false, false, "different-frame-hash");
    const auto frame_diffs = videoeye_test::CompareExactSequence(base, wrong_picture);
    EXPECT_FALSE(frame_diffs.empty()) << "画面被改写必须判红";
    EXPECT_NE(Join(frame_diffs).find("逐帧亮度序列"), std::string::npos) << Join(frame_diffs);

    // 时基整体平移是合法的（换容器会改起始 pts），不该误报。
    const auto shifted = MakeSyntheticSequence(1000, false, false, "abc123");
    ExpectSequenceClean("整体时基平移", videoeye_test::CompareExactSequence(base, shifted));

    // B 帧流的包级 PTS 天然锯齿（解码顺序 ≠ 显示顺序）。源与产物同样锯齿时
    // 不得误报 —— 真实语料 real_h264_aac_faststart.mp4 就是 has_b_frames=2。
    auto bframe_source = MakeSyntheticSequence(0, false, false, "abc123");
    auto bframe_output = MakeSyntheticSequence(0, false, false, "abc123");
    for (auto* seq : {&bframe_source, &bframe_output}) {
        auto& packets = seq->streams[0].packets;
        packets[0].pts = 0;
        packets[1].pts = 80;
        packets[2].pts = 40;
        seq->streams[0].pts_monotonic = false; // 合法，不能当缺陷
    }
    ExpectSequenceClean("B 帧包级 PTS 锯齿", videoeye_test::CompareExactSequence(bframe_source, bframe_output));

    // 合法时基换算（av_packet_rescale_ts 把刻度按新 time_base 改写）不得误报，
    // 但时间值真的变了必须判红 —— 按秒比较而不是按刻度比较才有这个判别力。
    auto timebase_source = MakeSyntheticSequence(0, false, false, "abc123");
    auto timebase_output = MakeSyntheticSequence(0, false, false, "abc123");
    timebase_source.streams[0].time_base_num = 1;
    timebase_source.streams[0].time_base_den = 1000;
    timebase_output.streams[0].time_base_num = 1;
    timebase_output.streams[0].time_base_den = 90000;
    for (auto& packet : timebase_output.streams[0].packets) {
        packet.pts *= 90; // 40 ticks@1/1000 == 3600 ticks@1/90000
        packet.dts *= 90;
    }
    ExpectSequenceClean("合法时基换算", videoeye_test::CompareExactSequence(timebase_source, timebase_output));
    for (auto& packet : timebase_output.streams[0].packets) {
        packet.pts *= 2; // 真实间隔翻倍
        packet.dts *= 2;
    }
    const auto interval_diffs = videoeye_test::CompareExactSequence(timebase_source, timebase_output);
    EXPECT_FALSE(interval_diffs.empty()) << "时间值真的变了必须判红";
    EXPECT_NE(Join(interval_diffs).find("PTS 间隔"), std::string::npos) << Join(interval_diffs);

    // 子序列：连续 3 包通过；取第 1、3 包（不连续）或全量都不得通过。
    videoeye_test::MediaSequence source;
    source.opened = true;
    source.streams.resize(1);
    source.streams[0].codec_type = "video";
    source.streams[0].decoded_frames = 3;
    for (int i = 0; i < 10; ++i) {
        videoeye_test::PacketFacts p;
        p.has_pts = true;
        p.has_dts = true;
        p.pts = i * 40;
        p.dts = p.pts;
        p.payload_hash = 100 + static_cast<uint64_t>(i);
        source.streams[0].packets.push_back(p);
    }
    source.total_packets = 10;

    const auto make_clip = [&source](const std::vector<int>& indices) {
        videoeye_test::MediaSequence clip;
        clip.opened = true;
        clip.streams.resize(1);
        clip.streams[0].codec_type = "video";
        clip.streams[0].decoded_frames = static_cast<uint64_t>(indices.size());
        for (int i : indices)
            clip.streams[0].packets.push_back(source.streams[0].packets[static_cast<size_t>(i)]);
        clip.total_packets = indices.size();
        return clip;
    };
    ExpectSequenceClean("连续子序列", videoeye_test::CompareSubsequence(source, make_clip({4, 5, 6})));
    EXPECT_FALSE(videoeye_test::CompareSubsequence(source, make_clip({4, 6, 7})).empty()) << "不连续裁剪必须判红";
    EXPECT_FALSE(videoeye_test::CompareSubsequence(source, make_clip({0, 1, 2, 3, 4, 5, 6, 7, 8, 9})).empty())
        << "没裁掉任何包必须判红";

    // 容器重打包放行条件的守护：只有逐帧画面真的一致才允许，帧变了不得放行。
    EXPECT_TRUE(videoeye_test::FramesIdentical(base, base));
    EXPECT_FALSE(videoeye_test::FramesIdentical(base, wrong_picture));
}

// ---------------------------------------------------------------------------
// 7) 可选：外部真实语料批量抽样。VE_EXPORT_CORPUS_DIR 指向语料树，
//    VE_EXPORT_CORPUS_LIMIT 控制样本数（默认 24）。CI 未设置时跳过。
//    预筛只排除「产品能力边界」（有意丢弃的非媒体轨、mp4 承载不了的编码）；
//    通过预筛后仍导出失败或序列不一致，一律算缺陷，不静默忽略。
// ---------------------------------------------------------------------------
TEST(ExportRoundtrip, ExternalCorpusBatchRemuxPreservesSequences) {
    const char* env_dir = std::getenv("VE_EXPORT_CORPUS_DIR");
    if (env_dir == nullptr || *env_dir == '\0')
        GTEST_SKIP() << "未设置 VE_EXPORT_CORPUS_DIR，跳过外部语料批量抽样";
    AppScope app;

    const char* env_limit = std::getenv("VE_EXPORT_CORPUS_LIMIT");
    const size_t limit = env_limit != nullptr ? static_cast<size_t>(std::max(1, std::atoi(env_limit))) : 40u;
    constexpr uint64_t kMaxBytes = 32ULL * 1024 * 1024;

    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(env_dir, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".mp4" && ext != ".mov" && ext != ".m4v" && ext != ".mkv")
            continue;
        const uint64_t size = std::filesystem::file_size(entry.path(), ec);
        if (ec || size < 4096 || size > kMaxBytes)
            continue;
        candidates.push_back(entry.path().string());
    }
    std::sort(candidates.begin(), candidates.end());
    // 固定步距取样，避免总是撞到同一批 FATE 样本；样本不足时取全部。
    std::vector<std::string> picked;
    if (!candidates.empty()) {
        const size_t stride = std::max<size_t>(1, candidates.size() / limit);
        for (size_t i = 0; i < candidates.size() && picked.size() < limit; i += stride)
            picked.push_back(candidates[i]);
    }

    // 能力边界预筛（轻量，只探测不解码）：MediaExporter 有意只保留视频/音频轨，
    // 字幕 / 数据 / 封面轨会被丢弃；多视频轨、mp4 承载不了的编码（ProRes、
    // GPMF、PGS）也不在「原样 remux」的保证范围内。这些不是缺陷，但预筛绝不能
    // 变成「悄悄跳过一切」—— 下面用 checked 下限与 skipped 上限做覆盖守护。
    static const std::set<std::string> kCopyableVideo = {"h264", "hevc",  "mpeg4", "av1",
                                                         "vp9",  "mjpeg", "h263",  "mpeg2video"};
    static const std::set<std::string> kCopyableAudio = {"aac",  "mp3",  "ac3",       "eac3",     "opus",
                                                         "flac", "alac", "pcm_s16le", "pcm_s24le"};
    const auto within_capability = [](const videoeye_test::MediaSequence& layout) {
        int video = 0;
        int audio = 0;
        for (const auto& stream : layout.streams) {
            if (stream.codec_type == "video") {
                ++video;
                if (kCopyableVideo.count(stream.codec_name) == 0)
                    return false;
            } else if (stream.codec_type == "audio") {
                ++audio;
                if (kCopyableAudio.count(stream.codec_name) == 0)
                    return false;
            } else {
                return false; // 字幕 / 数据 / 未知轨
            }
        }
        return video == 1 && audio <= 1;
    };

    size_t checked = 0;
    size_t skipped = 0;
    size_t repacked = 0;
    std::vector<std::string> failures;
    WorkDir work;
    ASSERT_TRUE(work.valid());
    for (size_t i = 0; i < picked.size(); ++i) {
        const auto layout = videoeye_test::ProbeStreamLayout(picked[i]);
        if (!layout.opened || !layout.error.empty() || !within_capability(layout)) {
            ++skipped;
            continue;
        }
        const auto expected = videoeye_test::CaptureMediaSequence(picked[i], true, 60000);
        if (!videoeye_test::HasDecodableVideo(expected) || !expected.error.empty()) {
            ++skipped;
            continue;
        }
        const QString output = work.path() + QString("/batch_%1.mp4").arg(i);
        const auto run = RunExport(MakeOptions(QString::fromStdString(picked[i]), output));
        if (!run.finished || run.error) {
            failures.push_back(picked[i] + " 导出失败: " + run.error_msg.toStdString());
            continue;
        }
        const auto actual = videoeye_test::CaptureMediaSequence(output.toStdString(), true, 60000);
        const auto diffs = videoeye_test::CompareExactSequence(expected, actual);
        // 跨容器 remux 时 muxer 会合法重打包 bitstream（MKV 的 Annex-B -> MP4
        // 的 AVCC 长度前缀），载荷字节变但画面不变。只有「仅此一条差异」且
        // 逐帧亮度序列 + 帧数完全一致时才放行；任何其它差异仍算缺陷，
        // 避免这个口子把真正的内容损坏一起放过去。
        const bool only_payload_diff = diffs.size() == 1 && diffs[0].find("逐包载荷序列不一致") != std::string::npos;
        if (only_payload_diff && videoeye_test::FramesIdentical(expected, actual)) {
            ++repacked;
        } else {
            for (const auto& diff : diffs)
                failures.push_back(picked[i] + " " + diff);
        }
        ++checked;
        QFile::remove(output);
    }

    std::cout << "[external corpus] candidates=" << candidates.size() << " picked=" << picked.size()
              << " checked=" << checked << " skipped=" << skipped << " repacked=" << repacked
              << " failures=" << failures.size() << std::endl;
    EXPECT_GE(picked.size(), 12u) << "语料树候选过少，抽样没有覆盖意义";
    EXPECT_GE(checked, 8u) << "真正完成序列配对的文件太少，覆盖守护失效（picked=" << picked.size()
                           << " skipped=" << skipped << "）";
    EXPECT_LE(skipped, picked.size() * 3 / 4) << "能力边界预筛过严，等于没测";
    EXPECT_TRUE(failures.empty()) << Join(failures);
}
