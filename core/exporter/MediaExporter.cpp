#include "MediaExporter.h"

#include <QFile>
#include <QFileInfo>
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

void MediaExporter::Cancel() {
    cancel_ = true;
}

namespace {

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
    exporting_ = true;
    cancel_ = false;

    AVFormatContext* in_fmt = nullptr;
    AVFormatContext* out_fmt = nullptr;
    std::vector<StreamCtx> streams;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    QString err_msg;
    bool ok = false;

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
            QFile::remove(opt.output_path); // 删除不完整产物
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

    if (!open_output(out_fmt, opt.output_path.toStdString(), err_msg)) {
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
    qint64 end_ms = (opt.end_ms > 0 && opt.end_ms < duration_ms) ? opt.end_ms : duration_ms;
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

            const char* enc_name = (mt == AVMEDIA_TYPE_VIDEO)
                ? video_enc_name.toUtf8().constData()
                : audio_enc_name.toUtf8().constData();
            const AVCodec* enc_codec = avcodec_find_encoder_by_name(enc_name);
            if (!enc_codec) {
                enc_name = (mt == AVMEDIA_TYPE_VIDEO) ? "mpeg4" : "aac";
                enc_codec = avcodec_find_encoder_by_name(enc_name);
            }
            if (!enc_codec) {
                err_msg = QString("找不到编码器: %1 (该格式可能需要完整版 FFmpeg)").arg(enc_name);
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
        while (avcodec_receive_packet(s.enc, epkt) >= 0) {
            epkt->stream_index = s.out_idx;
            av_packet_rescale_ts(epkt, s.enc->time_base, out_fmt->streams[s.out_idx]->time_base);
            av_interleaved_write_frame(out_fmt, epkt);
            av_packet_unref(epkt);
        }
        av_packet_free(&epkt);
    };

    auto encode_video_frame = [&](StreamCtx& s, AVFrame* src) {
        if (!s.sws) {
            s.sws = sws_getContext(src->width, src->height, (AVPixelFormat)src->format,
                                   s.enc->width, s.enc->height, s.enc_pix_fmt,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!s.sws) return;
        }
        AVFrame* out_frame = av_frame_alloc();
        out_frame->format = s.enc_pix_fmt;
        out_frame->width = s.enc->width;
        out_frame->height = s.enc->height;
        if (av_frame_get_buffer(out_frame, 0) < 0) {
            av_frame_free(&out_frame);
            return;
        }
        sws_scale(s.sws, src->data, src->linesize, 0, src->height,
                  out_frame->data, out_frame->linesize);
        out_frame->pts = (src->pts == AV_NOPTS_VALUE) ? s.enc_pts++ : src->pts;
        s.enc_pts = out_frame->pts + 1;
        if (avcodec_send_frame(s.enc, out_frame) == 0) {
            write_encoded_packets(s);
        }
        av_frame_free(&out_frame);
    };

    auto encode_audio_frame = [&](StreamCtx& s, AVFrame* src) {
        if (!s.swr) {
            if (swr_alloc_set_opts2(&s.swr,
                                    &s.enc->ch_layout, s.enc->sample_fmt, s.enc->sample_rate,
                                    &src->ch_layout, (AVSampleFormat)src->format, src->sample_rate,
                                    0, nullptr) < 0 || !s.swr) {
                return;
            }
            if (swr_init(s.swr) < 0) { swr_free(&s.swr); return; }
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
            return;
        }
        const int got = swr_convert(s.swr, out_frame->data, dst_nb,
                                    src->data, src->nb_samples);
        if (got < 0) { av_frame_free(&out_frame); return; }
        out_frame->nb_samples = got;
        out_frame->pts = s.enc_pts;
        s.enc_pts += got;
        if (avcodec_send_frame(s.enc, out_frame) == 0) {
            write_encoded_packets(s);
        }
        av_frame_free(&out_frame);
    };

    // 主读取循环
    bool reached_end = false;
    while (!cancel_) {
        const int ret = av_read_frame(in_fmt, pkt);
        if (ret < 0) { reached_end = true; break; }

        StreamCtx* sc = nullptr;
        for (auto& s : streams) if (s.in_idx == pkt->stream_index) { sc = &s; break; }
        if (!sc) { av_packet_unref(pkt); continue; }

        AVStream* in_st = in_fmt->streams[pkt->stream_index];

        // 区间终点检查
        if (end_ms > 0 && pkt->pts != AV_NOPTS_VALUE) {
            const qint64 pts_ms = static_cast<qint64>(
                pkt->pts * av_q2d(in_st->time_base) * 1000.0);
            if (pts_ms > end_ms) { av_packet_unref(pkt); reached_end = true; break; }
        }

        if (!sc->do_encode) {
            pkt->stream_index = sc->out_idx;
            av_packet_rescale_ts(pkt, in_st->time_base, out_fmt->streams[sc->out_idx]->time_base);
            av_interleaved_write_frame(out_fmt, pkt);
            av_packet_unref(pkt);
        } else {
            const AVMediaType mt = sc->dec->codec_type;
            if (mt == AVMEDIA_TYPE_VIDEO) {
                if (avcodec_send_packet(sc->dec, pkt) == 0) {
                    while (avcodec_receive_frame(sc->dec, frame) >= 0) {
                        encode_video_frame(*sc, frame);
                        av_frame_unref(frame);
                    }
                }
            } else {
                if (avcodec_send_packet(sc->dec, pkt) == 0) {
                    while (avcodec_receive_frame(sc->dec, frame) >= 0) {
                        encode_audio_frame(*sc, frame);
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_unref(pkt);
        }

        // 进度
        if (pkt->pts != AV_NOPTS_VALUE && duration_ms > 0) {
            qint64 cur = static_cast<qint64>(pkt->pts * av_q2d(in_st->time_base) * 1000.0);
            int pct = static_cast<int>((cur - start_ms) * 100 / (end_ms - start_ms));
            pct = qBound(0, pct, 100);
            if (pct != last_progress) { last_progress = pct; emit ExportProgress(pct); }
        }
    }

    // flush 编码器
    if (!cancel_) {
        for (auto& s : streams) {
            if (!s.do_encode) continue;
            avcodec_send_packet(s.dec, nullptr);
            while (avcodec_receive_frame(s.dec, frame) >= 0) {
                if (s.dec->codec_type == AVMEDIA_TYPE_VIDEO) encode_video_frame(s, frame);
                else encode_audio_frame(s, frame);
                av_frame_unref(frame);
            }
            avcodec_send_frame(s.enc, nullptr);
            write_encoded_packets(s);
        }
        av_write_trailer(out_fmt);
        emit ExportProgress(100);
    }

    // 统一清理
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    free_streams(streams);

    const bool canceled = cancel_.load();
    if (out_fmt->pb) avio_closep(&out_fmt->pb);
    avformat_free_context(out_fmt);
    out_fmt = nullptr;
    if (in_fmt) avformat_close_input(&in_fmt);

    exporting_ = false;

    if (canceled) {
        QFile::remove(opt.output_path);
        emit ExportCanceled(opt.output_path);
    } else if (reached_end) {
        emit ExportFinished(opt.output_path);
    } else {
        // 非取消也非正常结束 (读取出错但已写部分) -> 视为错误
        QFile::remove(opt.output_path);
        emit ExportError("导出过程中读取失败");
    }
}

} // namespace exporter
} // namespace videoeye
