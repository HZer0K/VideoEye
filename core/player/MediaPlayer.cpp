#include "MediaPlayer.h"
#include "utils/Logger.h"
#include <QFileInfo>
#include <iostream>

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
    Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    Cleanup();

    current_url_ = url;
    should_stop_ = false;
    
    // 打开输入流
    int ret = avformat_open_input(&format_ctx_, url.toStdString().c_str(), nullptr, nullptr);
    if (ret < 0) {
        emit Error(QString("Failed to open: %1").arg(url));
        return false;
    }
    
    // 读取流信息
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        emit Error("Failed to find stream info");
        Cleanup();
        return false;
    }
    
    // 查找视频和音频流
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
    
    for (unsigned int i = 0; i < format_ctx_->nb_streams; ++i) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ < 0) {
            video_stream_index_ = i;
        }
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index_ < 0) {
            audio_stream_index_ = i;
        }
    }
    
    if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
        emit Error("No video or audio stream found");
        Cleanup();
        return false;
    }
    
    // 初始化视频解码器
    if (video_stream_index_ >= 0) {
        video_decoder_ = std::make_unique<VideoDecoder>();
        if (!video_decoder_->Initialize(format_ctx_->streams[video_stream_index_]->codecpar)) {
            emit Error("Failed to initialize video decoder");
            Cleanup();
            return false;
        }
        
        stream_info_.video_width = video_decoder_->GetWidth();
        stream_info_.video_height = video_decoder_->GetHeight();
        stream_info_.codec_name_video = video_decoder_->GetCodecName();
    }
    
    // 初始化音频解码器
    if (audio_stream_index_ >= 0) {
        audio_decoder_ = std::make_unique<AudioDecoder>();
        if (!audio_decoder_->Initialize(format_ctx_->streams[audio_stream_index_]->codecpar)) {
            emit Error("Failed to initialize audio decoder");
            Cleanup();
            return false;
        }
        
        stream_info_.audio_sample_rate = audio_decoder_->GetSampleRate();
        stream_info_.audio_channels = audio_decoder_->GetChannels();
        stream_info_.codec_name_audio = audio_decoder_->GetCodecName();
    }
    
    // 填充流信息
    stream_info_.filename = url.toStdString();
    stream_info_.format_name = format_ctx_->iformat->name;
    stream_info_.duration_ms = format_ctx_->duration != AV_NOPTS_VALUE ? 
                               format_ctx_->duration / 1000 : 0;
    stream_info_.video_fps = video_stream_index_ >= 0 ? 
                            av_q2d(format_ctx_->streams[video_stream_index_]->avg_frame_rate) : 0;
    stream_info_.video_bitrate = format_ctx_->bit_rate;
    
    duration_ms_ = stream_info_.duration_ms;
    current_position_ms_ = 0;
    
    state_ = model::PlayerState::Idle;
    emit StateChanged(state_);
    
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
                
                // 转换为QImage并发出信号
                // 这里需要YUV到RGB的转换,简化处理
                QImage qimage(frame_data.width, frame_data.height, QImage::Format_RGB32);
                
                if (!qimage.isNull()) {
                    emit FrameReady(qimage);
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
