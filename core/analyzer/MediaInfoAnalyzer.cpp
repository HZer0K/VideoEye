#include "MediaInfoAnalyzer.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include "utils/Logger.h"

namespace videoeye {
namespace analyzer {

namespace {

// 把秒数格式化成 "1 h 23 min"、"4 min 5 s"、"678 ms" 这种短串
QString FormatDuration(double seconds) {
    if (seconds <= 0 || !std::isfinite(seconds)) return QStringLiteral("-");

    const long long total_ms = static_cast<long long>(seconds * 1000.0 + 0.5);
    const long long ms = total_ms % 1000;
    const long long total_s = total_ms / 1000;
    const long long s = total_s % 60;
    const long long total_min = total_s / 60;
    const long long min = total_min % 60;
    const long long h = total_min / 60;

    QString out;
    if (h > 0) out += QString::number(h) + QStringLiteral(" h ");
    if (h > 0 || min > 0) out += QString::number(min) + QStringLiteral(" min ");
    out += QString::number(s) + QStringLiteral(" s");
    if (h == 0 && total_min == 0) out += QStringLiteral(" ") + QString::number(ms) + QStringLiteral(" ms");
    return out;
}

QString FormatBitRate(int64_t bit_rate) {
    if (bit_rate <= 0) return QStringLiteral("-");
    if (bit_rate >= 1000000) {
        return QString::number(static_cast<double>(bit_rate) / 1000000.0, 'f', 1) + QStringLiteral(" Mb/s");
    }
    if (bit_rate >= 1000) {
        return QString::number(static_cast<double>(bit_rate) / 1000.0, 'f', 1) + QStringLiteral(" kb/s");
    }
    return QString::number(bit_rate) + QStringLiteral(" b/s");
}

QString FormatFileSize(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("-");
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', unit == 0 ? 0 : 2) + QStringLiteral(" ") + QString::fromLatin1(kUnits[unit]);
}

QString CodecName(const AVCodecParameters* par) {
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    if (desc && desc->name) return QString::fromUtf8(desc->name);
    return QString::fromUtf8(avcodec_get_name(par->codec_id));
}

QString CodecTag(const AVCodecParameters* par) {
    if (!par->codec_tag) return QString();
    char buf[AV_FOURCC_MAX_STRING_SIZE] = {0};
    av_fourcc_make_string(buf, par->codec_tag);
    return QString::fromUtf8(buf);
}

QString PixFmtName(int pix_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
    if (!desc) return QStringLiteral("-");
    QString name = QString::fromUtf8(desc->name);
    int bits = desc->comp[0].depth;
    if (bits > 0) name += QStringLiteral(" (") + QString::number(bits) + QStringLiteral(" bit)");
    return name;
}

QString SampleFmtName(int sample_fmt) {
    const char* name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(sample_fmt));
    return name ? QString::fromUtf8(name) : QStringLiteral("-");
}

QString RationalToString(const AVRational& q) {
    if (q.den == 0) return QStringLiteral("-");
    const double v = av_q2d(q);
    if (q.num == 0) return QStringLiteral("0");
    return QString::number(v, 'f', 3);
}

QString AspectRatioToString(const AVRational& q) {
    if (q.num <= 0 || q.den <= 0) return QStringLiteral("-");
    return QString::number(q.num) + QStringLiteral(":") + QString::number(q.den);
}

QString Trimmed(const char* s) {
    return (s && *s) ? QString::fromUtf8(s) : QString();
}

// 语言标签: eng -> English 之类；拿不到就原样返回
// 先读流级 metadata 再读容器级：容器级 language 是多流文件的「整体语言」，
// 反过来读的话会被套到每一条流上（多音轨文件里每条轨都显示成同一个语言）。
QString LanguageName(const AVDictionary* metadata, const AVStream* stream) {
    AVDictionaryEntry* lang = nullptr;
    if (stream) {
        // 流级优先：MP4/MKV 通常把语言写在每条流自己的 metadata 上
        lang = av_dict_get(stream->metadata, "language", nullptr, 0);
    }
    if (!lang && metadata) {
        lang = av_dict_get(metadata, "language", nullptr, 0);
    }
    if (!lang || !lang->value || !*lang->value) return QString();
    return QString::fromUtf8(lang->value);
}

void AppendMetadata(QString& out, const AVDictionary* dict, const QString& indent) {
    if (!dict) return;
    const AVDictionaryEntry* entry = nullptr;
    bool any = false;
    while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        if (!entry->value || !*entry->value) continue;
        if (!any) {
            out += indent + QStringLiteral("Metadata:\n");
            any = true;
        }
        out += indent + QStringLiteral("  ") + QString::fromUtf8(entry->key) +
               QStringLiteral(" : ") + QString::fromUtf8(entry->value) + QLatin1Char('\n');
    }
}

QString StreamTitle(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:    return QStringLiteral("Video");
        case AVMEDIA_TYPE_AUDIO:    return QStringLiteral("Audio");
        case AVMEDIA_TYPE_SUBTITLE: return QStringLiteral("Text");
        case AVMEDIA_TYPE_DATA:     return QStringLiteral("Data");
        case AVMEDIA_TYPE_ATTACHMENT: return QStringLiteral("Attachment");
        default:                    return QStringLiteral("Other");
    }
}

} // namespace

struct MediaInfoAnalyzer::Impl {
    AVFormatContext* fmt = nullptr;
    QString text;
    QString error;
    QString pcm_demuxer;
    int pcm_sample_rate = 0;
    int pcm_channels = 0;
    bool opened = false;
};

MediaInfoAnalyzer::MediaInfoAnalyzer() : impl_(new Impl()) {}

MediaInfoAnalyzer::~MediaInfoAnalyzer() {
    Close();
}

void MediaInfoAnalyzer::SetRawPcmHints(const QString& demuxer, int sample_rate, int channels) {
    impl_->pcm_demuxer = demuxer;
    impl_->pcm_sample_rate = sample_rate;
    impl_->pcm_channels = channels;
}

bool MediaInfoAnalyzer::Open(const QString& filePath) {
    Close();
    if (filePath.isEmpty()) {
        impl_->error = QStringLiteral("路径为空");
        return false;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        impl_->error = QStringLiteral("avformat_alloc_context 失败");
        return false;
    }

    AVDictionary* opts = nullptr;
    const AVInputFormat* iformat = nullptr;
    if (!impl_->pcm_demuxer.isEmpty()) {
        iformat = av_find_input_format(impl_->pcm_demuxer.toUtf8().constData());
        if (!iformat) {
            avformat_free_context(fmt);
            impl_->error = QStringLiteral("找不到裸流 demuxer: %1").arg(impl_->pcm_demuxer);
            return false;
        }
        if (impl_->pcm_sample_rate > 0) {
            av_dict_set_int(&opts, "sample_rate", impl_->pcm_sample_rate, 0);
        }
        if (impl_->pcm_channels > 0) {
            av_dict_set_int(&opts, "channels", impl_->pcm_channels, 0);
        }
    }

    // 探测字节数与分析时长收紧一些: 这里只要元信息, 不需要为了估算码率读一大段数据
    fmt->probesize = 32 * 1024 * 1024;
    fmt->max_analyze_duration = 5 * AV_TIME_BASE;

    int ret = avformat_open_input(&fmt, filePath.toUtf8().constData(),
                                  const_cast<AVInputFormat*>(iformat), &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        avformat_free_context(fmt);
        impl_->error = QStringLiteral("无法打开文件: %1").arg(QString::fromUtf8(errbuf));
        LOG_WARN(("MediaInfoAnalyzer: " + impl_->error).toStdString());
        return false;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        // 部分损坏文件拿不到完整流信息, 仍然保留已探测到的部分
        LOG_WARN("MediaInfoAnalyzer: find_stream_info 失败, 使用已探测到的部分信息");
    }

    impl_->fmt = fmt;
    impl_->opened = true;
    impl_->error.clear();

    // ---- 组装文本报告 ----
    QString out;
    const QString kIndent = QStringLiteral("  ");

    // ===== General =====
    out += QStringLiteral("General\n");
    out += kIndent + QStringLiteral("Complete name              : ") + filePath + QLatin1Char('\n');
    const QFileInfo fi(filePath);
    if (fi.exists()) {
        out += kIndent + QStringLiteral("File size                  : ") + FormatFileSize(fi.size()) + QLatin1Char('\n');
    }
    if (fmt->iformat) {
        QString container = Trimmed(fmt->iformat->name);
        if (const char* long_name = fmt->iformat->long_name) {
            if (long_name && *long_name) container += QStringLiteral(" (") + QString::fromUtf8(long_name) + QStringLiteral(")");
        }
        out += kIndent + QStringLiteral("Format                     : ") + container + QLatin1Char('\n');
    }
    if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE) {
        const double seconds = static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE);
        out += kIndent + QStringLiteral("Duration                   : ") + FormatDuration(seconds) + QLatin1Char('\n');
    }
    out += kIndent + QStringLiteral("Overall bit rate           : ") + FormatBitRate(fmt->bit_rate) + QLatin1Char('\n');
    if (fmt->start_time > 0 && fmt->start_time != AV_NOPTS_VALUE) {
        const double st = static_cast<double>(fmt->start_time) / static_cast<double>(AV_TIME_BASE);
        out += kIndent + QStringLiteral("Start time                 : ") + QString::number(st, 'f', 3) + QStringLiteral(" s") + QLatin1Char('\n');
    }
    out += kIndent + QStringLiteral("Stream count               : ") + QString::number(fmt->nb_streams) + QLatin1Char('\n');
    AppendMetadata(out, fmt->metadata, kIndent);
    out += QLatin1Char('\n');

    // ===== 每种流类型各一段 =====
    static const AVMediaType kOrder[] = {
        AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_SUBTITLE, AVMEDIA_TYPE_DATA
    };

    for (AVMediaType type : kOrder) {
        int index_in_type = 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* st = fmt->streams[i];
            if (!st || st->codecpar->codec_type != type) continue;

            const AVCodecParameters* par = st->codecpar;
            out += StreamTitle(type) + QStringLiteral(" #") + QString::number(index_in_type) + QLatin1Char('\n');
            ++index_in_type;

            out += kIndent + QStringLiteral("ID                         : ") + QString::number(st->index) + QLatin1Char('\n');
            out += kIndent + QStringLiteral("Format                     : ") + CodecName(par) + QLatin1Char('\n');
            if (const char* profile = avcodec_profile_name(par->codec_id, par->profile)) {
                if (profile && *profile) {
                    out += kIndent + QStringLiteral("Format profile             : ") + QString::fromUtf8(profile) + QLatin1Char('\n');
                }
            }
            if (par->level != AV_LEVEL_UNKNOWN && par->level != 0) {
                out += kIndent + QStringLiteral("Format level               : ") + QString::number(par->level) + QLatin1Char('\n');
            }
            if (!CodecTag(par).isEmpty()) {
                out += kIndent + QStringLiteral("Codec ID                   : ") + CodecTag(par) + QLatin1Char('\n');
            }
            if (par->codec_id == AV_CODEC_ID_NONE) {
                // pass-through / 未知编码
            }

            if (type == AVMEDIA_TYPE_VIDEO) {
                out += kIndent + QStringLiteral("Width                      : ") + QString::number(par->width) + QLatin1Char('\n');
                out += kIndent + QStringLiteral("Height                     : ") + QString::number(par->height) + QLatin1Char('\n');
                out += kIndent + QStringLiteral("Pixel format               : ") + PixFmtName(par->format) + QLatin1Char('\n');
                out += kIndent + QStringLiteral("Sample aspect ratio        : ") + AspectRatioToString(par->sample_aspect_ratio) + QLatin1Char('\n');

                AVRational dar = par->sample_aspect_ratio;
                if (dar.num > 0 && dar.den > 0 && par->width > 0 && par->height > 0) {
                    av_reduce(&dar.num, &dar.den,
                              static_cast<int64_t>(par->width) * dar.num,
                              static_cast<int64_t>(par->height) * dar.den,
                              INT_MAX);
                    out += kIndent + QStringLiteral("Display aspect ratio       : ") + AspectRatioToString(dar) + QLatin1Char('\n');
                }
                out += kIndent + QStringLiteral("Frame rate                 : ") + RationalToString(st->avg_frame_rate) + QStringLiteral(" fps") + QLatin1Char('\n');
                if (st->r_frame_rate.num > 0 && av_cmp_q(st->r_frame_rate, st->avg_frame_rate) != 0) {
                    out += kIndent + QStringLiteral("Real frame rate            : ") + RationalToString(st->r_frame_rate) + QStringLiteral(" fps") + QLatin1Char('\n');
                }
                if (st->nb_frames > 0) {
                    out += kIndent + QStringLiteral("Frame count                : ") + QString::number(st->nb_frames) + QLatin1Char('\n');
                }
                if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                    out += kIndent + QStringLiteral("Color space                : ") + QString::fromUtf8(av_color_space_name(par->color_space)) + QLatin1Char('\n');
                }
                if (par->color_range != AVCOL_RANGE_UNSPECIFIED) {
                    out += kIndent + QStringLiteral("Color range                : ") + QString::fromUtf8(av_color_range_name(par->color_range)) + QLatin1Char('\n');
                }
                if (par->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                    out += kIndent + QStringLiteral("Color primaries            : ") + QString::fromUtf8(av_color_primaries_name(par->color_primaries)) + QLatin1Char('\n');
                }
                if (par->color_trc != AVCOL_TRC_UNSPECIFIED) {
                    out += kIndent + QStringLiteral("Transfer characteristics   : ") + QString::fromUtf8(av_color_transfer_name(par->color_trc)) + QLatin1Char('\n');
                }
                if (par->chroma_location != AVCHROMA_LOC_UNSPECIFIED) {
                    out += kIndent + QStringLiteral("Chroma subsampling         : ") + QString::fromUtf8(av_chroma_location_name(par->chroma_location)) + QLatin1Char('\n');
                }
                // 旋转角度 (手机竖拍视频常见)
                // FFmpeg 8 移除了 AVStream::side_data，容器级 side data 只能从
                // AVCodecParameters::coded_side_data 读。
                const AVPacketSideData* display_matrix =
                    av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                            AV_PKT_DATA_DISPLAYMATRIX);
                if (display_matrix && display_matrix->size >= 36) {
                    double theta = 0.0;
                    const int32_t* m = reinterpret_cast<const int32_t*>(display_matrix->data);
                    if (m[0] == 0 && m[1] == 65536) theta = 90.0;
                    else if (m[0] == 0 && m[1] == -65536) theta = 270.0;
                    else if (m[0] == -65536 && m[1] == 0) theta = 180.0;
                    if (theta != 0.0) {
                        out += kIndent + QStringLiteral("Rotation                   : ") + QString::number(theta, 'f', 1) + QLatin1Char('\n');
                    }
                }
            } else if (type == AVMEDIA_TYPE_AUDIO) {
                // 注意: 这一段上面已经输出过 codec 的 Format，这里再叫 Format 会重名，
                // 而这一行其实是 AVSampleFormat（s16 / fltp ...），所以叫 Sample format。
                out += kIndent + QStringLiteral("Sample format              : ") + SampleFmtName(par->format) + QLatin1Char('\n');
                out += kIndent + QStringLiteral("Sample rate                : ") + QString::number(par->sample_rate) + QStringLiteral(" Hz") + QLatin1Char('\n');
                char layout[128] = {0};
                if (av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout)) > 0) {
                    out += kIndent + QStringLiteral("Channel layout             : ") + QString::fromUtf8(layout) + QLatin1Char('\n');
                }
                out += kIndent + QStringLiteral("Channels                   : ") + QString::number(par->ch_layout.nb_channels) + QLatin1Char('\n');
                if (par->bits_per_raw_sample > 0) {
                    out += kIndent + QStringLiteral("Bit depth                  : ") + QString::number(par->bits_per_raw_sample) + QStringLiteral(" bit") + QLatin1Char('\n');
                } else if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(par->format)) ||
                           par->format >= 0) {
                    const int bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(par->format)) * 8;
                    if (bps > 0) {
                        out += kIndent + QStringLiteral("Bit depth                  : ") + QString::number(bps) + QStringLiteral(" bit") + QLatin1Char('\n');
                    }
                }
                if (par->frame_size > 0) {
                    out += kIndent + QStringLiteral("Samples per frame          : ") + QString::number(par->frame_size) + QLatin1Char('\n');
                }
            } else if (type == AVMEDIA_TYPE_SUBTITLE) {
                out += kIndent + QStringLiteral("Codec                      : ") + CodecName(par) + QLatin1Char('\n');
            }

            if (par->bit_rate > 0) {
                out += kIndent + QStringLiteral("Bit rate                   : ") + FormatBitRate(par->bit_rate) + QLatin1Char('\n');
            }
            if (st->duration > 0 && st->duration != AV_NOPTS_VALUE) {
                const double seconds = static_cast<double>(st->duration) * av_q2d(st->time_base);
                out += kIndent + QStringLiteral("Duration                   : ") + FormatDuration(seconds) + QLatin1Char('\n');
            }
            const QString lang = LanguageName(fmt->metadata, st);
            if (!lang.isEmpty()) {
                out += kIndent + QStringLiteral("Language                   : ") + lang + QLatin1Char('\n');
            }
            if (st->disposition & AV_DISPOSITION_DEFAULT) {
                out += kIndent + QStringLiteral("Default                    : Yes") + QLatin1Char('\n');
            }
            if (st->disposition & AV_DISPOSITION_FORCED) {
                out += kIndent + QStringLiteral("Forced                     : Yes") + QLatin1Char('\n');
            }
            AppendMetadata(out, st->metadata, kIndent);
            out += QLatin1Char('\n');
        }
    }

    // 章节
    if (fmt->nb_chapters > 0) {
        out += QStringLiteral("Menu\n");
        for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
            const AVChapter* ch = fmt->chapters[i];
            const double start = static_cast<double>(ch->start) * av_q2d(ch->time_base);
            out += kIndent + QStringLiteral("Chapter #") + QString::number(i + 1) +
                   QStringLiteral("           : ") + QString::number(start, 'f', 3) + QStringLiteral(" s");
            AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", nullptr, 0);
            if (title && title->value) out += QStringLiteral(" - ") + QString::fromUtf8(title->value);
            out += QLatin1Char('\n');
        }
        out += QLatin1Char('\n');
    }

    impl_->text = out;
    return true;
}

QString MediaInfoAnalyzer::GetCompleteInfo() const {
    return impl_->opened ? impl_->text : QString();
}

void MediaInfoAnalyzer::Close() {
    if (impl_->fmt) {
        avformat_close_input(&impl_->fmt);
        impl_->fmt = nullptr;
    }
    impl_->opened = false;
    impl_->text.clear();
}

bool MediaInfoAnalyzer::IsReady() const {
    return impl_->opened;
}

QString MediaInfoAnalyzer::GetLastError() const {
    return impl_->error;
}

} // namespace analyzer
} // namespace videoeye
