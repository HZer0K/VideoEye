#include "MediaPlayer.h"
#include "utils/Logger.h"
#include <QFileInfo>
#include <QDebug>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <cmath>
#include <complex>
#include <vector>
#include <chrono>
#include <limits>
#include <QDir>
#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavcodec/version.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

namespace videoeye {
namespace player {

namespace {
constexpr int kWaveformPointCount = 128;
constexpr int kSpectrumBinCount = 32;
constexpr double kPi = 3.14159265358979323846;

using SteadyClock = std::chrono::steady_clock;

double PacketTimestampSeconds(const AVFormatContext* format_ctx, const AVPacket* packet) {
    if (!format_ctx || !packet || packet->stream_index < 0 ||
        packet->stream_index >= static_cast<int>(format_ctx->nb_streams)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    AVStream* stream = format_ctx->streams[packet->stream_index];
    if (!stream || stream->time_base.den == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const int64_t pts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
    if (pts == AV_NOPTS_VALUE) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return pts * av_q2d(stream->time_base);
}

int SelectPlaybackClockStreamIndex(int audio_stream_index, int video_stream_index) {
    // 当前并没有真实音频输出设备参与同步，UI 以视频显示为主，
    // 因此存在视频流时优先使用视频时钟来节流，避免按解码速度“倍速播放”。
    if (video_stream_index >= 0) {
        return video_stream_index;
    }
    if (audio_stream_index >= 0) {
        return audio_stream_index;
    }
    return -1;
}

struct PlaybackClock {
    void Reset() {
        started = false;
        first_media_ts = 0.0;
        wall_start = SteadyClock::now();
    }

    void OnPaused(const SteadyClock::duration& paused_for) {
        if (started) {
            wall_start += paused_for;
        }
    }

    void Sync(double media_ts) {
        if (!std::isfinite(media_ts)) {
            return;
        }
        if (!started) {
            started = true;
            first_media_ts = media_ts;
            wall_start = SteadyClock::now();
        }
    }

    void PaceTo(double media_ts) {
        if (!std::isfinite(media_ts)) {
            return;
        }
        if (!started) {
            Sync(media_ts);
            return;
        }
        if (media_ts < first_media_ts) {
            return;
        }

        const auto target = wall_start + std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(media_ts - first_media_ts));
        const auto now = SteadyClock::now();
        if (target > now) {
            std::this_thread::sleep_for(target - now);
        }
    }

    bool started = false;
    double first_media_ts = 0.0;
    SteadyClock::time_point wall_start = SteadyClock::now();
};

} // namespace

static std::string FormatDurationMs(int64_t ms) {
    if (ms <= 0) {
        return {};
    }
    int64_t total_seconds = ms / 1000;
    int64_t hours = total_seconds / 3600;
    int64_t minutes = (total_seconds % 3600) / 60;
    int64_t seconds = total_seconds % 60;

    std::ostringstream oss;
    if (hours > 0) {
        oss << hours << " h ";
    }
    if (minutes > 0 || hours > 0) {
        oss << minutes << " min ";
    }
    oss << seconds << " s";
    return oss.str();
}

static int64_t DurationMsFromFormat(const AVFormatContext* format_ctx) {
    if (!format_ctx) {
        return 0;
    }

    int64_t best_ms = 0;
    if (format_ctx->duration != AV_NOPTS_VALUE && format_ctx->duration > 0) {
        best_ms = av_rescale(format_ctx->duration, 1000, AV_TIME_BASE);
    }

    if (format_ctx->streams && format_ctx->nb_streams > 0) {
        const AVRational kMsBase{1, 1000};
        for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
            const AVStream* s = format_ctx->streams[i];
            if (!s || s->time_base.den == 0) {
                continue;
            }
            if (s->duration == AV_NOPTS_VALUE || s->duration <= 0) {
                continue;
            }
            const int64_t ms = av_rescale_q(s->duration, s->time_base, kMsBase);
            if (ms > best_ms) {
                best_ms = ms;
            }
        }
    }

    return best_ms;
}

static int ClampMsToInt(int64_t ms) {
    if (ms <= 0) {
        return 0;
    }
    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ms);
}

static int BitsPerSampleForCodec(AVCodecID codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S8:
        return 8;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE:
        return 16;
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_U24LE:
    case AV_CODEC_ID_PCM_U24BE:
        return 24;
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_U32LE:
    case AV_CODEC_ID_PCM_U32BE:
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F32BE:
        return 32;
    case AV_CODEC_ID_PCM_F64LE:
    case AV_CODEC_ID_PCM_F64BE:
        return 64;
    default:
        return 0;
    }
}

static int64_t EstimateRawPcmDurationMs(const QString& url, const AVCodecParameters* codecpar) {
    if (!codecpar || codecpar->sample_rate <= 0) {
        return 0;
    }

    int channels = codecpar->ch_layout.nb_channels;
    if (channels <= 0) {
        channels = 1;
    }

    int bits_per_sample = codecpar->bits_per_coded_sample;
    if (bits_per_sample <= 0) {
        bits_per_sample = BitsPerSampleForCodec(codecpar->codec_id);
    }
    if (bits_per_sample <= 0) {
        return 0;
    }

    const QFileInfo fi(url);
    if (!fi.exists() || !fi.isFile()) {
        return 0;
    }

    const int64_t bytes_per_second =
        static_cast<int64_t>(codecpar->sample_rate) * channels * bits_per_sample / 8;
    if (bytes_per_second <= 0) {
        return 0;
    }

    return (fi.size() * 1000LL) / bytes_per_second;
}

static std::string FormatBitrate(int64_t bps) {
    if (bps <= 0) {
        return {};
    }
    std::ostringstream oss;
    oss << (bps / 1000) << " kb/s";
    return oss.str();
}

static std::string FormatFrameRate(double fps) {
    if (fps <= 0.0) {
        return {};
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << fps << " FPS";
    return oss.str();
}

static std::string FormatPixels(int v) {
    if (v <= 0) {
        return {};
    }
    return std::to_string(v) + " pixels";
}

static std::string FourCC(uint32_t tag) {
    if (tag == 0) {
        return {};
    }
    char s[5];
    s[0] = static_cast<char>(tag & 0xFF);
    s[1] = static_cast<char>((tag >> 8) & 0xFF);
    s[2] = static_cast<char>((tag >> 16) & 0xFF);
    s[3] = static_cast<char>((tag >> 24) & 0xFF);
    s[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        if (s[i] == '\0') {
            s[i] = ' ';
        }
    }
    return std::string(s);
}

static std::string GetMetadata(AVDictionary* dict, const char* key) {
    if (!dict || !key) {
        return {};
    }
    if (AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0)) {
        if (e->value) {
            return std::string(e->value);
        }
    }
    return {};
}

static std::string ColorRangeName(AVColorRange range) {
    switch (range) {
    case AVCOL_RANGE_MPEG:
        return "Limited";
    case AVCOL_RANGE_JPEG:
        return "Full";
    default:
        return {};
    }
}

static std::string ScanTypeName(AVFieldOrder order) {
    switch (order) {
    case AV_FIELD_PROGRESSIVE:
        return "Progressive";
    case AV_FIELD_TT:
    case AV_FIELD_BB:
    case AV_FIELD_TB:
    case AV_FIELD_BT:
        return "Interlaced";
    default:
        return {};
    }
}

static std::string ChromaSubsamplingFromPixFmt(int pix_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
    if (!desc || desc->nb_components <= 0) {
        return {};
    }
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) {
        return "4:4:4";
    }
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
        return "4:2:2";
    }
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
        return "4:2:0";
    }
    return {};
}

static std::string BitDepthFromPixFmt(int pix_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
    if (!desc || desc->nb_components <= 0) {
        return {};
    }
    int depth = desc->comp[0].depth;
    if (depth <= 0) {
        return {};
    }
    return std::to_string(depth) + " bits";
}

static const uint8_t* GetCodedSideData(const AVCodecParameters* par, AVPacketSideDataType type, int* size) {
    if (size) {
        *size = 0;
    }
    if (!par || !par->coded_side_data || par->nb_coded_side_data <= 0) {
        return nullptr;
    }
    for (int i = 0; i < par->nb_coded_side_data; ++i) {
        const AVPacketSideData& sd = par->coded_side_data[i];
        if (sd.type == type && sd.data && sd.size > 0) {
            if (size) {
                *size = sd.size;
            }
            return sd.data;
        }
    }
    return nullptr;
}

static std::string FriendlyColorPrimaries(AVColorPrimaries prim) {
    switch (prim) {
    case AVCOL_PRI_BT709:
        return "BT.709";
    case AVCOL_PRI_BT2020:
        return "BT.2020";
    case AVCOL_PRI_SMPTE432:
        return "Display P3";
    default:
        break;
    }
    if (const char* n = av_color_primaries_name(prim)) {
        return n;
    }
    return {};
}

static std::string FriendlyTransfer(AVColorTransferCharacteristic trc) {
    switch (trc) {
    case AVCOL_TRC_BT709:
        return "BT.709";
    case AVCOL_TRC_SMPTE2084:
        return "PQ";
    case AVCOL_TRC_ARIB_STD_B67:
        return "HLG";
    default:
        break;
    }
    if (const char* n = av_color_transfer_name(trc)) {
        return n;
    }
    return {};
}

static std::string FriendlyColorSpace(AVColorSpace spc) {
    switch (spc) {
    case AVCOL_SPC_BT709:
        return "BT.709";
    case AVCOL_SPC_BT2020_NCL:
        return "BT.2020 (non-constant)";
    case AVCOL_SPC_BT2020_CL:
        return "BT.2020 (constant)";
    default:
        break;
    }
    if (const char* n = av_color_space_name(spc)) {
        return n;
    }
    return {};
}

static std::string FormatMasteringDisplayPrimaries(const AVMasteringDisplayMetadata& md) {
    std::ostringstream oss;

    auto RationalToDouble = [](AVRational r) -> double {
        if (r.num == 0 || r.den == 0) {
            return 0.0;
        }
        return static_cast<double>(r.num) / static_cast<double>(r.den);
    };

    oss.setf(std::ios::fixed);
    oss.precision(4);

    if (!md.has_primaries) {
        return {};
    }

    const double rx = RationalToDouble(md.display_primaries[0][0]);
    const double ry = RationalToDouble(md.display_primaries[0][1]);
    const double gx = RationalToDouble(md.display_primaries[1][0]);
    const double gy = RationalToDouble(md.display_primaries[1][1]);
    const double bx = RationalToDouble(md.display_primaries[2][0]);
    const double by = RationalToDouble(md.display_primaries[2][1]);
    const double wx = RationalToDouble(md.white_point[0]);
    const double wy = RationalToDouble(md.white_point[1]);

    oss << "R(" << rx << "," << ry << ") "
        << "G(" << gx << "," << gy << ") "
        << "B(" << bx << "," << by << ") "
        << "WP(" << wx << "," << wy << ")";
    return oss.str();
}

static std::string FormatMasteringDisplayLuminance(const AVMasteringDisplayMetadata& md) {
    if (!md.has_luminance) {
        return {};
    }
    auto LuminanceToDouble = [](AVRational r) -> double {
        if (r.num == 0 || r.den == 0) {
            return 0.0;
        }
        return static_cast<double>(r.num) / static_cast<double>(r.den);
    };

    const double max_l = LuminanceToDouble(md.max_luminance);
    const double min_l = LuminanceToDouble(md.min_luminance);

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << min_l << " - " << max_l << " cd/m2";
    return oss.str();
}

MediaPlayer::MediaPlayer(QObject* parent)
    : QObject(parent) {
    // 注册FFmpeg格式
    avformat_network_init();
}

void MediaPlayer::EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                                    qint64 pts, double timestamp_seconds,
                                    const QString& summary, const QString& detail) {
    model::AnalysisEvent event_info;
    event_info.index = analysis_event_index_++;
    event_info.severity = severity;
    event_info.type = type;
    event_info.stream_index = stream_index;
    event_info.pts = pts;
    event_info.timestamp_seconds = timestamp_seconds;
    event_info.summary = summary;
    event_info.detail = detail;
    emit AnalysisEventReady(event_info);
    EmitTimelineEvent(QStringLiteral("事件"), timestamp_seconds, summary, detail);
}

void MediaPlayer::EmitSyncSample(double audio_timestamp_seconds, double video_timestamp_seconds, bool audio_anchor) {
    if (!std::isfinite(audio_timestamp_seconds) || !std::isfinite(video_timestamp_seconds)) {
        return;
    }

    model::SyncSample sample;
    sample.index = sync_sample_index_++;
    sample.audio_timestamp_seconds = audio_timestamp_seconds;
    sample.video_timestamp_seconds = video_timestamp_seconds;
    sample.diff_ms = (audio_timestamp_seconds - video_timestamp_seconds) * 1000.0;
    sample.audio_anchor = audio_anchor;
    emit SyncSampleReady(sample);
}

void MediaPlayer::EmitTimelineEvent(const QString& category, double timestamp_seconds,
                                    const QString& label, const QString& detail) {
    if (!std::isfinite(timestamp_seconds)) {
        return;
    }

    model::TimelineEvent event;
    event.index = timeline_event_index_++;
    event.category = category;
    event.timestamp_seconds = timestamp_seconds;
    event.label = label;
    event.detail = detail;
    emit TimelineEventReady(event);
}

void MediaPlayer::EmitAudioVisualizationFrame(const int16_t* samples, int sample_count,
                                              int sample_rate, int channels,
                                              double timestamp_seconds, double level) {
    if (!samples || sample_count <= 0 || channels <= 0) {
        return;
    }

    model::AudioVisualizationFrame frame;
    frame.index = audio_visualization_index_++;
    frame.timestamp_seconds = timestamp_seconds;
    frame.level = level;
    frame.sample_rate = sample_rate;
    frame.channels = channels;

    const int frame_count = sample_count / channels;
    if (frame_count <= 0) {
        return;
    }

    const int waveform_points = std::min(kWaveformPointCount, frame_count);
    frame.waveform_points.reserve(waveform_points);
    for (int i = 0; i < waveform_points; ++i) {
        const int frame_index = (i * frame_count) / waveform_points;
        const int sample_index = frame_index * channels;
        double mono = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            mono += static_cast<double>(samples[sample_index + ch]) / 32768.0;
        }
        mono /= static_cast<double>(channels);
        frame.waveform_points.push_back(std::clamp(mono, -1.0, 1.0));
    }

    const int fft_size = std::min(frame_count, 256);
    if (fft_size >= 8) {
        frame.spectrum_bins.reserve(kSpectrumBinCount);
        for (int bin = 0; bin < kSpectrumBinCount; ++bin) {
            const double normalized_bin = static_cast<double>(bin) / static_cast<double>(kSpectrumBinCount);
            const int k = std::min(fft_size / 2 - 1, std::max(0, static_cast<int>(normalized_bin * (fft_size / 2 - 1))));
            std::complex<double> acc(0.0, 0.0);
            for (int n = 0; n < fft_size; ++n) {
                const int sample_index = n * channels;
                double mono = 0.0;
                for (int ch = 0; ch < channels; ++ch) {
                    mono += static_cast<double>(samples[sample_index + ch]) / 32768.0;
                }
                mono /= static_cast<double>(channels);
                const double angle = -2.0 * kPi * static_cast<double>(k * n) / static_cast<double>(fft_size);
                acc += std::complex<double>(mono * std::cos(angle), mono * std::sin(angle));
            }
            const double magnitude = std::abs(acc) / static_cast<double>(fft_size);
            frame.spectrum_bins.push_back(magnitude);
        }
    }

    emit AudioVisualizationReady(frame);
}

MediaPlayer::~MediaPlayer() {
    CancelVideoFrameExport();
    Stop();
    Cleanup();
    avformat_network_deinit();
}

bool MediaPlayer::Open(const QString& url) {
    return OpenInternal(url, nullptr, nullptr);
}

bool MediaPlayer::OpenRawPcm(const QString& url, const QString& demuxer_name, int sample_rate, int channels) {
    const AVInputFormat* input_format = av_find_input_format(demuxer_name.toUtf8().constData());
    if (!input_format) {
        emit Error(QString("Unsupported PCM format: %1").arg(demuxer_name));
        return false;
    }

    AVDictionary* input_options = nullptr;
    av_dict_set(&input_options, "sample_rate", QByteArray::number(sample_rate).constData(), 0);
    av_dict_set(&input_options, "channels", QByteArray::number(channels).constData(), 0);
    return OpenInternal(url, input_format, input_options);
}

bool MediaPlayer::OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options) {
    qDebug() << "\n===== MediaPlayer::Open START =====";
    qDebug() << "[OPEN-1] URL:" << url;
    
    qDebug() << "[OPEN-2] 调用 Stop()";
    Stop();

    qDebug() << "[OPEN-3] 获取锁";
    std::lock_guard<std::mutex> lock(mutex_);
    
    qDebug() << "[OPEN-4] 调用 Cleanup()";
    Cleanup();

    current_url_ = url;
    should_stop_ = false;
    video_frame_index_ = 0;
    audio_frame_index_ = 0;
    packet_index_ = 0;
    analysis_event_index_ = 0;
    sync_sample_index_ = 0;
    timeline_event_index_ = 0;
    audio_visualization_index_ = 0;
    audio_timeline_sample_counter_ = 0;
    last_packet_ts_by_stream_.clear();
    missing_packet_ts_reported_.clear();
    missing_audio_pts_reported_.clear();
    last_video_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    last_audio_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    emit VideoFrameListReset();
    emit AudioFrameListReset();
    emit PacketListReset();
    emit AnalysisEventListReset();
    emit SyncSampleListReset();
    emit TimelineEventListReset();
    emit AudioVisualizationReset();

    const unsigned header_avcodec_major = LIBAVCODEC_VERSION_MAJOR;
    const unsigned runtime_avcodec_major = static_cast<unsigned>(avcodec_version() >> 16);
    if (header_avcodec_major != runtime_avcodec_major) {
        emit Error(QString("FFmpeg libavcodec 版本不匹配：编译期头文件=%1，运行期库=%2。请清理build并确保使用同一套FFmpeg开发包/运行库。")
                       .arg(header_avcodec_major)
                       .arg(runtime_avcodec_major));
        return false;
    }
    
    qDebug() << "[OPEN-5] 调用 avformat_open_input";
    
    // 保存URL的C字符串，避免临时对象被销毁
    std::string url_str = url.toStdString();
    qDebug() << "[OPEN-5.1] URL string:" << url_str.c_str();
    
    // 打开输入流
    AVDictionary* open_options = input_options;
    int ret = avformat_open_input(&format_ctx_, url_str.c_str(), input_format, open_options ? &open_options : nullptr);
    if (open_options) {
        av_dict_free(&open_options);
    }
    qDebug() << "[OPEN-6] avformat_open_input 返回:" << ret;
    
    if (ret < 0) {
        qDebug() << "[OPEN-ERROR] 打开输入流失败:" << ret;
        emit Error(QString("Failed to open: %1").arg(url));
        return false;
    }
    
    qDebug() << "[OPEN-7] format_ctx_:" << format_ctx_;
    qDebug() << "[OPEN-7.1] nb_streams:" << (format_ctx_ ? format_ctx_->nb_streams : 0);
    
    // 读取流信息
    qDebug() << "[OPEN-8] 调用 avformat_find_stream_info";
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    qDebug() << "[OPEN-9] avformat_find_stream_info 返回:" << ret;
    
    if (ret < 0) {
        qDebug() << "[OPEN-ERROR] 查找流信息失败:" << ret;
        emit Error("Failed to find stream info");
        Cleanup();
        return false;
    }
    
    qDebug() << "[OPEN-10] 查找视频/音频流";
    
    // 关键检查：确保format_ctx_有效
    if (!format_ctx_) {
        qDebug() << "[OPEN-ERROR] format_ctx_为空";
        emit Error("Format context is null");
        return false;
    }
    
    unsigned int nb_streams = format_ctx_->nb_streams;
    qDebug() << "[OPEN-10.1] nb_streams:" << nb_streams;
    
    // 检查streams数组
    if (!format_ctx_->streams) {
        qDebug() << "[OPEN-ERROR] streams数组为空";
        emit Error("No streams found");
        Cleanup();
        return false;
    }
    
    qDebug() << "[OPEN-10.15] streams数组有效";
    
    // 使用FFmpeg推荐的av_find_best_stream API
    // 这个函数会安全地查找最佳流，避免直接访问可能无效的codecpar
    qDebug() << "[OPEN-10.2] 使用av_find_best_stream查找视频流";
    
    const AVCodec* best_video_codec = nullptr;
    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    qDebug() << "[OPEN-11] 视频流索引:" << video_stream_index_;
    
    qDebug() << "[OPEN-10.3] 使用av_find_best_stream查找音频流";
    const AVCodec* best_audio_codec = nullptr;
    audio_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, &best_audio_codec, 0);
    qDebug() << "[OPEN-12] 音频流索引:" << audio_stream_index_;
    
    qDebug() << "[OPEN-13] video_stream_index:" << video_stream_index_;
    qDebug() << "[OPEN-14] audio_stream_index:" << audio_stream_index_;
    
    if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
        qDebug() << "[OPEN-ERROR] 未找到有效的流";
        emit Error("No video or audio stream found");
        Cleanup();
        return false;
    }
    
    bool has_video = (video_stream_index_ >= 0);
    if (video_stream_index_ >= 0 && format_ctx_->streams && format_ctx_->streams[video_stream_index_]) {
        AVStream* vs = format_ctx_->streams[video_stream_index_];
        if (vs && (vs->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            has_video = false;
            
            if (vs->codecpar && vs->attached_pic.data && vs->attached_pic.size > 0) {
                const AVCodec* cover_codec = avcodec_find_decoder(vs->codecpar->codec_id);
                if (cover_codec) {
                    AVCodecContext* cover_ctx = avcodec_alloc_context3(cover_codec);
                    if (cover_ctx) {
                        if (avcodec_parameters_to_context(cover_ctx, vs->codecpar) >= 0) {
                            if (vs->time_base.den != 0) {
                                cover_ctx->pkt_timebase = vs->time_base;
                                cover_ctx->time_base = vs->time_base;
                            }
                            
                            if (avcodec_open2(cover_ctx, cover_codec, nullptr) >= 0) {
                                VideoDecoder cover_decoder;
                                if (cover_decoder.InitializeFromContext(cover_ctx)) {
                                    model::FrameData cover_frame;
                                    if (cover_decoder.DecodePacket(&vs->attached_pic, cover_frame)) {
                                        if (cover_frame.width > 0 && cover_frame.height > 0 && cover_frame.data[0]) {
                                            SwsContext* cover_sws = sws_getCachedContext(
                                                nullptr,
                                                cover_frame.width,
                                                cover_frame.height,
                                                static_cast<AVPixelFormat>(cover_frame.format),
                                                cover_frame.width,
                                                cover_frame.height,
                                                AV_PIX_FMT_BGRA,
                                                SWS_BILINEAR,
                                                nullptr,
                                                nullptr,
                                                nullptr);
                                            
                                            if (cover_sws) {
                                                QImage cover_img(cover_frame.width, cover_frame.height, QImage::Format_ARGB32);
                                                if (!cover_img.isNull()) {
                                                    uint8_t* dst_slices[4] = {cover_img.bits(), nullptr, nullptr, nullptr};
                                                    int dst_linesize[4] = {static_cast<int>(cover_img.bytesPerLine()), 0, 0, 0};
                                                    sws_scale(cover_sws,
                                                              cover_frame.data,
                                                              cover_frame.linesize,
                                                              0,
                                                              cover_frame.height,
                                                              dst_slices,
                                                              dst_linesize);
                                                    emit FrameReady(cover_img);
                                                }
                                                sws_freeContext(cover_sws);
                                            }
                                        }
                                    }
                                } else {
                                    avcodec_free_context(&cover_ctx);
                                }
                            } else {
                                avcodec_free_context(&cover_ctx);
                            }
                        } else {
                            avcodec_free_context(&cover_ctx);
                        }
                    }
                }
            }
            
            video_stream_index_ = -1;
        }
    }
    
    emit MediaModeChanged(has_video);
    
    // 初始化视频解码器
    if (video_stream_index_ >= 0) {
        qDebug() << "[OPEN-15] 初始化视频解码器";
        video_decoder_ = std::make_unique<VideoDecoder>();
        qDebug() << "[OPEN-16] video_decoder_ 创建成功";
        
        // 手动创建codec context，绕过有问题的codecpar
        AVStream* video_stream = format_ctx_->streams[video_stream_index_];
        if (!video_stream || !video_stream->codecpar) {
            qDebug() << "[OPEN-ERROR] 视频stream或codecpar为空";
            emit Error("Video codec parameters not available");
            Cleanup();
            return false;
        }

        const AVCodec* video_codec = best_video_codec;
        if (!video_codec) {
            video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        }
        
        if (!video_codec) {
            qDebug() << "[OPEN-ERROR] 找不到视频解码器";
            emit Error("Video codec not found");
            Cleanup();
            return false;
        }
        
        qDebug() << "[OPEN-16.1] 找到视频解码器:" << video_codec->name;
        
        // 创建codec context
        AVCodecContext* video_codec_ctx = avcodec_alloc_context3(video_codec);
        if (!video_codec_ctx) {
            qDebug() << "[OPEN-ERROR] 无法创建视频codec context";
            emit Error("Failed to create video codec context");
            Cleanup();
            return false;
        }
        
        // 从codecpar复制参数（即使codecpar是野指针，codec_id应该已经正确读取了）
        // 如果这里崩溃，说明codecpar完全不可用
        qDebug() << "[OPEN-16.2] 复制codec参数";
        int ret = avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);
        if (ret < 0) {
            qDebug() << "[OPEN-ERROR] 复制codec参数失败:" << ret;
            avcodec_free_context(&video_codec_ctx);
            emit Error("Failed to copy codec parameters");
            Cleanup();
            return false;
        }

        if (video_stream->time_base.den != 0) {
            video_codec_ctx->pkt_timebase = video_stream->time_base;
            video_codec_ctx->time_base = video_stream->time_base;
        }
        
        // 打开解码器
        qDebug() << "[OPEN-16.3] 打开视频解码器";
        ret = avcodec_open2(video_codec_ctx, video_codec, nullptr);
        if (ret < 0) {
            qDebug() << "[OPEN-ERROR] 打开视频解码器失败:" << ret;
            avcodec_free_context(&video_codec_ctx);
            emit Error("Failed to open video decoder");
            Cleanup();
            return false;
        }
        
        qDebug() << "[OPEN-16.4] 视频解码器已打开，尺寸:" 
                 << video_codec_ctx->width << "x" << video_codec_ctx->height;
        
        // 修改VideoDecoder以接受AVCodecContext*
        if (!video_decoder_->InitializeFromContext(video_codec_ctx)) {
            qDebug() << "[OPEN-ERROR] 视频解码器初始化失败";
            avcodec_free_context(&video_codec_ctx);
            emit Error("Failed to initialize video decoder");
            Cleanup();
            return false;
        }
        
        // InitializeFromContext会接管codec_ctx的所有权，所以不要free
        
        qDebug() << "[OPEN-17] 视频解码器初始化成功";
        qDebug() << "[OPEN-18] 视频信息:" << video_decoder_->GetWidth() << "x" 
                 << video_decoder_->GetHeight() << video_decoder_->GetCodecName().c_str();
    }
    
    // 初始化音频解码器
    if (audio_stream_index_ >= 0) {
        qDebug() << "[OPEN-19] 初始化音频解码器";
        audio_decoder_ = std::make_unique<AudioDecoder>();
        qDebug() << "[OPEN-20] audio_decoder_ 创建成功";
        
        // 安全获取codecpar
        AVStream* audio_stream = format_ctx_->streams[audio_stream_index_];
        
        qDebug() << "[OPEN-20.1] audio_stream:" << audio_stream;
        qDebug() << "[OPEN-20.2] audio_stream->codecpar:" << audio_stream->codecpar;
        
        if (!audio_stream->codecpar) {
            qDebug() << "[OPEN-ERROR] 音频codecpar为空";
            emit Error("Audio codec parameters not available");
            Cleanup();
            return false;
        }
        
        // 验证codecpar
        uintptr_t ptr_val_audio = reinterpret_cast<uintptr_t>(audio_stream->codecpar);
        qDebug() << "[OPEN-20.3] codecpar地址:" << Qt::hex << ptr_val_audio << Qt::dec;
        
        qDebug() << "[OPEN-20.5] 调用 audio_decoder_->Initialize()";
        if (!audio_decoder_->Initialize(audio_stream->codecpar)) {
            qDebug() << "[OPEN-ERROR] 音频解码器初始化失败";
            emit Error("Failed to initialize audio decoder");
            Cleanup();
            return false;
        }
        
        qDebug() << "[OPEN-21] 音频解码器初始化成功";
    }
    
    qDebug() << "[OPEN-22] 填充流信息";
    stream_info_.filename = url.toStdString();

    int64_t duration_ms = DurationMsFromFormat(format_ctx_);
    if (duration_ms <= 0 && audio_stream_index_ >= 0 && format_ctx_->streams && format_ctx_->streams[audio_stream_index_]) {
        duration_ms = EstimateRawPcmDurationMs(url, format_ctx_->streams[audio_stream_index_]->codecpar);
    }
    duration_ms_ = ClampMsToInt(duration_ms);
    current_position_ms_.store(0);

    stream_info_.extractor.complete_name = stream_info_.filename;
    if (format_ctx_->iformat) {
        if (format_ctx_->iformat->long_name) {
            stream_info_.extractor.format = format_ctx_->iformat->long_name;
        } else if (format_ctx_->iformat->name) {
            stream_info_.extractor.format = format_ctx_->iformat->name;
        }
    }
    stream_info_.extractor.format_profile = GetMetadata(format_ctx_->metadata, "major_brand");
    {
        std::string major_brand = GetMetadata(format_ctx_->metadata, "major_brand");
        std::string compatible = GetMetadata(format_ctx_->metadata, "compatible_brands");
        if (!major_brand.empty()) {
            if (!compatible.empty()) {
                stream_info_.extractor.codec_id = major_brand + " (" + compatible + ")";
            } else {
                stream_info_.extractor.codec_id = major_brand;
            }
        }
    }
    {
        QFileInfo fi(url);
        if (fi.exists() && fi.isFile()) {
            const qint64 size = fi.size();
            if (size > 0) {
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(1);
                oss << (static_cast<double>(size) / (1024.0 * 1024.0)) << " MiB";
                stream_info_.extractor.file_size = oss.str();
            }
        }
    }
    stream_info_.extractor.duration = FormatDurationMs(duration_ms);
    stream_info_.extractor.overall_bit_rate = FormatBitrate(format_ctx_->bit_rate);
    stream_info_.extractor.writing_application = GetMetadata(format_ctx_->metadata, "encoder");
    {
        std::string desc = GetMetadata(format_ctx_->metadata, "description");
        if (desc.empty()) {
            desc = GetMetadata(format_ctx_->metadata, "comment");
        }
        stream_info_.extractor.description = desc;
    }
    if (video_stream_index_ >= 0 && format_ctx_->streams && format_ctx_->streams[video_stream_index_]) {
        AVStream* vs = format_ctx_->streams[video_stream_index_];
        AVCodecParameters* vp = vs->codecpar;
        const double fps = av_q2d(vs->avg_frame_rate);
        stream_info_.extractor.frame_rate = FormatFrameRate(fps);

        stream_info_.video.id = std::to_string(video_stream_index_ + 1);
        const AVCodecDescriptor* vdesc = avcodec_descriptor_get(vp->codec_id);
        if (vdesc) {
            stream_info_.video.format = vdesc->name ? std::string(vdesc->name) : "";
            stream_info_.video.format_info = vdesc->long_name ? std::string(vdesc->long_name) : "";
            stream_info_.video.codec_id_info = stream_info_.video.format_info;
        } else {
            const char* name = avcodec_get_name(vp->codec_id);
            stream_info_.video.format = name ? std::string(name) : "";
        }
        {
            const char* profile = avcodec_profile_name(vp->codec_id, vp->profile);
            if (profile) {
                stream_info_.video.format_profile = profile;
            }
        }
        {
            std::string tag = FourCC(vp->codec_tag);
            if (!tag.empty()) {
                stream_info_.video.codec_id = tag;
            } else if (vdesc && vdesc->name) {
                stream_info_.video.codec_id = vdesc->name;
            }
        }
        stream_info_.video.duration = stream_info_.extractor.duration;
        stream_info_.video.bit_rate = FormatBitrate(vp->bit_rate > 0 ? vp->bit_rate : vs->codecpar->bit_rate);
        stream_info_.video.width = FormatPixels(vp->width);
        stream_info_.video.height = FormatPixels(vp->height);
        {
            AVRational sar = vs->sample_aspect_ratio.num ? vs->sample_aspect_ratio : vp->sample_aspect_ratio;
            if (sar.num > 0 && sar.den > 0 && vp->width > 0 && vp->height > 0) {
                const double dar = (static_cast<double>(vp->width) * sar.num) / (static_cast<double>(vp->height) * sar.den);
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(3);
                oss << dar;
                stream_info_.video.display_aspect_ratio = oss.str();
            }
        }
        stream_info_.video.frame_rate = FormatFrameRate(fps);
        if (vp->color_space != AVCOL_SPC_UNSPECIFIED) {
            stream_info_.video.color_space = FriendlyColorSpace(vp->color_space);
            stream_info_.video.matrix_coefficients = FriendlyColorSpace(vp->color_space);
        }
        stream_info_.video.chroma_subsampling = ChromaSubsamplingFromPixFmt(vp->format);
        stream_info_.video.bit_depth = BitDepthFromPixFmt(vp->format);
        stream_info_.video.scan_type = ScanTypeName(vp->field_order);
        stream_info_.video.color_range = ColorRangeName(vp->color_range);
        if (vp->color_primaries != AVCOL_PRI_UNSPECIFIED) {
            stream_info_.video.color_primaries = FriendlyColorPrimaries(vp->color_primaries);
        }
        if (vp->color_trc != AVCOL_TRC_UNSPECIFIED) {
            stream_info_.video.transfer_characteristics = FriendlyTransfer(vp->color_trc);
        }

        {
            bool is_pq = (vp->color_trc == AVCOL_TRC_SMPTE2084);
            bool is_hlg = (vp->color_trc == AVCOL_TRC_ARIB_STD_B67);
            if (is_pq) {
                stream_info_.video.hdr_format = "PQ";
            } else if (is_hlg) {
                stream_info_.video.hdr_format = "HLG";
                stream_info_.video.hdr_format_compatibility = "HLG";
            }
        }

        {
            int side_size = 0;
            const uint8_t* side = GetCodedSideData(vp, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &side_size);
            if (side && side_size >= static_cast<int>(sizeof(AVMasteringDisplayMetadata))) {
                const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(side);
                std::string prim = FormatMasteringDisplayPrimaries(*md);
                std::string lum = FormatMasteringDisplayLuminance(*md);
                if (!prim.empty()) {
                    stream_info_.video.mastering_display_color_primaries = prim;
                }
                if (!lum.empty()) {
                    stream_info_.video.mastering_display_luminance = lum;
                }
                if ((!prim.empty() || !lum.empty()) && vp->color_trc == AVCOL_TRC_SMPTE2084) {
                    stream_info_.video.hdr_format = "HDR10";
                    stream_info_.video.hdr_format_compatibility = "HDR10";
                }
            }
        }

        {
            int side_size = 0;
            const uint8_t* side = GetCodedSideData(vp, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &side_size);
            if (side && side_size >= static_cast<int>(sizeof(AVContentLightMetadata))) {
                const auto* cl = reinterpret_cast<const AVContentLightMetadata*>(side);
                if (cl->MaxCLL > 0) {
                    stream_info_.video.maximum_content_light_level = std::to_string(cl->MaxCLL) + " cd/m2";
                }
                if (cl->MaxFALL > 0) {
                    stream_info_.video.maximum_frame_average_light_level = std::to_string(cl->MaxFALL) + " cd/m2";
                }
                if ((cl->MaxCLL > 0 || cl->MaxFALL > 0) && vp->color_trc == AVCOL_TRC_SMPTE2084) {
                    stream_info_.video.hdr_format = "HDR10";
                    stream_info_.video.hdr_format_compatibility = "HDR10";
                }
            }
        }
    }

    if (audio_stream_index_ >= 0 && format_ctx_->streams && format_ctx_->streams[audio_stream_index_]) {
        AVStream* as = format_ctx_->streams[audio_stream_index_];
        AVCodecParameters* ap = as->codecpar;

        stream_info_.audio.id = std::to_string(audio_stream_index_ + 1);
        const AVCodecDescriptor* adesc = avcodec_descriptor_get(ap->codec_id);
        if (adesc) {
            stream_info_.audio.format = adesc->name ? std::string(adesc->name) : "";
            stream_info_.audio.format_info = adesc->long_name ? std::string(adesc->long_name) : "";
        } else {
            const char* name = avcodec_get_name(ap->codec_id);
            stream_info_.audio.format = name ? std::string(name) : "";
        }
        stream_info_.audio.codec_id = FourCC(ap->codec_tag);
        stream_info_.audio.duration = stream_info_.extractor.duration;
        int64_t audio_bit_rate = ap->bit_rate;
        if (audio_bit_rate <= 0 && ap->sample_rate > 0 && ap->ch_layout.nb_channels > 0) {
            const int bits_per_sample = ap->bits_per_coded_sample > 0 ? ap->bits_per_coded_sample
                                                                      : BitsPerSampleForCodec(ap->codec_id);
            if (bits_per_sample > 0) {
                audio_bit_rate = static_cast<int64_t>(ap->sample_rate) * ap->ch_layout.nb_channels * bits_per_sample;
            }
        }
        stream_info_.audio.bit_rate = FormatBitrate(audio_bit_rate);
        if (ap->sample_rate > 0) {
            stream_info_.audio.sampling_rate = std::to_string(ap->sample_rate / 1000.0) + " kHz";
        }
        if (ap->ch_layout.nb_channels > 0) {
            stream_info_.audio.channels = std::to_string(ap->ch_layout.nb_channels) + " channels";
            char buf[128];
            if (av_channel_layout_describe(&ap->ch_layout, buf, sizeof(buf)) > 0) {
                stream_info_.audio.channel_layout = buf;
            }
        }
        stream_info_.audio.is_default = (as->disposition & AV_DISPOSITION_DEFAULT) ? "Yes" : "No";
    }
    
    qDebug() << "[OPEN-23] 设置状态为 Idle";
    state_ = model::PlayerState::Idle;
    emit StateChanged(state_);
    
    qDebug() << "[OPEN-24] 返回 true";
    qDebug() << "===== MediaPlayer::Open END (SUCCESS) =====\n";
    
    return true;
}

void MediaPlayer::Play() {
    if (!format_ctx_) {
        emit Error("No media opened");
        return;
    }

    if (state_ == model::PlayerState::Playing) {
        return;
    }

    if (state_ == model::PlayerState::Paused) {
        should_stop_ = false;
        state_ = model::PlayerState::Playing;
        emit StateChanged(state_);
        cv_.notify_one();
        return;
    }

    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    should_stop_ = false;
    state_ = model::PlayerState::Playing;
    emit StateChanged(state_);

    decode_thread_ = std::thread(&MediaPlayer::DecodeThread, this);
}

void MediaPlayer::Pause() {
    if (state_ == model::PlayerState::Playing) {
        state_ = model::PlayerState::Paused;
        emit StateChanged(state_);
        cv_.notify_one();
    }
}

void MediaPlayer::Stop() {
    should_stop_ = true;
    state_ = model::PlayerState::Stopped;
    cv_.notify_one();
    
    if (decode_thread_.joinable() &&
        decode_thread_.get_id() != std::this_thread::get_id()) {
        decode_thread_.join();
    }
    
    current_position_ms_.store(0);
    emit StateChanged(state_);
}

void MediaPlayer::Seek(int position_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!format_ctx_) {
        return;
    }
    
    int64_t timestamp = position_ms * 1000LL;
    int ret = av_seek_frame(format_ctx_, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    
    if (ret < 0) {
        emit Error("Seek failed");
        return;
    }
    
    current_position_ms_.store(position_ms);
    emit PositionChanged(current_position_ms_.load(), duration_ms_);
}

void MediaPlayer::SetVolume(int volume) {
    volume_ = std::max(0, std::min(100, volume));
}

void MediaPlayer::EnableAnalysis(bool enable) {
    analysis_enabled_ = enable;
    if (enable) {
        stream_analyzer_.Start();
        LOG_INFO("已启用视频分析");
    } else {
        histogram_enabled_ = false;
        face_detection_enabled_ = false;
        stream_analyzer_.Stop();
        LOG_INFO("已禁用视频分析");
    }
}

void MediaPlayer::SetFrameTypeAnalysisEnabled(bool enable) {
    frame_type_analysis_enabled_ = enable;
}

void MediaPlayer::SetFaceDetectionEnabled(bool enable) {
    face_detection_enabled_ = enable;
    if (enable && face_detector_.GetTotalDetections() == 0) {
        // 初始化人脸检测器
        // 注意：需要从资源路径加载
        LOG_INFO("人脸检测已启用");
    }
}

void MediaPlayer::SetHistogramEnabled(bool enable) {
    histogram_enabled_ = enable;
}

analyzer::StreamStats MediaPlayer::GetCurrentStats() const {
    return stream_analyzer_.GetStats();
}

void MediaPlayer::StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality, int frame_interval) {
    CancelVideoFrameExport();

    if (current_url_.isEmpty()) {
        emit VideoFrameExportError("No media opened");
        return;
    }
    if (output_dir.isEmpty()) {
        emit VideoFrameExportError("Output directory is empty");
        return;
    }

    QDir dir(output_dir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit VideoFrameExportError(QString("Failed to create output directory: %1").arg(output_dir));
            return;
        }
    }

    export_cancel_ = false;
    const QString url = current_url_;
    export_thread_ = std::thread(&MediaPlayer::ExportVideoFramesWorker, this,
                                 url, output_dir, format.toLower(), jpg_quality, std::max(1, frame_interval));
}

void MediaPlayer::CancelVideoFrameExport() {
    export_cancel_ = true;
    if (export_thread_.joinable()) {
        export_thread_.join();
    }
    export_cancel_ = false;
}

void MediaPlayer::ExportVideoFramesWorker(QString url, QString output_dir, QString format, int jpg_quality, int frame_interval) {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* export_frame = nullptr;

    auto cleanup = [&]() {
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (frame) {
            av_frame_free(&frame);
            frame = nullptr;
        }
        if (export_frame) {
            av_frame_free(&export_frame);
            export_frame = nullptr;
        }
        if (pkt) {
            av_packet_free(&pkt);
            pkt = nullptr;
        }
        if (dec_ctx) {
            avcodec_free_context(&dec_ctx);
            dec_ctx = nullptr;
        }
        if (fmt) {
            avformat_close_input(&fmt);
            fmt = nullptr;
        }
    };

    std::string url_str = url.toStdString();
    if (avformat_open_input(&fmt, url_str.c_str(), nullptr, nullptr) < 0) {
        emit VideoFrameExportError(QString("Failed to open: %1").arg(url));
        cleanup();
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        emit VideoFrameExportError("Failed to find stream info");
        cleanup();
        return;
    }

    const AVCodec* best_video_codec = nullptr;
    int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    if (vindex >= 0 && fmt->streams && fmt->streams[vindex] &&
        (fmt->streams[vindex]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        int candidate = -1;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* s = fmt->streams[i];
            if (!s || s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
                continue;
            }
            if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                continue;
            }
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
        emit VideoFrameExportError("No video stream found");
        cleanup();
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
    emit VideoFrameExportStarted(total_frames);
    const AVCodec* codec = best_video_codec ? best_video_codec : avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        emit VideoFrameExportError("Video codec not found");
        cleanup();
        return;
    }

    dec_ctx = avcodec_alloc_context3(codec);
    if (!dec_ctx) {
        emit VideoFrameExportError("Failed to alloc codec context");
        cleanup();
        return;
    }
    if (avcodec_parameters_to_context(dec_ctx, vs->codecpar) < 0) {
        emit VideoFrameExportError("Failed to copy codec parameters");
        cleanup();
        return;
    }
    if (vs->time_base.den != 0) {
        dec_ctx->pkt_timebase = vs->time_base;
        dec_ctx->time_base = vs->time_base;
    }
    if (avcodec_open2(dec_ctx, codec, nullptr) < 0) {
        emit VideoFrameExportError("Failed to open video decoder");
        cleanup();
        return;
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        emit VideoFrameExportError("Failed to alloc packet/frame");
        cleanup();
        return;
    }

    const bool as_jpg = (format == "jpg" || format == "jpeg");
    const bool as_rgb = (format == "rgb");
    const bool as_yuv = (format == "yuv");
    const AVPixelFormat export_pix_fmt = as_rgb ? AV_PIX_FMT_RGB24 : (as_yuv ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NONE);
    if (!as_jpg && !as_rgb && !as_yuv) {
        emit VideoFrameExportError("Unsupported format (use jpg/rgb/yuv)");
        cleanup();
        return;
    }
    if (export_pix_fmt != AV_PIX_FMT_NONE) {
        export_frame = av_frame_alloc();
        if (!export_frame) {
            emit VideoFrameExportError("Failed to alloc export frame");
            cleanup();
            return;
        }
    }

    int exported = 0;
    int decoded_index = 0;
    int sws_src_w = 0;
    int sws_src_h = 0;
    int sws_src_fmt = AV_PIX_FMT_NONE;
    int export_dst_w = 0;
    int export_dst_h = 0;
    AVPixelFormat export_dst_fmt = AV_PIX_FMT_NONE;
    int export_buffer_size = 0;
    std::vector<uint8_t> export_buffer;

    auto write_file = [&](const QString& path, const uint8_t* data, int size) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            return false;
        }
        const qint64 wrote = f.write(reinterpret_cast<const char*>(data), size);
        f.close();
        return wrote == size;
    };

    auto ensure_export_buffer = [&](int width, int height) -> bool {
        if (!export_frame || export_pix_fmt == AV_PIX_FMT_NONE) {
            return true;
        }
        if (export_dst_w == width && export_dst_h == height && export_dst_fmt == export_pix_fmt &&
            !export_buffer.empty()) {
            return true;
        }

        export_buffer_size = av_image_get_buffer_size(export_pix_fmt, width, height, 1);
        if (export_buffer_size <= 0) {
            emit VideoFrameExportError("Failed to calc export buffer size");
            return false;
        }

        export_buffer.resize(static_cast<size_t>(export_buffer_size));
        av_frame_unref(export_frame);
        export_frame->format = export_pix_fmt;
        export_frame->width = width;
        export_frame->height = height;
        const int fill_ret = av_image_fill_arrays(export_frame->data,
                                                  export_frame->linesize,
                                                  export_buffer.data(),
                                                  export_pix_fmt,
                                                  width,
                                                  height,
                                                  1);
        if (fill_ret < 0) {
            emit VideoFrameExportError("Failed to setup export frame buffer");
            return false;
        }
        export_frame->extended_data = export_frame->data;
        export_dst_w = width;
        export_dst_h = height;
        export_dst_fmt = export_pix_fmt;
        return true;
    };

    auto export_one_frame = [&](AVFrame* src) -> bool {
        if (export_cancel_) {
            return false;
        }
        if (src->width <= 0 || src->height <= 0 || src->format < 0) {
            return true;
        }

        int64_t pts = src->pts;
        if (pts == AV_NOPTS_VALUE) {
            pts = src->best_effort_timestamp;
        }
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
            sws = sws_getCachedContext(sws,
                                       width, height, static_cast<AVPixelFormat>(src->format),
                                       width, height, AV_PIX_FMT_BGRA,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                emit VideoFrameExportError("Failed to init sws for jpg");
                return false;
            }

            QImage img(width, height, QImage::Format_ARGB32);
            if (img.isNull()) {
                emit VideoFrameExportError("Failed to alloc QImage");
                return false;
            }

            uint8_t* dst_slices[4] = {img.bits(), nullptr, nullptr, nullptr};
            int dst_linesize[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
            sws_scale(sws, src->data, src->linesize, 0, height, dst_slices, dst_linesize);

            QString filename = QString("frame_%1").arg(decoded_index, 8, 10, QChar('0'));
            if (pts != AV_NOPTS_VALUE) {
                filename += QString("_pts_%1").arg(static_cast<qint64>(pts));
            }
            if (ts_ms >= 0) {
                filename += QString("_tsms_%1").arg(static_cast<qint64>(ts_ms));
            }
            filename += ".jpg";
            const QString path = QDir(output_dir).filePath(filename);
            if (!img.save(path, "JPG", jpg_quality)) {
                emit VideoFrameExportError(QString("Failed to save jpg: %1").arg(path));
                return false;
            }
            return true;
        }

        if (as_rgb) {
            if (!ensure_export_buffer(width, height)) {
                return false;
            }
            sws = sws_getCachedContext(sws,
                                       width, height, static_cast<AVPixelFormat>(src->format),
                                       width, height, AV_PIX_FMT_RGB24,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                emit VideoFrameExportError("Failed to init sws for rgb");
                return false;
            }

            sws_scale(sws, src->data, src->linesize, 0, height, export_frame->data, export_frame->linesize);

            QString filename = QString("frame_%1").arg(decoded_index, 8, 10, QChar('0'));
            if (pts != AV_NOPTS_VALUE) {
                filename += QString("_pts_%1").arg(static_cast<qint64>(pts));
            }
            if (ts_ms >= 0) {
                filename += QString("_tsms_%1").arg(static_cast<qint64>(ts_ms));
            }
            filename += ".rgb";
            const QString path = QDir(output_dir).filePath(filename);
            if (!write_file(path, export_buffer.data(), export_buffer_size)) {
                emit VideoFrameExportError(QString("Failed to write rgb: %1").arg(path));
                return false;
            }
            return true;
        }

        if (as_yuv) {
            if (!ensure_export_buffer(width, height)) {
                return false;
            }
            sws = sws_getCachedContext(sws,
                                       width, height, static_cast<AVPixelFormat>(src->format),
                                       width, height, AV_PIX_FMT_YUV420P,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                emit VideoFrameExportError("Failed to init sws for yuv");
                return false;
            }

            sws_scale(sws, src->data, src->linesize, 0, height, export_frame->data, export_frame->linesize);

            QString filename = QString("frame_%1").arg(decoded_index, 8, 10, QChar('0'));
            if (pts != AV_NOPTS_VALUE) {
                filename += QString("_pts_%1").arg(static_cast<qint64>(pts));
            }
            if (ts_ms >= 0) {
                filename += QString("_tsms_%1").arg(static_cast<qint64>(ts_ms));
            }
            filename += ".yuv";
            const QString path = QDir(output_dir).filePath(filename);
            if (!write_file(path, export_buffer.data(), export_buffer_size)) {
                emit VideoFrameExportError(QString("Failed to write yuv: %1").arg(path));
                return false;
            }
            return true;
        }

        return true;
    };

    while (!export_cancel_) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            break;
        }
        if (pkt->stream_index != vindex) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(dec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (!export_cancel_) {
            r = avcodec_receive_frame(dec_ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                break;
            }
            if (r < 0) {
                break;
            }

            decoded_index++;
            if (frame_interval <= 1 || ((decoded_index - 1) % frame_interval) == 0) {
                if (!export_one_frame(frame)) {
                    cleanup();
                    return;
                }
                exported++;
            }
            if (exported % 25 == 0) {
                emit VideoFrameExportProgress(exported);
            }
            av_frame_unref(frame);
        }
    }

    if (!export_cancel_) {
        avcodec_send_packet(dec_ctx, nullptr);
        while (!export_cancel_) {
            int r = avcodec_receive_frame(dec_ctx, frame);
            if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) {
                break;
            }
            if (r < 0) {
                break;
            }

            decoded_index++;
            if (frame_interval <= 1 || ((decoded_index - 1) % frame_interval) == 0) {
                if (!export_one_frame(frame)) {
                    cleanup();
                    return;
                }
                exported++;
            }
            if (exported % 25 == 0) {
                emit VideoFrameExportProgress(exported);
            }
            av_frame_unref(frame);
        }
    }

    emit VideoFrameExportProgress(exported);
    if (!export_cancel_) {
        emit VideoFrameExportFinished(output_dir);
    } else {
        emit VideoFrameExportCanceled(exported, output_dir);
    }
    cleanup();
}

void MediaPlayer::DecodeThread() {
    AVPacket* packet = av_packet_alloc();
    model::FrameData frame_data;
    SwsContext* sws_ctx = nullptr;
    int sws_src_w = 0;
    int sws_src_h = 0;
    int sws_src_fmt = AV_PIX_FMT_NONE;
    std::vector<uint8_t> audio_buffer(192000);
    int last_emitted_position_ms = -1;
    
    const int clock_stream_index = SelectPlaybackClockStreamIndex(audio_stream_index_, video_stream_index_);
    const bool enable_pacing = (clock_stream_index >= 0);
    const bool frame_paced_video = (clock_stream_index >= 0 && clock_stream_index == video_stream_index_);
    PlaybackClock playback_clock;
    playback_clock.Reset();

    auto emit_position_if_needed = [&](double ts_sec) {
        if (!std::isfinite(ts_sec)) {
            return;
        }
        current_position_ms_.store(static_cast<int>(ts_sec * 1000.0));
        const int pos = current_position_ms_.load();
        int diff = pos - last_emitted_position_ms;
        if (diff < 0) {
            diff = -diff;
        }
        if (last_emitted_position_ms < 0 || diff >= 100) {
            last_emitted_position_ms = pos;
            emit PositionChanged(pos, duration_ms_);
        }
    };
    
    while (!should_stop_) {
        // 暂停状态检查
        if (state_ == model::PlayerState::Paused) {
            const auto pause_begin = SteadyClock::now();
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return state_ != model::PlayerState::Paused || should_stop_; });
            const auto pause_end = SteadyClock::now();
            if (enable_pacing) {
                playback_clock.OnPaused(pause_end - pause_begin);
            }
            continue;
        }
        
        // 读取数据包
        int ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) {
            // 文件结束
            emit PlaybackFinished();
            break;
        }

        const int64_t pkt_ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
        const double packet_ts_sec = PacketTimestampSeconds(format_ctx_, packet);
        model::PacketInfo packet_info;
        packet_info.index = packet_index_++;
        packet_info.stream_index = packet->stream_index;
        if (format_ctx_ && packet->stream_index >= 0 &&
            packet->stream_index < static_cast<int>(format_ctx_->nb_streams) &&
            format_ctx_->streams[packet->stream_index] &&
            format_ctx_->streams[packet->stream_index]->codecpar) {
            packet_info.stream_type = format_ctx_->streams[packet->stream_index]->codecpar->codec_type;
        }
        packet_info.pts = packet->pts;
        packet_info.dts = packet->dts;
        packet_info.duration = packet->duration;
        packet_info.size = packet->size;
        packet_info.flags = packet->flags;
        packet_info.pos = packet->pos;
        packet_info.timestamp_seconds = packet_ts_sec;
        emit PacketInfoReady(packet_info);
        if (!std::isfinite(packet_ts_sec)) {
            if (!missing_packet_ts_reported_[packet->stream_index]) {
                missing_packet_ts_reported_[packet->stream_index] = true;
                EmitAnalysisEvent(tr("警告"),
                                  tr("缺失时间戳"),
                                  packet->stream_index,
                                  static_cast<qint64>(pkt_ts),
                                  packet_ts_sec,
                                  tr("数据包缺少有效时间戳"),
                                  tr("该流存在 PTS/DTS 缺失的数据包，后续同步与定位可能不准确。"));
            }
        } else {
            auto it = last_packet_ts_by_stream_.find(packet->stream_index);
            if (it != last_packet_ts_by_stream_.end()) {
                const double delta_sec = packet_ts_sec - it->second;
                if (delta_sec < -0.001) {
                    EmitAnalysisEvent(tr("错误"),
                                      tr("时间戳回退"),
                                      packet->stream_index,
                                      static_cast<qint64>(pkt_ts),
                                      packet_ts_sec,
                                      tr("检测到非单调递增的包时间戳"),
                                      tr("当前时间戳早于上一包，可能存在封装异常、乱序或损坏。"));
                } else if (delta_sec > 2.0) {
                    EmitAnalysisEvent(tr("警告"),
                                      tr("时间戳跳变"),
                                      packet->stream_index,
                                      static_cast<qint64>(pkt_ts),
                                      packet_ts_sec,
                                      tr("检测到较大的包时间戳跳变"),
                                      tr("相邻包时间差超过 2 秒，可能出现断流、裁切或时间基异常。"));
                }
            }
            last_packet_ts_by_stream_[packet->stream_index] = packet_ts_sec;
        }
        if (analysis_enabled_) {
            stream_analyzer_.AnalyzePacket(packet, format_ctx_);
        }
        if (packet->stream_index == clock_stream_index && !frame_paced_video) {
            playback_clock.PaceTo(packet_ts_sec);
        }
        
        // 播放位置跟随主时钟流, 为后续接入真实音频时钟保留一致语义。
        if (packet->stream_index == clock_stream_index && !frame_paced_video) {
            emit_position_if_needed(packet_ts_sec);
        }
        
        // 视频解码
        if (packet->stream_index == video_stream_index_ && video_decoder_) {
            if (video_decoder_->SendPacket(packet)) {
                while (!should_stop_ && video_decoder_->ReceiveFrame(frame_data)) {
                    double frame_ts = frame_data.timestamp;
                    if (format_ctx_ && video_stream_index_ >= 0) {
                        AVStream* vs = format_ctx_->streams[video_stream_index_];
                        if (vs && vs->time_base.den != 0 && frame_data.pts != AV_NOPTS_VALUE) {
                            frame_ts = frame_data.pts * av_q2d(vs->time_base);
                            frame_data.timestamp = frame_ts;
                        }
                    }

                    if (frame_paced_video) {
                        playback_clock.PaceTo(frame_ts);
                        emit_position_if_needed(frame_ts);
                    }

                    last_video_sync_ts_ = frame_ts;
                    if (std::isfinite(last_audio_sync_ts_)) {
                        EmitSyncSample(last_audio_sync_ts_, last_video_sync_ts_, false);
                    }

                    // 验证帧数据有效性 (防止崩溃)
                    if (frame_data.width <= 0 || frame_data.height <= 0 || !frame_data.data[0]) {
                        LOG_WARN("跳过无效帧数据");
                        EmitAnalysisEvent(tr("错误"),
                                          tr("无效视频帧"),
                                          video_stream_index_,
                                          static_cast<qint64>(frame_data.pts),
                                          frame_data.timestamp,
                                          tr("检测到无效视频帧"),
                                          tr("视频帧的宽高或数据指针无效，已被跳过。"));
                        continue;
                    }

                    if (frame_data.format >= 0) {
                        if (sws_src_w != frame_data.width ||
                            sws_src_h != frame_data.height ||
                            sws_src_fmt != frame_data.format) {
                            sws_src_w = frame_data.width;
                            sws_src_h = frame_data.height;
                            sws_src_fmt = frame_data.format;
                        }

                        sws_ctx = sws_getCachedContext(
                            sws_ctx,
                            frame_data.width,
                            frame_data.height,
                            static_cast<AVPixelFormat>(frame_data.format),
                            frame_data.width,
                            frame_data.height,
                            AV_PIX_FMT_BGRA,
                            SWS_BILINEAR,
                            nullptr,
                            nullptr,
                            nullptr);
                    }

                    if (sws_ctx) {
                        QImage qimage(frame_data.width, frame_data.height, QImage::Format_ARGB32);
                        if (!qimage.isNull()) {
                            uint8_t* dst_slices[4] = {qimage.bits(), nullptr, nullptr, nullptr};
                            int dst_linesize[4] = {static_cast<int>(qimage.bytesPerLine()), 0, 0, 0};

                            sws_scale(sws_ctx,
                                      frame_data.data,
                                      frame_data.linesize,
                                      0,
                                      frame_data.height,
                                      dst_slices,
                                      dst_linesize);

                            emit FrameReady(qimage);
                        }
                    }

                    if (frame_type_analysis_enabled_) {
                        double ts = frame_data.timestamp;
                        const int emitted_index = video_frame_index_;
                        const bool is_key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                        if ((ts == 0.0 || std::isnan(ts) || std::isinf(ts)) && format_ctx_ && video_stream_index_ >= 0) {
                            AVStream* vs = format_ctx_->streams[video_stream_index_];
                            if (vs && vs->time_base.den != 0) {
                                int64_t pts = frame_data.pts;
                                if (pts == AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE) {
                                    pts = packet->pts;
                                }
                                if (pts != AV_NOPTS_VALUE) {
                                    ts = pts * av_q2d(vs->time_base);
                                }
                            }
                        }
                        emit VideoFrameInfoReady(video_frame_index_++,
                                                 static_cast<int>(video_decoder_->GetLastPictureType()),
                                                 is_key_frame,
                                                 static_cast<qint64>(frame_data.pts),
                                                 ts);
                        if (is_key_frame) {
                            EmitTimelineEvent(QStringLiteral("视频关键帧"),
                                              ts,
                                              QStringLiteral("关键帧 #%1").arg(emitted_index));
                        }
                    }

                    // 实时分析 (仅帧有效时)
                    if (analysis_enabled_) {
                        stream_analyzer_.AnalyzeVideoFrame(video_decoder_->GetLastPictureType());
                        
                        // 每隔10帧进行一次帧分析 (降低CPU占用)
                        analysis_frame_counter_++;
                        if (analysis_frame_counter_ % 10 == 0) {
                            // 直方图分析 (添加异常处理)
                            if (histogram_enabled_) {
                                try {
                                    auto hist = frame_analyzer_.ComputeHistogram(frame_data);
                                    emit HistogramReady(hist);
                                } catch (const std::exception& e) {
                                    LOG_ERROR("直方图分析失败: " + std::string(e.what()));
                                }
                            }
                            
                            // 人脸检测 (添加异常处理)
                            if (face_detection_enabled_) {
                                try {
                                    auto faces = face_detector_.DetectFaces(frame_data);
                                    if (!faces.empty()) {
                                        emit FaceDetectionReady(faces);
                                    }
                                } catch (const std::exception& e) {
                                    LOG_ERROR("人脸检测失败: " + std::string(e.what()));
                                }
                            }

                            // 发送流统计 (每秒一次)
                            auto stats = stream_analyzer_.GetStats();
                            emit StreamStatsReady(stats);
                        }
                    }
                }
            }
        }
        
        // 音频解码
        if (packet->stream_index == audio_stream_index_ && audio_decoder_) {
            if (audio_decoder_->SendPacket(packet)) {
                int out_size = 0;
                while (audio_decoder_->ReceiveFrame(audio_buffer.data(),
                                                    static_cast<int>(audio_buffer.size()),
                                                    out_size)) {
                    const qint64 frame_pts = static_cast<qint64>(audio_decoder_->GetLastFramePts());
                    double ts = current_position_ms_.load() / 1000.0;
                    if (frame_pts == AV_NOPTS_VALUE && !missing_audio_pts_reported_[audio_stream_index_]) {
                        missing_audio_pts_reported_[audio_stream_index_] = true;
                        EmitAnalysisEvent(tr("警告"),
                                          tr("音频帧缺失PTS"),
                                          audio_stream_index_,
                                          frame_pts,
                                          ts,
                                          tr("检测到缺少 PTS 的音频帧"),
                                          tr("音频帧将退回使用包时间戳或当前位置，可能影响精确同步分析。"));
                    }
                    if (format_ctx_ && audio_stream_index_ >= 0) {
                        AVStream* as = format_ctx_->streams[audio_stream_index_];
                        if (as && as->time_base.den != 0 && frame_pts != AV_NOPTS_VALUE) {
                            ts = frame_pts * av_q2d(as->time_base);
                        } else if (std::isfinite(packet_ts_sec)) {
                            ts = packet_ts_sec;
                        }
                    } else if (std::isfinite(packet_ts_sec)) {
                        ts = packet_ts_sec;
                    }

                    emit AudioFrameInfoReady(audio_frame_index_++,
                                             frame_pts,
                                             ts,
                                             audio_decoder_->GetLastFrameSampleCount(),
                                             audio_decoder_->GetLastFrameSampleRate(),
                                             audio_decoder_->GetLastFrameChannels(),
                                             out_size);
                    ++audio_timeline_sample_counter_;
                    if (audio_timeline_sample_counter_ % 20 == 0) {
                        EmitTimelineEvent(QStringLiteral("音频采样"),
                                          ts,
                                          QStringLiteral("音频帧 #%1").arg(audio_frame_index_ - 1));
                    }

                    last_audio_sync_ts_ = ts;
                    if (std::isfinite(last_video_sync_ts_)) {
                        EmitSyncSample(last_audio_sync_ts_, last_video_sync_ts_, true);
                    }

                    if (analysis_enabled_) {
                        stream_analyzer_.AnalyzeAudioFrame();
                        if (video_stream_index_ < 0) {
                            analysis_frame_counter_++;
                            if (analysis_frame_counter_ % 10 == 0) {
                                auto stats = stream_analyzer_.GetStats();
                                emit StreamStatsReady(stats);
                            }
                        }
                    }

                    if (out_size < static_cast<int>(sizeof(int16_t))) {
                        continue;
                    }

                    const int16_t* samples = reinterpret_cast<const int16_t*>(audio_buffer.data());
                    const int sample_count = out_size / static_cast<int>(sizeof(int16_t));

                    long double sumsq = 0.0;
                    for (int i = 0; i < sample_count; ++i) {
                        const long double s = static_cast<long double>(samples[i]);
                        sumsq += s * s;
                    }

                    double level = 0.0;
                    if (sample_count > 0) {
                        level = std::sqrt(static_cast<double>(sumsq / sample_count)) / 32768.0;
                        if (level < 0.0) {
                            level = 0.0;
                        } else if (level > 1.0) {
                            level = 1.0;
                        }
                    }

                    emit AudioLevelReady(level, ts);
                    EmitAudioVisualizationFrame(samples,
                                                sample_count,
                                                audio_decoder_->GetLastFrameSampleRate(),
                                                std::max(1, audio_decoder_->GetLastFrameChannels()),
                                                ts,
                                                level);
                }
            }
        }
        
        av_packet_unref(packet);
    }
    
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }
    av_packet_free(&packet);
}

void MediaPlayer::Cleanup() {
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    
    video_decoder_.reset();
    audio_decoder_.reset();
    
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
}

} // namespace player
} // namespace videoeye
