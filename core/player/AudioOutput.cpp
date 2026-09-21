#include "AudioOutput.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <string>

#include "utils/Logger.h"
#include "utils/ScopedTimer.h"

// ============================================================
// 平台后端
// ============================================================
#if defined(Q_OS_WIN) || defined(_WIN32)
// ---------------- Windows: WASAPI (共享模式, 事件驱动) ----------------
#define VIDEOEYE_AUDIO_WIN 1
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
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

#if defined(VIDEOEYE_AUDIO_WIN)

// KSDATAFORMAT_SUBTYPE_PCM / KSDATAFORMAT_SUBTYPE_IEEE_FLOAT。
// 直接把 GUID 写死，省得为两个常量引入 ksmedia.h（WDK 头）。
static const GUID kSubtypePcm =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// 设备的 mix format 是不是 16bit PCM。
// 只有这一种情况可以直接拿 mix format 去 Initialize —— 它的字节布局和
// Pull() 产出的 S16 交错 PCM 完全一致，不需要任何转换。
bool IsS16Pcm(const WAVEFORMATEX* wfx) {
    if (wfx == nullptr || wfx->wBitsPerSample != 16) return false;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return IsEqualGUID(ext->SubFormat, kSubtypePcm) != FALSE;
    }
    return false;
}

class WasapiBackend final : public AudioOutput::Backend {
public:
    explicit WasapiBackend(PullFn pull) : pull_(std::move(pull)) {}
    ~WasapiBackend() override { Close(); }

    bool Open(int& sample_rate, int& channels) override {
        want_rate_ = sample_rate > 0 ? sample_rate : 44100;
        want_channels_ = channels > 0 ? channels : 2;
        running_.store(true);
        finished_.store(false);
        thread_ = std::thread([this]() { ThreadMain(); });

        // 等待后台线程完成设备初始化（含 COM 初始化，必须在同一线程内配对）
        std::unique_lock<std::mutex> lock(ready_mutex_);
        ready_cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return ready_.load(); });
        if (!ready_.load()) {
            running_.store(false);
            SetEvent(event_);
            if (thread_.joinable()) thread_.join();
            return false;
        }
        if (!ok_.load()) {
            running_.store(false);
            SetEvent(event_);
            if (thread_.joinable()) thread_.join();
            return false;
        }
        sample_rate = actual_rate_;
        channels = actual_channels_;
        return true;
    }

    void Start() override {
        if (client_) client_->Start();
    }

    void Stop() override {
        if (client_) client_->Stop();
    }

    void Close() override {
        if (!thread_.joinable()) return;
        running_.store(false);
        if (event_) SetEvent(event_);
        thread_.join();
    }

private:
    void ThreadMain() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

        bool ok = false;
        std::string err;
        if (com_ok) ok = InitDevice(err);

        // 通知 Open() 结果；失败也要通知，否则调用方会卡满 5 s
        actual_rate_ = want_rate_;
        actual_channels_ = want_channels_;
        ok_.store(ok);
        ready_.store(true);
        ready_cv_.notify_all();

        if (!ok) {
            if (com_ok) CoUninitialize();
            finished_.store(true);
            LOG_WARN("AudioOutput(WASAPI): 打开音频设备失败" + (err.empty() ? std::string() : (": " + err)));
            return;
        }

        PumpLoop();
        CleanupDevice();
        if (com_ok) CoUninitialize();
        finished_.store(true);
    }

    bool InitDevice(std::string& err) {
        constexpr REFERENCE_TIME kBufferDuration = 2000000;  // 200 ms (100ns 单位)

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) { err = "CoCreateInstance(MMDeviceEnumerator) 失败"; return false; }

        IMMDevice* device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(hr)) { err = "GetDefaultAudioEndpoint 失败 (无声卡?)"; return false; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client_));
        device->Release();
        if (FAILED(hr)) { err = "Activate(IAudioClient) 失败"; return false; }

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = static_cast<WORD>(want_channels_);
        wfx.nSamplesPerSec = static_cast<DWORD>(want_rate_);
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = static_cast<WORD>(want_channels_ * 2);
        wfx.nAvgBytesPerSec = static_cast<DWORD>(want_rate_ * want_channels_ * 2);
        wfx.cbSize = 0;
        block_align_ = wfx.nBlockAlign;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 kBufferDuration, 0, &wfx, nullptr);
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            // 设备不接受「16bit PCM + 源的采样率」。常见原因是声卡跑在 48 kHz 而我们按
            // 源的 44.1 kHz 请求。
            //
            // 关键约束: Pull() 只产出 S16 交错 PCM。所以回退时**不能**直接把设备 mix
            // format 拿来 Initialize —— mix format 常常是 float32，那样会把 S16 的字节流
            // 当浮点播，表现为杂音或静音。先只对齐采样率/声道数、保持 S16。
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client_->GetMixFormat(&mix)) && mix) {
                WAVEFORMATEX retry{};
                retry.wFormatTag = WAVE_FORMAT_PCM;
                retry.nChannels = mix->nChannels;
                retry.nSamplesPerSec = mix->nSamplesPerSec;
                retry.wBitsPerSample = 16;
                retry.nBlockAlign = static_cast<WORD>(mix->nChannels * 2);
                retry.nAvgBytesPerSec = retry.nSamplesPerSec * retry.nBlockAlign;
                retry.cbSize = 0;
                hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         kBufferDuration, 0, &retry, nullptr);
                if (SUCCEEDED(hr)) {
                    actual_rate_ = static_cast<int>(retry.nSamplesPerSec);
                    actual_channels_ = retry.nChannels;
                    block_align_ = retry.nBlockAlign;
                    LOG_INFO("AudioOutput(WASAPI): 16bit PCM 未受支持, 已按设备采样率 " +
                             std::to_string(actual_rate_) + " Hz / " +
                             std::to_string(actual_channels_) + " ch 重新打开");
                } else if (IsS16Pcm(mix)) {
                    // 兜底: mix format 本身就是 16bit PCM，字节布局与 Pull() 一致，可以直用
                    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             kBufferDuration, 0, mix, nullptr);
                    if (SUCCEEDED(hr)) {
                        actual_rate_ = static_cast<int>(mix->nSamplesPerSec);
                        actual_channels_ = mix->nChannels;
                        block_align_ = mix->nBlockAlign;
                    }
                }
                // 其余情况（mix 是 float32 / 24bit ...）宁可没有声音，也不要把 S16 当别的格式播
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
        if (FAILED(hr)) { err = "GetService(IAudioRenderClient) 失败"; return false; }

        if (FAILED(client_->GetBufferSize(&buffer_frames_))) { err = "GetBufferSize 失败"; return false; }

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) { err = "CreateEventW 失败"; return false; }
        if (FAILED(client_->SetEventHandle(event_))) { err = "SetEventHandle 失败"; return false; }
        return true;
    }

    void PumpLoop() {
        while (running_.load()) {
            const DWORD wait = WaitForSingleObject(event_, 100);
            if (!running_.load()) break;
            if (wait == WAIT_TIMEOUT) continue;

            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) continue;
            const UINT32 available = buffer_frames_ - padding;
            if (available == 0) continue;

            BYTE* data = nullptr;
            if (FAILED(render_->GetBuffer(available, &data))) continue;
            pull_(reinterpret_cast<uint8_t*>(data), static_cast<size_t>(available) * block_align_);
            render_->ReleaseBuffer(available, 0);
        }
    }

    void CleanupDevice() {
        if (client_) client_->Stop();
        if (render_) { render_->Release(); render_ = nullptr; }
        if (client_) { client_->Release(); client_ = nullptr; }
        if (event_) { CloseHandle(event_); event_ = nullptr; }
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
    int want_rate_ = 44100;
    int want_channels_ = 2;
    int actual_rate_ = 44100;
    int actual_channels_ = 2;
};

#elif defined(VIDEOEYE_AUDIO_MAC)

class AudioQueueBackend final : public AudioOutput::Backend {
public:
    explicit AudioQueueBackend(PullFn pull) : pull_(std::move(pull)) {}
    ~AudioQueueBackend() override { Close(); }

    bool Open(int& sample_rate, int& channels) override {
        rate_ = sample_rate > 0 ? sample_rate : 44100;
        channels_ = channels > 0 ? channels : 2;

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
        if (queue_) AudioQueueStart(queue_, nullptr);
    }

    void Stop() override {
        if (queue_) AudioQueuePause(queue_);
    }

    void Close() override {
        if (!queue_) return;
        AudioQueueStop(queue_, true);
        AudioQueueDispose(queue_, true);
        queue_ = nullptr;
    }

private:
    static void Fill(void* userdata, AudioQueueRef, AudioQueueBufferRef buffer) {
        auto* self = static_cast<AudioQueueBackend*>(userdata);
        if (self) self->FillBuffer(buffer);
    }

    void FillBuffer(AudioQueueBufferRef buffer) {
        if (!buffer) return;
        const size_t n = pull_(reinterpret_cast<uint8_t*>(buffer->mAudioData), buffer->mAudioDataBytesCapacity);
        buffer->mAudioDataByteSize = static_cast<UInt32>(n);
        if (queue_) AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
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
    explicit AlsaBackend(PullFn pull) : pull_(std::move(pull)) {}
    ~AlsaBackend() override { Close(); }

    bool Open(int& sample_rate, int& channels) override {
        if (!LoadAlsa()) {
            LOG_WARN("AudioOutput(ALSA): 未找到 libasound.so.2, 音频输出不可用");
            return false;
        }
        rate_ = sample_rate > 0 ? sample_rate : 44100;
        channels_ = channels > 0 ? channels : 2;
        frame_bytes_ = static_cast<size_t>(channels_) * 2;

        if (snd_pcm_open(&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) != 0) {
            LOG_WARN("AudioOutput(ALSA): snd_pcm_open(default) 失败");
            return false;
        }
        if (snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               static_cast<unsigned>(channels_), static_cast<unsigned>(rate_),
                               1, 100000) != 0) {
            LOG_WARN("AudioOutput(ALSA): snd_pcm_set_params 失败");
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }
        period_frames_ = 1024;
        running_.store(true);
        thread_ = std::thread([this]() { PumpLoop(); });
        return true;
    }

    void Start() override { started_.store(true); }

    void Stop() override {
        started_.store(false);
        if (pcm_ && snd_pcm_drop) snd_pcm_drop(pcm_);
    }

    void Close() override {
        if (!thread_.joinable()) {
            if (pcm_) { snd_pcm_close(pcm_); pcm_ = nullptr; }
            return;
        }
        running_.store(false);
        thread_.join();
        if (pcm_) { snd_pcm_close(pcm_); pcm_ = nullptr; }
    }

private:
    using snd_pcm_t_ = struct _snd_pcm;
    using snd_pcm_sframes_t_ = long;

    bool LoadAlsa() {
        handle_ = dlopen("libasound.so.2", RTLD_LAZY);
        if (!handle_) return false;
        snd_pcm_open = reinterpret_cast<int (*)(snd_pcm_t_**, const char*, int, int)>(
            dlsym(handle_, "snd_pcm_open"));
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
                if (snd_pcm_prepare) snd_pcm_prepare(pcm_);   // underrun / EPIPE 恢复
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

bool AudioOutput::OnOpenThread() const {
    return open_thread_.joinable() &&
           open_thread_.get_id() == std::this_thread::get_id();
}

size_t AudioOutput::Pull(uint8_t* dst, size_t max_bytes) {
    if (!dst || max_bytes == 0) return 0;

    size_t copied = 0;
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
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
    if (copied < max_bytes) {
        std::memset(dst + copied, 0, max_bytes - copied);   // 缓冲空: 补静音
    }

    // 音量缩放（S16 交错）
    const double vol = volume_.load();
    if (vol != 1.0) {
        auto* samples = reinterpret_cast<int16_t*>(dst);
        const size_t n = max_bytes / 2;
        for (size_t i = 0; i < n; ++i) {
            int v = static_cast<int>(static_cast<double>(samples[i]) * vol);
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            samples[i] = static_cast<int16_t>(v);
        }
    }
    return max_bytes;
}

bool AudioOutput::Open(int sample_rate, int channels) {
    Stop();
    stopping_.store(false);

    std::lock_guard<std::mutex> lock(device_mutex_);
    const int want_rate = sample_rate > 0 ? sample_rate : 44100;
    const int want_channels = channels > 0 ? channels : 2;

    auto pull = [this](uint8_t* dst, size_t n) { return Pull(dst, n); };
#if defined(VIDEOEYE_AUDIO_WIN)
    backend_ = std::make_unique<WasapiBackend>(pull);
#elif defined(VIDEOEYE_AUDIO_MAC)
    backend_ = std::make_unique<AudioQueueBackend>(pull);
#else
    backend_ = std::make_unique<AlsaBackend>(pull);
#endif

    int actual_rate = want_rate;
    int actual_channels = want_channels;
    {
        VE_PERF("音频设备打开");
        if (!backend_->Open(actual_rate, actual_channels)) {
            backend_.reset();
            LOG_WARN("AudioOutput: 打开音频设备失败, 本次播放无声音");
            return false;
        }
    }

    sample_rate_ = actual_rate;
    channels_ = actual_channels;

    // 约 400 ms 的缓冲：够抗抖动，又不会让 seek 后的残留声音拖太久
    ring_capacity_ = static_cast<size_t>(actual_rate) * static_cast<size_t>(actual_channels) * 2 * 2 / 5;
    ring_.assign(ring_capacity_, 0);
    write_pos_ = read_pos_ = filled_ = 0;

    opened_.store(true);
    LOG_INFO("AudioOutput: 音频设备已打开 " + std::to_string(actual_rate) + " Hz / " +
             std::to_string(actual_channels) + " ch");
    if (pending_play_.exchange(false)) {
        backend_->Start();
    }
    return true;
}

void AudioOutput::OpenAsync(int sample_rate, int channels) {
    if (open_thread_.joinable() && !OnOpenThread()) {
        open_thread_.join();   // 上一轮还没开完就先收尾
    }
    stopping_.store(false);
    open_thread_ = std::thread([this, sample_rate, channels]() {
        // std::thread 的入口在 MSVC STL 里是 noexcept 的：这里任何异常逃逸都会
        // 直接 std::terminate()（表现为整个进程闪退，异常码 0xC0000409）。
        // 音频设备打不开最多是没声音，不能把播放器一起带走，所以兜底捕获。
        try {
            const bool ok = Open(sample_rate, channels);
            if (!ok) pending_play_.store(false);
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("AudioOutput: 异步打开音频设备异常: ") + e.what());
            pending_play_.store(false);
        } catch (...) {
            LOG_ERROR("AudioOutput: 异步打开音频设备发生未知异常");
            pending_play_.store(false);
        }
    });
}

void AudioOutput::Enqueue(const uint8_t* data, int len) {
    if (!opened_.load() || !data || len <= 0) return;   // 异步打开未完成: 丢弃

    // 分片写入。旧实现先等「整个 len 都能放下」再写，一旦单次 PCM 块大于
    // ring_capacity_（高采样率 / 多声道 / 大 AAC 帧解码出来很容易超），
    // filled_ + len 永远大于容量，wait 就再也醒不过来 —— 解码线程直接卡死。
    // 改成「有多少空位写多少」，每次至少推进 1 字节，循环必然收敛。
    size_t remaining = static_cast<size_t>(len);
    const uint8_t* src = data;
    while (remaining > 0) {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait(lock, [this]() {
            return !opened_.load() || filled_ < ring_capacity_;
        });
        if (!opened_.load()) return;   // 已被 Stop

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
        pending_play_.store(true);   // 设备就绪后由打开流程补一次
        return;
    }
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (backend_) backend_->Start();
}

void AudioOutput::Pause() {
    pending_play_.store(false);
    std::lock_guard<std::mutex> lock(device_mutex_);
    if (backend_) backend_->Stop();
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
    stopping_.store(true);

    // 不能 join 自己：Open() 是在 open_thread_ 上执行的（OpenAsync 路径），
    // self-join 会抛 std::system_error 并让整个进程 terminate。
    if (open_thread_.joinable() && !OnOpenThread()) {
        open_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        opened_.store(false);
        if (backend_) {
            backend_->Close();
            backend_.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        filled_ = write_pos_ = read_pos_ = 0;
        ring_.clear();
        ring_capacity_ = 0;
    }
    ring_cv_.notify_all();   // 唤醒可能在 Enqueue 中阻塞的解码线程
}

} // namespace player
} // namespace videoeye
