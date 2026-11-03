// S16Converter（源 S16 → 设备格式）的行为回归。
//
// 背景: Windows 共享模式只接受设备的 mix format。设备拒绝源采样率/声道数时，
// 后端会改按设备格式 Initialize —— 但解码器仍按源格式产出 S16。中间少一层转换，
// 听感就是"变调"（44.1 kHz 素材在 48 kHz 设备上快 8.8%），或者把 S16 字节当
// float32 播（杂音/静音）。这一层就是补在 AudioOutput 出口的转换器。
//
// 这里不需要声卡: 转换器是纯计算，用合成正弦波覆盖
//   * 格式一致时直通（不得偷偷加一道重采样）
//   * 升采样帧数守恒（1 s 进 = 1 s 出）
//   * 升采样不改变信号能量（只改速率，不该变响或变轻）
//   * 单声道 → 立体声、5.1 → 立体声的声道数
//   * S16 → float32 的量程（-1.0 ~ 1.0）
//   * 输出缓冲比输入小很多时不丢样本（AudioOutput 的 Pull 是按设备缓冲大小分块的，
//     容量不足必须由 swr 内部留存，下一轮继续吐 —— 丢了就是持续的"沙沙"缺口）

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "core/player/S16Converter.h"

namespace {

using videoeye::player::PcmSampleFormat;
using videoeye::player::S16Converter;

constexpr double kPi = 3.14159265358979323846;

// 生成 frames 帧 S16 交错 PCM（每声道 440 Hz 正弦，幅度 0.5 FS）
std::vector<uint8_t> MakeSine(int frames, int channels, int sample_rate, double freq = 440.0, double amplitude = 0.5) {
    std::vector<int16_t> samples(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (int i = 0; i < frames; ++i) {
        const double phase = 2.0 * kPi * freq * static_cast<double>(i) / static_cast<double>(sample_rate);
        const auto value = static_cast<int16_t>(amplitude * 32767.0 * std::sin(phase));
        for (int ch = 0; ch < channels; ++ch) {
            samples[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(ch)] = value;
        }
    }
    std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t));
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

std::vector<int16_t> AsS16(const std::vector<uint8_t>& bytes) {
    std::vector<int16_t> out(bytes.size() / sizeof(int16_t));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(int16_t));
    }
    return out;
}

double Rms(const std::vector<int16_t>& samples) {
    if (samples.empty())
        return 0.0;
    long double sum = 0.0;
    for (const int16_t s : samples) {
        const long double v = static_cast<long double>(s);
        sum += v * v;
    }
    return std::sqrt(static_cast<double>(sum / static_cast<long double>(samples.size())));
}

} // namespace

// 源格式 == 设备格式: 不该建 swr，也不该改动任何一个字节（多一道无谓的
// 重采样会引入延迟与精度损失，而且量化噪声会累积）。
TEST(S16ConverterTest, PassthroughWhenFormatUnchanged) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(48000, 2, 48000, 2, PcmSampleFormat::S16));
    EXPECT_FALSE(converter.Active()) << "格式一致还启用转换 = 白吃一次重采样";

    const std::vector<uint8_t> input = MakeSine(480, 2, 48000);
    std::vector<uint8_t> output(input.size(), 0xAA);
    const size_t written = converter.Convert(input.data(), input.size(), output.data(), output.size());

    EXPECT_EQ(input.size(), written);
    EXPECT_EQ(input, output) << "直通路径必须逐字节相同";
}

// 44.1 kHz → 48 kHz: 帧数按比例守恒（1 s 进 = 1 s 出）。
// 这一条正是"变速/变调" bug 的判别点: 不转换的话 4800 帧 44.1k 播放时长是 108 ms，
// 而不是 100 ms。
TEST(S16ConverterTest, UpsampleKeepsDuration) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));
    ASSERT_TRUE(converter.Active());

    const int in_frames = 44100; // 1 s
    const std::vector<uint8_t> input = MakeSine(in_frames, 2, 44100);
    std::vector<uint8_t> output(static_cast<size_t>(in_frames + 4096) * 2 * sizeof(int16_t), 0);

    size_t written = converter.Convert(input.data(), input.size(), output.data(), output.size());
    // 重采样器有滤波器延迟, 尾巴要冲出来
    for (int guard = 0; guard < 32; ++guard) {
        const size_t tail = converter.Drain(output.data() + written, output.size() - written);
        if (tail == 0) {
            break;
        }
        written += tail;
    }

    const size_t out_frames = written / (2 * sizeof(int16_t));
    EXPECT_NEAR(48000, static_cast<double>(out_frames), 480.0) << "1 s 的 44.1 kHz 应产出约 1 s 的 48 kHz";
    // 反向守护: 没转换的话会是 44100 帧, 这条断言必须能抓住
    EXPECT_GT(out_frames, static_cast<size_t>(46000)) << "输出帧数没变 = 根本没做重采样";
}

// 升采样只该改速率，不该改能量（变调的同时通常伴随电平异常）。
TEST(S16ConverterTest, UpsamplePreservesLevel) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));

    const std::vector<uint8_t> input = MakeSine(44100, 2, 44100);
    std::vector<uint8_t> output(static_cast<size_t>(52000) * 2 * sizeof(int16_t), 0);
    size_t written = converter.Convert(input.data(), input.size(), output.data(), output.size());
    for (int guard = 0; guard < 32; ++guard) {
        const size_t tail = converter.Drain(output.data() + written, output.size() - written);
        if (tail == 0) {
            break;
        }
        written += tail;
    }
    output.resize(written);

    const double in_rms = Rms(AsS16(input));
    const double out_rms = Rms(AsS16(output));
    ASSERT_GT(in_rms, 100.0);
    // 掐掉首尾的滤波器过渡段再比, 只看稳态
    EXPECT_NEAR(in_rms, out_rms, in_rms * 0.15) << "重采样后电平偏差过大";
}

// 单声道 → 立体声: 帧数不变，两个声道都要有信号（不能只写左声道）。
TEST(S16ConverterTest, MonoToStereoDuplicatesChannel) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(48000, 1, 48000, 2, PcmSampleFormat::S16));
    ASSERT_TRUE(converter.Active());

    const int in_frames = 4800;
    const std::vector<uint8_t> input = MakeSine(in_frames, 1, 48000);
    std::vector<uint8_t> output(static_cast<size_t>(in_frames) * 2 * sizeof(int16_t), 0);
    const size_t written = converter.Convert(input.data(), input.size(), output.data(), output.size());

    EXPECT_EQ(static_cast<size_t>(in_frames) * 2 * sizeof(int16_t), written);
    const std::vector<int16_t> samples = AsS16(output);
    long left_energy = 0;
    long right_energy = 0;
    for (size_t i = 0; i + 1 < samples.size(); i += 2) {
        left_energy += std::abs(static_cast<int>(samples[i]));
        right_energy += std::abs(static_cast<int>(samples[i + 1]));
    }
    EXPECT_GT(left_energy, 0);
    EXPECT_GT(right_energy, 0) << "右声道整段静音 = 声道数没转换对";
}

// 5.1 → 立体声: 帧数不变，输出不能是静音（混音要做下混而不是只取前两声道丢弃）。
TEST(S16ConverterTest, DownmixSurroundToStereoKeepsFrames) {
    S16Converter converter;
    // 5.1 的默认布局在 swr 里是 6 声道; 只喂前两个声道有信号，其余静音，
    // 用来确认输出仍然是"有内容"的立体声。
    ASSERT_TRUE(converter.Configure(48000, 6, 48000, 2, PcmSampleFormat::S16));
    ASSERT_TRUE(converter.Active());

    const int in_frames = 4800;
    const size_t src_frame_bytes = 6 * sizeof(int16_t);
    std::vector<uint8_t> input(static_cast<size_t>(in_frames) * src_frame_bytes, 0);
    for (int i = 0; i < in_frames; ++i) {
        const double phase = 2.0 * kPi * 440.0 * i / 48000.0;
        const auto value = static_cast<int16_t>(0.5 * 32767.0 * std::sin(phase));
        auto* frame = reinterpret_cast<int16_t*>(input.data() + static_cast<size_t>(i) * src_frame_bytes);
        frame[0] = value; // L
        frame[1] = value; // R
    }

    std::vector<uint8_t> output(static_cast<size_t>(in_frames) * 2 * sizeof(int16_t), 0);
    const size_t written = converter.Convert(input.data(), input.size(), output.data(), output.size());

    EXPECT_EQ(static_cast<size_t>(in_frames) * 2 * sizeof(int16_t), written);
    EXPECT_GT(Rms(AsS16(output)), 100.0) << "下混后整段静音";
}

// S16 → float32（Windows 设备最常见的 mix format）: 样本量程必须在 -1.0 ~ 1.0。
// 这条是"把 S16 字节硬塞给 float32 设备"的判别点: 那样写进去的是
// 1e-40 量级的非规格化数, 表现为静音或爆音。
TEST(S16ConverterTest, ConvertsToFloat32WithNormalizedRange) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(48000, 2, 48000, 2, PcmSampleFormat::F32));
    ASSERT_TRUE(converter.Active());

    const int in_frames = 480;
    const std::vector<uint8_t> input = MakeSine(in_frames, 2, 48000);
    std::vector<uint8_t> output(static_cast<size_t>(in_frames) * 2 * sizeof(float), 0);
    const size_t written = converter.Convert(input.data(), input.size(), output.data(), output.size());

    EXPECT_EQ(static_cast<size_t>(in_frames) * 2 * sizeof(float), written);
    const auto* samples = reinterpret_cast<const float*>(output.data());
    const size_t count = written / sizeof(float);
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        EXPECT_GE(samples[i], -1.001f);
        EXPECT_LE(samples[i], 1.001f);
        peak = std::max(peak, std::fabs(samples[i]));
    }
    // 输入幅度 0.5 FS → 浮点峰值约 0.5
    EXPECT_NEAR(0.5f, peak, 0.06f) << "浮点量程不对（S16 直接喂 float 设备就是在这里翻车）";
}

// 输出缓冲小于一轮产出时，剩下的样本必须留在转换器内部、下一轮继续吐出来。
// AudioOutput::Pull 就是按设备缓冲（几百帧）分块拉的 —— 这里丢样本会变成周期性缺口。
TEST(S16ConverterTest, SmallOutputBufferLosesNoSamples) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));

    const int in_frames = 4410; // 100 ms
    const std::vector<uint8_t> input = MakeSine(in_frames, 2, 44100);

    std::vector<uint8_t> chunk(static_cast<size_t>(7) * 2 * sizeof(int16_t), 0); // 一次只给 7 帧
    size_t written = converter.Convert(input.data(), input.size(), chunk.data(), chunk.size());
    size_t out_frames = written / (2 * sizeof(int16_t));
    for (int guard = 0; guard < 4096; ++guard) {
        const size_t tail = converter.Drain(chunk.data(), chunk.size());
        if (tail == 0) {
            break;
        }
        out_frames += tail / (2 * sizeof(int16_t));
    }

    EXPECT_NEAR(4800, static_cast<double>(out_frames), 96.0) << "小缓冲分块时丢了样本";
}

// 「这一轮该喂多少源帧」的余数累积 (NextFeedFrames)。
//
// 回归点: AudioOutput::PullConverted 每次声卡回调都要按设备缓冲容量反算该读多少
// 源数据。逐轮 ceil 会每轮多喂零点几帧，swr 把吃不下的存进内部 FIFO —— 2000 次
// 回调后能压到上千帧（听感是声音越来越滞后、A/V 越拖越开）；逐轮 floor 会每轮
// 少喂，于是每次回调都补一段静音（持续的咔哒声）。余数必须跨轮累积。

// 整数比例: 44.1k→48k、容量 480 帧，实际每轮恰是 441 源帧，不许抖动。
TEST(S16ConverterTest, FeedFramesIsExactForIntegerRatio) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(441u, converter.NextFeedFrames(480)) << "第 " << i << " 轮";
    }
}

// 非整数比例: 44.1k→48k、容量 512 帧 → 每轮 470.4 源帧。
// 单轮只能是 470 或 471，但 1000 轮累计必须等于 470400（不能系统性偏多/偏少）。
TEST(S16ConverterTest, FeedFramesAccumulatesRemainderAcrossCalls) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));
    size_t total = 0;
    for (int i = 0; i < 1000; ++i) {
        const size_t feed = converter.NextFeedFrames(512);
        EXPECT_TRUE(feed == 470u || feed == 471u) << "第 " << i << " 轮喂了 " << feed;
        total += feed;
    }
    EXPECT_EQ(470400u, total) << "余数没有跨轮累积: 长期平均偏离 44100/48000";
}

// 降采样方向（48k→44.1k）同样成立: 容量 441 帧要喂 480 源帧。
TEST(S16ConverterTest, FeedFramesForDownsampleIsExact) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(48000, 2, 44100, 2, PcmSampleFormat::S16));
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(480u, converter.NextFeedFrames(441)) << "第 " << i << " 轮";
    }
}

// 格式一致（直通）时一进一出，不做任何比例换算。
TEST(S16ConverterTest, FeedFramesIsOneToOneWhenPassthrough) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(48000, 2, 48000, 2, PcmSampleFormat::S16));
    ASSERT_FALSE(converter.Active());
    EXPECT_EQ(512u, converter.NextFeedFrames(512));
    EXPECT_EQ(0u, converter.NextFeedFrames(0));
}

// 参数不合法: 拒绝配置，且保持"不转换"状态（调用方据此放弃本次播放）。
TEST(S16ConverterTest, RejectsInvalidConfiguration) {
    S16Converter converter;
    EXPECT_FALSE(converter.Configure(0, 2, 48000, 2, PcmSampleFormat::S16));
    EXPECT_FALSE(converter.Active());
    EXPECT_FALSE(converter.Configure(48000, 0, 48000, 2, PcmSampleFormat::S16));
    EXPECT_FALSE(converter.Active());
    // 失败后仍可重新配成合法值
    EXPECT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));
    EXPECT_TRUE(converter.Active());
}

// Reset 之后回到"未配置"状态（Stop 时调用；再 Configure 不能沿用旧 swr）。
TEST(S16ConverterTest, ResetReturnsToUnconfiguredState) {
    S16Converter converter;
    ASSERT_TRUE(converter.Configure(44100, 2, 48000, 2, PcmSampleFormat::S16));
    ASSERT_TRUE(converter.Active());

    converter.Reset();
    EXPECT_FALSE(converter.Active());
    EXPECT_EQ(0, converter.GetDstChannels());
    EXPECT_EQ(0u, converter.DstFrameBytes());

    // 重置后可直通配置
    ASSERT_TRUE(converter.Configure(48000, 2, 48000, 2, PcmSampleFormat::S16));
    EXPECT_FALSE(converter.Active());
}
