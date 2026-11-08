// 音频设备层长稳 / 格式回退回归（不需要真声卡）。
//
// 背景: test_s16_converter.cpp 只钉住了「这一轮该喂多少源帧」的算术，而事故发生在
// AudioOutput::PullConverted 这条真实链路上 —— swr_convert 不丢输入，每轮多喂一点
// 就会把吃不下的样本压进内部 FIFO，实测 2000 次回调后压了 320 万帧（约 65 秒），
// 听感是声音越来越滞后、环形缓冲永远满、A/V 越拖越开。算术对了但调用链错了，
// 纯转换器测试一条都不会红。
//
// 这里用注入的假后端直接驱动 AudioOutput::Pull()：设备格式由用例说了算，声卡回调
// 由用例主动发起，于是「Windows 共享模式只给 48 kHz / float32」「连续回调是否积压」
// 「异步打开期间的音频会不会被丢」都能在离屏环境里稳定复现并量化。
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "core/player/AudioOutput.h"

namespace {

using videoeye::player::AudioOutput;
using videoeye::player::PcmBytesPerSample;
using videoeye::player::PcmSampleFormat;
using PcmFormat = AudioOutput::PcmFormat;

constexpr int kSrcRate = 44100;
constexpr int kSrcChannels = 2;
constexpr size_t kSrcFrameBytes = kSrcChannels * sizeof(int16_t);
constexpr size_t kCallbacks = 2000;
// 512 目标帧是刻意的: 44.1k→48k 时 512*44100/48000 = 470.4 源帧，逐轮 ceil / floor
// 都会产生系统性偏差。若换成刚好整除的 480 帧，这两种退化就测不出来了。
constexpr size_t kDeviceFrames = 512;
constexpr size_t kFeedFrames = 470;
constexpr int kTargetBufferedMs = 100; // ring 容量 400 ms，留足余量，Enqueue 不会阻塞

PcmFormat Src() {
    PcmFormat f;
    f.sample_rate = kSrcRate;
    f.channels = kSrcChannels;
    f.sample = PcmSampleFormat::S16;
    return f;
}

PcmFormat MakeFormat(int rate, int channels, PcmSampleFormat sample) {
    PcmFormat f;
    f.sample_rate = rate;
    f.channels = channels;
    f.sample = sample;
    return f;
}

size_t FrameBytes(const PcmFormat& f) {
    return static_cast<size_t>(f.channels) * static_cast<size_t>(PcmBytesPerSample(f.sample));
}

// 静音在 U8 是 0x80，其余格式是全 0。整帧逐字节比，避免把「非零但其实是静音」
// 或「S16 常量刚好低位为 0」误判。
bool FrameIsSilence(const uint8_t* p, size_t frame_bytes, PcmSampleFormat sample) {
    const uint8_t silence = sample == PcmSampleFormat::U8 ? 0x80 : 0x00;
    for (size_t i = 0; i < frame_bytes; ++i) {
        if (p[i] != silence)
            return false;
    }
    return true;
}

size_t CountSilentFrames(const std::vector<uint8_t>& bytes, size_t frame_bytes, PcmSampleFormat sample) {
    size_t silent = 0;
    for (size_t off = 0; off + frame_bytes <= bytes.size(); off += frame_bytes) {
        if (FrameIsSilence(bytes.data() + off, frame_bytes, sample))
            ++silent;
    }
    return silent;
}

// 常量 0x4000（约 0.25 FS）。任何格式下都不会与静音字节混淆。
std::vector<uint8_t> MakeSourcePcm(size_t frames) {
    std::vector<uint8_t> pcm(frames * kSrcFrameBytes, 0);
    auto* samples = reinterpret_cast<int16_t*>(pcm.data());
    for (size_t i = 0; i < frames * kSrcChannels; ++i)
        samples[i] = 0x4000;
    return pcm;
}

// 假后端: 不碰系统音频，只把「设备实际格式」回填给 Open()，并把拉取回调交给测试。
class FakeBackend final : public AudioOutput::Backend {
public:
    FakeBackend(PcmFormat actual, AudioOutput::PullFn pull, int open_delay_ms = 0)
        : actual_(actual), pull_(std::move(pull)), open_delay_ms_(open_delay_ms) {
    }

    bool Open(const PcmFormat& requested, PcmFormat& actual) override {
        requested_ = requested;
        if (open_delay_ms_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(open_delay_ms_));
        actual = actual_;
        ++open_count_;
        return true;
    }
    void Start() override {
    }
    void Stop() override {
    }
    void Close() override {
        ++close_count_;
    }

    size_t Pull(uint8_t* dst, size_t n) {
        return pull_(dst, n);
    }

    const PcmFormat& requested() const {
        return requested_;
    }
    int open_count() const {
        return open_count_;
    }
    int close_count() const {
        return close_count_;
    }

private:
    PcmFormat actual_;
    AudioOutput::PullFn pull_;
    int open_delay_ms_;
    PcmFormat requested_;
    int open_count_ = 0;
    int close_count_ = 0;
};

// 工厂是进程级静态，用例结束必须复位，否则会污染其它测试 / 真实播放。
class BackendInjection {
public:
    explicit BackendInjection(AudioOutput::BackendFactory factory) {
        AudioOutput::SetBackendFactoryForTesting(std::move(factory));
    }
    ~BackendInjection() {
        AudioOutput::SetBackendFactoryForTesting({});
    }
    BackendInjection(const BackendInjection&) = delete;
    BackendInjection& operator=(const BackendInjection&) = delete;
};

// 把 ring 补到目标水位。按消费量补，不会像「每轮固定多喂」那样把 ring 撑满后
// 在 Enqueue 的背压里自锁（拉取和喂入在同一个线程）。
size_t TopUpRing(AudioOutput& output, const std::vector<uint8_t>& chunk, size_t frames_per_chunk) {
    size_t fed = 0;
    for (int guard = 0; guard < 200 && output.GetBufferedMs(kSrcRate, kSrcChannels) < kTargetBufferedMs; ++guard) {
        output.Enqueue(chunk.data(), static_cast<int>(chunk.size()));
        fed += frames_per_chunk;
    }
    return fed;
}

// 统计器自证: 长稳用例全靠它区分「真实 PCM」和「补静音」，统计器自己失灵的话
// 后面所有断言都会变成假绿。
TEST(AudioOutputStabilityTest, SilenceDetectorIsDiscriminative) {
    const std::vector<uint8_t> zero(8, 0x00);
    const std::vector<uint8_t> u8_silence(8, 0x80);
    std::vector<uint8_t> tone(8, 0x00);
    tone[1] = 0x40; // 一个 S16 0x4000 样本

    EXPECT_TRUE(FrameIsSilence(zero.data(), zero.size(), PcmSampleFormat::S16));
    EXPECT_TRUE(FrameIsSilence(zero.data(), zero.size(), PcmSampleFormat::F32));
    EXPECT_FALSE(FrameIsSilence(u8_silence.data(), u8_silence.size(), PcmSampleFormat::S16));
    EXPECT_TRUE(FrameIsSilence(u8_silence.data(), u8_silence.size(), PcmSampleFormat::U8));
    EXPECT_FALSE(FrameIsSilence(tone.data(), tone.size(), PcmSampleFormat::S16));
    EXPECT_EQ(2u, CountSilentFrames(std::vector<uint8_t>(16, 0), 8, PcmSampleFormat::S16));
}

// 设备格式回退矩阵: 请求恒为 44.1 kHz / 2ch / S16，设备可以给任意 mix format。
// 每组都必须真的拉出可听 PCM —— 只断言 Open 成功是典型的「做了才有用、坏了看不出来」。
TEST(AudioOutputStabilityTest, DeviceFormatFallbackMatrixProducesAudiblePcm) {
    struct Case {
        const char* name;
        PcmFormat actual;
    };
    const std::vector<Case> cases = {
        {"win_shared_float32_48k", MakeFormat(48000, 2, PcmSampleFormat::F32)},
        {"upsampled_s16_48k", MakeFormat(48000, 2, PcmSampleFormat::S16)},
        {"s32_wide", MakeFormat(44100, 2, PcmSampleFormat::S32)},
        {"u8_device", MakeFormat(48000, 2, PcmSampleFormat::U8)},
        {"f64_device", MakeFormat(48000, 2, PcmSampleFormat::F64)},
        {"downmix_mono", MakeFormat(48000, 1, PcmSampleFormat::S16)},
        {"upmix_5_1", MakeFormat(48000, 6, PcmSampleFormat::S16)},
        {"downsample_22k", MakeFormat(22050, 2, PcmSampleFormat::F32)},
        {"passthrough", Src()},
    };
    EXPECT_GE(cases.size(), 9u) << "回退矩阵不许悄悄缩水";

    const std::vector<uint8_t> pcm = MakeSourcePcm(4000);
    size_t driven = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        FakeBackend* backend = nullptr;
        const PcmFormat actual = c.actual;
        BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
            auto b = std::make_unique<FakeBackend>(actual, std::move(pull));
            backend = b.get();
            return b;
        });

        AudioOutput output;
        ASSERT_TRUE(output.Open(kSrcRate, kSrcChannels)) << c.name;
        ASSERT_NE(nullptr, backend);
        EXPECT_EQ(Src(), backend->requested()) << "请求格式必须是解码器产出的 S16 源格式";

        output.Enqueue(pcm.data(), static_cast<int>(pcm.size()));
        const size_t frame_bytes = FrameBytes(actual);
        std::vector<uint8_t> out(kDeviceFrames * frame_bytes, 0);
        ASSERT_EQ(out.size(), backend->Pull(out.data(), out.size()));

        const size_t silent = CountSilentFrames(out, frame_bytes, actual.sample);
        EXPECT_LT(silent, kDeviceFrames / 10) << c.name << ": 拉出来的几乎是静音（S16 字节喂错格式 / 重采样没出数）";
        EXPECT_LT(output.GetConverterDelayFrames(), 128) << c.name << ": 首轮就积压";
        ++driven;
    }
    EXPECT_EQ(cases.size(), driven) << "每组都必须真的驱动过一次声卡回调";
}

// 格式完全一致时不建重采样器，内部延迟恒 0。
TEST(AudioOutputStabilityTest, PassthroughHasNoConverterBacklog) {
    FakeBackend* backend = nullptr;
    BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
        auto b = std::make_unique<FakeBackend>(Src(), std::move(pull));
        backend = b.get();
        return b;
    });
    AudioOutput output;
    ASSERT_TRUE(output.Open(kSrcRate, kSrcChannels));

    const std::vector<uint8_t> pcm = MakeSourcePcm(2000);
    output.Enqueue(pcm.data(), static_cast<int>(pcm.size()));
    std::vector<uint8_t> out(kDeviceFrames * kSrcFrameBytes, 0);
    ASSERT_EQ(out.size(), backend->Pull(out.data(), out.size()));

    EXPECT_EQ(0, output.GetConverterDelayFrames());
    EXPECT_EQ(0u, CountSilentFrames(out, kSrcFrameBytes, PcmSampleFormat::S16));
}

// 欠载（设备回调快于解码）时必须按设备格式补静音，供给恢复后不能带着
// 假延迟继续播。U8 的静音是 0x80 而不是 0，写错了就是持续的直流噪声。
TEST(AudioOutputStabilityTest, UnderrunSilenceMatchesDeviceFormatAndRecovers) {
    struct Case {
        const char* name;
        PcmFormat actual;
    };
    const std::vector<Case> cases = {
        {"float32", MakeFormat(48000, 2, PcmSampleFormat::F32)},
        {"u8", MakeFormat(48000, 2, PcmSampleFormat::U8)},
        {"s16", MakeFormat(48000, 2, PcmSampleFormat::S16)},
    };
    EXPECT_EQ(3u, cases.size()) << "欠载矩阵不许悄悄缩水";

    size_t driven = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        FakeBackend* backend = nullptr;
        const PcmFormat actual = c.actual;
        BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
            auto b = std::make_unique<FakeBackend>(actual, std::move(pull));
            backend = b.get();
            return b;
        });
        AudioOutput output;
        ASSERT_TRUE(output.Open(kSrcRate, kSrcChannels));
        ASSERT_NE(nullptr, backend);

        const size_t frame_bytes = FrameBytes(actual);
        const uint8_t silence_byte = actual.sample == PcmSampleFormat::U8 ? 0x80 : 0x00;
        std::vector<uint8_t> out(kDeviceFrames * frame_bytes, 0xAA);

        // 阶段 1: ring 空，连拉 20 轮，每轮都必须是纯静音字节。
        for (int i = 0; i < 20; ++i) {
            std::fill(out.begin(), out.end(), 0xAA);
            ASSERT_EQ(out.size(), backend->Pull(out.data(), out.size()));
            EXPECT_TRUE(std::all_of(out.begin(), out.end(), [silence_byte](uint8_t b) { return b == silence_byte; }))
                << c.name << " 第 " << i << " 轮欠载: 补静音的字节值不对";
        }
        EXPECT_EQ(0, output.GetBufferedMs(kSrcRate, kSrcChannels));

        // 阶段 2: 恢复供给后必须重新出数，不能一直静音下去。
        const std::vector<uint8_t> pcm = MakeSourcePcm(4000);
        output.Enqueue(pcm.data(), static_cast<int>(pcm.size()));
        std::fill(out.begin(), out.end(), 0xAA);
        ASSERT_EQ(out.size(), backend->Pull(out.data(), out.size()));
        EXPECT_LT(CountSilentFrames(out, frame_bytes, actual.sample), kDeviceFrames / 10)
            << c.name << ": 供给恢复后仍不出数";
        EXPECT_LE(output.GetConverterDelayFrames(), 64) << c.name << ": 欠载后残留了假延迟";
        ++driven;
    }
    EXPECT_EQ(cases.size(), driven);
}

// 核心长稳: 连续 2000 次声卡回调。四个独立事实同时钉死
//   1) swr 内部 FIFO 不随回调次数增长（原事故）；
//   2) 喂入源帧与产出目标帧守恒（多喂会在 FIFO 里凭空消失）；
//   3) 输出里补静音的比例极低（少喂会每轮咔哒一声）；
//   4) ring 水位有界（背压没把解码线程卡死）。
TEST(AudioOutputStabilityTest, ContinuousCallbacksKeepDelayBoundedAndConserveFrames) {
    const PcmFormat actual = MakeFormat(48000, 2, PcmSampleFormat::F32);
    FakeBackend* backend = nullptr;
    BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
        auto b = std::make_unique<FakeBackend>(actual, std::move(pull));
        backend = b.get();
        return b;
    });
    AudioOutput output;
    ASSERT_TRUE(output.Open(kSrcRate, kSrcChannels));
    ASSERT_NE(nullptr, backend);

    const std::vector<uint8_t> chunk = MakeSourcePcm(kFeedFrames);
    const size_t frame_bytes = FrameBytes(actual);
    std::vector<uint8_t> out(kDeviceFrames * frame_bytes, 0);

    uint64_t fed_frames = 0;
    uint64_t out_frames = 0;
    uint64_t silent_frames = 0;
    int64_t peak_delay = 0;
    int64_t first_window_delay = 0;
    int64_t last_window_delay = 0;
    int peak_buffered_ms = 0;
    size_t callbacks = 0;

    for (size_t i = 0; i < kCallbacks; ++i) {
        fed_frames += TopUpRing(output, chunk, kFeedFrames);
        peak_buffered_ms = std::max<int>(peak_buffered_ms, output.GetBufferedMs(kSrcRate, kSrcChannels));

        std::fill(out.begin(), out.end(), 0);
        const size_t got = backend->Pull(out.data(), out.size());
        ASSERT_EQ(out.size(), got) << "第 " << i << " 轮回调没填满设备缓冲";
        out_frames += got / frame_bytes;
        silent_frames += CountSilentFrames(out, frame_bytes, actual.sample);

        const int64_t delay = output.GetConverterDelayFrames();
        peak_delay = std::max<int64_t>(peak_delay, delay);
        if (i < 200)
            first_window_delay += delay;
        if (i + 200 >= kCallbacks)
            last_window_delay += delay;
        ++callbacks;
    }

    EXPECT_EQ(kCallbacks, callbacks) << "长稳回路没跑满，断言就成了空转";
    EXPECT_EQ(kCallbacks * kDeviceFrames, out_frames);

    // 稳态只该剩滤波器固有长度（几十帧）。原事故 2000 轮后是百万帧量级。
    EXPECT_LE(peak_delay, 64) << "内部 FIFO 积压失控: 喂给 swr 的量不是按输出容量反算的";
    EXPECT_LE(last_window_delay, first_window_delay + 200 * 8) << "延迟随回调次数单调上涨（逐轮 ceil / 整块喂）";

    const double expected_fed = static_cast<double>(out_frames) * kSrcRate / actual.sample_rate;
    EXPECT_NEAR(expected_fed, static_cast<double>(fed_frames), expected_fed * 0.05)
        << "喂入与产出不守恒: 源数据被超量抽进重采样器内部";
    EXPECT_LT(silent_frames, out_frames / 100) << "每轮都在补静音（逐轮 floor / 喂少了）";
    EXPECT_LE(peak_buffered_ms, 250) << "ring 长期贴着容量，解码线程会卡在背压里";
}

// 异步打开期间送进来的 PCM 不能丢。WASAPI 首次打开实测约 1 s，解码线程不会等；
// 这里让假后端刻意慢 300 ms，主线程立刻喂一块标记数据。
TEST(AudioOutputStabilityTest, DelayedAsyncOpenKeepsInitialPcm) {
    const PcmFormat actual = MakeFormat(48000, 2, PcmSampleFormat::F32);
    std::atomic<FakeBackend*> backend{nullptr};
    BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
        auto b = std::make_unique<FakeBackend>(actual, std::move(pull), 300);
        backend.store(b.get(), std::memory_order_release);
        return b;
    });
    AudioOutput output;
    output.OpenAsync(kSrcRate, kSrcChannels);

    const std::vector<uint8_t> marker = MakeSourcePcm(4000);
    output.Enqueue(marker.data(), static_cast<int>(marker.size()));

    for (int i = 0; i < 200 && !output.IsOpen(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(output.IsOpen()) << "设备最终没有就绪";
    FakeBackend* ready = backend.load(std::memory_order_acquire);
    ASSERT_NE(nullptr, ready);

    const size_t frame_bytes = FrameBytes(actual);
    std::vector<uint8_t> out(kDeviceFrames * frame_bytes, 0);
    ASSERT_EQ(out.size(), ready->Pull(out.data(), out.size()));
    const size_t silent = CountSilentFrames(out, frame_bytes, actual.sample);
    EXPECT_LT(silent, kDeviceFrames / 10) << "打开设备前送进来的整段音频被丢了（开局无声）";

    output.Stop();
}

// 设备给了非法格式时必须放弃本次播放，而不是硬开一条出杂音的链路。
TEST(AudioOutputStabilityTest, InvalidDeviceFormatIsRejected) {
    FakeBackend* backend = nullptr;
    BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
        auto b = std::make_unique<FakeBackend>(MakeFormat(0, 2, PcmSampleFormat::S16), std::move(pull));
        backend = b.get();
        return b;
    });
    AudioOutput output;
    EXPECT_FALSE(output.Open(kSrcRate, kSrcChannels));
    EXPECT_FALSE(output.IsOpen());
    ASSERT_NE(nullptr, backend);
    EXPECT_EQ(1, backend->open_count());
    EXPECT_EQ(1, backend->close_count()) << "失败路径必须把后端关掉再释放";
    EXPECT_EQ(0, output.GetConverterDelayFrames());
}

// 注入工厂返回空时 Open 要干净地失败，不能拿空 backend 去解引用。
TEST(AudioOutputStabilityTest, EmptyBackendFromFactoryFailsCleanly) {
    int factory_calls = 0;
    BackendInjection injection([&](AudioOutput::PullFn) -> std::unique_ptr<AudioOutput::Backend> {
        ++factory_calls;
        return nullptr;
    });
    AudioOutput output;
    EXPECT_FALSE(output.Open(kSrcRate, kSrcChannels));
    EXPECT_EQ(1, factory_calls);
    EXPECT_FALSE(output.IsOpen());
}

// Stop 要把转换器内部积压和 ring 一起清掉，seek 后不能带着旧延迟重开。
TEST(AudioOutputStabilityTest, StopClearsBacklogAndAllowsReopen) {
    const PcmFormat actual = MakeFormat(48000, 2, PcmSampleFormat::F32);
    FakeBackend* first = nullptr;
    FakeBackend* second = nullptr;
    BackendInjection injection([&](AudioOutput::PullFn pull) -> std::unique_ptr<AudioOutput::Backend> {
        auto b = std::make_unique<FakeBackend>(actual, std::move(pull));
        if (first == nullptr)
            first = b.get();
        else
            second = b.get();
        return b;
    });
    AudioOutput output;
    ASSERT_TRUE(output.Open(kSrcRate, kSrcChannels));
    const std::vector<uint8_t> pcm = MakeSourcePcm(4000);
    output.Enqueue(pcm.data(), static_cast<int>(pcm.size()));
    std::vector<uint8_t> out(kDeviceFrames * FrameBytes(actual), 0);
    ASSERT_EQ(out.size(), first->Pull(out.data(), out.size()));
    ASSERT_EQ(1, first->open_count());

    output.Stop();
    EXPECT_EQ(0, output.GetConverterDelayFrames());
    EXPECT_EQ(0, output.GetBufferedMs(kSrcRate, kSrcChannels));
    EXPECT_EQ(1, first->close_count());

    ASSERT_TRUE(output.Open(kSrcRate, kSrcChannels)) << "Stop 之后必须能重新打开";
    ASSERT_NE(nullptr, second);
    EXPECT_NE(first, second) << "重新打开必须换一个新后端";
    EXPECT_EQ(Src(), second->requested());
}

} // namespace
