#include "MediaPlayer.h"
#include "utils/Logger.h"
#include <QFileInfo>
#include <QDebug>
#include <cstdint>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libswscale/swscale.h>
}

namespace videoeye {
namespace player {

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
        stream_info_.video_width = video_decoder_->GetWidth();
        stream_info_.video_height = video_decoder_->GetHeight();
        stream_info_.codec_name_video = video_decoder_->GetCodecName();
        
        qDebug() << "[OPEN-18] 视频信息:" << stream_info_.video_width << "x" 
                 << stream_info_.video_height << stream_info_.codec_name_video.c_str();
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
        stream_info_.audio_sample_rate = audio_decoder_->GetSampleRate();
        stream_info_.audio_channels = audio_decoder_->GetChannels();
        stream_info_.codec_name_audio = audio_decoder_->GetCodecName();
    }
    
    qDebug() << "[OPEN-22] 填充流信息";
    // 填充流信息
    stream_info_.filename = url.toStdString();
    stream_info_.format_name = (format_ctx_->iformat && format_ctx_->iformat->name) ? format_ctx_->iformat->name : "";
    stream_info_.duration_ms = format_ctx_->duration != AV_NOPTS_VALUE ? 
                               format_ctx_->duration / 1000 : 0;
    if (video_stream_index_ >= 0 && format_ctx_->streams && format_ctx_->streams[video_stream_index_]) {
        stream_info_.video_fps = av_q2d(format_ctx_->streams[video_stream_index_]->avg_frame_rate);
    } else {
        stream_info_.video_fps = 0;
    }
    stream_info_.video_bitrate = format_ctx_->bit_rate;
    
    duration_ms_ = stream_info_.duration_ms;
    current_position_ms_ = 0;
    
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
        stream_analyzer_.Stop();
        LOG_INFO("已禁用视频分析");
    }
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
                
                // 实时分析 (仅帧有效时)
                if (analysis_enabled_) {
                    // 流分析 (每个包)
                    stream_analyzer_.AnalyzePacket(packet, format_ctx_);
                    
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
