#include "core/exporter/MediaExporter.h"

#include "core/ffmpeg_io/FfmpegInterrupt.h" // FFmpeg 阻塞 IO 的中断/超时
#include "infrastructure/logging/PerfMetrics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>
#include <QtGlobal> // Q_OS_WIN / Qt::CaseSensitivity
#include <algorithm>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace videoeye {
namespace exporter {

MediaExporter::MediaExporter(QObject* parent) : QObject(parent) {
}

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

// 导出耗时只在**真正导出成功**时记：取消/失败那次测的是"多久退出来"，混进平均值里
// 会把正常样本拉低，看不出长素材导出的真实成本。
// 成功 / 失败 / 取消的出口在 Export() 里散落多处（收尾漏斗 + 末尾三分支），
// 每处手写一次 Discard() 迟早漏一处 —— 交给析构时看一眼标志位统一判。
class ExportTimingGuard {
public:
    explicit ExportTimingGuard(const bool* succeeded) : succeeded_(succeeded), scope_(videoeye::kMetricFrameExportMs) {
    }
    ~ExportTimingGuard() {
        if (succeeded_ == nullptr || !*succeeded_)
            scope_.Discard();
    }

    ExportTimingGuard(const ExportTimingGuard&) = delete;
    ExportTimingGuard& operator=(const ExportTimingGuard&) = delete;

private:
    const bool* succeeded_;
    videoeye::PerfScope scope_;
};

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
    // sws 是按"建它的那一刻"的源规格建出来的。同一条流里源分辨率/像素格式变了
    // （多分辨率拼接、流的规格中途切换、编码器改了 pix_fmt）时，旧的那份 sws 还按老规格
    // 换到 enc_pix_fmt，画面缩放/转换全错，极端情况下直接崩。这三项记下建 sws 时用的源规格，
    // 送帧时比对，不一致就重建。
    int sws_src_w = 0;
    int sws_src_h = 0;
    AVPixelFormat sws_src_fmt = AV_PIX_FMT_NONE;
};

void free_streams(std::vector<StreamCtx>& streams) {
    for (auto& s : streams) {
        if (s.sws) {
            sws_freeContext(s.sws);
            s.sws = nullptr;
        }
        if (s.swr) {
            swr_free(&s.swr);
            s.swr = nullptr;
        }
        if (s.dec) {
            avcodec_free_context(&s.dec);
            s.dec = nullptr;
        }
        if (s.enc) {
            avcodec_free_context(&s.enc);
            s.enc = nullptr;
        }
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
        if (opt.format == "mp3")
            audio_enc = "libmp3lame";
        else if (opt.format == "wav")
            audio_enc = "pcm_s16le";
        else if (opt.format == "m4a")
            audio_enc = "aac";
        else if (opt.format == "flac")
            audio_enc = "flac";
        else if (opt.format == "ogg")
            audio_enc = "libopus";
        else
            audio_enc = "aac";
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
        list << QStringLiteral("libx264") << QStringLiteral("libopenh264") << QStringLiteral("mpeg4");
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
    return QString::number(static_cast<qint64>(QCoreApplication::applicationPid())) + QStringLiteral("-") +
           QUuid::createUuid().toString(QUuid::WithoutBraces);
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
        QFile::remove(backup_path); // UUID 撞名的概率可以忽略, 这里只是兜底
        if (!QFile::rename(target_path, backup_path)) {
            *err = QStringLiteral("无法备份已存在的目标文件 (替换已中止, 原文件未改动)");
            return false;
        }
    }

    if (QFile::rename(temp_path, target_path)) {
        if (target_exists)
            QFile::remove(backup_path);
        return true;
    }

    // 替换失败: 把备份改回去, 目标文件恢复原样
    if (target_exists) {
        if (QFile::rename(backup_path, target_path)) {
            *err = QStringLiteral("无法写入目标路径 (重命名失败, 原文件已恢复)");
            return false;
        }
        *err = QStringLiteral("无法写入目标路径, 且恢复原文件失败 (原文件已备份到: %1)").arg(backup_path);
        return false;
    }
    *err = QStringLiteral("无法写入目标路径 (重命名失败)");
    return false;
}

// ExportOptions.format 里 UI 传的是"扩展名"写法（mp4/mkv/ts/m4a/...），而
// av_guess_format() 的第一个参数是 FFmpeg 的**封装器名**（matroska/mpegts/ipod/...）。
// 两者对不上时 av_guess_format() 返回空，不能让 UI 去背这套名字。
QString MuxerNameForFormat(const QString& format) {
    if (format == QLatin1String("mkv"))
        return QStringLiteral("matroska");
    if (format == QLatin1String("ts"))
        return QStringLiteral("mpegts");
    if (format == QLatin1String("m4a"))
        return QStringLiteral("ipod");
    return format;
}

// 判定"两条路径是不是同一个文件"的键。
//
// 文件已存在时用规范路径：canonicalFilePath 会把 `./a.mp4`、`a/../a.mp4`
// 与符号链接都归一成同一条绝对路径 —— 手写字符串比较会漏掉这些写法，而用户
// 在导出框里敲 `./a.mp4` 是很常见的。
// 文件还不存在时（输出路径大多如此）canonicalFilePath 返回空，退回绝对路径 ——
// 它对不存在的路径同样返回一条完整的绝对路径，所以两条不同的路径不会退化成
// 两个空串、进而被误判成"同一个文件"。
QString SameFileKey(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    return QDir::cleanPath(info.absoluteFilePath());
}

// 真实路径是"临时文件"（xxx.mp4.part-<pid>-<uuid>），交给 avformat 按文件名猜封装时
// 它取的是**最后一个 '.'** 之后的那段（"part-<pid>-<uuid>"），匹配不到任何封装器 ——
// 导出因此 100% 失败。所以封装格式必须显式指定，不能靠猜。
bool open_output(AVFormatContext*& out_fmt, const ExportOptions& opt, const std::string& out_path, QString& err) {
    const QString muxer = MuxerNameForFormat(opt.format);
    if (avformat_alloc_output_context2(&out_fmt, av_guess_format(muxer.toUtf8().constData(), nullptr, nullptr), nullptr,
                                       out_path.c_str()) < 0 ||
        !out_fmt) {
        err = QString("无法确定输出格式 (扩展名可能不被支持): %1").arg(opt.format);
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

// 建立 输入流 -> 输出流 的映射，并为需要重编码的流备好解码器/编码器。
//
// 从 Export() 里整段搬出来的（原来 200 行挤在主流程中间，读的人要同时跟
// 上下文、错误收尾、区间裁剪四条线）。本函数只负责"建流"：
//   * 失败时把原因写进 err 并返回 false，收尾（释放 + 发信号）由调用方做；
//   * 它自己造出来、但还没进 streams 的 StreamCtx 必须在返回前释放 ——
//     那些指针此时 release_all() 看不见（见 abort_stream_setup）。
bool BuildOutputStreamMapping(AVFormatContext* in_fmt, AVFormatContext* out_fmt, const ExportOptions& opt,
                              const QString& video_enc_name, const QString& audio_enc_name,
                              std::vector<StreamCtx>& streams, QString& err) {
    // 建立流映射
    for (unsigned i = 0; i < in_fmt->nb_streams; ++i) {
        AVStream* in_st = in_fmt->streams[i];
        if (!in_st || !in_st->codecpar)
            continue;
        const AVMediaType mt = in_st->codecpar->codec_type;
        if (in_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            continue; // 跳过封面图

        bool include = false;
        if (opt.kind == ExportKind::Audio) {
            include = (mt == AVMEDIA_TYPE_AUDIO);
        } else {
            // 视频导出: 默认包含音轨; 勾选 no_audio 时仅导出视频画面
            include = (mt == AVMEDIA_TYPE_VIDEO) || (!opt.no_audio && mt == AVMEDIA_TYPE_AUDIO);
        }
        if (!include)
            continue;

        bool do_encode = opt.reencode;
        if (!do_encode) {
            // 容器是否原生支持该编码 -> 不支持则必须重编码
            const int q = avformat_query_codec(out_fmt->oformat, in_st->codecpar->codec_id, FF_COMPLIANCE_NORMAL);
            if (q != 1)
                do_encode = true;
        }

        AVStream* out_st = avformat_new_stream(out_fmt, nullptr);
        if (!out_st) {
            err = "无法创建输出流";
            return false;
        }

        StreamCtx sc;
        sc.in_idx = static_cast<int>(i);
        sc.out_idx = static_cast<int>(out_st->index);
        sc.do_encode = do_encode;

        // sc 要到本轮循环体末尾才 push_back 进 streams, 在此之前调用方的 release_all() /
        // free_streams() 都看不见它。所以这段里所有提前 return 都必须走这个函数 ——
        // 否则 dec / enc 就漏在半路上了。它顺带把失败原因写进 err（调用方据此收尾）。
        auto abort_stream_setup = [&](const QString& msg) -> bool {
            if (sc.sws) {
                sws_freeContext(sc.sws);
                sc.sws = nullptr;
            }
            if (sc.swr) {
                swr_free(&sc.swr);
                sc.swr = nullptr;
            }
            if (sc.dec) {
                avcodec_free_context(&sc.dec);
                sc.dec = nullptr;
            }
            if (sc.enc) {
                avcodec_free_context(&sc.enc);
                sc.enc = nullptr;
            }
            err = msg;
            return false;
        };

        if (!do_encode) {
            if (avcodec_parameters_copy(out_st->codecpar, in_st->codecpar) < 0) {
                return abort_stream_setup("复制流参数失败");
            }
            out_st->codecpar->codec_tag = 0;
            out_st->time_base = in_st->time_base;
        } else {
            const AVCodec* dec_codec = avcodec_find_decoder(in_st->codecpar->codec_id);
            if (!dec_codec) {
                return abort_stream_setup(QString("找不到解码器: %1").arg(avcodec_get_name(in_st->codecpar->codec_id)));
            }
            // 两处 AVCodecContext 都必须先看返回值: 紧接着就是 `enc->width = ...`，
            // 拿到 nullptr 会当场崩，而不是报出可读的 ExportError。
            AVCodecContext* dec = avcodec_alloc_context3(dec_codec);
            if (!dec) {
                return abort_stream_setup(QStringLiteral("内存不足: 无法分配解码器上下文"));
            }
            sc.dec = dec; // 从这里起所有权归 sc, 后续任一路径都由 abort_stream_setup 收回
            // 参数拷不过来时后续要用的是半初始化的上下文，宁可停在这里。
            if (avcodec_parameters_to_context(dec, in_st->codecpar) < 0) {
                return abort_stream_setup(QStringLiteral("解码器参数拷贝失败"));
            }
            if (avcodec_open2(dec, dec_codec, nullptr) < 0) {
                return abort_stream_setup("打开解码器失败");
            }

            const bool is_video = (mt == AVMEDIA_TYPE_VIDEO);
            const QStringList candidates = is_video ? video_encoder_candidates(video_enc_name)
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
                return abort_stream_setup(QString("找不到 %1 编码器（已尝试: %2；该 FFmpeg 构建可能未包含它们）")
                                              .arg(is_video ? QStringLiteral("视频") : QStringLiteral("音频"),
                                                   candidates.join(QStringLiteral(" / "))));
            }
            AVCodecContext* enc = avcodec_alloc_context3(enc_codec);
            if (!enc) {
                return abort_stream_setup(QStringLiteral("内存不足: 无法分配编码器上下文"));
            }
            sc.enc = enc; // 同上: 交给 sc 之后就不存在漏释放的路径
            if (mt == AVMEDIA_TYPE_VIDEO) {
                enc->width = dec->width;
                enc->height = dec->height;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100) // FFmpeg >= 7.1
                {
                    // FFmpeg 9.0 移除了 AVCodec::pix_fmts, 改用 avcodec_get_supported_config
                    const void* supported = nullptr;
                    if (avcodec_get_supported_config(nullptr, enc_codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &supported,
                                                     nullptr) >= 0 &&
                        supported)
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
                const int br = (opt.videoQuality == 0) ? base * 4 : (opt.videoQuality == 1) ? base * 2 : base;
                enc->bit_rate = static_cast<int64_t>(br) * 1000;
                enc->gop_size = 25;
                enc->max_b_frames = 2;
                enc->thread_count = 0;
                if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER)
                    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            } else {
                enc->sample_rate = dec->sample_rate > 0 ? dec->sample_rate : 44100;
                // av_channel_layout_copy() 返回负值表示失败，此时 dst 处于未定义状态，
                // 不能直接拿去编码。先按源声道数兜一次默认值，兜不住才判失败。
                int layout_ret = av_channel_layout_copy(&enc->ch_layout, &dec->ch_layout);
                if (layout_ret >= 0 && enc->ch_layout.nb_channels == 0) {
                    // av_channel_layout_default() 返回 void（FFmpeg 里本来就没有返回值），
                    // 不能赋值给 layout_ret —— 成功与否看下面 enc->ch_layout.nb_channels 即可。
                    av_channel_layout_default(&enc->ch_layout,
                                              dec->ch_layout.nb_channels > 0 ? dec->ch_layout.nb_channels : 2);
                }
                if (layout_ret < 0 || enc->ch_layout.nb_channels == 0) {
                    return abort_stream_setup(QStringLiteral("声道布局初始化失败: %1").arg(AvErrorString(layout_ret)));
                }
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100) // FFmpeg >= 7.1
                {
                    // FFmpeg 9.0 移除了 AVCodec::sample_fmts, 改用 avcodec_get_supported_config
                    const void* supported = nullptr;
                    if (avcodec_get_supported_config(nullptr, enc_codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &supported,
                                                     nullptr) >= 0 &&
                        supported)
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
                return abort_stream_setup("打开编码器失败");
            }
            if (avcodec_parameters_from_context(out_st->codecpar, enc) < 0) {
                // 这条路径以前会连带漏掉 dec / enc: sc 还没进 streams, release_all() 看不见它。
                return abort_stream_setup("写入编码器参数失败");
            }
            out_st->time_base = enc->time_base;
            if (mt == AVMEDIA_TYPE_VIDEO && dec->sample_aspect_ratio.num)
                out_st->sample_aspect_ratio = dec->sample_aspect_ratio;
        }
        streams.push_back(sc);
    }
    return true;
}

void MediaExporter::Export(const ExportOptions& opt) {
    // 取消状态只由外部写，这里绝不重置：启动前点的取消必须能被看到，
    // 否则"立即取消"会被吃掉、导出照常跑完。
    if (IsCanceled()) {
        emit ExportCanceled(opt.output_path);
        return;
    }
    // ---- 输出路径就是源文件时直接拒绝 ----
    //
    // 原子替换（目标 -> 备份 / 临时 -> 目标 / 删备份）保证了"失败或取消时原目标
    // 完好"，但目标就是源文件时，这套保护的方向正好反了：三条 rename 全成功之后
    // 备份被删掉，源片被片段**永久**替换，没有提示、也没有第二份拷贝。用户只是在
    // 导出框里选了同一个名字（或默认填了同名），不该因此静默丢掉原始素材。
    // "原地裁剪"不是本工具承诺的能力 —— 想要它，请先备份。
    if (!opt.input_path.isEmpty() && !opt.output_path.isEmpty()) {
#ifdef Q_OS_WIN
        // NTFS 不区分大小写：clip.mp4 与 CLIP.MP4 是同一个文件
        const Qt::CaseSensitivity path_cs = Qt::CaseInsensitive;
#else
        const Qt::CaseSensitivity path_cs = Qt::CaseSensitive;
#endif
        if (SameFileKey(opt.output_path).compare(SameFileKey(opt.input_path), path_cs) == 0) {
            emit ExportError(QStringLiteral("输出路径与源文件是同一个文件：完成后会用片段原地替换原文件，"
                                            "且无法撤销。请另选输出路径"));
            return;
        }
    }
    // 见上方 ExportTimingGuard：只有下面两处 ExportFinished 会把标志置起来
    bool export_succeeded = false;
    ExportTimingGuard export_timing(&export_succeeded);
    exporting_ = true;

    AVFormatContext* in_fmt = nullptr;
    AVFormatContext* out_fmt = nullptr;
    // pkt / frame 会被下面的 release_all lambda 使用，所以必须先在这里声明；
    // 真正的分配推迟到主循环之前（见下方 av_packet_alloc / av_frame_alloc）。
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    std::vector<StreamCtx> streams;

    QString err_msg;

    // 原子替换: 先写入同目录临时文件, 成功写完并关闭后才替换目标路径,
    // 这样导出失败/取消时原目标文件不受影响 (避免数据丢失)。
    // 临时文件名带 PID + UUID: 上一次导出卡住没退出时, 新导出不会写同一个 .part。
    const QString temp_path = opt.output_path + QStringLiteral(".part-") + UniqueSuffix();

    // 释放全部 FFmpeg 对象。三条收尾路径(成功 / 失败 / 取消)都从这里走。
    auto release_all = [&]() {
        if (pkt)
            av_packet_free(&pkt);
        if (frame)
            av_frame_free(&frame);
        free_streams(streams);
        if (out_fmt) {
            if (out_fmt->pb)
                avio_closep(&out_fmt->pb);
            avformat_free_context(out_fmt);
            out_fmt = nullptr;
        }
        if (in_fmt) {
            avformat_close_input(&in_fmt);
            in_fmt = nullptr;
        }
    };

    auto cleanup_and_emit = [&](bool is_error, const QString& msg) {
        release_all();
        exporting_ = false;
        if (is_error) {
            QFile::remove(temp_path); // 删除不完整产物 (不碰原目标文件)
            emit ExportError(msg.isEmpty() ? "导出失败" : msg);
        } else {
            export_succeeded = true;
            emit ExportFinished(opt.output_path);
        }
    };

    // 被取消打断的收尾: 只删临时文件, 保留原目标文件, 且**不报错** ——
    // 取消不是失败, 报成 ExportError 会让界面弹出"导出失败"的假告警。
    auto cancel_and_emit = [&]() {
        release_all();
        exporting_ = false;
        QFile::remove(temp_path);
        emit ExportCanceled(opt.output_path);
    };

    const std::string in_path = opt.input_path.toStdString();

    // pkt / frame 在主循环的每一轮都要用：拿到空指针不会报错，而是让 av_read_frame()
    // 和 av_frame_unref() 直接解引用 nullptr。低内存下 av_packet_alloc() 是会失败的，
    // 所以在动 FFmpeg 之前先把它俩要到手。
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        cleanup_and_emit(true, QStringLiteral("内存不足: 无法分配导出用的包/帧对象"));
        return;
    }

    // 输入上下文**必须自己分配**: AVIOInterruptCB 要在 avformat_open_input 之前装好，
    // 而 avformat_open_input(&ctx=nullptr, ...) 会让 FFmpeg 内部自己分配，没有地方装。
    in_fmt = avformat_alloc_context();
    if (!in_fmt) {
        err_msg = QStringLiteral("无法分配解封装上下文");
        cleanup_and_emit(true, err_msg);
        return;
    }
    // 打开阶段带绝对超时: 网络地址/管道/异常设备可能永远不返回;
    // 取消标志复用本对象的 cancel_ —— 它正是 RequestStopMediaExport 置位的那个。
    ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = &cancel_;
    ffmpeg_io::AttachInterrupt(in_fmt, interrupt, ffmpeg_io::kOpenTimeoutUs);

    const int open_ret = avformat_open_input(&in_fmt, in_path.c_str(), nullptr, nullptr);
    if (open_ret < 0) {
        // AVERROR_EXIT 只可能来自我们自己的中断回调: 取消 -> 走取消收尾; 超时 -> 报打开超时。
        // (以前这里不看返回值来源, 被取消也会被报成"无法打开输入文件"。)
        if (open_ret == AVERROR_EXIT && IsCanceled()) {
            cancel_and_emit();
            return;
        }
        err_msg = (open_ret == AVERROR_EXIT)
                      ? QString("打开输入超时 (%1 秒): %2").arg(ffmpeg_io::kOpenTimeoutUs / 1000000).arg(opt.input_path)
                      : QString("无法打开输入文件: %1").arg(opt.input_path);
        cleanup_and_emit(true, err_msg);
        return;
    }

    // 探测阶段允许更长时间, 但仍受取消约束
    interrupt.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
    const int probe_ret = avformat_find_stream_info(in_fmt, nullptr);
    if (probe_ret < 0) {
        if (probe_ret == AVERROR_EXIT && IsCanceled()) {
            cancel_and_emit();
            return;
        }
        err_msg =
            (probe_ret == AVERROR_EXIT) ? QStringLiteral("获取输入流信息超时") : QStringLiteral("无法获取输入流信息");
        cleanup_and_emit(true, err_msg);
        return;
    }
    // 进入逐包读取/编码阶段: 关掉绝对截止时间, 只保留取消响应。
    // (长素材的正常读取可以远超打开超时, 留着它会误杀。)
    interrupt.deadline_us = 0;

    // 失败收尾统一交给 cleanup_and_emit: 它比手写的这堆 free 多做了两件事 ——
    // 关掉可能已经打开的输出 pb、删掉可能已经生成的临时文件。
    if (!open_output(out_fmt, opt, temp_path.toStdString(), err_msg)) {
        cleanup_and_emit(true, err_msg);
        return;
    }

    const qint64 duration_ms = (in_fmt->duration != AV_NOPTS_VALUE)
                                   ? static_cast<qint64>(in_fmt->duration / (double)AV_TIME_BASE * 1000.0)
                                   : 0;
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

    if (!BuildOutputStreamMapping(in_fmt, out_fmt, opt, video_enc_name, audio_enc_name, streams, err_msg)) {
        cleanup_and_emit(true, err_msg);
        return;
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
            if (s.dec)
                avcodec_flush_buffers(s.dec);
            if (s.enc)
                avcodec_flush_buffers(s.enc);
        }
    }

    int last_progress = -1;
    // 进度基线。各个流的 PTS 互不相通（time_base / 时基起点都不同），所以逐流记一份
    // "已见到的最大 PTS(ms)"，取全局最大当进度位置 —— 任一流的 PTS 回退（环绕时基、
    // 非单调时间轴）都不会把进度条往回拽。
    std::vector<qint64> stream_seen_ms(static_cast<size_t>(in_fmt->nb_streams), 0);
    qint64 max_seen_ms = 0;     // 各流已见最大 PTS(ms)，只增不减
    qint64 base_pts = start_ms; // 进度零点（导出的起点）

    // 把一帧送进编码器并写出数据包
    auto write_encoded_packets = [&](StreamCtx& s) {
        AVPacket* epkt = av_packet_alloc();
        if (!epkt) {
            // 每写一轮都重新分配, 所以这里失败只影响本轮; 记进 err_msg 让外层按失败收尾。
            if (err_msg.isEmpty())
                err_msg = QStringLiteral("内存不足: 无法分配编码输出包");
            return;
        }
        for (;;) {
            const int ret = avcodec_receive_packet(s.enc, epkt);
            // EAGAIN(输出还没就绪) / EOF(编码器已排空) 都是正常状态, 不算错误
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
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
            if (wr < 0 && err_msg.isEmpty())
                err_msg = "写入编码输出包失败";
        }
        av_packet_free(&epkt);
    };

    auto encode_video_frame = [&](StreamCtx& s, AVFrame* src) -> bool {
        // 源规格变了（或首次）就重建 sws。原来只建一份、之后一直复用，源流中途改
        // 分辨率/像素格式时缩放结果全错。
        if (s.sws &&
            (s.sws_src_w != src->width || s.sws_src_h != src->height || s.sws_src_fmt != (AVPixelFormat)src->format)) {
            sws_freeContext(s.sws);
            s.sws = nullptr;
        }
        if (!s.sws) {
            s.sws = sws_getContext(src->width, src->height, (AVPixelFormat)src->format, s.enc->width, s.enc->height,
                                   s.enc_pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!s.sws) {
                err_msg = "初始化图像缩放器失败";
                return false;
            }
            s.sws_src_w = src->width;
            s.sws_src_h = src->height;
            s.sws_src_fmt = (AVPixelFormat)src->format;
        }
        AVFrame* out_frame = av_frame_alloc();
        if (!out_frame) {
            err_msg = QStringLiteral("内存不足: 无法分配视频编码帧");
            return false;
        }
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
        const int scaled =
            sws_scale(s.sws, src->data, src->linesize, 0, src->height, out_frame->data, out_frame->linesize);
        if (scaled < s.enc->height) {
            av_frame_free(&out_frame);
            err_msg = "图像缩放失败";
            return false;
        }
        out_frame->pts = (src->pts == AV_NOPTS_VALUE) ? s.enc_pts++ : src->pts;
        s.enc_pts = out_frame->pts + 1;
        const int send_ret = avcodec_send_frame(s.enc, out_frame);
        av_frame_free(&out_frame);
        if (send_ret < 0) {
            err_msg = "送入编码帧失败";
            return false;
        }
        write_encoded_packets(s); // 写入失败会在内部写 err_msg
        return err_msg.isEmpty();
    };

    auto encode_audio_frame = [&](StreamCtx& s, AVFrame* src) -> bool {
        if (!s.swr) {
            if (swr_alloc_set_opts2(&s.swr, &s.enc->ch_layout, s.enc->sample_fmt, s.enc->sample_rate, &src->ch_layout,
                                    (AVSampleFormat)src->format, src->sample_rate, 0, nullptr) < 0 ||
                !s.swr) {
                err_msg = "初始化音频重采样器失败";
                return false;
            }
            if (swr_init(s.swr) < 0) {
                swr_free(&s.swr);
                err_msg = "初始化音频重采样器失败";
                return false;
            }
        }
        const int dst_nb = av_rescale_rnd(src->nb_samples, s.enc->sample_rate, src->sample_rate, AV_ROUND_UP);
        AVFrame* out_frame = av_frame_alloc();
        if (!out_frame) {
            err_msg = QStringLiteral("内存不足: 无法分配音频编码帧");
            return false;
        }
        out_frame->format = s.enc->sample_fmt;
        out_frame->sample_rate = s.enc->sample_rate;
        // 布局拷不过来时, av_frame_get_buffer() 会按 nb_channels=0 分配出 0 字节缓冲,
        // 后面 swr_convert() 就写成越界。这里是每帧都要做的操作, 必须看返回值。
        const int layout_ret = av_channel_layout_copy(&out_frame->ch_layout, &s.enc->ch_layout);
        if (layout_ret < 0) {
            av_frame_free(&out_frame);
            err_msg = QStringLiteral("拷贝声道布局失败: %1").arg(AvErrorString(layout_ret));
            return false;
        }
        out_frame->nb_samples = dst_nb;
        if (av_frame_get_buffer(out_frame, 0) < 0) {
            av_frame_free(&out_frame);
            err_msg = "分配编码帧缓冲区失败";
            return false;
        }
        // swr_convert 需要 const uint8_t**；AVFrame::data 是 uint8_t*[8]
        const uint8_t** src_data = (const uint8_t**)src->data;
        const int got = swr_convert(s.swr, out_frame->data, dst_nb, src_data, src->nb_samples);
        if (got < 0) {
            av_frame_free(&out_frame);
            err_msg = "音频重采样失败";
            return false;
        }
        out_frame->nb_samples = got;
        out_frame->pts = s.enc_pts;
        s.enc_pts += got;
        const int send_ret = avcodec_send_frame(s.enc, out_frame);
        av_frame_free(&out_frame);
        if (send_ret < 0) {
            err_msg = "送入编码帧失败";
            return false;
        }
        write_encoded_packets(s);
        return err_msg.isEmpty();
    };

    // 区间**起点**裁剪。
    //
    // av_seek_frame() 只能落在关键帧上，而请求的起点几乎总在两个关键帧之间：
    // 回退之后拿到的第一个包可能**早于** start_ms。以前这里只把 start_ms 用于 seek
    // 和进度零点，没有任何地方丢弃早于起点的包/帧 —— 于是"仅导出 0.8s 之后"实际会把
    // 0~0.8s 的内容一起写进产物（实测 1.6s 素材导出 800~1600ms 得到整条 1.6s）。
    // 终点一直有检查，起点没有，是这个 bug 藏这么久的原因。
    //
    // 两种模式的丢法必须不同：
    //   * 拷贝：整包丢。产物从非关键帧开头，这是 -c copy 裁剪的固有限制
    //     （ffmpeg 命令行同样如此），但至少不把用户排除掉的内容塞进来。
    //   * 编码：包**照喂**给解码器（从关键帧起才能解出后续帧，跳过会直接解码报错），
    //     只丢掉解出来但早于起点的帧。
    auto packet_before_start = [&](const AVPacket* p, const AVStream* st) -> bool {
        if (start_ms <= 0 || p->pts == AV_NOPTS_VALUE)
            return false;
        const qint64 pts_ms = static_cast<qint64>(p->pts * av_q2d(st->time_base) * 1000.0);
        return pts_ms < start_ms;
    };
    auto frame_outside_range = [&](const StreamCtx& s, const AVFrame* f) -> bool {
        if (start_ms <= 0 && opt.end_ms <= 0)
            return false;
        const int64_t ts = (f->pts != AV_NOPTS_VALUE) ? f->pts : f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            return false;
        const qint64 pts_ms =
            static_cast<qint64>(ts * av_q2d(in_fmt->streams[static_cast<unsigned>(s.in_idx)]->time_base) * 1000.0);
        if (start_ms > 0 && pts_ms < start_ms)
            return true;
        // 终点只按用户明确要求的 opt.end_ms 裁：end_ms 在时长未知时是 int64 上限、
        // 在时长已知时是容器声明的时长，拿它裁会误伤元数据少报的真实尾帧。
        return opt.end_ms > 0 && pts_ms > end_ms;
    };

    // 主读取循环
    bool reached_end = false;
    // 被中断回调打断的读: 必须与"读错了"分开 —— 前者是取消, 后者是失败。
    // (前者以前会被当成"读取输入文件失败", 用户点取消却看到一条报错。)
    bool interrupted = false;
    while (!IsCanceled()) {
        const int ret = av_read_frame(in_fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                reached_end = true; // 正常读到文件尾
            } else if (ret == AVERROR_EXIT) {
                // 中断回调打断。导出阶段已把 deadline 清零, 所以只可能是用户取消。
                interrupted = true;
            } else if (err_msg.isEmpty()) {
                err_msg = "读取输入文件失败"; // 真实读错误, 不应被当作成功结束
            }
            break;
        }

        StreamCtx* sc = nullptr;
        for (auto& s : streams)
            if (s.in_idx == pkt->stream_index) {
                sc = &s;
                break;
            }
        if (!sc) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* in_st = in_fmt->streams[pkt->stream_index];
        // PTS(ms) 一次算好复用：进度统计、起点裁剪、终点检查都要它，
        // 原来每处各算一遍（浮点换算重复三次，改一处漏两处）。
        const qint64 pkt_pts_ms = (pkt->pts != AV_NOPTS_VALUE)
                                      ? static_cast<qint64>(pkt->pts * av_q2d(in_st->time_base) * 1000.0)
                                      : AV_NOPTS_VALUE;
        if (pkt_pts_ms != AV_NOPTS_VALUE && duration_ms > 0) {
            qint64& seen = stream_seen_ms[static_cast<size_t>(pkt->stream_index)];
            if (pkt_pts_ms > seen)
                seen = pkt_pts_ms;
            if (seen > max_seen_ms)
                max_seen_ms = seen;
        }

        // 区间起点检查（拷贝模式才丢包；编码模式的丢帧在解出帧之后，见 encode 分支）
        if (!sc->do_encode && packet_before_start(pkt, in_st)) {
            av_packet_unref(pkt);
            continue;
        }

        // 区间终点检查
        if (end_ms > 0 && pkt_pts_ms != AV_NOPTS_VALUE && pkt_pts_ms > end_ms) {
            av_packet_unref(pkt);
            reached_end = true;
            break;
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
                    if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF)
                        break;
                    if (recv_ret < 0) {
                        // 真实解码错误。旧代码写成 `while (... >= 0)`, 这里会静默退出,
                        // 截断/损坏的输入于是被当成"读完了", 最终误报导出成功。
                        if (err_msg.isEmpty())
                            err_msg = QString("解码失败: %1").arg(AvErrorString(recv_ret));
                        break;
                    }
                    // 包已喂进解码器（从关键帧起才解得动），但解出来的帧可能早于起点
                    // 或晚于终点 —— 这些不进编码器，否则产物仍会带上区间外的内容。
                    if (frame_outside_range(*sc, frame)) {
                        av_frame_unref(frame);
                        continue;
                    }
                    const bool ok =
                        (mt == AVMEDIA_TYPE_VIDEO) ? encode_video_frame(*sc, frame) : encode_audio_frame(*sc, frame);
                    av_frame_unref(frame);
                    if (!ok)
                        break; // 编码/重采样失败 -> err_msg 已置, 外层据此停止
                }
            } else if (send_ret != AVERROR(EAGAIN)) {
                if (err_msg.isEmpty())
                    err_msg = QString("解码输入失败: %1").arg(AvErrorString(send_ret));
            }
            av_packet_unref(pkt);
        }

        // 写入失败立即停止, 避免后续包继续写入并掩盖错误 (否则会被误报为成功)
        if (!err_msg.isEmpty())
            break;

        // 进度：按"已见最大 PTS"算，不再用当前包单点，因此只会往前走。
        if (duration_ms > 0) {
            const qint64 span = end_ms - start_ms;
            int progress_percent = 0;
            if (span > 0) {
                // 先比大小再乘：end_ms 可能是 int64 上限（时长未知时），直接乘 100 会溢出。
                const qint64 elapsed = max_seen_ms - base_pts;
                progress_percent = (elapsed >= span) ? 100 : static_cast<int>(elapsed * 100 / span);
            }
            progress_percent = qBound(0, progress_percent, 100);
            if (progress_percent != last_progress) {
                last_progress = progress_percent;
                emit ExportProgress(progress_percent);
            }
        }
    }

    // flush 编码器
    bool flushed_ok = false;
    if (!IsCanceled() && err_msg.isEmpty()) {
        for (auto& s : streams) {
            if (!s.do_encode)
                continue;

            // 排空解码器。AVERROR_EOF 表示此前已排空过, 与 EAGAIN 一样属于正常状态。
            const int dp = avcodec_send_packet(s.dec, nullptr);
            if (dp < 0 && dp != AVERROR_EOF) {
                err_msg = QString("结束解码失败: %1").arg(AvErrorString(dp));
                break;
            }
            for (;;) {
                const int rr = avcodec_receive_frame(s.dec, frame);
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF)
                    break;
                if (rr < 0) {
                    err_msg = QString("解码失败: %1").arg(AvErrorString(rr));
                    break;
                }
                if (frame_outside_range(s, frame)) {
                    av_frame_unref(frame);
                    continue;
                }
                const bool ok = (s.dec->codec_type == AVMEDIA_TYPE_VIDEO) ? encode_video_frame(s, frame)
                                                                          : encode_audio_frame(s, frame);
                av_frame_unref(frame);
                if (!ok)
                    break; // 编码/重采样失败, err_msg 已置
            }
            if (!err_msg.isEmpty())
                break; // 解码/编码失败则停止 flush

            // 排空编码器。AVERROR_EOF 表示编码器此前已被 flush 过, 不是错误;
            // 原来这里连返回值都不看, 编码器真出错时照样会走到"替换目标文件"那一步。
            const int ef = avcodec_send_frame(s.enc, nullptr);
            if (ef < 0 && ef != AVERROR_EOF) {
                err_msg = QString("结束编码失败: %1").arg(AvErrorString(ef));
                break;
            }
            write_encoded_packets(s);
            if (!err_msg.isEmpty())
                break; // 写入失败则停止 flush
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
    if (pkt)
        av_packet_free(&pkt);
    if (frame)
        av_frame_free(&frame);
    free_streams(streams);

    const bool canceled = IsCanceled() || interrupted;
    if (out_fmt && out_fmt->pb)
        avio_closep(&out_fmt->pb);
    avformat_free_context(out_fmt);
    out_fmt = nullptr;
    if (in_fmt)
        avformat_close_input(&in_fmt);

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
            export_succeeded = true;
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
