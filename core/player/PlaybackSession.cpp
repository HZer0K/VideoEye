#include "core/player/PlaybackSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include <QImage>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace player {

namespace {
using SteadyClock = std::chrono::steady_clock;
} // namespace

PlaybackSession::PlaybackSession(QObject* parent) : QObject(parent) {
}

PlaybackSession::~PlaybackSession() {
    // 必须先停线程: 它持有 this 与 FFmpeg 上下文, 晚一步就是野指针
    Stop();
    Release();
}

void PlaybackSession::SetHooks(Hooks hooks) {
    hooks_ = std::move(hooks);
}

// --- 媒体资源 ---

void PlaybackSession::AdoptFormatContext(AVFormatContext* ctx) {
    if (format_ctx_ && format_ctx_ != ctx) {
        avformat_close_input(&format_ctx_);
    }
    format_ctx_ = ctx;
}

void PlaybackSession::SetStreamIndices(int video_index, int audio_index) {
    video_stream_index_ = video_index;
    audio_stream_index_ = audio_index;
}

void PlaybackSession::SetVideoDecoder(std::unique_ptr<VideoDecoder> decoder) {
    video_decoder_ = std::move(decoder);
}

void PlaybackSession::SetAudioDecoder(std::unique_ptr<AudioDecoder> decoder) {
    audio_decoder_ = std::move(decoder);
}

void PlaybackSession::SetAudioOutput(std::unique_ptr<AudioOutput> output) {
    audio_output_ = std::move(output);
}

void PlaybackSession::Release() {
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    video_decoder_.reset();
    audio_decoder_.reset();
    audio_output_.reset();
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
}

// --- 播放控制 ---

bool PlaybackSession::Play() {
    if (!format_ctx_)
        return false;
    if (state_.load() == model::PlayerState::Playing)
        return true;

    // 状态迁移与 cv_ 唤醒必须在同一把锁里完成。
    // 否则解码线程可能已经求值完谓词、正准备进阻塞，唤醒却在它之前落到空处 ——
    // 这一次通知就丢了，等待线程永久挂住（表现为 Pause 后 Play 失效、Stop 卡在 join）。
    if (state_.load() == model::PlayerState::Paused) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_stop_.store(false);
            state_.store(model::PlayerState::Playing);
            cv_.notify_one();
        }
        emit StateChanged(state_.load());
        if (audio_output_)
            audio_output_->Play();
        return true;
    }

    if (decode_thread_.joinable())
        decode_thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_stop_.store(false);
        state_.store(model::PlayerState::Playing);
        cv_.notify_one();
    }
    emit StateChanged(state_.load());
    if (audio_output_)
        audio_output_->Play();
    decode_thread_ = std::thread(&PlaybackSession::DecodeThread, this);
    LOG_INFO("PlaybackSession: decode thread started");
    return true;
}

void PlaybackSession::Pause() {
    if (state_.load() == model::PlayerState::Playing) {
        {
            // 同上: 状态迁移 + 唤醒在锁内, 消除丢失唤醒的窗口
            std::lock_guard<std::mutex> lock(mutex_);
            state_.store(model::PlayerState::Paused);
            cv_.notify_one();
        }
        emit StateChanged(state_.load());
        if (audio_output_)
            audio_output_->Pause();
    }
}

void PlaybackSession::Stop() {
    {
        // 状态迁移 + 唤醒进同一把锁: 解码线程要么在进阻塞前看到 new 状态,
        // 要么在阻塞中被这次唤醒叫醒 —— 不会再出现"通知打在没人的地方"。
        std::lock_guard<std::mutex> lock(mutex_);
        should_stop_.store(true);
        state_.store(model::PlayerState::Stopped);
        cv_.notify_one();
    }
    // 先停设备, 唤醒可能在 Enqueue 中阻塞的解码线程
    if (audio_output_)
        audio_output_->Stop();
    if (decode_thread_.joinable() && decode_thread_.get_id() != std::this_thread::get_id()) {
        decode_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_seek_.store(false);
        seek_request_ms_ = 0.0;
    }
    drop_until_sec_.store(-1.0);
    audio_output_.reset();
    current_position_ms_.store(0);
    emit StateChanged(state_.load());
}

void PlaybackSession::Seek(int position_ms, model::SeekMode mode) {
    const int target_ms = duration_ms_ > 0 ? std::clamp(position_ms, 0, duration_ms_) : std::max(0, position_ms);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!format_ctx_)
            return;
        // 只投递 seek 请求: AVFormatContext 由解码线程独占调用 av_seek_frame / av_read_frame,
        // 避免 UI 线程与解码线程并发访问同一个 FFmpeg 上下文。
        pending_seek_mode_.store(mode);
        seek_request_ms_ = target_ms;
        pending_seek_.store(true);
        // 唤醒与置位在同一把锁内: 解码线程正卡在 Pause 分支的 cv_.wait 上,
        // 只置标志不唤醒, 这个定位请求就会一直排着不执行。
        cv_.notify_one();
    }
    // 丢弃已缓冲的旧音频, 避免 seek 后播放过期声音
    if (audio_output_)
        audio_output_->Clear();
    current_position_ms_.store(target_ms);
    emit PositionChanged(current_position_ms_.load(), duration_ms_);
}

void PlaybackSession::SetIdle() {
    state_.store(model::PlayerState::Idle, std::memory_order_release);
    emit StateChanged(state_.load());
}

void PlaybackSession::SetVolume(double volume_0_1) {
    if (audio_output_)
        audio_output_->SetVolume(volume_0_1);
}

void PlaybackSession::ResetSyncTimestamps() {
    last_video_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    last_audio_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
}

// --- 解码线程 ---

void PlaybackSession::EmitPositionIfNeeded(double timestamp_seconds, int& last_emitted_ms) {
    if (!std::isfinite(timestamp_seconds))
        return;
    current_position_ms_.store(static_cast<int>(timestamp_seconds * 1000.0));
    const int pos = current_position_ms_.load();
    int diff = pos - last_emitted_ms;
    if (diff < 0)
        diff = -diff;
    if (last_emitted_ms < 0 || diff >= 100) {
        last_emitted_ms = pos;
        emit PositionChanged(pos, duration_ms_);
    }
}

void PlaybackSession::DecodeThread() {
    LOG_INFO("DecodeThread START");
    AVPacket* packet = av_packet_alloc();
    model::FrameData frame_data;
    SwsContext* sws_ctx = nullptr;
    int sws_src_w = 0, sws_src_h = 0, sws_src_fmt = AV_PIX_FMT_NONE;
    std::vector<std::uint8_t> audio_buffer(192000);
    int last_emitted_position_ms = -1;
    std::int64_t decoded_frames = 0;

    try {
        const int clock_stream_index = SelectPlaybackClockStreamIndex(audio_stream_index_, video_stream_index_);
        const bool enable_pacing = (clock_stream_index >= 0);
        const bool frame_paced_video = (clock_stream_index >= 0 && clock_stream_index == video_stream_index_);
        PlaybackClock playback_clock;
        playback_clock.Reset();

        auto process_pending_seek = [&]() {
            double target_ms = 0.0;
            model::SeekMode mode = model::SeekMode::NearestKeyframe;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pending_seek_.load()) {
                    return false;
                }
                target_ms = seek_request_ms_;
                mode = pending_seek_mode_.load();
                pending_seek_.store(false);
            }

            if (!format_ctx_) {
                return true;
            }

            av_packet_unref(packet);
            const std::int64_t timestamp = static_cast<std::int64_t>(target_ms) * 1000LL;
            const int flags = AVSEEK_FLAG_BACKWARD;
            const int ret = av_seek_frame(format_ctx_, -1, timestamp, flags);
            if (ret < 0) {
                drop_until_sec_.store(-1.0);
                emit Error("Seek failed");
                return true;
            }

            if (video_decoder_)
                video_decoder_->Flush();
            if (audio_decoder_)
                audio_decoder_->Flush();

            // 定位后重置流统计: 统计语义是"自上次打开/定位起的当前片段", 而非全片累计。
            // 放在 seek 成功 + 解码器 flush 之后, 保证旧位置的残留包不会被计入新片段。
            if (hooks_.on_seek_done)
                hooks_.on_seek_done(target_ms, mode);

            // 定位之后必须重置播放时钟, 把新位置作为新的时间基准。
            // 否则 PaceTo 仍按"视频起点"计算需要 sleep 的时长, 产生长达 (目标时间戳 - 起点)
            // 秒的 sleep —— 解码线程被睡死, 画面/进度条冻结。
            playback_clock.Reset();
            drop_until_sec_.store((mode == model::SeekMode::ExactFrame) ? (target_ms / 1000.0) : -1.0);
            current_position_ms_.store(static_cast<int>(target_ms));
            last_emitted_position_ms = static_cast<int>(target_ms);
            emit PositionChanged(current_position_ms_.load(), duration_ms_);
            return true;
        };

        while (!should_stop_.load()) {
            if (process_pending_seek()) {
                continue;
            }

            if (state_.load() == model::PlayerState::Paused) {
                const auto pause_begin = SteadyClock::now();
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return state_.load() != model::PlayerState::Paused || should_stop_.load() || pending_seek_.load();
                });
                const auto pause_end = SteadyClock::now();
                if (enable_pacing)
                    playback_clock.OnPaused(pause_end - pause_begin);
                continue;
            }

            int ret = av_read_frame(format_ctx_, packet);
            if (ret < 0) {
                LOG_INFO("DecodeThread: av_read_frame EOF -> PlaybackFinished");
                // 播到结尾: 通知分析侧闭合还开着的缺陷段, 否则最后一条要等下次播放才出现
                if (hooks_.on_end_of_stream)
                    hooks_.on_end_of_stream();
                emit PlaybackFinished();
                break;
            }

            const std::int64_t pkt_ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
            const double packet_ts_sec = PacketTimestampSeconds(format_ctx_, packet);

            if (hooks_.on_packet) {
                PacketContext ctx;
                ctx.packet = packet;
                ctx.format_ctx = format_ctx_;
                ctx.timestamp_seconds = packet_ts_sec;
                ctx.raw_timestamp = pkt_ts;
                hooks_.on_packet(ctx);
            }

            if (packet->stream_index == clock_stream_index && !frame_paced_video) {
                playback_clock.PaceTo(packet_ts_sec);
                EmitPositionIfNeeded(packet_ts_sec, last_emitted_position_ms);
            }

            // --- 视频解码 ---
            if (packet->stream_index == video_stream_index_ && video_decoder_) {
                if (video_decoder_->SendPacket(packet)) {
                    while (!should_stop_.load() && video_decoder_->ReceiveFrame(frame_data)) {
                        double frame_ts = frame_data.timestamp;
                        if (format_ctx_ && video_stream_index_ >= 0) {
                            AVStream* vs = format_ctx_->streams[video_stream_index_];
                            if (vs && vs->time_base.den != 0 && frame_data.pts != AV_NOPTS_VALUE) {
                                frame_ts = frame_data.pts * av_q2d(vs->time_base);
                                frame_data.timestamp = frame_ts;
                            }
                        }
                        // 精确帧定位: 从关键帧向前逐帧解码到目标时间戳。
                        // 追赶阶段放行显示(画面快速前进到目标, 不再冻结), 但跳过分析开销
                        // (由 catching_up 告知分析侧); 越过目标帧后清除标志。
                        if (drop_until_sec_.load() >= 0.0 && frame_ts + 0.001 >= drop_until_sec_.load()) {
                            drop_until_sec_.store(-1.0);
                        }

                        if (frame_paced_video) {
                            playback_clock.PaceTo(frame_ts);
                            EmitPositionIfNeeded(frame_ts, last_emitted_position_ms);
                        }
                        last_video_sync_ts_ = frame_ts;
                        if (std::isfinite(last_audio_sync_ts_) && hooks_.on_sync_sample) {
                            hooks_.on_sync_sample(last_audio_sync_ts_, last_video_sync_ts_, false);
                        }

                        // 画面输出只有一条路径: sws_scale 转 BGRA -> QImage -> FrameReady。
                        // 播放区隐藏时整段跳过: 解码、实时分析、音频均不受影响。
                        if (!rendering_suppressed_.load(std::memory_order_relaxed)) {
                            // 与 OnVideoFrame 的判空保持一致: 格式合法但像素平面为空 /
                            // 宽高为 0 的帧(畸形帧或异常像素格式)不能交给 sws_scale,
                            // 否则会拿空 src_data[0] 段错误, 且不在 try/catch 内 -> 整进程崩。
                            if (frame_data.format >= 0 && frame_data.data[0] &&
                                frame_data.width > 0 && frame_data.height > 0) {
                                if (sws_src_w != frame_data.width || sws_src_h != frame_data.height ||
                                    sws_src_fmt != frame_data.format) {
                                    sws_src_w = frame_data.width;
                                    sws_src_h = frame_data.height;
                                    sws_src_fmt = frame_data.format;
                                }
                                sws_ctx = sws_getCachedContext(sws_ctx, frame_data.width, frame_data.height,
                                                               static_cast<AVPixelFormat>(frame_data.format),
                                                               frame_data.width, frame_data.height, AV_PIX_FMT_BGRA,
                                                               SWS_BILINEAR, nullptr, nullptr, nullptr);
                            }
                            if (sws_ctx) {
                                QImage qimage(frame_data.width, frame_data.height, QImage::Format_ARGB32);
                                if (!qimage.isNull()) {
                                    std::uint8_t* dst_slices[4] = {qimage.bits(), nullptr, nullptr, nullptr};
                                    int dst_linesize[4] = {static_cast<int>(qimage.bytesPerLine()), 0, 0, 0};
                                    sws_scale(sws_ctx, frame_data.data, frame_data.linesize, 0, frame_data.height,
                                              dst_slices, dst_linesize);
                                    emit FrameReady(qimage);
                                    if (++decoded_frames % 120 == 0) {
                                        LOG_INFO("DecodeThread heartbeat: frames=" + std::to_string(decoded_frames) +
                                                 " pos_ms=" + std::to_string(current_position_ms_.load()));
                                    }
                                }
                            }
                        }

                        if (hooks_.on_video_frame) {
                            VideoFrameContext ctx;
                            ctx.frame = &frame_data;
                            ctx.raw_frame = video_decoder_->GetLastRawFrame();
                            ctx.packet = packet;
                            ctx.format_ctx = format_ctx_;
                            ctx.decoder = video_decoder_.get();
                            ctx.video_stream_index = video_stream_index_;
                            ctx.timestamp_seconds = frame_ts;
                            ctx.catching_up = drop_until_sec_.load() >= 0.0;
                            hooks_.on_video_frame(ctx);
                        }
                    }
                }
            }

            // --- 音频解码 ---
            if (packet->stream_index == audio_stream_index_ && audio_decoder_) {
                if (audio_decoder_->SendPacket(packet)) {
                    int out_size = 0;
                    while (audio_decoder_->ReceiveFrame(audio_buffer.data(), static_cast<int>(audio_buffer.size()),
                                                        out_size)) {
                        // 精确帧定位追赶阶段 / 拖动进度条期间: 丢弃音频输出与分析事件, 避免过期声音。
                        // 越过目标时间戳后必须像视频路径一样把标志清掉: 原来只有视频分支清,
                        // 纯音频流(永远等不到视频帧来清)会一直停在丢弃状态 —— 音频永久静音,
                        // 剩下的视频帧也因 catching_up 恒真而关掉全部视频分析。
                        if (drop_until_sec_.load() >= 0.0 && packet_ts_sec + 0.001 >= drop_until_sec_.load()) {
                            drop_until_sec_.store(-1.0);
                        }

                        if (drop_until_sec_.load() >= 0.0 || drag_seeking_.load())
                            continue;

                        const std::int64_t frame_pts = static_cast<std::int64_t>(audio_decoder_->GetLastFramePts());
                        double ts = current_position_ms_.load() / 1000.0;
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

                        last_audio_sync_ts_ = ts;
                        if (std::isfinite(last_video_sync_ts_) && hooks_.on_sync_sample) {
                            hooks_.on_sync_sample(last_audio_sync_ts_, last_video_sync_ts_, true);
                        }

                        // 推送 PCM 到环形缓冲，由音频后端线程 pull 播放
                        bool enqueued = false;
                        double level = 0.0;
                        if (out_size >= static_cast<int>(sizeof(std::int16_t))) {
                            if (audio_output_) {
                                audio_output_->Enqueue(audio_buffer.data(), out_size);
                            }
                            enqueued = true;

                            const std::int16_t* samples = reinterpret_cast<const std::int16_t*>(audio_buffer.data());
                            const int sample_count = out_size / static_cast<int>(sizeof(std::int16_t));
                            long double sumsq = 0.0;
                            for (int i = 0; i < sample_count; ++i) {
                                const long double s = static_cast<long double>(samples[i]);
                                sumsq += s * s;
                            }
                            if (sample_count > 0) {
                                level = std::sqrt(static_cast<double>(sumsq / sample_count)) / 32768.0;
                                level = std::clamp(level, 0.0, 1.0);
                            }
                        }

                        if (hooks_.on_audio_frame) {
                            AudioFrameContext ctx;
                            ctx.packet = packet;
                            ctx.format_ctx = format_ctx_;
                            ctx.decoder = audio_decoder_.get();
                            ctx.samples = reinterpret_cast<const std::int16_t*>(audio_buffer.data());
                            ctx.sample_count = out_size / static_cast<int>(sizeof(std::int16_t));
                            ctx.sample_rate = audio_decoder_->GetLastFrameSampleRate();
                            ctx.channels = std::max(1, audio_decoder_->GetLastFrameChannels());
                            ctx.byte_count = out_size;
                            ctx.frame_pts = frame_pts;
                            ctx.timestamp_seconds = ts;
                            ctx.level = level;
                            ctx.audio_stream_index = audio_stream_index_;
                            ctx.enqueued = enqueued;
                            hooks_.on_audio_frame(ctx);
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }

    } catch (const std::exception& e) {
        LOG_ERROR("DecodeThread exception: " + std::string(e.what()));
        emit Error(QString("解码线程发生异常，已停止播放: %1").arg(e.what()));
        should_stop_.store(true);
    } catch (...) {
        LOG_ERROR("DecodeThread unknown exception");
        emit Error(QString("解码线程发生未知异常，已停止播放"));
        should_stop_.store(true);
    }

    LOG_INFO("DecodeThread EXIT");
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }
    av_packet_free(&packet);
    if (state_.load() != model::PlayerState::Stopped) {
        state_.store(model::PlayerState::Stopped);
        emit StateChanged(state_.load());
    }
}

} // namespace player
} // namespace videoeye
