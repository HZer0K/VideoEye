#include "AudioOutput.h"

#include <SDL.h>

#include <algorithm>
#include <cstring>

#include "utils/Logger.h"
#include "utils/ScopedTimer.h"

namespace videoeye {
namespace player {

AudioOutput::~AudioOutput() {
    Stop();
}

bool AudioOutput::Open(int sample_rate, int channels) {
    if (sample_rate <= 0) sample_rate = 44100;
    if (channels <= 0) channels = 2;
    sample_rate_ = sample_rate;
    channels_ = channels;

    {
        VE_PERF("SDL_Init(AUDIO)");
        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            LOG_ERROR("AudioOutput: SDL_Init(AUDIO) 失败: " + std::string(SDL_GetError()));
            return false;
        }
    }

    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = sample_rate;
    want.format = AUDIO_S16SYS; // 本机字节序有符号 16bit，与 AV_SAMPLE_FMT_S16 匹配
    want.channels = static_cast<Uint8>(channels);
    want.samples = 4096;
    want.callback = &AudioOutput::FillCallback;
    want.userdata = this;

    {
        VE_PERF("SDL_OpenAudioDevice");
        device_id_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    }
    if (device_id_ == 0) {
        LOG_ERROR("AudioOutput: SDL_OpenAudioDevice 失败: " + std::string(SDL_GetError()));
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    // 接受设备实际参数
    sample_rate_ = have.freq;
    channels_ = have.channels;

    // 环形缓冲容量约为 2 秒
    ring_capacity_ = static_cast<size_t>(sample_rate_) * static_cast<size_t>(channels_) * 2 * 2;
    ring_.resize(ring_capacity_);
    write_pos_ = read_pos_ = filled_ = 0;

    LOG_INFO("AudioOutput: 音频设备已打开 freq=" + std::to_string(sample_rate_) +
             " channels=" + std::to_string(channels_));
    return true;
}

void AudioOutput::OpenAsync(int sample_rate, int channels) {
    // 上一轮还没开完就先收尾（join 期间最多等一个设备打开周期）
    if (open_thread_.joinable()) {
        open_thread_.join();
    }
    open_thread_ = std::thread([this, sample_rate, channels]() {
        const bool ok = Open(sample_rate, channels);
        if (ok && pending_play_.exchange(false)) {
            SDL_PauseAudioDevice(device_id_.load(), 0);
        }
        if (!ok) pending_play_.store(false);
    });
}

void AudioOutput::FillCallback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<AudioOutput*>(userdata);
    if (self) self->Fill(stream, len);
}

void AudioOutput::Fill(uint8_t* stream, int len) {
    if (len <= 0) return;

    std::lock_guard<std::mutex> lock(ring_mutex_);
    const int to_copy = std::min<int>(len, static_cast<int>(filled_));

    if (to_copy == 0) {
        // 无数据：输出静音
        std::memset(stream, 0, len);
        return;
    }

    const size_t first = std::min<size_t>(static_cast<size_t>(to_copy), ring_capacity_ - read_pos_);
    std::memcpy(stream, ring_.data() + read_pos_, first);
    if (first < static_cast<size_t>(to_copy)) {
        std::memcpy(stream + first, ring_.data(), to_copy - first);
    }
    read_pos_ = (read_pos_ + to_copy) % ring_capacity_;
    filled_ -= static_cast<size_t>(to_copy);
    ring_cv_.notify_one(); // 通知生产者（背压）有空位

    if (to_copy < len) {
        std::memset(stream + to_copy, 0, len - to_copy);
    }

    // 音量缩放（S16）
    const double vol = volume_.load();
    if (vol != 1.0) {
        int16_t* samples = reinterpret_cast<int16_t*>(stream);
        const int n = len / 2;
        for (int i = 0; i < n; ++i) {
            int v = static_cast<int>(static_cast<double>(samples[i]) * vol);
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            samples[i] = static_cast<int16_t>(v);
        }
    }
}

void AudioOutput::Enqueue(const uint8_t* data, int len) {
    if (device_id_.load() == 0 || !data || len <= 0) return;   // 异步打开未完成: 丢弃

    std::unique_lock<std::mutex> lock(ring_mutex_);
    // 背压：缓冲将溢出时等待有空位（避免无限增长）
    while (filled_ + static_cast<size_t>(len) > ring_capacity_) {
        ring_cv_.wait(lock);
        if (device_id_.load() == 0) return; // 已被 Stop
    }

    const size_t first = std::min<size_t>(static_cast<size_t>(len), ring_capacity_ - write_pos_);
    std::memcpy(ring_.data() + write_pos_, data, first);
    if (first < static_cast<size_t>(len)) {
        std::memcpy(ring_.data(), data + first, len - first);
    }
    write_pos_ = (write_pos_ + static_cast<size_t>(len)) % ring_capacity_;
    filled_ += static_cast<size_t>(len);
    ring_cv_.notify_one();
}

void AudioOutput::Play() {
    const uint32_t id = device_id_.load();
    if (id == 0) {
        // 异步打开还没完成: 记下来, 设备就绪时由后台线程补一次 Play
        pending_play_.store(true);
        return;
    }
    SDL_PauseAudioDevice(id, 0);
}

void AudioOutput::Pause() {
    pending_play_.store(false);
    const uint32_t id = device_id_.load();
    if (id != 0) {
        SDL_PauseAudioDevice(id, 1);
    }
}

void AudioOutput::Clear() {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    write_pos_ = read_pos_ = filled_ = 0;
    ring_cv_.notify_all();
}

void AudioOutput::SetVolume(double volume) {
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;
    volume_.store(volume);
}

int AudioOutput::GetBufferedMs(int sample_rate, int channels) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (sample_rate <= 0 || channels <= 0) return 0;
    return static_cast<int>(filled_ * 1000 / (sample_rate * channels * 2));
}

void AudioOutput::Stop() {
    pending_play_.store(false);
    if (open_thread_.joinable()) {
        open_thread_.join();
    }
    const uint32_t id = device_id_.load();
    if (id != 0) {
        SDL_PauseAudioDevice(id, 1);
        SDL_CloseAudioDevice(id);
        device_id_.store(0);
    }
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        filled_ = write_pos_ = read_pos_ = 0;
        ring_.clear();
        ring_capacity_ = 0;
    }
    ring_cv_.notify_all();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

} // namespace player
} // namespace videoeye
