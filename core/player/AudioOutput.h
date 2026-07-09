#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

// 基于 SDL2 的音频输出。
// 解码线程通过 Enqueue() 推送 S16 交错 PCM 数据，SDL 在其音频线程中通过
// 回调从环形缓冲拉取并播放。环形缓冲带背压：解码过快时 Enqueue 阻塞，
// 自然把解码节奏对齐到声卡消费速率。
namespace videoeye {
namespace player {

class AudioOutput {
public:
    AudioOutput() = default;
    ~AudioOutput();

    // 以指定采样率/声道数打开音频设备（S16 交错）。成功返回 true。
    bool Open(int sample_rate, int channels);

    // 推送一帧 PCM 数据。缓冲接近满时阻塞（背压），避免无限增长。
    void Enqueue(const uint8_t* data, int len);

    // 开始播放（取消 SDL 暂停）。
    void Play();
    // 暂停播放。
    void Pause();
    // 停止并关闭设备。
    void Stop();

    // 清空环形缓冲（seek 时调用，避免播放过期音频）。
    void Clear();

    // 设置音量 0.0 - 1.0（回调中缩放样本）。
    void SetVolume(double volume);

    // 当前已缓冲的毫秒数（诊断用）。
    int GetBufferedMs(int sample_rate, int channels) const;

    bool IsOpen() const { return device_id_ > 0; }

private:
    static void FillCallback(void* userdata, uint8_t* stream, int len);
    void Fill(uint8_t* stream, int len);

    uint32_t device_id_ = 0;
    int sample_rate_ = 0;
    int channels_ = 0;

    // 环形缓冲
    std::vector<uint8_t> ring_;
    size_t ring_capacity_ = 0;
    size_t write_pos_ = 0;
    size_t read_pos_ = 0;
    size_t filled_ = 0;
    mutable std::mutex ring_mutex_;
    std::condition_variable ring_cv_; // 生产者等待有空位

    std::atomic<double> volume_{1.0};
};

} // namespace player
} // namespace videoeye
