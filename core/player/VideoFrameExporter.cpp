#include "core/player/VideoFrameExporter.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace videoeye {
namespace player {

namespace {

// FFmpeg 错误码 -> 可读描述。不用 av_err2str 宏 (MSVC 不支持其 compound literal 写法)。
QString AvErrorString(int ret) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(ret, errbuf, sizeof(errbuf)) != 0) {
        return QStringLiteral("错误码 %1").arg(ret);
    }
    return QString::fromUtf8(errbuf);
}

// 取帧循环的三种结局: 必须分清楚，否则"文件损坏导致提前收尾"会被报成导出成功。
enum class DrainResult {
    Drained,   // 正常排空 (EAGAIN / EOF)
    Canceled,  // 用户中止
    Failed,    // 解码或写文件出错
};

} // namespace

VideoFrameExporter::VideoFrameExporter(QObject* parent)
    : QObject(parent) {}

bool VideoFrameExporter::IsCanceled() const {
    return cancel_.load(std::memory_order_acquire) || cancel_token_.IsCanceled();
}

void VideoFrameExporter::Cancel() {
    cancel_.store(true, std::memory_order_release);
}

void VideoFrameExporter::Export(const QString& url, const QString& output_dir,
                                 const QString& format, int jpg_quality, int frame_interval) {
    exporting_ = true;
    // 取消状态只由外部写，这里绝不重置：启动前点的取消必须能被看到，
    // 否则"立即取消"会被吃掉、导出照常跑完全片。
    if (IsCanceled()) {
        emit ExportCanceled(0, output_dir);
        exporting_ = false;
        return;
    }

    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* export_frame = nullptr;

    auto cleanup = [&]() {
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (export_frame) { av_frame_free(&export_frame); export_frame = nullptr; }
        if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
        if (dec_ctx) { avcodec_free_context(&dec_ctx); dec_ctx = nullptr; }
        if (fmt) { avformat_close_input(&fmt); fmt = nullptr; }
    };

    // 统一收尾: 任何退出路径都只发**一个**终态信号。
    // 少了 UI 永远卡在"导出中"(QThread 也不会 quit)，多了会出现"先报完成再报失败"。
    QString err_msg;
    int exported = 0;
    auto finish = [&](bool canceled, bool failed) {
        cleanup();
        exporting_ = false;
        emit ExportProgress(exported);
        if (canceled) {
            emit ExportCanceled(exported, output_dir);
        } else if (failed) {
            emit ExportError(err_msg.isEmpty() ? QStringLiteral("抽帧导出失败") : err_msg);
        } else {
            emit ExportFinished(output_dir);
        }
    };
    // 准备阶段的失败: 还没有任何产物，直接报错退出
    auto fail_now = [&](const QString& msg) {
        err_msg = msg;
        finish(false, true);
    };

    std::string url_str = url.toStdString();
    if (avformat_open_input(&fmt, url_str.c_str(), nullptr, nullptr) < 0) {
        fail_now(QString("Failed to open: %1").arg(url));
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        fail_now(QStringLiteral("Failed to find stream info"));
        return;
    }

    const AVCodec* best_video_codec = nullptr;
    int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    // 跳过封面图
    if (vindex >= 0 && fmt->streams && fmt->streams[vindex] &&
        (fmt->streams[vindex]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        int candidate = -1;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* s = fmt->streams[i];
            if (!s || s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
            candidate = static_cast<int>(i);
            break;
        }
        vindex = candidate;
        best_video_codec = nullptr;
        if (vindex >= 0 && fmt->streams && fmt->streams[vindex] && fmt->streams[vindex]->codecpar) {
            best_video_codec = avcodec_find_decoder(fmt->streams[vindex]->codecpar->codec_id);
        }
    }

    if (vindex < 0 || !fmt->streams || !fmt->streams[vindex] || !fmt->streams[vindex]->codecpar) {
        fail_now(QStringLiteral("No video stream found"));
        return;
    }

    AVStream* vs = fmt->streams[vindex];
    int total_frames = 0;
    if (vs->nb_frames > 0) {
        total_frames = static_cast<int>(vs->nb_frames);
    } else if (fmt->duration > 0) {
        const double duration_sec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        const double fps = av_q2d(vs->avg_frame_rate);
        if (duration_sec > 0.0 && fps > 0.0) {
            const double est = duration_sec * fps;
            if (est > 0.0 && est < static_cast<double>(std::numeric_limits<int>::max())) {
                total_frames = static_cast<int>(est + 0.5);
            }
        }
    }
    if (total_frames > 0 && frame_interval > 1) {
        total_frames = (total_frames + frame_interval - 1) / frame_interval;
    }
    emit ExportStarted(total_frames);

    const AVCodec* codec = best_video_codec ? best_video_codec : avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        fail_now(QStringLiteral("Video codec not found"));
        return;
    }

    dec_ctx = avcodec_alloc_context3(codec);
    if (!dec_ctx) {
        fail_now(QStringLiteral("Failed to alloc codec context"));
        return;
    }
    if (avcodec_parameters_to_context(dec_ctx, vs->codecpar) < 0) {
        fail_now(QStringLiteral("Failed to copy codec parameters"));
        return;
    }
    if (vs->time_base.den != 0) {
        dec_ctx->pkt_timebase = vs->time_base;
        dec_ctx->time_base = vs->time_base;
    }
    if (avcodec_open2(dec_ctx, codec, nullptr) < 0) {
        fail_now(QStringLiteral("Failed to open video decoder"));
        return;
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        fail_now(QStringLiteral("Failed to alloc packet/frame"));
        return;
    }

    const bool as_jpg = (format == "jpg" || format == "jpeg");
    const bool as_rgb = (format == "rgb");
    const bool as_yuv = (format == "yuv");
    const AVPixelFormat export_pix_fmt = as_rgb ? AV_PIX_FMT_RGB24 : (as_yuv ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NONE);
    if (!as_jpg && !as_rgb && !as_yuv) {
        fail_now(QStringLiteral("Unsupported format (use jpg/rgb/yuv)"));
        return;
    }
    if (export_pix_fmt != AV_PIX_FMT_NONE) {
        export_frame = av_frame_alloc();
        if (!export_frame) {
            fail_now(QStringLiteral("Failed to alloc export frame"));
            return;
        }
    }

    int decoded_index = 0;
    int sws_src_w = 0, sws_src_h = 0, sws_src_fmt = AV_PIX_FMT_NONE;
    int export_dst_w = 0, export_dst_h = 0;
    AVPixelFormat export_dst_fmt = AV_PIX_FMT_NONE;
    int export_buffer_size = 0;
    std::vector<uint8_t> export_buffer;

    auto write_file = [&](const QString& path, const uint8_t* data, int size) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        const qint64 wrote = f.write(reinterpret_cast<const char*>(data), size);
        f.close();
        return wrote == size;
    };

    auto ensure_export_buffer = [&](int width, int height) -> bool {
        if (!export_frame || export_pix_fmt == AV_PIX_FMT_NONE) return true;
        if (export_dst_w == width && export_dst_h == height && export_dst_fmt == export_pix_fmt &&
            !export_buffer.empty()) return true;
        export_buffer_size = av_image_get_buffer_size(export_pix_fmt, width, height, 1);
        if (export_buffer_size <= 0) {
            err_msg = QStringLiteral("Failed to calc export buffer size");
            return false;
        }
        export_buffer.resize(static_cast<size_t>(export_buffer_size));
        av_frame_unref(export_frame);
        export_frame->format = export_pix_fmt;
        export_frame->width = width;
        export_frame->height = height;
        const int fill_ret = av_image_fill_arrays(export_frame->data, export_frame->linesize,
                                                   export_buffer.data(), export_pix_fmt,
                                                   width, height, 1);
        if (fill_ret < 0) {
            err_msg = QStringLiteral("Failed to setup export frame buffer");
            return false;
        }
        export_frame->extended_data = export_frame->data;
        export_dst_w = width;
        export_dst_h = height;
        export_dst_fmt = export_pix_fmt;
        return true;
    };

    // 导出单帧。失败时只写 err_msg、不发信号 —— 终态信号统一由 finish() 发。
    auto export_one_frame = [&](AVFrame* src) -> bool {
        if (IsCanceled()) return false;
        if (src->width <= 0 || src->height <= 0 || src->format < 0) return true;

        int64_t pts = src->pts;
        if (pts == AV_NOPTS_VALUE) pts = src->best_effort_timestamp;
        int64_t ts_ms = -1;
        if (pts != AV_NOPTS_VALUE && vs && vs->time_base.den != 0) {
            const double ts = pts * av_q2d(vs->time_base);
            ts_ms = static_cast<int64_t>(ts * 1000.0 + 0.5);
        }

        if (sws_src_w != src->width || sws_src_h != src->height || sws_src_fmt != src->format) {
            sws_src_w = src->width;
            sws_src_h = src->height;
            sws_src_fmt = src->format;
        }

        const int width = src->width;
        const int height = src->height;

        if (as_jpg) {
            sws = sws_getCachedContext(sws, width, height, static_cast<AVPixelFormat>(src->format),
                                       width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) { err_msg = QStringLiteral("Failed to init sws for jpg"); return false; }
            QImage img(width, height, QImage::Format_ARGB32);
            if (img.isNull()) { err_msg = QStringLiteral("Failed to alloc QImage"); return false; }
            uint8_t* dst_slices[4] = {img.bits(), nullptr, nullptr, nullptr};
            int dst_linesize[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
            const int scaled = sws_scale(sws, src->data, src->linesize, 0, height, dst_slices, dst_linesize);
            if (scaled != height) { err_msg = QStringLiteral("Failed to convert frame"); return false; }
            QString filename = QString("frame_%1").arg(decoded_index, 8, 10, QChar('0'));
            if (pts != AV_NOPTS_VALUE) filename += QString("_pts_%1").arg(static_cast<qint64>(pts));
            if (ts_ms >= 0) filename += QString("_tsms_%1").arg(static_cast<qint64>(ts_ms));
            filename += ".jpg";
            if (!img.save(QDir(output_dir).filePath(filename), "JPG", jpg_quality)) {
                err_msg = QStringLiteral("Failed to save jpg");
                return false;
            }
            return true;
        }

        if (as_rgb || as_yuv) {
            if (!ensure_export_buffer(width, height)) return false;
            AVPixelFormat dst_fmt = as_rgb ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_YUV420P;
            sws = sws_getCachedContext(sws, width, height, static_cast<AVPixelFormat>(src->format),
                                       width, height, dst_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) { err_msg = QStringLiteral("Failed to init sws"); return false; }
            const int scaled = sws_scale(sws, src->data, src->linesize, 0, height,
                                         export_frame->data, export_frame->linesize);
            if (scaled != height) { err_msg = QStringLiteral("Failed to convert frame"); return false; }
            QString ext = as_rgb ? ".rgb" : ".yuv";
            QString filename = QString("frame_%1").arg(decoded_index, 8, 10, QChar('0'));
            if (pts != AV_NOPTS_VALUE) filename += QString("_pts_%1").arg(static_cast<qint64>(pts));
            if (ts_ms >= 0) filename += QString("_tsms_%1").arg(static_cast<qint64>(ts_ms));
            filename += ext;
            if (!write_file(QDir(output_dir).filePath(filename), export_buffer.data(), export_buffer_size)) {
                err_msg = QStringLiteral("Failed to write %1").arg(ext);
                return false;
            }
            return true;
        }
        return true;
    };

    // 取走解码器里已就绪的帧
    auto drain_decoder = [&]() -> DrainResult {
        while (!IsCanceled()) {
            const int r = avcodec_receive_frame(dec_ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return DrainResult::Drained;
            if (r < 0) {   // 真正的解码错误, 不能当成"读完了"
                err_msg = QStringLiteral("解码失败: %1").arg(AvErrorString(r));
                return DrainResult::Failed;
            }
            decoded_index++;
            if (frame_interval <= 1 || ((decoded_index - 1) % frame_interval) == 0) {
                if (!export_one_frame(frame)) {
                    return IsCanceled() ? DrainResult::Canceled : DrainResult::Failed;
                }
                exported++;
            }
            if (exported % 25 == 0) emit ExportProgress(exported);
            av_frame_unref(frame);
        }
        return DrainResult::Canceled;
    };

    // 主解码循环: EOF / 取消 / 真实错误三种结局必须分清
    bool canceled = false;
    bool failed = false;
    while (!canceled && !failed) {
        if (IsCanceled()) { canceled = true; break; }

        const int r = av_read_frame(fmt, pkt);
        if (r == AVERROR_EOF) {   // 正常读到文件尾
            av_packet_unref(pkt);
            break;
        }
        if (r < 0) {   // 损坏 / 截断 / IO 错误: 不能报成功
            err_msg = QStringLiteral("读取数据包失败: %1").arg(AvErrorString(r));
            av_packet_unref(pkt);
            failed = true;
            break;
        }
        if (pkt->stream_index != vindex) { av_packet_unref(pkt); continue; }

        int sr = avcodec_send_packet(dec_ctx, pkt);
        if (sr == AVERROR(EAGAIN)) {
            // 解码器输出队列满: 先取走已解码的帧, 再重试一次
            const DrainResult d = drain_decoder();
            if (d != DrainResult::Drained) {
                canceled = (d == DrainResult::Canceled);
                failed = (d == DrainResult::Failed);
                av_packet_unref(pkt);
                break;
            }
            sr = avcodec_send_packet(dec_ctx, pkt);
        }
        av_packet_unref(pkt);
        if (sr < 0) {
            err_msg = QStringLiteral("送入解码器失败: %1").arg(AvErrorString(sr));
            failed = true;
            break;
        }

        const DrainResult d = drain_decoder();
        if (d == DrainResult::Drained) continue;
        canceled = (d == DrainResult::Canceled);
        failed = (d == DrainResult::Failed);
        break;
    }

    // 排空解码器 (只有既没取消也没出错才值得做)
    if (!canceled && !failed) {
        const int fr = avcodec_send_packet(dec_ctx, nullptr);
        if (fr < 0 && fr != AVERROR_EOF) {
            err_msg = QStringLiteral("结束解码失败: %1").arg(AvErrorString(fr));
            failed = true;
        } else {
            const DrainResult d = drain_decoder();
            canceled = (d == DrainResult::Canceled);
            failed = (d == DrainResult::Failed);
        }
    }

    finish(canceled, failed);
}

} // namespace player
} // namespace videoeye
