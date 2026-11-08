#include "core/player/AudioOutput.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "core/domain/task/ThreadBody.h"
#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"

// ============================================================
// 平台后端
// ============================================================
#if defined(Q_OS_WIN) || defined(_WIN32)
// ---------------- Windows: WASAPI (共享模式, 事件驱动) ----------------
#define VIDEOEYE_AUDIO_WIN 1
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <windows.h>
#elif defined(__APPLE__)
// ---------------- macOS: AudioQueue ----------------
#define VIDEOEYE_AUDIO_MAC 1
#include <AudioToolbox/AudioToolbox.h>
#else
// ---------------- Linux/其他: ALSA (运行时 dlopen，无构建期依赖) ----------------
#define VIDEOEYE_AUDIO_ALSA 1
#include <dlfcn.h>
#include <errno.h>
#endif

namespace videoeye {
namespace player {

using PullFn = std::function<size_t(uint8_t*, size_t)>;

namespace {

// 测试注入的后端工厂。进程内只有测试会设置它，且必须由用例在退出前复位；
// 生产代码从不碰这里，Open() 仍然走下面的平台分支。
AudioOutput::BackendFactory& TestBackendFactory() {
    static AudioOutput::BackendFactory factory;
    return factory;
}

} // namespace

void AudioOutput::SetBackendFactoryForTesting(BackendFactory factory) {
    TestBackendFactory() = std::move(factory);
}

// Enqueue() 等设备就绪的上限。WASAPI 后端自己的初始化等待是 5 s，超过它说明
// 打开流程已经卡死或失败，继续等只会把整条解码线程（含视频）一起拖住。
constexpr std::chrono::milliseconds kDeviceOpenWait{5000};

#if defined(VIDEOEYE_AUDIO_WIN)

// KSDATAFORMAT_SUBTYPE_PCM / KSDATAFORMAT_SUBTYPE_IEEE_FLOAT。
// 直接把 GUID 写死，省得为两个常量引入 ksmedia.h（WDK 头）。
static const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// 把 WAVEFORMATEX 映射成本项目的采样格式。
//
// 只接受"解码链路能产出来"的线性 PCM：压缩格式（AC3/DTS 的 SPDIF 直通等）和
// 奇怪位宽一律拒绝 —— 明知产不出来还硬开，播出来就是杂音。
bool MapWaveFormat(const WAVEFORMATEX* wfx, PcmSampleFormat& out) {
    if (wfx == nullptr)
        return false;

    bool is_float = false;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (IsEqualGUID(ext->SubFormat, kSubtypePcm)) {
            is_float = false;
        } else if (IsEqualGUID(ext->SubFormat, kSubtypeFloat)) {
            is_float = true;
        } else {
            return false;
        }
    } else if (wfx->wFormatTag != WAVE_FORMAT_PCM) {
        return false;
    }

    if (is_float) {
        if (wfx->wBitsPerSample == 32) {
            out = PcmSampleFormat::F32;
            return true;
        }
        if (wfx->wBitsPerSample == 64) {
            out = PcmSampleFormat::F64;
            return true;
        }
        return false;
    }

    switch (wfx->wBitsPerSample) {
    case 8:
        out = PcmSampleFormat::U8;
        return true;
    case 16:
        out = PcmSampleFormat::S16;
        return true;
    case 24:
        // FFmpeg 8 已删掉 AV_SAMPLE_FMT_S24（3 字节紧凑打包），产不出这种布局。
        // Windows 的 mix format 实际几乎不会出现 24bit 打包（要么 float32，要么
        // 24-in-32 的 EXTENSIBLE 32 位容器），真遇到就放弃本次播放。
        return false;
    case 32:
        out = PcmSampleFormat::S32;
        return true;
    default:
        return false;
    }
}

class WasapiBackend final : public AudioOutput::Backend {
public:
    explicit WasapiBackend(PullFn pull) : pull_(std::move(pull)) {
    }
    ~WasapiBackend() override {
        Close();
    }

    bool Open(const PcmFormat& requested, PcmFormat& actual) override {
        want_ = requested;
        if (want_.sample_rate <= 0)
            want_.sample_rate = 44100;
        if (want_.channels <= 0)
            want_.channels = 2;
        // 默认按请求格式打开；InitDevice 只有在设备拒绝时才会改写它
        actual_ = want_;
        running_.store(true);
        finished_.store(false);
        thread_ = std::thread([this]() {
            // 与其它所有线程入口共用同一份异常收口（core/domain/task/ThreadBody.h）：
            // std::thread 的入口在 MSVC STL 里是 noexcept，异常逃逸 = 进程闪退
            // （0xC0000409）。音频设备打不开最多是没声音，不能把播放器一起带走。
            const std::optional<std::string> failure = task::GuardBody([this]() { ThreadMain(); });
            if (failure) {
                running_.store(false);
                LOG_ERROR(std::string("AudioOutput(WASAPI): 输出线程异常退出: ") + *failure);
            }
        });

        // 等待后台线程完成设备初始化（含 COM 初始化，必须在同一线程内配对）
        std::unique_lock<std::mutex> lock(ready_mutex_);
        ready_cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return ready_.load(); });
        if (!ready_.load()) {
            running_.store(false);
            SetEvent(event_);
            if (thread_.joinable())
                thread_.join();
            return false;
        }
        if (!ok_.load()) {
            running_.store(false);
            SetEvent(event_);
            if (thread_.joinable())
                thread_.join();
            return false;
        }
        actual = actual_;
        return true;
    }

    void Start() override {
        if (client_)
            client_->Start();
    }

    void Stop() override {
        if (client_)
            client_->Stop();
    }

    void Close() override {
        if (!thread_.joinable())
            return;
        running_.store(false);
        if (event_)
            SetEvent(event_);
        thread_.join();
    }

private:
    void ThreadMain() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

        // 先按"请求值"填，再由 InitDevice 覆盖成设备实际值。
        // ❗ 顺序不能反: 以前是在 InitDevice 之后无条件写回 want_*，于是设备回退
        // （源 44.1 kHz → 设备 48 kHz）得到的实际值被当场抹掉，Open() 拿到的还是
        // 请求值 —— 环形缓冲按错格式算容量，且下游根本不知道需要转换。
        actual_ = want_;

        bool ok = false;
        std::string err;
        if (com_ok)
            ok = InitDevice(err);

        // 通知 Open() 结果；失败也要通知，否则调用方会卡满 5 s
        ok_.store(ok);
        ready_.store(true);
        ready_cv_.notify_all();

        if (!ok) {
            if (com_ok)
                CoUninitialize();
            finished_.store(true);
            LOG_WARN("AudioOutput(WASAPI): 打开音频设备失败" + (err.empty() ? std::string() : (": " + err)));
            return;
        }

        PumpLoop();
        CleanupDevice();
        if (com_ok)
            CoUninitialize();
        finished_.store(true);
    }

    bool InitDevice(std::string& err) {
        constexpr REFERENCE_TIME kBufferDuration = 2000000; // 200 ms (100ns 单位)

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            err = "CoCreateInstance(MMDeviceEnumerator) 失败";
            return false;
        }

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr)) {
            err = "GetDefaultAudioEndpoint 失败 (无声卡?)";
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
        device->Release();
        if (FAILED(hr)) {
            err = "Activate(IAudioClient) 失败";
            return false;
        }

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = static_cast<WORD>(want_.channels);
        wfx.nSamplesPerSec = static_cast<DWORD>(want_.sample_rate);
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = static_cast<WORD>(want_.channels * 2);
        wfx.nAvgBytesPerSec = static_cast<DWORD>(want_.sample_rate * want_.channels * 2);
        wfx.cbSize = 0;
        block_align_ = wfx.nBlockAlign;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, &wfx,
                                 nullptr);
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            // 设备不接受「S16 + 源的采样率/声道数」。共享模式的规则是只能用设备的
            // mix format（实测常见 48 kHz / float32），所以这里直接拿 mix format 原样
            // 重开，并把它的**完整格式**记进 actual_ —— 由 AudioOutput 那一层把
            // 解码器产出的 S16 转成设备格式。
            //
            // ❗ 旧实现只对齐采样率/声道数、仍坚持 16bit，于是两条路都错：mix 是
            // float32 时 Initialize 再次失败（整段无声），即便成功也会把 S16 字节
            // 当浮点播（杂音）。
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client_->GetMixFormat(&mix)) && mix) {
                PcmSampleFormat mix_sample{};
                if (!MapWaveFormat(mix, mix_sample)) {
                    CoTaskMemFree(mix);
                    client_->Release();
                    client_ = nullptr;
                    err = "设备 mix format 不是可直接播的线性 PCM";
                    return false;
                }
                hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration,
                                         0, mix, nullptr);
                if (SUCCEEDED(hr)) {
                    actual_.sample_rate = static_cast<int>(mix->nSamplesPerSec);
                    actual_.channels = mix->nChannels;
                    actual_.sample = mix_sample;
                    block_align_ = mix->nBlockAlign;
                    LOG_INFO(
                        "AudioOutput(WASAPI): 设备不接受源格式, 已按设备格式 " + std::to_string(actual_.sample_rate) +
                        " Hz / " + std::to_string(actual_.channels) + " ch / " + std::to_string(mix->wBitsPerSample) +
                        "bit" +
                        (mix_sample == PcmSampleFormat::F32 || mix_sample == PcmSampleFormat::F64 ? " (浮点)" : "") +
                        " 重新打开");
                }
                CoTaskMemFree(mix);
            }
        }
        if (FAILED(hr)) {
            err = "IAudioClient::Initialize 失败 (hr=0x" + [hr] {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
                return std::string(buf);
            }() + ")";
            client_->Release();
            client_ = nullptr;
            return false;
        }

        hr = client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_));
        if (FAILED(hr)) {
            err = "GetService(IAudioRenderClient) 失败";
            return false;
        }

        if (FAILED(client_->GetBufferSize(&buffer_frames_))) {
            err = "GetBufferSize 失败";
            return false;
        }

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            err = "CreateEventW 失败";
            return false;
        }
        if (FAILED(client_->SetEventHandle(event_))) {
            err = "SetEventHandle 失败";
            return false;
        }
        return true;
    }

    void PumpLoop() {
        while (running_.load()) {
            const DWORD wait = WaitForSingleObject(event_, 100);
            if (!running_.load())
                break;
            if (wait == WAIT_TIMEOUT)
                continue;

            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding)))
                continue;
            const UINT32 available = buffer_frames_ - padding;
            if (available == 0)
                continue;

            BYTE* data = nullptr;
            if (FAILED(render_->GetBuffer(available, &data)))
                continue;
            pull_(reinterpret_cast<uint8_t*>(data), static_cast<size_t>(available) * block_align_);
            render_->ReleaseBuffer(available, 0);
        }
    }

    void CleanupDevice() {
        if (client_)
            client_->Stop();
        if (render_) {
            render_->Release();
            render_ = nullptr;
        }
        if (client_) {
            client_->Release();
            client_ = nullptr;
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    PullFn pull_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> ok_{false};
    std::atomic<bool> finished_{false};
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;

    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    HANDLE event_ = nullptr;
    UINT32 buffer_frames_ = 0;
    UINT32 block_align_ = 4;
    PcmFormat want_;   // 请求格式
    PcmFormat actual_; // 设备最终使用的格式
};

#elif defined(VIDEOEYE_AUDIO_MAC)

class AudioQueueBackend final : public AudioOutput::Backend {
public:
    explicit AudioQueueBackend(PullFn pull) : pull_(std::move(pull)) {
    }
    ~AudioQueueBackend() override {
        Close();
    }

    bool Open(const PcmFormat& requested, PcmFormat& actual) override {
        rate_ = requested.sample_rate > 0 ? requested.sample_rate : 44100;
        channels_ = requested.channels > 0 ? requested.channels : 2;
        // AudioQueue 不做格式协商: 要么按请求格式开成功，要么失败
        actual = requested;
        actual.sample_rate = rate_;
        actual.channels = channels_;
        actual.sample = PcmSampleFormat::S16;

        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = static_cast<Float64>(rate_);
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mBytesPerPacket = static_cast<UInt32>(channels_ * 2);
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = static_cast<UInt32>(channels_ * 2);
        fmt.mChannelsPerFrame = static_cast<UInt32>(channels_);
        fmt.mBitsPerChannel = 16;

        if (AudioQueueNewOutput(&fmt, &AudioQueueBackend::Fill, this, nullptr, nullptr, 0, &queue_) != 0) {
            LOG_WARN("AudioOutput(AudioQueue): AudioQueueNewOutput 失败");
            return false;
        }

        constexpr UInt32 kBufferBytes = 8192;
        for (int i = 0; i < kBufferCount; ++i) {
            if (AudioQueueAllocateBuffer(queue_, kBufferBytes, &buffers_[i]) != 0) {
                LOG_WARN("AudioOutput(AudioQueue): AudioQueueAllocateBuffer 失败");
                return false;
            }
            FillBuffer(buffers_[i]);
        }
        return true;
    }

    void Start() override {
        if (queue_)
            AudioQueueStart(queue_, nullptr);
    }

    void Stop() override {
        if (queue_)
            AudioQueuePause(queue_);
    }

    void Close() override {
        if (!queue_)
            return;
        AudioQueueStop(queue_, true);
        AudioQueueDispose(queue_, true);
        queue_ = nullptr;
    }

private:
    static void Fill(void* userdata, AudioQueueRef, AudioQueueBufferRef buffer) {
        auto* self = static_cast<AudioQueueBackend*>(userdata);
        if (self)
            self->FillBuffer(buffer);
    }

    void FillBuffer(AudioQueueBufferRef buffer) {
        if (!buffer)
            return;
        const size_t n = pull_(reinterpret_cast<uint8_t*>(buffer->mAudioData), buffer->mAudioDataBytesCapacity);
        buffer->mAudioDataByteSize = static_cast<UInt32>(n);
        if (queue_)
            AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
    }

    static constexpr int kBufferCount = 3;
    PullFn pull_;
    AudioQueueRef queue_ = nullptr;
    AudioQueueBufferRef buffers_[kBufferCount]{};
    int rate_ = 44100;
    int channels_ = 2;
};

#else

// ALSA 通过 dlopen 载入: 没有 libasound 时只是没有声音, 不影响程序其它功能
class AlsaBackend final : public AudioOutput::Backend {
public:
    explicit AlsaBackend(PullFn pull) : pull_(std::move(pull)) {
    }
    ~AlsaBackend() override {
        Close();
    }

    bool Open(const PcmFormat& requested, PcmFormat& actual) override {
        if (!LoadAlsa()) {
            LOG_WARN("AudioOutput(ALSA): 未找到 libasound.so.2, 音频输出不可用");
            return false;
        }
        rate_ = requested.sample_rate > 0 ? requested.sample_rate : 44100;
        channels_ = requested.channels > 0 ? requested.channels : 2;
        // ALSA 的 "default" 走 plug 层，自己会重采样/混音，所以报回请求格式即可
        actual = requested;
        actual.sample_rate = rate_;
        actual.channels = channels_;
        actual.sample = PcmSampleFormat::S16;
        frame_bytes_ = static_cast<size_t>(channels_) * 2;

        if (snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) != 0) {
            LOG_WARN("AudioOutput(ALSA): snd_pcm_open(default) 失败");
            return false;
        }
        if (snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               static_cast<unsigned>(channels_), static_cast<unsigned>(rate_), 1, 100000) != 0) {
            LOG_WARN("AudioOutput(ALSA): snd_pcm_set_params 失败");
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }
        period_frames_ = 1024;
        running_.store(true);
        thread_ = std::thread([this]() {
            // 异常收口与其它线程入口同一份模板（见上面的 WASAPI 版注释）。
            const std::optional<std::string> failure = task::GuardBody([this]() { PumpLoop(); });
            if (failure) {
                running_.store(false);
                LOG_ERROR(std::string("AudioOutput(ALSA): 输出线程异常退出: ") + *failure);
            }
        });
        return true;
    }

    void Start() override {
        started_.store(true);
    }

    void Stop() override {
        started_.store(false);
        if (pcm_ && snd_pcm_drop)
            snd_pcm_drop(pcm_);
    }

    void Close() override {
        if (!thread_.joinable()) {
            if (pcm_) {
                snd_pcm_close(pcm_);
                pcm_ = nullptr;
            }
            return;
        }
        running_.store(false);
        thread_.join();
        if (pcm_) {
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
    }

private:
    using snd_pcm_t_ = struct _snd_pcm;
    using snd_pcm_sframes_t_ = long;

    bool LoadAlsa() {
        handle_ = dlopen("libasound.so.2", RTLD_LAZY);
        if (!handle_)
            return false;
        snd_pcm_open = reinterpret_cast<int (*)(snd_pcm_t_**, const char*, int, int)>(dlsym(handle_, "snd_pcm_open"));
        snd_pcm_close = reinterpret_cast<int (*)(snd_pcm_t_*)>(dlsym(handle_, "snd_pcm_close"));
        snd_pcm_set_params = reinterpret_cast<int (*)(snd_pcm_t_*, int, int, unsigned, unsigned, int, unsigned)>(
            dlsym(handle_, "snd_pcm_set_params"));
        snd_pcm_writei = reinterpret_cast<snd_pcm_sframes_t_ (*)(snd_pcm_t_*, const void*, unsigned long)>(
            dlsym(handle_, "snd_pcm_writei"));
        snd_pcm_prepare = reinterpret_cast<int (*)(snd_pcm_t_*)>(dlsym(handle_, "snd_pcm_prepare"));
        snd_pcm_drop = reinterpret_cast<int (*)(snd_pcm_t_*)>(dlsym(handle_, "snd_pcm_drop"));
        return snd_pcm_open && snd_pcm_close && snd_pcm_set_params && snd_pcm_writei;
    }

    void PumpLoop() {
        std::vector<uint8_t> buf(period_frames_ * frame_bytes_);
        while (running_.load()) {
            if (!started_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const size_t want = buf.size();
            pull_(buf.data(), want);
            long written = snd_pcm_writei(pcm_, buf.data(), period_frames_);
            if (written < 0) {
                if (snd_pcm_prepare)
                    snd_pcm_prepare(pcm_); // underrun / EPIPE 恢复
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    // ALSA 常量值（与 alsa/pcm.h 一致，避免为几个常量引入头文件依赖）
    static constexpr int SND_PCM_STREAM_PLAYBACK = 0;
    static constexpr int SND_PCM_FORMAT_S16_LE = 2;
    static constexpr int SND_PCM_ACCESS_RW_INTERLEAVED = 3;

    PullFn pull_;
    void* handle_ = nullptr;
    snd_pcm_t_* pcm_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    int rate_ = 44100;
    int channels_ = 2;
    unsigned long period_frames_ = 1024;
    size_t frame_bytes_ = 4;

    int (*snd_pcm_open)(snd_pcm_t_**, const char*, int, int) = nullptr;
    int (*snd_pcm_close)(snd_pcm_t_*) = nullptr;
    int (*snd_pcm_set_params)(snd_pcm_t_*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
    snd_pcm_sframes_t_ (*snd_pcm_writei)(snd_pcm_t_*, const void*, unsigned long) = nullptr;
    int (*snd_pcm_prepare)(snd_pcm_t_*) = nullptr;
    int (*snd_pcm_drop)(snd_pcm_t_*) = nullptr;
};

#endif

// ============================================================
// AudioOutput
// ============================================================
AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    Stop();
}

// 本线程当前正在执行哪个 AudioOutput 的异步打开（没有则为 nullptr）。
//
// 为什么不能用 open_thread_ 自己来判断: OpenAsync 里的
//     open_thread_ = std::thread(body);
// 是**先起线程、后完成 move 赋值**。body 一开跑就可能走到 Open() -> Stop() ->
// OnOpenThread()，那时主线程的赋值还没落地，于是"新线程读 open_thread_"和
// "主线程写 open_thread_"撞成数据竞争（TSan 在 Linux CI 上就抓到了这一处：
// std::thread::get_id() vs std::thread::operator=(std::thread&&)）。
// 而且 joinable() 此时还可能读到 false，"我是打开线程"这件事反而判错。
//
// 改成线程局部标记后，body 完全不碰 open_thread_；析构/Stop() 在主线程那一侧
// 照旧通过 open_thread_ 判断要不要 join，两条路互不干扰。
namespace {
thread_local const AudioOutput* t_open_thread_owner = nullptr;
} // namespace

bool AudioOutput::OnOpenThread() const {
    return t_open_thread_owner == this;
}

size_t AudioOutput::ReadRing(uint8_t* dst, size_t max_bytes) {
    if (!dst || max_bytes == 0)
        return 0;

    size_t copied = 0;
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        if (ring_capacity_ == 0)
            return 0;
        copied = std::min<size_t>(max_bytes, filled_);
        if (copied > 0) {
            const size_t first = std::min<size_t>(copied, ring_capacity_ - read_pos_);
            std::memcpy(dst, ring_.data() + read_pos_, first);
            if (first < copied) {
                std::memcpy(dst + first, ring_.data(), copied - first);
            }
            read_pos_ = (read_pos_ + copied) % ring_capacity_;
            filled_ -= copied;
            ring_cv_.notify_one();
        }
    }

    // 音量缩放（S16 交错）。放在**源格式**这一侧做: 一旦需要转换，出口就是设备的
    // float32/S32 布局，那时再按 int16 去乘会写坏数据。静音补零不受音量影响。
    const double vol = volume_.load();
    if (vol != 1.0 && copied > 0) {
        auto* samples = reinterpret_cast<int16_t*>(dst);
        const size_t n = copied / 2;
        for (size_t i = 0; i < n; ++i) {
            int v = static_cast<int>(static_cast<double>(samples[i]) * vol);
            if (v > 32767)
                v = 32767;
            else if (v < -32768)
                v = -32768;
            samples[i] = static_cast<int16_t>(v);
        }
    }
    return copied;
}

void AudioOutput::FillSilence(uint8_t* dst, size_t bytes) {
    // 8bit 无符号 PCM 的静音是 0x80；其余（S16/S32/浮点）都是全 0。
    std::memset(dst, device_format_.sample == PcmSampleFormat::U8 ? 0x80 : 0x00, bytes);
}

size_t AudioOutput::PullConverted(uint8_t* dst, size_t max_bytes) {
    const size_t src_frame = src_format_.FrameBytes();
    const size_t dst_frame = device_format_.FrameBytes();
    if (src_frame == 0 || dst_frame == 0 || conv_in_.empty())
        return 0;

    // 设备缓冲容量未必是整数帧（AudioQueue 给的是字节容量），余数交给补静音 ——
    // 但输出必须在帧边界上结束，否则下一帧会整体错位半个样本。
    const size_t usable = max_bytes - max_bytes % dst_frame;
    size_t produced = 0;
    bool drained = false;
    while (produced + dst_frame <= usable) {
        // ❗ 每次喂给 swr 的源帧数按**这次能产出多少**反算（且带余数累积），
        // 不能把手上的整块塞进去 —— swr 不丢输入，吃不下的会存进内部 FIFO，
        // 于是每轮多喂一点就一路堆积（实测 2000 次回调后压了 320 万帧 ≈ 65 秒），
        // 表现为声音越来越滞后、环形缓冲永远满着、A/V 越拖越开。
        const size_t out_cap_frames = (usable - produced) / dst_frame;
        const size_t want_frames = converter_.NextFeedFrames(out_cap_frames);
        // ❗ 用 std::min<size_t> 而不是 std::min: 本文件 include 了 windows.h，
        // 它的 min 宏会把 std::min( 里的 '(' 当非法 token。
        const size_t cap_frames = std::min<size_t>(conv_in_.size() / src_frame, want_frames);
        const size_t got_src = cap_frames > 0 ? ReadRing(conv_in_.data(), cap_frames * src_frame) : 0;
        size_t got = 0;
        if (got_src > 0) {
            got = converter_.Convert(conv_in_.data(), got_src, dst + produced, usable - produced);
        } else if (!drained) {
            // 环形缓冲空了（或这轮不该再喂）: 先把 swr 里压着的尾巴（重采样滤波器
            // 延迟）吐干净，之后才允许补静音。
            drained = true;
            got = converter_.Drain(dst + produced, usable - produced);
        } else {
            break; // 环空 + 已冲刷: 剩下的补静音
        }
        if (got > 0) {
            produced += got;
        } else if (got_src == 0) {
            break;
        }
        // got == 0 但还有源数据: swr 在攒够一帧，继续读
    }
    return produced;
}

size_t AudioOutput::Pull(uint8_t* dst, size_t max_bytes) {
    if (!dst || max_bytes == 0)
        return 0;

    const size_t produced = converter_.Active() ? PullConverted(dst, max_bytes) : ReadRing(dst, max_bytes);
    if (produced < max_bytes) {
        FillSilence(dst + produced, max_bytes - produced); // 缓冲空: 补静音
    }
    return max_bytes;
}

void AudioOutput::MarkOpenFinished() {
    open_finished_.store(true);
    ring_cv_.notify_all();
}

bool AudioOutput::Open(int sample_rate, int channels) {
    // ❗ 顺序有讲究: 先把"打开中"亮出来，再走停止流程。StopInternal(false) 不会
    // 把 open_finished_ 翻回 true —— 否则 OpenAsync 起线程前刚置的 false 会被
    // 自己内部的 Stop() 推翻，中间那几微秒里 Enqueue() 会判定"打开已有结论"
    // 而把开头几帧整段丢掉（实测放慢这个窗口后 3/3 复现开局无声）。
    open_finished_.store(false);
    stopping_.store(false);
    StopInternal(false);

    std::lock_guard<std::mutex> lock(device_mutex_);
    PcmFormat requested;
    requested.sample_rate = sample_rate > 0 ? sample_rate : 44100;
    requested.channels = channels > 0 ? channels : 2;
    requested.sample = PcmSampleFormat::S16; // 解码链路（AudioDecoder）恒定产出 S16 交错

    auto pull = [this](uint8_t* dst, size_t n) { return Pull(dst, n); };
    const auto& factory = TestBackendFactory();
    if (factory) {
        backend_ = factory(pull);
        if (!backend_) {
            MarkOpenFinished();
            return false;
        }
    } else {
#if defined(VIDEOEYE_AUDIO_WIN)
        backend_ = std::make_unique<WasapiBackend>(pull);
#elif defined(VIDEOEYE_AUDIO_MAC)
        backend_ = std::make_unique<AudioQueueBackend>(pull);
#else
        backend_ = std::make_unique<AlsaBackend>(pull);
#endif
    }

    PcmFormat actual = requested;
    {
        VE_PERF("音频设备打开");
        if (!backend_->Open(requested, actual)) {
            backend_.reset();
            LOG_WARN("AudioOutput: 打开音频设备失败, 本次播放无声音");
            MarkOpenFinished();
            return false;
        }
    }

    src_format_ = requested;
    device_format_ = actual;

    // 设备格式与源格式不一致时（典型: Windows 共享模式只给 48 kHz / float32），
    // 必须在这里补一层转换。不做的话 44.1 kHz 素材会变调、S16 字节喂给 float32
    // 设备直接是杂音或静音。
    const bool need_convert = (src_format_ != device_format_);
    if (!converter_.Configure(src_format_.sample_rate, src_format_.channels, device_format_.sample_rate,
                              device_format_.channels, device_format_.sample) ||
        (need_convert && !converter_.Active())) {
        LOG_ERROR("AudioOutput: 源格式与设备格式不一致但转换器初始化失败, 放弃本次播放 (避免杂音)");
        backend_->Close();
        backend_.reset();
        MarkOpenFinished();
        return false;
    }
    if (need_convert) {
        LOG_INFO("AudioOutput: 源格式 " + std::to_string(src_format_.sample_rate) + " Hz / " +
                 std::to_string(src_format_.channels) + " ch 与设备格式不一致, 已启用重采样");
    }

    // 转换时的源字节暂存: 一次进一环，源帧对齐
    const size_t src_frame_bytes = static_cast<size_t>(src_format_.FrameBytes());
    const size_t scratch =
        (src_frame_bytes > 0) ? std::max<size_t>(src_frame_bytes, 8192 / src_frame_bytes * src_frame_bytes) : 0;
    conv_in_.assign(scratch, 0);

    // 约 400 ms 的缓冲：够抗抖动，又不会让 seek 后的残留声音拖太久。
    // 容量按**源**格式算: 环形缓冲里装的是解码器产出的字节。
    ring_capacity_ =
        static_cast<size_t>(src_format_.sample_rate) * static_cast<size_t>(src_format_.channels) * 2 * 2 / 5;
    ring_.assign(ring_capacity_, 0);
    write_pos_ = read_pos_ = filled_ = 0;

    opened_.store(true);
    MarkOpenFinished(); // 唤醒可能在 Enqueue() 里等设备就绪的解码线程
    LOG_INFO("AudioOutput: 音频设备已打开 " + std::to_string(device_format_.sample_rate) + " Hz / " +
             std::to_string(device_format_.channels) + " ch");
    if (pending_play_.exchange(false)) {
        backend_->Start();
    }
    return true;
}

void AudioOutput::OpenAsync(int sample_rate, int channels) {
    // 先判"我是不是打开线程"再碰 open_thread_: 反过来写会在上面那个窗口里
    // 读到正在被赋值的 open_thread_（见 OnOpenThread 处的说明）。
    if (!OnOpenThread() && open_thread_.joinable()) {
        open_thread_.join(); // 上一轮还没开完就先收尾
    }
    stopping_.store(false);
    // ❗ 必须在起线程之前就把"打开中"标上。Open() 自己也置了一次，但那是新线程
    // 跑起来之后的事：这中间的窗口里 Enqueue() 会看到一个"上次已结束"的
    // open_finished_=true，判定设备永远不会就绪，直接把开头几帧音频扔掉 ——
    // 正是 P1「设备未就绪时丢弃已解码音频」的残留形态。
    open_finished_.store(false);
    open_thread_ = std::thread([this, sample_rate, channels]() {
        t_open_thread_owner = this;
        const auto clear_owner = task::MakeScopeExit([]() { t_open_thread_owner = nullptr; });
        // 见 ThreadMain 那处的注释: 与其它线程入口共用同一份异常收口模板。
        // 音频设备打不开最多是没声音，不能把播放器一起带走。
        const std::optional<std::string> failure = task::GuardBody([&]() {
            const bool ok = Open(sample_rate, channels);
            if (!ok)
                pending_play_.store(false);
        });
        if (failure) {
            LOG_ERROR(std::string("AudioOutput: 异步打开音频设备异常: ") + *failure);
            pending_play_.store(false);
            MarkOpenFinished();
        }
    });
}

void AudioOutput::Enqueue(const uint8_t* data, int len) {
    if (!data || len <= 0)
        return;

    // 设备还在异步打开（Windows 首次打开实测 ~1 s）。这时候**不能直接丢**：
    // 解码线程不会等设备，短音频在设备就绪前就被整段扔掉 —— 表现出来就是
    // "点了播放却没声音"。只等"正在打开"的那段时间；打开已有结论（失败/已停止）
    // 就立刻放弃，绝不为此卡住解码线程。
    if (!opened_.load()) {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, kDeviceOpenWait, [this]() { return opened_.load() || open_finished_.load(); });
        if (!opened_.load())
            return;
    }

    // 分片写入。旧实现先等「整个 len 都能放下」再写，一旦单次 PCM 块大于
    // ring_capacity_（高采样率 / 多声道 / 大 AAC 帧解码出来很容易超），
    // filled_ + len 永远大于容量，wait 就再也醒不过来 —— 解码线程直接卡死。
    // 改成「有多少空位写多少」，每次至少推进 1 字节，循环必然收敛。
    size_t remaining = static_cast<size_t>(len);
    const uint8_t* src = data;
    while (remaining > 0) {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait(lock, [this]() { return !opened_.load() || filled_ < ring_capacity_; });
        if (!opened_.load())
            return; // 已被 Stop

        const size_t space = ring_capacity_ - filled_;
        const size_t chunk = std::min<size_t>(remaining, space);
        const size_t first = std::min<size_t>(chunk, ring_capacity_ - write_pos_);
        std::memcpy(ring_.data() + write_pos_, src, first);
        if (first < chunk) {
            std::memcpy(ring_.data(), src + first, chunk - first);
        }
        write_pos_ = (write_pos_ + chunk) % ring_capacity_;
        filled_ += chunk;
        remaining -= chunk;
        src += chunk;
    }
}

void AudioOutput::Play() {
    if (!opened_.load()) {
        pending_play_.store(true); // 设备就绪后由打开流程补一次
        return;
    }
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (backend_)
        backend_->Start();
}

void AudioOutput::Pause() {
    pending_play_.store(false);
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (backend_)
        backend_->Stop();
}

void AudioOutput::Clear() {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    write_pos_ = read_pos_ = filled_ = 0;
    ring_cv_.notify_all();
}

void AudioOutput::SetVolume(double volume) {
    if (volume < 0.0)
        volume = 0.0;
    if (volume > 1.0)
        volume = 1.0;
    volume_.store(volume);
}

int64_t AudioOutput::GetConverterDelayFrames() const {
    // 只在音频线程 / 单线程诊断里读；与 GetBufferedMs 一样不持 device_mutex_。
    return converter_.PendingOutputFrames();
}

int AudioOutput::GetBufferedMs(int sample_rate, int channels) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (sample_rate <= 0 || channels <= 0)
        return 0;
    return static_cast<int>(filled_ * 1000 / (sample_rate * channels * 2));
}

void AudioOutput::Stop() {
    StopInternal(true);
}

void AudioOutput::StopInternal(bool notify_open_waiters) {
    pending_play_.store(false);
    stopping_.store(true);

    // 不能 join 自己：Open() 是在 open_thread_ 上执行的（OpenAsync 路径），
    // self-join 会抛 std::system_error 并让整个进程 terminate。
    // 判定顺序同样必须是"先排除自己"再读 open_thread_（见 OnOpenThread 处的说明）。
    if (!OnOpenThread() && open_thread_.joinable()) {
        open_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        opened_.store(false);
        if (backend_) {
            backend_->Close(); // 内部 join 音频线程, 之后才能安全释放转换器
            backend_.reset();
        }
        // 转换器只在音频线程里用（Pull），线程停了才能安全释放
        converter_.Reset();
    }

    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        filled_ = write_pos_ = read_pos_ = 0;
        ring_.clear();
        ring_capacity_ = 0;
    }
    // 主动停止才宣布"打开流程已有结论"；Open() 内部的重置不算（见 Open 处说明）。
    if (notify_open_waiters)
        open_finished_.store(true);
    ring_cv_.notify_all(); // 唤醒可能在 Enqueue 中阻塞的解码线程
}

} // namespace player
} // namespace videoeye
