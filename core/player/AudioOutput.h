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

#include "core/player/S16Converter.h"

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
    // 一路交错 PCM 的完整格式描述。
    //
    // 「请求格式」与「设备实际格式」是两件事：解码器永远按源的
    // rate/channels 产出 S16，而设备（尤其是 Windows 共享模式）可能只接受自己的
    // mix format（常见 48 kHz / float32）。两者不一致时由 Pull() 里的
    // S16Converter 负责转换，而不是把 S16 字节硬塞给 float32 设备。
    struct PcmFormat {
        int sample_rate = 0;
        int channels = 0;
        PcmSampleFormat sample = PcmSampleFormat::S16;

        int BytesPerSample() const {
            return PcmBytesPerSample(sample);
        }
        int FrameBytes() const {
            return channels * BytesPerSample();
        }
        bool operator==(const PcmFormat& other) const {
            return sample_rate == other.sample_rate && channels == other.channels && sample == other.sample;
        }
        bool operator!=(const PcmFormat& other) const {
            return !(*this == other);
        }
    };

    // 平台后端的统一接口（实现在 .cpp，每个平台一份）
    class Backend {
    public:
        // 平台后端要能写 `PcmFormat`，而 AudioOutput 的嵌套类名不会自动对派生类可见
        using PcmFormat = AudioOutput::PcmFormat;

        virtual ~Backend() = default;
        // 打开设备。requested 是调用方希望的格式，actual 回填设备最终使用的格式
        // （设备可能拒绝 requested 而改用 mix format —— 由调用方负责转换）
        virtual bool Open(const PcmFormat& requested, PcmFormat& actual) = 0;
        virtual void Start() = 0; // 开始播放（拉取数据）
        virtual void Stop() = 0;  // 暂停（保留设备）
        virtual void Close() = 0; // 停止线程并释放设备
    };

    // 仅供测试：把平台后端换成假后端，这样「设备实际格式回退」「连续声卡回调」
    // 这些路径才不需要真声卡、也不依赖离屏环境里有没有音频设备。
    // 传入空 factory 即恢复平台后端；调用方负责在退出前复位（RAII）。
    // 生产代码不设置它，Open() 走的仍是原平台分支。
    using PullFn = std::function<size_t(uint8_t*, size_t)>;
    using BackendFactory = std::function<std::unique_ptr<Backend>(PullFn)>;
    static void SetBackendFactoryForTesting(BackendFactory factory);

    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /// 以指定采样率/声道数打开音频设备（S16 交错）。成功返回 true。
    /// 注意: 设备可能不接受这两个参数而回退到自己的格式，此后 Enqueue() 送进来的
    /// 源格式 PCM 由 Pull() 负责转换成设备格式（见 S16Converter）。
    bool Open(int sample_rate, int channels);

    /// 后台线程打开音频设备，主线程立即返回。
    /// 设备就绪前 Enqueue() 会等待（有上限），而不是直接丢弃 —— 否则短音频
    /// 会在设备初始化完成前就被解码线程整段扔掉，表现为整段无声。
    /// 就绪后若期间调用过 Play() 会自动补上，不需要调用方轮询。
    void OpenAsync(int sample_rate, int channels);

    /// 推送一帧 PCM 数据。缓冲接近满时阻塞（背压），避免无限增长。
    void Enqueue(const uint8_t* data, int len);

    void Play();  // 开始播放（取消暂停）
    void Pause(); // 暂停播放
    void Stop();  // 停止并关闭设备
    void Clear(); // 清空环形缓冲（seek 时调用）

    /// 设置音量 0.0 - 1.0
    void SetVolume(double volume);

    /// 当前已缓冲的毫秒数（诊断用）
    int GetBufferedMs(int sample_rate, int channels) const;

    /// 重采样器内部还压着多少源帧（诊断 / 长稳守护用；直通时恒 0）。
    /// 稳态只应有滤波器固有长度；随回调次数一路涨即代表喂入量算错了。
    int64_t GetConverterDelayFrames() const;

    bool IsOpen() const {
        return opened_.load();
    }

private:
    // Stop() 的内部实现。notify_open_waiters 只在**用户主动停止**时为 true：
    // Open() 为了重置设备会走一遍停止流程，但那不代表"本次打开已有结论"，
    // 若此时把 open_finished_ 翻成 true，正在等设备就绪的 Enqueue() 会误判
    // 并丢掉开头几帧音频（异步打开期的开局无声竞态）。
    void StopInternal(bool notify_open_waiters);

    // 由平台音频线程调用：从环形缓冲取数据，不足时补静音
    size_t Pull(uint8_t* dst, size_t max_bytes);

    // 从环形缓冲读出至多 max_bytes（按源帧对齐），只读不等待。
    size_t ReadRing(uint8_t* dst, size_t max_bytes);

    // 源格式 → 设备格式的转换路径（converter_ 未激活时不会走到）。
    size_t PullConverted(uint8_t* dst, size_t max_bytes);

    // 按设备格式写静音（8bit 无符号是 0x80，其余全 0）
    void FillSilence(uint8_t* dst, size_t bytes);

    // 标记"打开流程已结束"（成功或失败），唤醒等待中的 Enqueue()
    void MarkOpenFinished();

    // 当前线程是不是本对象的异步打开线程（open_thread_）本身。
    // Open() 会在 open_thread_ 上被调用（OpenAsync 路径），此时 Stop() 若去
    // join open_thread_ 就是「线程 join 自己」，MSVC 会抛
    // std::system_error(resource_deadlock_would_occur) —— 而 std::thread 的
    // 入口函数是 noexcept 的，异常逃逸会直接 std::terminate()（进程闪退）。
    //
    // ❗ 判定不能用 open_thread_ 自己（"joinable() && get_id() == 本线程"）：
    // 那样新线程一开跑就会去读一个正在被主线程赋值的 std::thread，是实打实的
    // 数据竞争。实现改成线程局部标记，详见 .cpp 里的说明。
    bool OnOpenThread() const;

    std::unique_ptr<Backend> backend_;
    std::mutex device_mutex_; // 保护 backend_ 的创建/销毁/控制

    std::atomic<bool> opened_{false};
    // 打开流程是否已经"有结论"（成功或失败）。Enqueue() 在设备就绪前等待的依据:
    // 只等"正在打开"的这段时间，打开已失败就不再白等。
    std::atomic<bool> open_finished_{true};

    // 源格式（解码器产出、Enqueue 送入的格式）与设备实际格式。
    // ring_ 里存的是**源格式**字节（S16 交错），Pull() 出口才是设备格式。
    PcmFormat src_format_;
    PcmFormat device_format_;
    S16Converter converter_;
    std::vector<uint8_t> conv_in_; // 转换用的源字节暂存（一次读一环）

    // 环形缓冲
    std::vector<uint8_t> ring_;
    size_t ring_capacity_ = 0;
    size_t write_pos_ = 0;
    size_t read_pos_ = 0;
    size_t filled_ = 0;
    mutable std::mutex ring_mutex_;
    std::condition_variable ring_cv_; // 生产者等待有空位

    std::atomic<double> volume_{1.0};

    std::thread open_thread_;
    std::atomic<bool> pending_play_{false}; // 设备就绪前调用过 Play()
    std::atomic<bool> stopping_{false};     // Stop() 已调用
};

} // namespace player
} // namespace videoeye
