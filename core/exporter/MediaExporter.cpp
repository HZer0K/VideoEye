#include "core/exporter/MediaExporter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace videoeye {
namespace exporter {

MediaExporter::MediaExporter(QObject* parent)
    : QObject(parent) {}

bool MediaExporter::IsCanceled() const {
    return cancel_.load(std::memory_order_acquire) || cancel_token_.IsCanceled();
}

void MediaExporter::Cancel() {
    cancel_.store(true, std::memory_order_release);
}

namespace {

// FFmpeg 错误码 -> 可读描述。不用 av_err2str 宏 (MSVC 不支持其 compound literal 写法)。
QString AvErrorString(int ret) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(ret, errbuf, sizeof(errbuf)) != 0) {
        return QStringLiteral("错误码 %1").arg(ret);
    }
    return QString::fromUtf8(errbuf);
}

// FFmpeg send/receive 返回值的分类约定（本文件内所有收发点统一遵守）:
//   * ret >= 0                        -> 正常
//   * AVERROR(EAGAIN)                 -> 正常: 送入队列暂满 / 输出还没就绪, 稍后重试
//   * AVERROR_EOF                     -> 正常: 该方向已排空
//   * 其它负值                         -> **真实错误**, 必须写 err_msg 并立即停止
//
// 为什么必须分: 旧代码写的是 `while (avcodec_receive_packet(enc, pkt) >= 0)` /
// `while (avcodec_receive_frame(dec, frame) >= 0)`, 编解码器报真错时循环直接退出、
// err_msg 仍是空的 —— 截断/损坏的输入会被当成"读完了", 最终错误地发出 ExportFinished
// 并原子替换掉用户的目标文件。

// 单条流的上下文 (copy 或 encode)
struct StreamCtx {
    int in_idx = -1;
    int out_idx = -1;
    bool do_encode = false;
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVPixelFormat enc_pix_fmt = AV_PIX_FMT_YUV420P;
    int64_t enc_pts = 0; // 音频输出采样累计 (用于 pts)
};

void free_streams(std::vector<StreamCtx>& streams) {
    for (auto& s : streams) {
        if (s.sws) { sws_freeContext(s.sws); s.sws = nullptr; }
        if (s.swr) { swr_free(&s.swr); s.swr = nullptr; }
        if (s.dec) { avcodec_free_context(&s.dec); s.dec = nullptr; }
        if (s.enc) { avcodec_free_context(&s.enc); s.enc = nullptr; }
    }
    streams.clear();
}

// 依据目标格式推断默认编码器名称
void resolve_encoder_names(const ExportOptions& opt, QString& video_enc, QString& audio_enc) {
    if (opt.format == "webm") {
        video_enc = "libvpx-vp9";
        audio_enc = "libopus";
    } else if (opt.format == "avi") {
        video_enc = "libx264";
        audio_enc = "libmp3lame";
    } else if (opt.kind == ExportKind::Audio) {
        if (opt.format == "mp3")       audio_enc = "libmp3lame";
        else if (opt.format == "wav")  audio_enc = "pcm_s16le";
        else if (opt.format == "m4a")  audio_enc = "aac";
        else if (opt.format == "flac") audio_enc = "flac";
        else if (opt.format == "ogg")  audio_enc = "libopus";
        else                           audio_enc = "aac";
    } else {
        // mp4 / mkv / mov / ts
        video_enc = "libx264";
        audio_enc = "aac";
    }
}

// 编码器候选链: 首选在前, 逐个降级
//
// 为什么需要这条链: libx264 是 **GPL-only** 组件, LGPL 构建的 FFmpeg 里没有它。
// VideoEye 自身是 MIT, 允许用 LGPL 构建的 FFmpeg 来构建它（见
// docs/FFMPEG_COMMAND_WORKBENCH.md「许可证」一节）—— 那时不能因为缺 libx264
// 就让整个导出功能废掉, 于是退到 libopenh264（Cisco 的 OpenH264, BSD 授权,
// 同样是 H.264），再退到 mpeg4（MPEG-4 Part 2, LGPL）。
// 用 GPL 构建时首选仍是 libx264, 行为完全不变。
QStringList video_encoder_candidates(const QString& preferred) {
    QStringList list;
    list << preferred;
    if (preferred == QLatin1String("libvpx-vp9")) {
        // webm 只认 VPx/AV1, mpeg4 塞不进去
        list << QStringLiteral("libvpx");
    } else {
        list << QStringLiteral("libx264") << QStringLiteral("libopenh264")
             << QStringLiteral("mpeg4");
    }
    list.removeDuplicates();
    return list;
}

// 给临时文件/Rename 备份生成一个全局唯一后缀。
//
// 为什么不能用固定的 ".part": 上一次导出被取消后线程可能还没退出(FFmpeg 卡在网络 IO)，
// 用户立刻再导出一次到同一个目标路径 —— 两个进程/两个线程就会写同一个临时文件，
// 互相把对方的产物截断。带上 PID + UUID 后，同目录并发也各写各的。
QString UniqueSuffix() {
    return QString::number(static_cast<qint64>(QCoreApplication::applicationPid())) +
           QStringLiteral("-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// 原子替换目标文件。
//
// 不能"先删除目标再 rename 临时文件": rename 一旦失败(目标被占用/磁盘满/权限问题)，
// 原文件已经被删掉，用户同时失去新旧两份。这里的顺序是
//   目标 -> 备份   (rename)
//   临时 -> 目标   (rename)
//   删除备份
// 任一步失败都把备份改回目标，保证"要么换成功，要么原文件完好"。
bool ReplaceTargetAtomic(const QString& temp_path, const QString& target_path, QString* err) {
    const bool target_exists = QFileInfo::exists(target_path);
    QString backup_path;
    if (target_exists) {
        backup_path = target_path + QStringLiteral(".bak-") + UniqueSuffix();
        QFile::remove(backup_path);  // UUID 撞名的概率可以忽略, 这里只是兜底
        if (!QFile::rename(target_path, backup_path)) {
            *err = QStringLiteral("无法备份已存在的目标文件 (替换已中止, 原文件未改动)");
            return false;
        }
    }

    if (QFile::rename(temp_path, target_path)) {
        if (target_exists) QFile::remove(backup_path);
        return true;
    }

    // 替换失败: 把备份改回去, 目标文件恢复原样
    if (target_exists) {
        if (QFile::rename(backup_path, target_path)) {
            *err = QStringLiteral("无法写入目标路径 (重命名失败, 原文件已恢复)");
            return false;
        }
        *err = QStringLiteral("无法写入目标路径, 且恢复原文件失败 (原文件已备份到: %1)")
                   .arg(backup_path);
        return false;
    }
    *err = QStringLiteral("无法写入目标路径 (重命名失败)");
    return false;
}

bool open_output(AVFormatContext*& out_fmt, const std::string& out_path, QString& err) {
    if (avformat_alloc_output_context2(&out_fmt, nullptr, nullptr, out_path.c_str()) < 0 || !out_fmt) {
        err = QString("无法确定输出格式 (扩展名可能不被支持)");
        return false;
    }
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            err = "无法创建输出文件 (路径不可写或磁盘已满)";
            return false;
        }
    }
    return true;
}

} // namespace

void MediaExporter::Export(const ExportOptions& opt) {
    // 取消状态只由外部写，这里绝不重置：启动前点的取消必须能被看到，
    // 否则"立即取消"会被吃掉、导出照常跑完。
    if (IsCanceled()) {
        emit ExportCanceled(opt.output_path);
        return;
    }
    exporting_ = true;

    AVFormatContext* in_fmt = nullptr;
    AVFormatContext* out_fmt = nullptr;
    std::vector<StreamCtx> streams;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    QString err_msg;

    // 原子替换: 先写入同目录临时文件, 成功写完并关闭后才替换目标路径,
    // 这样导出失败/取消时原目标文件不受影响 (避免数据丢失)。
    // 临时文件名带 PID + UUID: 上一次导出卡住没退出时, 新导出不会写同一个 .part。
    const QString temp_path = opt.output_path + QStringLiteral(".part-") + UniqueSuffix();

    auto cleanup_and_emit = [&](bool is_error, const QString& msg) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        free_streams(streams);
        if (out_fmt) {
            if (out_fmt->pb) avio_closep(&out_fmt->pb);
            avformat_free_context(out_fmt);
            out_fmt = nullptr;
        }
        if (in_fmt) { avformat_close_input(&in_fmt); in_fmt = nullptr; }
        exporting_ = false;
        if (is_error) {
            QFile::remove(temp_path); // 删除不完整产物 (不碰原目标文件)
            emit ExportError(msg.isEmpty() ? "导出失败" : msg);
        } else {
            emit ExportFinished(opt.output_path);
        }
    };

    const std::string in_path = opt.input_path.toStdString();
    if (avformat_open_input(&in_fmt, in_path.c_str(), nullptr, nullptr) < 0) {
        err_msg = QString("无法打开输入文件: %1").arg(opt.input_path);
        cleanup_and_emit(true, err_msg);
        return;
    }
    if (avformat_find_stream_info(in_fmt, nullptr) < 0) {
        err_msg = "无法获取输入流信息";
        cleanup_and_emit(true, err_msg);
        return;
    }

    if (!open_output(out_fmt, temp_path.toStdString(), err_msg)) {
        // out_fmt 可能为 nullptr
        if (out_fmt) { avformat_free_context(out_fmt); out_fmt = nullptr; }
        if (in_fmt) avformat_close_input(&in_fmt);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        exporting_ = false;
        emit ExportError(err_msg);
        return;
    }

    const qint64 duration_ms = (in_fmt->duration != AV_NOPTS_VALUE)
        ? static_cast<qint64>(in_fmt->duration / (double)AV_TIME_BASE * 1000.0) : 0;
    qint64 start_ms = (opt.start_ms > 0) ? opt.start_ms : 0;
    qint64 end_ms = duration_ms;
    if (opt.end_ms > 0) {
        end_ms = (duration_ms > 0) ? std::min(opt.end_ms, duration_ms) : opt.end_ms;
    } else if (duration_ms <= 0) {
        end_ms = std::numeric_limits<qint64>::max();
    }
    if (start_ms >= end_ms) {
        err_msg = "导出区间无效 (起点需小于终点)";
        cleanup_and_emit(true, err_msg);
        return;
    }

    QString video_enc_name, audio_enc_name;
    resolve_encoder_names(opt, video_enc_name, audio_enc_name);

    // 建立流映射
    for (unsigned i = 0; i < in_fmt->nb_streams; ++i) {
        AVStream* in_st = in_fmt->streams[i];
        if (!in_st || !in_st->codecpar) continue;
        const AVMediaType mt = in_st->codecpar->codec_type;
        if (in_st->disposition & AV_DISPOSITION_ATTACHED_PIC) continue; // 跳过封面图

        bool include = false;
        if (opt.kind == ExportKind::Audio) {
            include = (mt == AVMEDIA_TYPE_AUDIO);
        } else {
            // 视频导出: 默认包含音轨; 勾选 no_audio 时仅导出视频画面
            include = (mt == AVMEDIA_TYPE_VIDEO) ||
                      (!opt.no_audio && mt == AVMEDIA_TYPE_AUDIO);
        }
        if (!include) continue;

        bool do_encode = opt.reencode;
        if (!do_encode) {
            // 容器是否原生支持该编码 -> 不支持则必须重编码
            const int q = avformat_query_codec(out_fmt->oformat, in_st->codecpar->codec_id,
                                               FF_COMPLIANCE_NORMAL);
            if (q != 1) do_encode = true;
        }

        AVStream* out_st = avformat_new_stream(out_fmt, nullptr);
        if (!out_st) { err_msg = "无法创建输出流"; cleanup_and_emit(true, err_msg); return; }

        StreamCtx sc;
        sc.in_idx = static_cast<int>(i);
        sc.out_idx = static_cast<int>(out_st->index);
        sc.do_encode = do_encode;

        if (!do_encode) {
            if (avcodec_parameters_copy(out_st->codecpar, in_st->codecpar) < 0) {
                err_msg = "复制流参数失败";
                cleanup_and_emit(true, err_msg);
                return;
            }
            out_st->codecpar->codec_tag = 0;
            out_st->time_base = in_st->time_base;
        } else {
            const AVCodec* dec_codec = avcodec_find_decoder(in_st->codecpar->codec_id);
            if (!dec_codec) {
                err_msg = QString("找不到解码器: %1").arg(avcodec_get_name(in_st->codecpar->codec_id));
                cleanup_and_emit(true, err_msg);
                return;
            }
            AVCodecContext* dec = avcodec_alloc_context3(dec_codec);
            avcodec_parameters_to_context(dec, in_st->codecpar);
            if (avcodec_open2(dec, dec_codec, nullptr) < 0) {
                avcodec_free_context(&dec);
                err_msg = "打开解码器失败";
                cleanup_and_emit(true, err_msg);
                return;
            }
            sc.dec = dec;

            const bool is_video = (mt == AVMEDIA_TYPE_VIDEO);
            const QStringList candidates =
                is_video ? video_encoder_candidates(video_enc_name)
                         : QStringList{audio_enc_name, QStringLiteral("aac")};

            QString enc_name;
            const AVCodec* enc_codec = nullptr;
            for (const QString& candidate : candidates) {
                const QByteArray name_bytes = candidate.toUtf8();
                enc_codec = avcodec_find_encoder_by_name(name_bytes.constData());
                if (enc_codec) {
                    enc_name = candidate;
                    break;
                }
            }
            if (!enc_codec) {
                err_msg = QString("找不到 %1 编码器（已尝试: %2；该 FFmpeg 构建可能未包含它们）")
                              .arg(is_video ? QStringLiteral("视频") : QStringLiteral("音频"),
                                   candidates.join(QStringLiteral(" / ")));
                cleanup_and_emit(true, err_msg);
                return;
            }
            AVCodecContext* enc = avcodec_alloc_context3(enc_codec);
            if (mt == AVMEDIA_TYPE_VIDEO) {
                enc->width = dec->width;
                enc->height = dec->height;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)   // FFmpeg >= 7.1
                {
                    // FFmpeg 9.0 移除了 AVCodec::pix_fmts, 改用 avcodec_get_supported_config
                    const void* supported = nullptr;
                    if (avcodec_get_supported_config(nullptr, enc_codec,
                            AV_CODEC_CONFIG_PIX_FORMAT, 0, &supported, nullptr) >= 0 && supported)
                        enc->pix_fmt = static_cast<const AVPixelFormat*>(supported)[0];
                    else
                        enc->pix_fmt = AV_PIX_FMT_YUV420P;
                }
#else
                enc->pix_fmt = (enc_codec->pix_fmts) ? enc_codec->pix_fmts[0] : AV_PIX_FMT_YUV420P;
#endif
                sc.enc_pix_fmt = enc->pix_fmt;
                enc->time_base = in_st->time_base;
                const int base = enc->width * enc->height;
                const int br = (opt.videoQuality == 0) ? base * 4
                             : (opt.videoQuality == 1) ? base * 2 : base;
                enc->bit_rate = static_cast<int64_t>(br) * 1000;
                enc->gop_size = 25;
                enc->max_b_frames = 2;
                enc->thread_count = 0;
                if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER)
                    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            } else {
                enc->sample_rate = dec->sample_rate > 0 ? dec->sample_rate : 44100;
                av_channel_layout_copy(&enc->ch_layout, &dec->ch_layout);
                if (enc->ch_layout.nb_channels == 0) {
                    av_channel_layout_default(&enc->ch_layout, dec->ch_layout.nb_channels ? dec->ch_layout.nb_channels : 2);
                }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)   // FFmpeg >= 7.1
                {
                    // FFmpeg 9.0 移除了 AVCodec::sample_fmts, 改用 avcodec_get_supported_config
                    const void* supported = nullptr;
                    if (avcodec_get_supported_config(nullptr, enc_codec,
                            AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &supported, nullptr) >= 0 && supported)
                        enc->sample_fmt = static_cast<const AVSampleFormat*>(supported)[0];
                    else
                        enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                }
#else
                enc->sample_fmt = (enc_codec->sample_fmts) ? enc_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
                enc->bit_rate = static_cast<int64_t>(opt.audioBitrateKbps) * 1000;
                enc->time_base = {1, enc->sample_rate};
                if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER)
                    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (avcodec_open2(enc, enc_codec, nullptr) < 0) {
                avcodec_free_context(&enc);
                avcodec_free_context(&dec);
                err_msg = "打开编码器失败";
                cleanup_and_emit(true, err_msg);
                return;
            }
            sc.enc = enc;
            if (avcodec_parameters_from_context(out_st->codecpar, enc) < 0) {
                err_msg = "写入编码器参数失败";
                cleanup_and_emit(true, err_msg);
                return;
            }
            out_st->time_base = enc->time_base;
            if (mt == AVMEDIA_TYPE_VIDEO && dec->sample_aspect_ratio.num)
                out_st->sample_aspect_ratio = dec->sample_aspect_ratio;
        }
        streams.push_back(sc);
    }

    if (streams.empty()) {
        err_msg = (opt.kind == ExportKind::Audio) ? "未找到音频流" : "未找到可导出的视频/音频流";
        cleanup_and_emit(true, err_msg);
        return;
    }

    if (avformat_write_header(out_fmt, nullptr) < 0) {
        err_msg = "写入文件头失败";
        cleanup_and_emit(true, err_msg);
        return;
    }

    emit ExportStarted(duration_ms);

    // 定位到起点
    if (start_ms > 0) {
        const int64_t seek_ts = start_ms * 1000LL; // 微秒
        if (av_seek_frame(in_fmt, -1, seek_ts, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(in_fmt, -1, seek_ts, AVSEEK_FLAG_ANY);
        }
        for (auto& s : streams) {
            if (s.dec) avcodec_flush_buffers(s.dec);
            if (s.enc) avcodec_flush_buffers(s.enc);
        }
    }

    int last_progress = -1;

    // 把一帧送进编码器并写出数据包
    auto write_encoded_packets = [&](StreamCtx& s) {
        AVPacket* epkt = av_packet_alloc();
        for (;;) {
            const int ret = avcodec_receive_packet(s.enc, epkt);
            // EAGAIN(输出还没就绪) / EOF(编码器已排空) 都是正常状态, 不算错误
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                // 编码器真实错误: 必须记下来, 否则外层会把它当成"正常结束"并报成功
                if (err_msg.isEmpty())
                    err_msg = QString("取出编码输出包失败: %1").arg(AvErrorString(ret));
                break;
            }
            epkt->stream_index = s.out_idx;
            av_packet_rescale_ts(epkt, s.enc->time_base, out_fmt->streams[s.out_idx]->time_base);
            const int wr = av_interleaved_write_frame(out_fmt, epkt);
            av_packet_unref(epkt);
            if (wr < 0 && err_msg.isEmpty()) err_msg = "写入编码输出包失败";
        }
        av_packet_free(&epkt);
    };

    auto encode_video_frame = [&](StreamCtx& s, AVFrame* src) -> bool {
        if (!s.sws) {
            s.sws = sws_getContext(src->width, src->height, (AVPixelFormat)src->format,
                                   s.enc->width, s.enc->height, s.enc_pix_fmt,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!s.sws) { err_msg = "初始化图像缩放器失败"; return false; }
        }
        AVFrame* out_frame = av_frame_alloc();
        out_frame->format = s.enc_pix_fmt;
        out_frame->width = s.enc->width;
        out_frame->height = s.enc->height;
        if (av_frame_get_buffer(out_frame, 0) < 0) {
            av_frame_free(&out_frame);
            err_msg = "分配编码帧缓冲区失败";
            return false;
        }
        // sws_scale 成功时返回输出图像的高度, 负值表示缩放失败。
        // 原来这里完全没看返回值: 缩放失败会继续把未初始化/残缺的帧送进编码器,
        // 产物是花屏甚至空轨, 却因为 err_msg 仍为空而被判成"导出成功"。
        const int scaled = sws_scale(s.sws, src->data, src->linesize, 0, src->height,
                                     out_frame->data, out_frame->linesize);
        if (scaled < s.enc->height) {
            av_frame_free(&out_frame);
            err_msg = "图像缩放失败";
            return false;
        }
        out_frame->pts = (src->pts == AV_NOPTS_VALUE) ? s.enc_pts++ : src->pts;
        s.enc_pts = out_frame->pts + 1;
        const int send_ret = avcodec_send_frame(s.enc, out_frame);
        av_frame_free(&out_frame);
        if (send_ret < 0) { err_msg = "送入编码帧失败"; return false; }
        write_encoded_packets(s);   // 写入失败会在内部写 err_msg
        return err_msg.isEmpty();
    };

    auto encode_audio_frame = [&](StreamCtx& s, AVFrame* src) -> bool {
        if (!s.swr) {
            if (swr_alloc_set_opts2(&s.swr,
                                    &s.enc->ch_layout, s.enc->sample_fmt, s.enc->sample_rate,
                                    &src->ch_layout, (AVSampleFormat)src->format, src->sample_rate,
                                    0, nullptr) < 0 || !s.swr) {
                err_msg = "初始化音频重采样器失败";
                return false;
            }
            if (swr_init(s.swr) < 0) { swr_free(&s.swr); err_msg = "初始化音频重采样器失败"; return false; }
        }
        const int dst_nb = av_rescale_rnd(src->nb_samples, s.enc->sample_rate,
                                          src->sample_rate, AV_ROUND_UP);
        AVFrame* out_frame = av_frame_alloc();
        out_frame->format = s.enc->sample_fmt;
        out_frame->sample_rate = s.enc->sample_rate;
        av_channel_layout_copy(&out_frame->ch_layout, &s.enc->ch_layout);
        out_frame->nb_samples = dst_nb;
        if (av_frame_get_buffer(out_frame, 0) < 0) {
            av_frame_free(&out_frame);
            err_msg = "分配编码帧缓冲区失败";
            return false;
        }
        // swr_convert 需要 const uint8_t**；AVFrame::data 是 uint8_t*[8]
        const uint8_t** src_data = (const uint8_t**)src->data;
        const int got = swr_convert(s.swr, out_frame->data, dst_nb,
                                    src_data, src->nb_samples);
        if (got < 0) { av_frame_free(&out_frame); err_msg = "音频重采样失败"; return false; }
        out_frame->nb_samples = got;
        out_frame->pts = s.enc_pts;
        s.enc_pts += got;
        const int send_ret = avcodec_send_frame(s.enc, out_frame);
        av_frame_free(&out_frame);
        if (send_ret < 0) { err_msg = "送入编码帧失败"; return false; }
        write_encoded_packets(s);
        return err_msg.isEmpty();
    };

    // 主读取循环
    bool reached_end = false;
    while (!IsCanceled()) {
        const int ret = av_read_frame(in_fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                reached_end = true;   // 正常读到文件尾
            } else if (err_msg.isEmpty()) {
                err_msg = "读取输入文件失败";   // 真实读错误, 不应被当作成功结束
            }
            break;
        }

        StreamCtx* sc = nullptr;
        for (auto& s : streams) if (s.in_idx == pkt->stream_index) { sc = &s; break; }
        if (!sc) { av_packet_unref(pkt); continue; }

        AVStream* in_st = in_fmt->streams[pkt->stream_index];
        int progress_percent = -1;
        if (pkt->pts != AV_NOPTS_VALUE && duration_ms > 0) {
            qint64 cur = static_cast<qint64>(pkt->pts * av_q2d(in_st->time_base) * 1000.0);
            progress_percent = static_cast<int>((cur - start_ms) * 100 / (end_ms - start_ms));
            progress_percent = qBound(0, progress_percent, 100);
        }

        // 区间终点检查
        if (end_ms > 0 && pkt->pts != AV_NOPTS_VALUE) {
            const qint64 pts_ms = static_cast<qint64>(
                pkt->pts * av_q2d(in_st->time_base) * 1000.0);
            if (pts_ms > end_ms) { av_packet_unref(pkt); reached_end = true; break; }
        }

        if (!sc->do_encode) {
            pkt->stream_index = sc->out_idx;
            av_packet_rescale_ts(pkt, in_st->time_base, out_fmt->streams[sc->out_idx]->time_base);
            const int write_ret = av_interleaved_write_frame(out_fmt, pkt);
            av_packet_unref(pkt);
            if (write_ret < 0) {
                err_msg = "写入输出包失败";
                reached_end = false;
                break;
            }
        } else {
            const AVMediaType mt = sc->dec->codec_type;
            // 解码输入: 真实错误(非 EAGAIN)要记录, 否则损坏输入会被当成正常结束
            const int send_ret = avcodec_send_packet(sc->dec, pkt);
            if (send_ret == 0) {
                for (;;) {
                    const int recv_ret = avcodec_receive_frame(sc->dec, frame);
                    // EAGAIN(还要继续喂包) / EOF(解码器已排空) 都是正常状态
                    if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) break;
                    if (recv_ret < 0) {
                        // 真实解码错误。旧代码写成 `while (... >= 0)`, 这里会静默退出,
                        // 截断/损坏的输入于是被当成"读完了", 最终误报导出成功。
                        if (err_msg.isEmpty())
                            err_msg = QString("解码失败: %1").arg(AvErrorString(recv_ret));
                        break;
                    }
                    const bool ok = (mt == AVMEDIA_TYPE_VIDEO)
                                        ? encode_video_frame(*sc, frame)
                                        : encode_audio_frame(*sc, frame);
                    av_frame_unref(frame);
                    if (!ok) break;   // 编码/重采样失败 -> err_msg 已置, 外层据此停止
                }
            } else if (send_ret != AVERROR(EAGAIN)) {
                if (err_msg.isEmpty())
                    err_msg = QString("解码输入失败: %1").arg(AvErrorString(send_ret));
            }
            av_packet_unref(pkt);
        }

        // 写入失败立即停止, 避免后续包继续写入并掩盖错误 (否则会被误报为成功)
        if (!err_msg.isEmpty()) break;

        // 进度
        if (progress_percent >= 0 && progress_percent != last_progress) {
            last_progress = progress_percent;
            emit ExportProgress(progress_percent);
        }
    }

    // flush 编码器
    bool flushed_ok = false;
    if (!IsCanceled() && err_msg.isEmpty()) {
        for (auto& s : streams) {
            if (!s.do_encode) continue;

            // 排空解码器。AVERROR_EOF 表示此前已排空过, 与 EAGAIN 一样属于正常状态。
            const int dp = avcodec_send_packet(s.dec, nullptr);
            if (dp < 0 && dp != AVERROR_EOF) {
                err_msg = QString("结束解码失败: %1").arg(AvErrorString(dp));
                break;
            }
            for (;;) {
                const int rr = avcodec_receive_frame(s.dec, frame);
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
                if (rr < 0) {
                    err_msg = QString("解码失败: %1").arg(AvErrorString(rr));
                    break;
                }
                const bool ok = (s.dec->codec_type == AVMEDIA_TYPE_VIDEO)
                                    ? encode_video_frame(s, frame)
                                    : encode_audio_frame(s, frame);
                av_frame_unref(frame);
                if (!ok) break;   // 编码/重采样失败, err_msg 已置
            }
            if (!err_msg.isEmpty()) break;   // 解码/编码失败则停止 flush

            // 排空编码器。AVERROR_EOF 表示编码器此前已被 flush 过, 不是错误;
            // 原来这里连返回值都不看, 编码器真出错时照样会走到"替换目标文件"那一步。
            const int ef = avcodec_send_frame(s.enc, nullptr);
            if (ef < 0 && ef != AVERROR_EOF) {
                err_msg = QString("结束编码失败: %1").arg(AvErrorString(ef));
                break;
            }
            write_encoded_packets(s);
            if (!err_msg.isEmpty()) break;   // 写入失败则停止 flush
        }
        if (err_msg.isEmpty()) {
            if (av_write_trailer(out_fmt) < 0) {
                err_msg = "写入文件尾失败";
            } else {
                emit ExportProgress(100);
                flushed_ok = true;
            }
        }
    }

    // 统一清理 (先关闭 pb 与 context, 再做文件替换)
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    free_streams(streams);

    const bool canceled = IsCanceled();
    if (out_fmt && out_fmt->pb) avio_closep(&out_fmt->pb);
    avformat_free_context(out_fmt);
    out_fmt = nullptr;
    if (in_fmt) avformat_close_input(&in_fmt);

    exporting_ = false;

    if (canceled) {
        // 取消: 只删除临时文件, 保留原目标文件
        QFile::remove(temp_path);
        emit ExportCanceled(opt.output_path);
    } else if (reached_end && err_msg.isEmpty() && flushed_ok) {
        // 成功: 写完临时文件并关闭后才替换目标。
        // 替换走"目标改名备份 -> 临时改名目标 -> 删备份", 任一步失败都能把备份改回来,
        // 不会出现"旧文件已删、新文件又没换上"的双向丢失。
        QString replace_err;
        if (ReplaceTargetAtomic(temp_path, opt.output_path, &replace_err)) {
            emit ExportFinished(opt.output_path);
        } else {
            QFile::remove(temp_path);
            emit ExportError(replace_err);
        }
    } else {
        // 读/写/封装失败: 删除临时文件, 保留原目标文件
        QFile::remove(temp_path);
        emit ExportError(err_msg.isEmpty() ? "导出过程中读取/写入失败" : err_msg);
    }
}

} // namespace exporter
} // namespace videoeye
