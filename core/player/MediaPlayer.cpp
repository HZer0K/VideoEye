#include "MediaPlayer.h"
#include "utils/Logger.h"
#include <QFileInfo>
#include <QDebug>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavcodec/version.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

namespace videoeye {
namespace player {

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

MediaPlayer::~MediaPlayer() {
    Stop();
    Cleanup();
    avformat_network_deinit();
}

bool MediaPlayer::Open(const QString& url) {
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
    emit VideoFrameListReset();

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
    int ret = avformat_open_input(&format_ctx_, url_str.c_str(), nullptr, nullptr);
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

    const int64_t duration_ms = (format_ctx_->duration != AV_NOPTS_VALUE) ? (format_ctx_->duration / 1000) : 0;
    duration_ms_ = static_cast<int>(duration_ms);
    current_position_ms_ = 0;

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
        stream_info_.audio.bit_rate = FormatBitrate(ap->bit_rate);
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
    
    current_position_ms_ = 0;
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
    
    current_position_ms_ = position_ms;
    emit PositionChanged(current_position_ms_, duration_ms_);
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

void MediaPlayer::DecodeThread() {
    AVPacket* packet = av_packet_alloc();
    model::FrameData frame_data;
    SwsContext* sws_ctx = nullptr;
    int sws_src_w = 0;
    int sws_src_h = 0;
    int sws_src_fmt = AV_PIX_FMT_NONE;
    
    while (!should_stop_) {
        // 暂停状态检查
        if (state_ == model::PlayerState::Paused) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return state_ != model::PlayerState::Paused || should_stop_; });
            continue;
        }
        
        // 读取数据包
        int ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) {
            // 文件结束
            emit PlaybackFinished();
            break;
        }
        
        // 更新当前位置
        if (packet->pts != AV_NOPTS_VALUE) {
            AVRational time_base = format_ctx_->streams[packet->stream_index]->time_base;
            current_position_ms_ = packet->pts * av_q2d(time_base) * 1000;
            emit PositionChanged(current_position_ms_, duration_ms_);
        }
        
        // 视频解码
        if (packet->stream_index == video_stream_index_ && video_decoder_) {
            if (video_decoder_->DecodePacket(packet, frame_data)) {
                // 验证帧数据有效性 (防止崩溃)
                if (frame_data.width <= 0 || frame_data.height <= 0 || !frame_data.data[0]) {
                    LOG_WARN("跳过无效帧数据");
                    av_packet_unref(packet);
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
                        int dst_linesize[4] = {qimage.bytesPerLine(), 0, 0, 0};

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
                                             static_cast<qint64>(frame_data.pts),
                                             ts);
                }
                
                // 实时分析 (仅帧有效时)
                if (analysis_enabled_) {
                    // 流分析 (每个包)
                    stream_analyzer_.AnalyzePacket(packet, format_ctx_);
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
        
        // 音频解码
        if (packet->stream_index == audio_stream_index_ && audio_decoder_) {
            // 音频解码逻辑
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
