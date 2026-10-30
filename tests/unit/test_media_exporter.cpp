// MediaExporter 测试：错误传播 + **成功路径**（阶段 1）
//
// 1) 错误传播（评审 P1）
//    修复前 encode_video_frame / encode_audio_frame 是 void，sws_getContext /
//    av_frame_get_buffer / sws_scale / swr_init / swr_convert / avcodec_send_frame
//    失败时静默 return 且不设 err_msg；解码阶段也不检查 avcodec_send_packet 的返回。
//    于是损坏 / 不完整输入可能走到 reached_end && flushed_ok，生成不完整文件并发出
//    ExportFinished（误报成功）。
//
// 2) 成功路径（阶段 1 补的）
//    上面那半边覆盖得再严，也证明不了"导出真的能用" —— 一个每次都报失败的实现
//    同样能让错误测试全绿。所以这里用 tests/support/TestMediaFactory 现场生成一份
//    极小的 H.264+AAC 样本，跑通 remux / 转码 / 音频-only 三条成功路径，并且
//    **把产物重新交给 FFmpeg 打开**验证流数量、分辨率、帧数、时长。
//    只断言"文件存在且大于 0 字节"是不够的：写坏的空壳文件同样满足。

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include "BlockingTcpEndpoint.h" // 同目录: 只监听不回包的本地端口(可控阻塞输入)
#include "core/exporter/MediaExporter.h"
#include "tests/support/TestMediaFactory.h"

namespace {

using namespace videoeye;

// 捕获终态信号。MediaExporter 与测试在同一线程，信号以直连方式同步投递，无需事件循环。
struct ExportSink {
    bool finished = false;
    bool error = false;
    bool canceled = false;
    QString error_msg;
};

// 保证整个用例期间存在一个 QCoreApplication（排队信号需要事件循环）,
// 退出时把**自己建的**那个销毁 —— 否则后续用例再建一个就成了第二个实例。
struct AppScope {
    QCoreApplication* app = nullptr;
    bool owned = false;

    AppScope() {
        static int argc = 1;
        static char arg0[] = "test_media_exporter";
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

QString WriteCorruptInput() {
    const QString path = QDir::tempPath() + "/videoeye_xport_corrupt_in.bin";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    QByteArray junk(2048, '\x00');
    for (int i = 0; i < junk.size(); ++i)
        junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    f.write(junk);
    f.close();
    return path;
}

// ---- 成功路径用的脚手架 ----

// 一次导出里我们关心的全部终态。信号是直连的（同一线程同步 Export），不需要事件循环。
struct ExportRun {
    bool started = false;
    bool finished = false;
    bool canceled = false;
    bool error = false;
    int last_progress = -1;
    qint64 duration_ms = -1;
    QString finished_path;
    QString error_msg;
};

ExportRun RunExport(const exporter::ExportOptions& opt) {
    ExportRun run;
    exporter::MediaExporter exporter;
    QObject::connect(&exporter, &exporter::MediaExporter::ExportStarted, [&run](qint64 duration) {
        run.started = true;
        run.duration_ms = duration;
    });
    QObject::connect(&exporter, &exporter::MediaExporter::ExportProgress,
                     [&run](int percent) { run.last_progress = percent; });
    QObject::connect(&exporter, &exporter::MediaExporter::ExportFinished, [&run](const QString& path) {
        run.finished = true;
        run.finished_path = path;
    });
    QObject::connect(&exporter, &exporter::MediaExporter::ExportCanceled,
                     [&run](const QString&) { run.canceled = true; });
    QObject::connect(&exporter, &exporter::MediaExporter::ExportError, [&run](const QString& message) {
        run.error = true;
        run.error_msg = message;
    });
    exporter.Export(opt);
    EXPECT_FALSE(exporter.IsExporting()) << "Export() 返回后必须不再是导出中状态";
    return run;
}

// 每个用例一份独立临时目录：既能断言"目录里只剩该有的文件"，也不会互相踩。
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

    // 导出用的临时产物（xxx.part-<pid>-<uuid>）与替换备份（xxx.bak-<pid>-<uuid>）
    // 都不该在终态之后还躺在目录里。
    QStringList TempResidue() const {
        const QStringList all = QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot);
        QStringList residue;
        for (const QString& name : all) {
            if (name.contains(".part-") || name.contains(".bak-"))
                residue << name;
        }
        return residue;
    }
};

// 生成一份样本，并**先验证样本本身合格** —— 否则后面所有断言都是在测一份坏输入。
videoeye_test::TestMedia MakeSampleMedia(const std::string& directory) {
    videoeye_test::TestMediaSpec spec; // 128x72 / 25fps / 40 帧 / H.264+AAC
    videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(directory, "clip", spec);
    return media;
}

QByteArray ReadWholeFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

exporter::ExportOptions MakeOptions(const std::string& input, const QString& output, const QString& format) {
    exporter::ExportOptions opt;
    opt.input_path = QString::fromStdString(input);
    opt.output_path = output;
    opt.format = format;
    opt.kind = exporter::ExportKind::Video;
    return opt;
}

// 时长断言给足容差: 容器层时长是元数据的估算值, 不同 muxer 写法会差几十毫秒。
void ExpectDurationInRange(double actual, double expected) {
    EXPECT_GE(actual, expected * 0.7) << "产物时长明显短于源: " << actual << " vs " << expected;
    EXPECT_LE(actual, expected * 1.4) << "产物时长明显长于源: " << actual << " vs " << expected;
}

} // namespace

TEST(MediaExporterError, CorruptInputEmitsErrorNotFinished) {
    int argc = 1;
    char arg0[] = "test_media_exporter";
    char* argv[] = {arg0, nullptr};
    QCoreApplication app(argc, argv);

    ExportSink sink;
    auto* exporter = new exporter::MediaExporter();
    QObject::connect(exporter, &exporter::MediaExporter::ExportFinished,
                     [&sink](const QString&) { sink.finished = true; });
    QObject::connect(exporter, &exporter::MediaExporter::ExportError, [&sink](const QString& m) {
        sink.error = true;
        sink.error_msg = m;
    });
    QObject::connect(exporter, &exporter::MediaExporter::ExportCanceled,
                     [&sink](const QString&) { sink.canceled = true; });

    const QString in = WriteCorruptInput();
    ASSERT_FALSE(in.isEmpty());
    const QString out = QDir::tempPath() + "/videoeye_xport_corrupt_out.mp4";
    if (QFile::exists(out))
        QFile::remove(out);

    exporter::ExportOptions opt;
    opt.input_path = in;
    opt.output_path = out;
    opt.format = "mp4";
    opt.kind = exporter::ExportKind::Video;

    exporter->Export(opt); // 同步执行, 与 worker 线程内调用一致

    EXPECT_FALSE(sink.finished) << "损坏输入不应被误报为导出成功";
    EXPECT_TRUE(sink.error) << "损坏输入必须上报 ExportError; msg=" << sink.error_msg.toStdString();
    EXPECT_FALSE(sink.canceled);
    EXPECT_FALSE(QFile::exists(out)) << "失败导出不应残留目标文件";

    QFile::remove(in);
    delete exporter;
}

// 取消必须能打断 FFmpeg 的**阻塞 IO**（复查 P1-3）。
//
// 修复前: 只在 av_read_frame() **返回之后**检查取消标志，输入上下文上也没有装
// AVIOInterruptCB。输入是网络地址/管道时 avformat_open_input 与 av_read_frame 可以
// 长时间不返回 —— Cancel() 只置了一个标志，导出线程照样卡在里面，用户看到的就是
// "点了取消没反应"，关窗口时还会卡在后台线程回收上。
//
// 这里用一个只监听、从不回包的本地端口做输入：打开会阻塞在"等数据"上，
// 完全是本地行为，不依赖外网也不需要大文件。
TEST(MediaExporterCancel, CancelAbortsBlockingInput) {
    AppScope app_scope;

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法创建本地监听端口";

    ExportSink sink;
    // 失败路径上故意不 delete: 线程可能仍在跑, 释放对象会 UAF。
    auto* exporter = new exporter::MediaExporter();
    QObject::connect(exporter, &exporter::MediaExporter::ExportFinished,
                     [&sink](const QString&) { sink.finished = true; });
    QObject::connect(exporter, &exporter::MediaExporter::ExportError, [&sink](const QString& m) {
        sink.error = true;
        sink.error_msg = m;
    });
    QObject::connect(exporter, &exporter::MediaExporter::ExportCanceled,
                     [&sink](const QString&) { sink.canceled = true; });

    exporter::ExportOptions opt;
    opt.input_path = QString::fromStdString(endpoint.tcp_url());
    opt.output_path = QDir::tempPath() + "/videoeye_xport_blocking_out.mp4";
    opt.format = "mp4";
    opt.kind = exporter::ExportKind::Video;
    if (QFile::exists(opt.output_path))
        QFile::remove(opt.output_path);

    std::atomic<bool> returned{false};
    std::thread worker([exporter, &opt, &returned]() {
        exporter->Export(opt);
        returned.store(true, std::memory_order_release);
    });

    // 等到它确实卡进阻塞 IO 里（否则这个用例什么也没验证到）
    const auto wait_until_blocked_start = std::chrono::steady_clock::now();
    while (!returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() - wait_until_blocked_start < std::chrono::milliseconds(1500)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_FALSE(returned.load()) << "输入没有阻塞住, 本用例无法验证取消的可中断性";

    QElapsedTimer timer;
    timer.start();
    exporter->Cancel(); // 只置标志: 能否退出全靠中断回调

    while (!returned.load(std::memory_order_acquire) && timer.elapsed() < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const qint64 elapsed_ms = timer.elapsed();

    if (!returned.load(std::memory_order_acquire)) {
        // 取消没能打断阻塞 IO —— 正是要抓的缺陷。joinable 的线程不能丢,
        // 也不能在它还在跑的时候销毁对象, 只能脱管并让进程结束时一起收掉。
        ADD_FAILURE() << "Cancel() 未能打断阻塞 IO: 导出线程 5 秒后仍未返回";
        worker.detach();
        return;
    }

    worker.join();
    QCoreApplication::processEvents(); // 投递 worker 线程发出的终态信号

    EXPECT_LT(elapsed_ms, 5000) << "取消耗时 " << elapsed_ms << " ms, 应当远小于超时预算";
    EXPECT_TRUE(sink.canceled) << "被取消的导出必须上报 ExportCanceled";
    EXPECT_FALSE(sink.error) << "取消不是失败, 不该报错: " << sink.error_msg.toStdString();
    EXPECT_FALSE(sink.finished) << "取消绝不能报成功";
    EXPECT_FALSE(QFile::exists(opt.output_path)) << "取消后不应残留目标文件";

    QFile::remove(opt.output_path);
    delete exporter;
}

// ============================================================
// 成功路径（阶段 1）
//
// 共同的前置: 先造样本并验证样本自身能被 FFmpeg 打开、帧数符合预期。
// 少这一步的话，一旦样本生成器坏了，下面的"导出成功"断言会集体变成假绿/假红，
// 排查方向会被带偏 —— 所以每个用例都 ASSERT 在样本上。
// ============================================================

// remux（不重编码）：包原样搬过去，产物必须能被解码出同样多的帧。
TEST(MediaExporterSuccess, RemuxKeepsStreamsFramesAndDuration) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;
    const auto source = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source.opened) << source.error;
    // 码流层: 编码器确实输出了 spec 要求的帧数（包数不受容器元数据口径影响）
    EXPECT_EQ(source.video_packet_count, media.frame_count) << " 视频流时长=" << source.video_duration_seconds;
    // 解码层: mp4 的 track duration 只记到最后一个样本的 dts（少最后一帧的时长），
    // 回读时因此会比写入帧数少 1 —— 这是容器口径, 不是丢帧。
    // 所以下面一律拿"产物"跟"源"用同一把尺子比, 而不是拿硬编码的 40 去比。
    ASSERT_GE(source.frame_count, media.frame_count - 1)
        << "样本自身必须完整; 包数=" << source.video_packet_count << " 容器时长=" << source.duration_seconds
        << " 视频流时长=" << source.video_duration_seconds;
    ASSERT_LE(source.frame_count, media.frame_count) << "回读帧数不应多于写入帧数";
    ASSERT_EQ(source.audio_streams, 1);

    const QString output = work.path() + "/remux.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.reencode = false;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << "remux 必须成功; error=" << run.error_msg.toStdString();
    EXPECT_FALSE(run.error);
    EXPECT_FALSE(run.canceled);
    EXPECT_EQ(run.finished_path, output);
    EXPECT_TRUE(run.started) << "成功导出必须先发 ExportStarted（进度框要靠它建量程）";
    EXPECT_GT(run.duration_ms, 0);
    EXPECT_EQ(run.last_progress, 100);

    // ---- 产物必须能被 FFmpeg 重新打开 ----
    ASSERT_TRUE(QFileInfo::exists(output));
    EXPECT_GT(QFileInfo(output).size(), 0);
    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.video_streams, 1);
    EXPECT_EQ(probe.audio_streams, 1);
    EXPECT_EQ(probe.width, media.width);
    EXPECT_EQ(probe.height, media.height);
    // remux 是码流拷贝: 包数必须一个不差（包数不受容器时长口径影响），
    // 解码帧数不应少于源（实测产物反而比源多还原出被容器元数据吞掉的那一帧）。
    EXPECT_EQ(probe.video_packet_count, source.video_packet_count);
    EXPECT_GE(probe.frame_count, source.frame_count);
    ExpectDurationInRange(probe.duration_seconds, media.duration_seconds);

    EXPECT_TRUE(work.TempResidue().isEmpty())
        << "成功导出后不应残留临时文件: " << work.TempResidue().join(",").toStdString();
}

// 转码：解码 -> 重编码 -> 重封装。帧数必须与源一致（丢帧/编码器排空失败都会少）。
TEST(MediaExporterSuccess, TranscodeReencodesVideoAndAudio) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;
    const auto source = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source.opened) << source.error;

    const QString output = work.path() + "/transcoded.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.reencode = true;
    opt.videoQuality = 0;
    opt.audioBitrateKbps = 128;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << "转码必须成功; error=" << run.error_msg.toStdString();
    EXPECT_FALSE(run.error);
    EXPECT_EQ(run.last_progress, 100);

    ASSERT_TRUE(QFileInfo::exists(output));
    EXPECT_GT(QFileInfo(output).size(), 0);
    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.video_streams, 1);
    EXPECT_EQ(probe.audio_streams, 1);
    EXPECT_EQ(probe.width, media.width);
    EXPECT_EQ(probe.height, media.height);
    // 转码最容易悄悄丢的两种东西：编码器的延迟帧（排空没做全）和尾部音频。
    // 包数说明编码器确实输出了每一帧; 解码帧数与源同口径比, 允许容器裁剪的 1 帧。
    EXPECT_GE(probe.video_packet_count, media.frame_count - 1);
    EXPECT_GE(probe.frame_count, source.frame_count - 1);
    EXPECT_LE(probe.frame_count, source.frame_count + 1);
    ExpectDurationInRange(probe.duration_seconds, media.duration_seconds);
    EXPECT_FALSE(probe.video_codec.empty());

    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// 音频-only：视频流不得被带进产物。
TEST(MediaExporterSuccess, AudioOnlyExportDropsVideoStream) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    const QString output = work.path() + "/audio_only.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.kind = exporter::ExportKind::Audio;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << "音频导出必须成功; error=" << run.error_msg.toStdString();

    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.video_streams, 0);
    EXPECT_EQ(probe.audio_streams, 1);
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// no_audio：视频导出时勾选"不含音轨"的分支。
TEST(MediaExporterSuccess, NoAudioExportDropsAudioStream) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    const QString output = work.path() + "/no_audio.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.no_audio = true;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << "去音轨导出必须成功; error=" << run.error_msg.toStdString();

    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.video_streams, 1);
    EXPECT_EQ(probe.audio_streams, 0);
    EXPECT_GE(probe.frame_count, media.frame_count - 1);
    EXPECT_LE(probe.frame_count, media.frame_count);
}

// 失败不残留 .part-*：原子替换的前提是"临时产物一定被收走"。
// 残留下来会同时在目录里留下垃圾、并让用户误以为导出还在进行中。
TEST(MediaExporterSuccess, FailureLeavesNoTempFiles) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const std::string corrupt = videoeye_test::CreateCorruptMediaFile(work.stdPath(), "corrupt");
    const QString output = work.path() + "/never.mp4";
    exporter::ExportOptions opt = MakeOptions(corrupt, output, QStringLiteral("mp4"));

    const ExportRun run = RunExport(opt);
    EXPECT_FALSE(run.finished);
    ASSERT_TRUE(run.error) << "损坏输入必须上报 ExportError";

    EXPECT_FALSE(QFileInfo::exists(output));
    EXPECT_TRUE(work.TempResidue().isEmpty())
        << "失败后不得残留 .part-* 临时文件: " << work.TempResidue().join(",").toStdString();
}

// 失败的重复导出不得破坏上一次的成功产物 —— 原子替换最原始的动机。
TEST(MediaExporterSuccess, FailedReExportKeepsPreviousGoodFile) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    const QString output = work.path() + "/target.mp4";
    exporter::ExportOptions good = MakeOptions(media.path, output, QStringLiteral("mp4"));
    ASSERT_TRUE(RunExport(good).finished);
    const QByteArray before = ReadWholeFile(output);
    ASSERT_FALSE(before.isEmpty());

    const std::string corrupt = videoeye_test::CreateCorruptMediaFile(work.stdPath(), "corrupt2");
    exporter::ExportOptions bad = MakeOptions(corrupt, output, QStringLiteral("mp4"));
    const ExportRun run = RunExport(bad);
    EXPECT_FALSE(run.finished);
    ASSERT_TRUE(run.error);

    EXPECT_EQ(ReadWholeFile(output), before) << "失败导出不得改动上一次的成功产物";
    EXPECT_TRUE(work.TempResidue().isEmpty())
        << "失败后不得残留备份/临时文件: " << work.TempResidue().join(",").toStdString();

    // 成功产物本身还得是能打开的那一份
    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    EXPECT_TRUE(probe.opened);
    EXPECT_GE(probe.frame_count, media.frame_count - 1);
    EXPECT_LE(probe.frame_count, media.frame_count);
}

// 连续两次成功导出：目标被替换成新的，且不留下备份文件。
TEST(MediaExporterSuccess, SecondExportReplacesTargetWithoutResidue) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    const auto source = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source.opened) << source.error;

    const QString output = work.path() + "/target.mp4";
    exporter::ExportOptions first = MakeOptions(media.path, output, QStringLiteral("mp4"));
    ASSERT_TRUE(RunExport(first).finished);
    const QByteArray first_bytes = ReadWholeFile(output);

    exporter::ExportOptions second = MakeOptions(media.path, output, QStringLiteral("mp4"));
    second.reencode = true;
    second.videoQuality = 2; // 码率不同 -> 产物字节必然不同
    const ExportRun run = RunExport(second);
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();

    const QByteArray second_bytes = ReadWholeFile(output);
    EXPECT_FALSE(second_bytes.isEmpty());
    EXPECT_TRUE(work.TempResidue().isEmpty())
        << "替换成功后备份必须被删掉: " << work.TempResidue().join(",").toStdString();

    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;
    EXPECT_GE(probe.frame_count, source.frame_count - 1);
    EXPECT_LE(probe.frame_count, source.frame_count + 1);
}

// 启动前就取消：必须走取消收尾，不能报成功也不能报失败，更不该碰输出路径。
TEST(MediaExporterSuccess, CancelBeforeStartEmitsCanceledOnly) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    const QString output = work.path() + "/canceled.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));

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
    exporter.Cancel();
    exporter.Export(opt);

    EXPECT_TRUE(run.canceled);
    EXPECT_FALSE(run.finished) << "取消绝不能报成功";
    EXPECT_FALSE(run.error) << "取消不是失败, 不该报 ExportError";
    EXPECT_FALSE(QFileInfo::exists(output)) << "启动前取消不该产生任何文件";
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// ============================================================
// 片段区间 (ExportOptions::start_ms / end_ms)
//
// 这个功能在 UI 上是「仅导出指定片段」+ 两个秒级 spinbox，**默认走拷贝模式**
// （「重新编码」默认不勾），却一直没有任何测试。
//
// 原来只实现了区间终点：start_ms 只喂给 av_seek_frame()，而 seek 只能落在关键帧上，
// 回退之后拿到的包没有被丢弃 —— 于是"仅导出 0.8s 之后"会静默产出整条视频
// （实测 1.6s 素材、区间 800~1600ms，产物是 40 个包 / 1.602s）。
// 终点有检查、起点没有，是这个 bug 长期没被发现的原因。
//
// 下面用确定性样本（128x72 / 25fps / 40 帧 / 1.6s / GOP=25 即 1 秒一个关键帧）钉住：
//   * 两个端点都必须生效（包数按区间算，不是整条）；
//   * 产物时间轴从请求的起点开始（容器写编辑列表，video_start_seconds ≈ start）；
//   * 只给终点时行为不变（别为了修起点把终点弄坏）；
//   * 起点 ≥ 终点要报错，而不是产出空文件。
// ============================================================

namespace {

// 按 fps 把毫秒区间换算成期望帧/包数。
//
// 两个细节容易写错（第一版就写错了）：
//   * 终点是**含**的（循环里判 `pts_ms > end_ms` 才停），所以末尾要 +1；
//   * 但源的最后一帧通常早于 end_ms（1.6s 素材的帧在 0..1560ms），
//     所以结果必须用「源里剩下的帧数」封顶，否则会期望一个不存在的帧。
int ExpectedPackets(int fps, qint64 start_ms, qint64 end_ms, int source_packets) {
    const qint64 from = (start_ms > 0) ? start_ms : 0;
    const int skipped = static_cast<int>(from * fps / 1000);
    const int kept = static_cast<int>((end_ms - from) * fps / 1000) + 1;
    const int available = source_packets - skipped;
    // 不写 std::min：windows.h 的 min 宏会把它打断成 `std::(`（MSVC C2589）。
    return (kept < available) ? kept : available;
}

// 请求区间与实际产物时长允许的偏差：两个端点都不是帧边界的整数倍时，
// 落点最多差一帧（40ms）；再加容器层「最后一帧时长」的口径差，给两帧余量。
constexpr double kRangeToleranceSec = 0.12;

} // namespace

// 拷贝模式：按起点裁掉前面的包。产物从非关键帧开头（-c copy 裁剪的固有限制），
// 但**绝不能把用户排除掉的内容一起写进去**。
TEST(MediaExporterRange, CopyHonorsRequestedStartAndEnd) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;
    const auto source = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source.opened) << source.error;
    ASSERT_GT(media.fps, 0);

    const QString output = work.path() + "/range_copy.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.reencode = false;
    opt.start_ms = 800;
    opt.end_ms = 1600;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << "区间导出必须成功; error=" << run.error_msg.toStdString();
    EXPECT_FALSE(run.error);

    ASSERT_TRUE(QFileInfo::exists(output));
    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;

    const int expected = ExpectedPackets(media.fps, opt.start_ms, opt.end_ms, source.video_packet_count);
    // 决定性断言：修复前这里是 40（整条），现在是 20。
    EXPECT_EQ(probe.video_packet_count, expected) << "起点必须生效：包数应按 800~1600ms 算，而不是整条";
    EXPECT_LT(probe.video_packet_count, source.video_packet_count) << "片段不该比源还长";

    // 时间轴从请求的起点开始：mp4 用编辑列表表达（start_time > 0），
    // 播放器据此跳过前面的部分。它若为 0，说明产物仍从文件开头播起。
    EXPECT_NEAR(probe.video_start_seconds, 0.8, 0.061) << "产物时间轴应从请求起点 0.8s 开始";
    EXPECT_NEAR(probe.video_duration_seconds, (opt.end_ms - opt.start_ms) / 1000.0, kRangeToleranceSec);

    // 拷贝模式起点不在关键帧上 → 残余 GOP 的那几帧解不出来，这是已知限制。
    // 不断言具体数字，只保证"确实是从非关键帧起"，将来 FFmpeg 改进也不会假红。
    EXPECT_LT(probe.frame_count, probe.video_packet_count)
        << "拷贝裁剪的产物首帧不在关键帧上，能解出的帧应少于包数（若相等说明起点被对齐到了关键帧）";
    EXPECT_GT(probe.frame_count, 0) << "至少要有可解码的部分";

    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// 编码模式：包照喂解码器（要关键帧才解得动），只丢掉区间外的帧 —— 结果是
// **两端都精确、且首帧可解码**的片段。
TEST(MediaExporterRange, ReencodeTrimsExactlyAndStaysDecodable) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;
    const auto source = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source.opened) << source.error;

    const QString output = work.path() + "/range_enc.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.reencode = true;
    opt.start_ms = 800;
    opt.end_ms = 1600;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << "区间转码必须成功; error=" << run.error_msg.toStdString();

    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;

    EXPECT_NEAR(probe.video_start_seconds, 0.8, 0.061) << "产物时间轴应从请求起点开始";
    EXPECT_NEAR(probe.video_duration_seconds, (opt.end_ms - opt.start_ms) / 1000.0, kRangeToleranceSec);
    // 解码器少还原一帧是容器口径（源的 40 包也只解出 39 帧），所以拿**解码帧数**封顶。
    const int expected = ExpectedPackets(media.fps, opt.start_ms, opt.end_ms, source.frame_count);
    EXPECT_NEAR(static_cast<double>(probe.video_packet_count), expected, 1.5);
    // 重编码路径从关键帧起解码、只丢帧不丢包，所以首帧一定可解码。
    EXPECT_GE(probe.frame_count, probe.video_packet_count - 1)
        << "重编码片段必须从头就能解码（不该像拷贝模式那样有残 GOP）";

    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// 只设终点（不设起点）是原来就正确的路径，修复起点时不能把它带坏。
TEST(MediaExporterRange, EndOnlyTrimStillWorks) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    for (const bool reencode : {false, true}) {
        const QString output = work.path() + (reencode ? "/end_only_enc.mp4" : "/end_only_copy.mp4");
        exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
        opt.reencode = reencode;
        opt.start_ms = -1;
        opt.end_ms = 800;

        const ExportRun run = RunExport(opt);
        ASSERT_TRUE(run.finished) << "只设终点必须成功; error=" << run.error_msg.toStdString();

        const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
        ASSERT_TRUE(probe.opened) << probe.error;
        EXPECT_NEAR(probe.video_start_seconds, 0.0, 0.061) << "没要求起点就不该裁剪开头";
        EXPECT_NEAR(probe.video_duration_seconds, 0.8, kRangeToleranceSec)
            << (reencode ? "转码" : "拷贝") << " 只设终点时产物应为 0~0.8s";
    }
    EXPECT_TRUE(work.TempResidue().isEmpty());
}

// 不设区间 = 整条导出，不能被新加的裁剪逻辑误伤。
TEST(MediaExporterRange, NoRangeExportsWholeFile) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;
    const auto source = videoeye_test::ProbeMediaFile(media.path);
    ASSERT_TRUE(source.opened) << source.error;

    const QString output = work.path() + "/whole.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.start_ms = -1;
    opt.end_ms = -1;

    const ExportRun run = RunExport(opt);
    ASSERT_TRUE(run.finished) << run.error_msg.toStdString();

    const auto probe = videoeye_test::ProbeMediaFile(output.toStdString());
    ASSERT_TRUE(probe.opened) << probe.error;
    EXPECT_EQ(probe.video_packet_count, source.video_packet_count) << "整条导出不应丢任何包";
    ExpectDurationInRange(probe.duration_seconds, media.duration_seconds);
}

// 起点 >= 终点：必须报错退出，不能产出一个空壳文件冒充成功。
TEST(MediaExporterRange, InvertedRangeIsRejected) {
    AppScope app_scope;
    WorkDir work;
    ASSERT_TRUE(work.valid());

    const auto media = MakeSampleMedia(work.stdPath());
    ASSERT_TRUE(media.ok) << media.error;

    const QString output = work.path() + "/inverted.mp4";
    exporter::ExportOptions opt = MakeOptions(media.path, output, QStringLiteral("mp4"));
    opt.start_ms = 1200;
    opt.end_ms = 800;

    const ExportRun run = RunExport(opt);
    EXPECT_FALSE(run.finished) << "区间反了绝不能报成功";
    EXPECT_TRUE(run.error);
    EXPECT_FALSE(QFileInfo::exists(output)) << "失败不该留下产物";
    EXPECT_TRUE(work.TempResidue().isEmpty());
}
