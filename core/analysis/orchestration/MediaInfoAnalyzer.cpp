#include "core/analysis/orchestration/MediaInfoAnalyzer.h"
#include "core/analysis/detail/AnalysisTextUtil.h"  // Fixed / StrCat
#include "core/media/streaming/ManifestText.h"      // videoeye::manifest::FileSizeOf
#include "core/ffmpeg_io/FfmpegInterrupt.h"  // 共享 FFmpeg 中断回调


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

#include "infrastructure/logging/Logger.h"

namespace videoeye {

namespace {

// 把秒数格式化成 "1 h 23 min"、"4 min 5 s"、"678 ms" 这种短串
std::string FormatDuration(double seconds) {
    if (seconds <= 0 || !std::isfinite(seconds)) return "-";

    const long long total_ms = static_cast<long long>(seconds * 1000.0 + 0.5);
    const long long ms = total_ms % 1000;
    const long long total_s = total_ms / 1000;
    const long long s = total_s % 60;
    const long long total_min = total_s / 60;
    const long long min = total_min % 60;
    const long long h = total_min / 60;

    std::string out;
    if (h > 0) out += std::to_string(h) + " h ";
    if (h > 0 || min > 0) out += std::to_string(min) + " min ";
    out += std::to_string(s) + " s";
    if (h == 0 && total_min == 0) out += " " + std::to_string(ms) + " ms";
    return out;
}

std::string FormatBitRate(int64_t bit_rate) {
    if (bit_rate <= 0) return "-";
    if (bit_rate >= 1000000) {
        return Fixed(static_cast<double>(bit_rate) / 1000000.0, 1) + " Mb/s";
    }
    if (bit_rate >= 1000) {
        return Fixed(static_cast<double>(bit_rate) / 1000.0, 1) + " kb/s";
    }
    return std::to_string(bit_rate) + " b/s";
}

std::string FormatFileSize(int64_t bytes) {
    if (bytes <= 0) return "-";
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return Fixed(value, unit == 0 ? 0 : 2) + " " + kUnits[unit];
}

std::string CodecName(const AVCodecParameters* par) {
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    if (desc && desc->name) return desc->name;
    return avcodec_get_name(par->codec_id);
}

std::string CodecTag(const AVCodecParameters* par) {
    if (!par->codec_tag) return std::string();
    char buf[AV_FOURCC_MAX_STRING_SIZE] = {0};
    av_fourcc_make_string(buf, par->codec_tag);
    return buf;
}

std::string PixFmtName(int pix_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
    if (!desc) return "-";
    std::string name = desc->name;
    int bits = desc->comp[0].depth;
    if (bits > 0) name += " (" + std::to_string(bits) + " bit)";
    return name;
}

std::string SampleFmtName(int sample_fmt) {
    const char* name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(sample_fmt));
    return name ? name : "-";
}

std::string RationalToString(const AVRational& q) {
    if (q.den == 0) return "-";
    const double v = av_q2d(q);
    if (q.num == 0) return "0";
    return Fixed(v, 3);
}

std::string AspectRatioToString(const AVRational& q) {
    if (q.num <= 0 || q.den <= 0) return "-";
    return std::to_string(q.num) + ":" + std::to_string(q.den);
}

std::string Trimmed(const char* s) {
    return (s && *s) ? s : std::string();
}

// 语言标签: eng -> English 之类；拿不到就原样返回
// 先读流级 metadata 再读容器级：容器级 language 是多流文件的「整体语言」，
// 反过来读的话会被套到每一条流上（多音轨文件里每条轨都显示成同一个语言）。
std::string LanguageName(const AVDictionary* metadata, const AVStream* stream) {
    AVDictionaryEntry* lang = nullptr;
    if (stream) {
        // 流级优先：MP4/MKV 通常把语言写在每条流自己的 metadata 上
        lang = av_dict_get(stream->metadata, "language", nullptr, 0);
    }
    if (!lang && metadata) {
        lang = av_dict_get(metadata, "language", nullptr, 0);
    }
    if (!lang || !lang->value || !*lang->value) return std::string();
    return lang->value;
}

void AppendMetadata(std::string& out, const AVDictionary* dict, const std::string& indent) {
    if (!dict) return;
    const AVDictionaryEntry* entry = nullptr;
    bool any = false;
    while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        if (!entry->value || !*entry->value) continue;
        if (!any) {
            out += indent + "Metadata:\n";
            any = true;
        }
        out += indent + "  " + entry->key +
               " : " + entry->value + '\n';
    }
}

std::string StreamTitle(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:    return "Video";
        case AVMEDIA_TYPE_AUDIO:    return "Audio";
        case AVMEDIA_TYPE_SUBTITLE: return "Text";
        case AVMEDIA_TYPE_DATA:     return "Data";
        case AVMEDIA_TYPE_ATTACHMENT: return "Attachment";
        default:                    return "Other";
    }
}

} // namespace

struct MediaInfoAnalyzer::Impl {
    AVFormatContext* fmt = nullptr;
    std::string text;
    std::string error;
    std::string pcm_demuxer;
    int pcm_sample_rate = 0;
    int pcm_channels = 0;
    bool opened = false;
};

MediaInfoAnalyzer::MediaInfoAnalyzer() : impl_(new Impl()) {}

MediaInfoAnalyzer::~MediaInfoAnalyzer() {
    Close();
}

void MediaInfoAnalyzer::SetRawPcmHints(const std::string& demuxer, int sample_rate, int channels) {
    impl_->pcm_demuxer = demuxer;
    impl_->pcm_sample_rate = sample_rate;
    impl_->pcm_channels = channels;
}

bool MediaInfoAnalyzer::Open(const std::string& filePath, std::shared_ptr<std::atomic<bool>> cancel) {
    Close();
    if (filePath.empty()) {
        impl_->error = "路径为空";
        return false;
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        impl_->error = "avformat_alloc_context 失败";
        return false;
    }

    // 安装中断回调: 打开/探测阶段带绝对超时, 关闭流程(CancelAll)置标志后能及时退出,
    // 不再让后台 std::thread 卡在 FFmpeg 阻塞 IO 上、导致 WaitForAll 在 join 时挂死。
    ffmpeg_io::AvInterruptState interrupt;
    interrupt.cancel = cancel.get();
    ffmpeg_io::AttachInterrupt(fmt, interrupt, ffmpeg_io::kOpenTimeoutUs);

    AVDictionary* opts = nullptr;
    const AVInputFormat* iformat = nullptr;
    if (!impl_->pcm_demuxer.empty()) {
        iformat = av_find_input_format(impl_->pcm_demuxer.c_str());
        if (!iformat) {
            avformat_free_context(fmt);
            impl_->error = StrCat("找不到裸流 demuxer: %1", impl_->pcm_demuxer);
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

    int ret = avformat_open_input(&fmt, filePath.c_str(),
                                  const_cast<AVInputFormat*>(iformat), &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        avformat_free_context(fmt);
        impl_->error = StrCat("无法打开文件: %1", errbuf);
        LOG_WARN(("MediaInfoAnalyzer: " + impl_->error));
        return false;
    }

    // 探测阶段允许更长时间, 但仍受取消/超时约束
    interrupt.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        // 部分损坏文件拿不到完整流信息, 仍然保留已探测到的部分
        LOG_WARN("MediaInfoAnalyzer: find_stream_info 失败, 使用已探测到的部分信息");
    }

    impl_->fmt = fmt;
    impl_->opened = true;
    impl_->error.clear();

    // ---- 组装文本报告 ----
    std::string out;
    const std::string kIndent = "  ";

    // ===== General =====
    out += "General\n";
    out += kIndent + "Complete name              : " + filePath + '\n';
    // 以前是 QFileInfo(filePath).exists() + fi.size()，那一套在 analysis 层带不出 Qt。
    // 换成 core/media 的 FileSizeOf：它一次 stat 同时给出"存在"和"大小"，
    // 省掉 QFileInfo 那层对象，也让这行不和 UI 侧的类型纠缠。
    int64_t file_size = 0;
    if (videoeye::manifest::FileSizeOf(filePath, file_size) && file_size > 0) {
        out += kIndent + "File size                  : " + FormatFileSize(file_size) + '\n';
    }
    if (fmt->iformat) {
        std::string container = Trimmed(fmt->iformat->name);
        if (const char* long_name = fmt->iformat->long_name) {
            if (long_name && *long_name) container += StrCat(" (%1)", long_name);
        }
        out += kIndent + "Format                     : " + container + '\n';
    }
    if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE) {
        const double seconds = static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE);
        out += kIndent + "Duration                   : " + FormatDuration(seconds) + '\n';
    }
    out += kIndent + "Overall bit rate           : " + FormatBitRate(fmt->bit_rate) + '\n';
    if (fmt->start_time > 0 && fmt->start_time != AV_NOPTS_VALUE) {
        const double st = static_cast<double>(fmt->start_time) / static_cast<double>(AV_TIME_BASE);
        out += kIndent + "Start time                 : " + Fixed(st, 3) + " s" + '\n';
    }
    out += kIndent + "Stream count               : " + std::to_string(fmt->nb_streams) + '\n';
    AppendMetadata(out, fmt->metadata, kIndent);
    out += '\n';

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
            out += StreamTitle(type) + " #" + std::to_string(index_in_type) + '\n';
            ++index_in_type;

            out += kIndent + "ID                         : " + std::to_string(st->index) + '\n';
            out += kIndent + "Format                     : " + CodecName(par) + '\n';
            if (const char* profile = avcodec_profile_name(par->codec_id, par->profile)) {
                if (profile && *profile) {
                    out += kIndent + "Format profile             : " + profile + '\n';
                }
            }
            if (par->level != AV_LEVEL_UNKNOWN && par->level != 0) {
                out += kIndent + "Format level               : " + std::to_string(par->level) + '\n';
            }
            if (!CodecTag(par).empty()) {
                out += kIndent + "Codec ID                   : " + CodecTag(par) + '\n';
            }
            if (par->codec_id == AV_CODEC_ID_NONE) {
                // pass-through / 未知编码
            }

            if (type == AVMEDIA_TYPE_VIDEO) {
                out += kIndent + "Width                      : " + std::to_string(par->width) + '\n';
                out += kIndent + "Height                     : " + std::to_string(par->height) + '\n';
                out += kIndent + "Pixel format               : " + PixFmtName(par->format) + '\n';
                out += kIndent + "Sample aspect ratio        : " + AspectRatioToString(par->sample_aspect_ratio) + '\n';

                AVRational dar = par->sample_aspect_ratio;
                if (dar.num > 0 && dar.den > 0 && par->width > 0 && par->height > 0) {
                    av_reduce(&dar.num, &dar.den,
                              static_cast<int64_t>(par->width) * dar.num,
                              static_cast<int64_t>(par->height) * dar.den,
                              INT_MAX);
                    out += kIndent + "Display aspect ratio       : " + AspectRatioToString(dar) + '\n';
                }
                out += kIndent + "Frame rate                 : " + RationalToString(st->avg_frame_rate) + " fps" + '\n';
                if (st->r_frame_rate.num > 0 && av_cmp_q(st->r_frame_rate, st->avg_frame_rate) != 0) {
                    out += kIndent + "Real frame rate            : " + RationalToString(st->r_frame_rate) + " fps" + '\n';
                }
                if (st->nb_frames > 0) {
                    out += kIndent + "Frame count                : " + std::to_string(st->nb_frames) + '\n';
                }
                if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                    out += kIndent + "Color space                : " + av_color_space_name(par->color_space) + '\n';
                }
                if (par->color_range != AVCOL_RANGE_UNSPECIFIED) {
                    out += kIndent + "Color range                : " + av_color_range_name(par->color_range) + '\n';
                }
                if (par->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                    out += kIndent + "Color primaries            : " + av_color_primaries_name(par->color_primaries) + '\n';
                }
                if (par->color_trc != AVCOL_TRC_UNSPECIFIED) {
                    out += kIndent + "Transfer characteristics   : " + av_color_transfer_name(par->color_trc) + '\n';
                }
                if (par->chroma_location != AVCHROMA_LOC_UNSPECIFIED) {
                    out += kIndent + "Chroma subsampling         : " + av_chroma_location_name(par->chroma_location) + '\n';
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
                        out += kIndent + "Rotation                   : " + Fixed(theta, 1) + '\n';
                    }
                }
            } else if (type == AVMEDIA_TYPE_AUDIO) {
                // 注意: 这一段上面已经输出过 codec 的 Format，这里再叫 Format 会重名，
                // 而这一行其实是 AVSampleFormat（s16 / fltp ...），所以叫 Sample format。
                out += kIndent + "Sample format              : " + SampleFmtName(par->format) + '\n';
                out += kIndent + "Sample rate                : " + std::to_string(par->sample_rate) + " Hz" + '\n';
                char layout[128] = {0};
                if (av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout)) > 0) {
                    out += kIndent + "Channel layout             : " + layout + '\n';
                }
                out += kIndent + "Channels                   : " + std::to_string(par->ch_layout.nb_channels) + '\n';
                if (par->bits_per_raw_sample > 0) {
                    out += kIndent + "Bit depth                  : " + std::to_string(par->bits_per_raw_sample) + " bit" + '\n';
                } else if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(par->format)) ||
                           par->format >= 0) {
                    const int bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(par->format)) * 8;
                    if (bps > 0) {
                        out += kIndent + "Bit depth                  : " + std::to_string(bps) + " bit" + '\n';
                    }
                }
                if (par->frame_size > 0) {
                    out += kIndent + "Samples per frame          : " + std::to_string(par->frame_size) + '\n';
                }
            } else if (type == AVMEDIA_TYPE_SUBTITLE) {
                out += kIndent + "Codec                      : " + CodecName(par) + '\n';
            }

            if (par->bit_rate > 0) {
                out += kIndent + "Bit rate                   : " + FormatBitRate(par->bit_rate) + '\n';
            }
            if (st->duration > 0 && st->duration != AV_NOPTS_VALUE) {
                const double seconds = static_cast<double>(st->duration) * av_q2d(st->time_base);
                out += kIndent + "Duration                   : " + FormatDuration(seconds) + '\n';
            }
            const std::string lang = LanguageName(fmt->metadata, st);
            if (!lang.empty()) {
                out += kIndent + "Language                   : " + lang + '\n';
            }
            if (st->disposition & AV_DISPOSITION_DEFAULT) {
                out += kIndent + "Default                    : Yes" + '\n';
            }
            if (st->disposition & AV_DISPOSITION_FORCED) {
                out += kIndent + "Forced                     : Yes" + '\n';
            }
            AppendMetadata(out, st->metadata, kIndent);
            out += '\n';
        }
    }

    // 章节
    if (fmt->nb_chapters > 0) {
        out += "Menu\n";
        for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
            const AVChapter* ch = fmt->chapters[i];
            const double start = static_cast<double>(ch->start) * av_q2d(ch->time_base);
            out += kIndent + "Chapter #" + std::to_string(i + 1) +
                   "           : " + Fixed(start, 3) + " s";
            AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", nullptr, 0);
            if (title && title->value) out += StrCat(" - %1", title->value);
            out += '\n';
        }
        out += '\n';
    }

    impl_->text = out;
    return true;
}

std::string MediaInfoAnalyzer::GetCompleteInfo() const {
    return impl_->opened ? impl_->text : std::string();
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

std::string MediaInfoAnalyzer::GetLastError() const {
    return impl_->error;
}

} // namespace videoeye
