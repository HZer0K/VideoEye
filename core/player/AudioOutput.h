#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// 音频输出。
//
// 实现: 平台原生后端 (Windows WASAPI / Linux ALSA / macOS AudioQueue)，
// 之所以不用 Qt QAudioSink: 它在 QtMultimedia 模块里，会把「Qt Widgets + FFmpeg」
// 这两个硬依赖再拉进来一整个 Qt 子模块（连带 qtshadertools），与本项目
// 「依赖越少越好」的目标冲突。后端代码量可控（约 300 行），且各平台都是系统自带库。
//
// 模型: 解码线程通过 Enqueue() 推送 S16 交错 PCM 到环形缓冲，平台音频线程
// 主动拉取。环形缓冲带背压：解码过快时 Enqueue 阻塞，自然把解码节奏对齐到
// 声卡消费速率（与原先 SDL 回调版语义一致）。
namespace videoeye {
namespace player {

class AudioOutput {
public:
    // 平台后端的统一接口（实现在 .cpp，每个平台一份）
    class Backend {
    public:
        virtual ~Backend() = default;
        // 打开设备；sample_rate/channels 为入参，可能被后端调整为设备实际支持值
        virtual bool Open(int& sample_rate, int& channels) = 0;
        virtual void Start() = 0;   // 开始播放（拉取数据）
        virtual void Stop() = 0;    // 暂停（保留设备）
        virtual void Close() = 0;   // 停止线程并释放设备
    };

    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /// 以指定采样率/声道数打开音频设备（S16 交错）。成功返回 true。
    bool Open(int sample_rate, int channels);

    /// 后台线程打开音频设备，主线程立即返回。
    /// 设备就绪前 Enqueue() 直接丢弃 PCM（视频照常播放），
    /// 就绪后若期间调用过 Play() 会自动补上，不需要调用方轮询。
    void OpenAsync(int sample_rate, int channels);

    /// 推送一帧 PCM 数据。缓冲接近满时阻塞（背压），避免无限增长。
    void Enqueue(const uint8_t* data, int len);

    void Play();     // 开始播放（取消暂停）
    void Pause();    // 暂停播放
    void Stop();     // 停止并关闭设备
    void Clear();    // 清空环形缓冲（seek 时调用）

    /// 设置音量 0.0 - 1.0
    void SetVolume(double volume);

    /// 当前已缓冲的毫秒数（诊断用）
    int GetBufferedMs(int sample_rate, int channels) const;

    bool IsOpen() const { return opened_.load(); }

private:
    // 由平台音频线程调用：从环形缓冲取数据，不足时补静音
    size_t Pull(uint8_t* dst, size_t max_bytes);

    // 当前线程是不是 open_thread_ 本身。
    // Open() 会在 open_thread_ 上被调用（OpenAsync 路径），此时 Stop() 若去
    // join open_thread_ 就是「线程 join 自己」，MSVC 会抛
    // std::system_error(resource_deadlock_would_occur) —— 而 std::thread 的
    // 入口函数是 noexcept 的，异常逃逸会直接 std::terminate()（进程闪退）。
    bool OnOpenThread() const;

    std::unique_ptr<Backend> backend_;
    std::mutex device_mutex_;      // 保护 backend_ 的创建/销毁/控制

    std::atomic<bool> opened_{false};
    int sample_rate_ = 0;
    int channels_ = 0;

    // 环形缓冲
    std::vector<uint8_t> ring_;
    size_t ring_capacity_ = 0;
    size_t write_pos_ = 0;
    size_t read_pos_ = 0;
    size_t filled_ = 0;
    mutable std::mutex ring_mutex_;
    std::condition_variable ring_cv_;   // 生产者等待有空位

    std::atomic<double> volume_{1.0};

    std::thread open_thread_;
    std::atomic<bool> pending_play_{false};   // 设备就绪前调用过 Play()
    std::atomic<bool> stopping_{false};       // Stop() 已调用
};

} // namespace player
} // namespace videoeye
