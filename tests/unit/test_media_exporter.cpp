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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>

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
