// MediaExporter 错误传播回归测试（对应评审 P1）
//
// 修复前 encode_video_frame / encode_audio_frame 是 void，sws_getContext /
// av_frame_get_buffer / sws_scale / swr_init / swr_convert / avcodec_send_frame
// 失败时静默 return 且不设 err_msg；解码阶段也不检查 avcodec_send_packet 的返回。
// 于是损坏 / 不完整输入可能走到 reached_end && flushed_ok，生成不完整文件并发出
// ExportFinished（误报成功）。
//
// 本测试用一个明显损坏的输入文件验证：导出必须上报 ExportError，绝不能被误报成
// ExportFinished，且不应残留任何输出文件。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "BlockingTcpEndpoint.h"  // 同目录: 只监听不回包的本地端口(可控阻塞输入)
#include "core/exporter/MediaExporter.h"

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
        if (owned) delete app;
    }
    AppScope(const AppScope&) = delete;
    AppScope& operator=(const AppScope&) = delete;
};

QString WriteCorruptInput() {
    const QString path = QDir::tempPath() + "/videoeye_xport_corrupt_in.bin";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    QByteArray junk(2048, '\x00');
    for (int i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    f.write(junk);
    f.close();
    return path;
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
    QObject::connect(exporter, &exporter::MediaExporter::ExportError,
                     [&sink](const QString& m) { sink.error = true; sink.error_msg = m; });
    QObject::connect(exporter, &exporter::MediaExporter::ExportCanceled,
                     [&sink](const QString&) { sink.canceled = true; });

    const QString in = WriteCorruptInput();
    ASSERT_FALSE(in.isEmpty());
    const QString out = QDir::tempPath() + "/videoeye_xport_corrupt_out.mp4";
    if (QFile::exists(out)) QFile::remove(out);

    exporter::ExportOptions opt;
    opt.input_path = in;
    opt.output_path = out;
    opt.format = "mp4";
    opt.kind = exporter::ExportKind::Video;

    exporter->Export(opt);  // 同步执行, 与 worker 线程内调用一致

    EXPECT_FALSE(sink.finished) << "损坏输入不应被误报为导出成功";
    EXPECT_TRUE(sink.error) << "损坏输入必须上报 ExportError; msg="
                            << sink.error_msg.toStdString();
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
    QObject::connect(exporter, &exporter::MediaExporter::ExportError,
                     [&sink](const QString& m) { sink.error = true; sink.error_msg = m; });
    QObject::connect(exporter, &exporter::MediaExporter::ExportCanceled,
                     [&sink](const QString&) { sink.canceled = true; });

    exporter::ExportOptions opt;
    opt.input_path = QString::fromStdString(endpoint.tcp_url());
    opt.output_path = QDir::tempPath() + "/videoeye_xport_blocking_out.mp4";
    opt.format = "mp4";
    opt.kind = exporter::ExportKind::Video;
    if (QFile::exists(opt.output_path)) QFile::remove(opt.output_path);

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
    exporter->Cancel();   // 只置标志: 能否退出全靠中断回调

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
    QCoreApplication::processEvents();   // 投递 worker 线程发出的终态信号

    EXPECT_LT(elapsed_ms, 5000) << "取消耗时 " << elapsed_ms << " ms, 应当远小于超时预算";
    EXPECT_TRUE(sink.canceled) << "被取消的导出必须上报 ExportCanceled";
    EXPECT_FALSE(sink.error) << "取消不是失败, 不该报错: " << sink.error_msg.toStdString();
    EXPECT_FALSE(sink.finished) << "取消绝不能报成功";
    EXPECT_FALSE(QFile::exists(opt.output_path)) << "取消后不应残留目标文件";

    QFile::remove(opt.output_path);
    delete exporter;
}
