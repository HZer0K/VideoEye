#pragma once

#include <cstddef>
#include <cstdint>

// FFmpeg 的重采样上下文（仅实现文件里用到，头里保持不透明）
struct SwrContext;

namespace videoeye {
namespace player {

// 设备侧会用到的交错 PCM 采样格式。
// 只列 Windows 共享模式 mix format 实际会出现的那几种（float32 最常见），
// 以及解码链路自己产出的 S16。
//
// ⚠️ 没有 S24（3 字节紧凑打包）: 项目依赖的 FFmpeg 8 已经删掉 AV_SAMPLE_FMT_S24，
// 产不出这种布局，所以设备若是这种格式只能放弃本次播放（见 AudioOutput 的映射表）。
enum class PcmSampleFormat {
    U8,
    S16,
    S32,
    F32,
    F64,
};

// 每样本字节数
int PcmBytesPerSample(PcmSampleFormat format);

// S16 交错 PCM 的「源格式 → 设备格式」转换器。
//
// 为什么需要它：Windows 共享模式 WASAPI 只接受设备 mix format，源采样率/声道数
// 不被支持时后端会按**设备**格式重新 Initialize（常见是 48 kHz / float32）——
// 但解码器是按**源**格式产出 PCM 的（AudioDecoder 的 swr 固定输出到源的
// rate/channels 的 S16）。不在这里补一层转换：44.1 kHz 素材在 48 kHz 设备上会快
// 8.8%（听感是变调），多声道素材在立体声设备上会整段错位，而 S16 字节喂给
// float32 设备直接就是杂音或静音。
//
// 转换点放在 AudioOutput（格式分歧真正产生的地方），解码器与分析侧仍然只认
// 源格式，不受影响。
class S16Converter {
public:
    S16Converter();
    ~S16Converter();

    S16Converter(const S16Converter&) = delete;
    S16Converter& operator=(const S16Converter&) = delete;

    // 配置转换方向（S16 交错 → dst_format 交错）。参数非法或 swr 初始化失败时
    // 返回 false，此时保持「不转换」状态（宁可快放也不要杂音/静音）。
    bool Configure(int src_rate, int src_channels, int dst_rate, int dst_channels, PcmSampleFormat dst_format);

    // 是否真的需要转换（源格式与目标格式完全一致时为 false）
    bool Active() const {
        return swr_ != nullptr;
    }
    int GetSrcSampleRate() const {
        return src_rate_;
    }
    int GetSrcChannels() const {
        return src_channels_;
    }
    int GetDstSampleRate() const {
        return dst_rate_;
    }
    int GetDstChannels() const {
        return dst_channels_;
    }
    size_t SrcFrameBytes() const {
        return static_cast<size_t>(src_channels_) * 2; // 源恒为 S16
    }
    size_t DstFrameBytes() const {
        return static_cast<size_t>(dst_channels_) * static_cast<size_t>(PcmBytesPerSample(dst_format_));
    }

    // 转换 src 里的一段源 S16 PCM，写出到 dst（最多 dst_capacity_bytes 字节）。
    // 返回写出的字节数（一定是目标帧的整数倍）。**能力不足时不丢样本**：
    // swr 会把没写出的部分留在内部，下次调用继续吐出来。
    // src_bytes 必须是源帧的整数倍（调用方保证）。
    size_t Convert(const uint8_t* src, size_t src_bytes, uint8_t* dst, size_t dst_capacity_bytes);

    // 冲刷重采样器内部的滤波延迟（源数据已经喂完、需要最后那几十个样本时用）。
    size_t Drain(uint8_t* dst, size_t dst_capacity_bytes);

    // 这一轮该喂多少源帧才能产出 out_frames 个目标帧（含余数累积）。
    //
    // ❗ 调用方必须按这个数喂，不能"把手上的一整块直接塞进去"。swr 不丢输入 ——
    // 它把吃不下的那部分存进内部 FIFO；一旦每轮都多喂，FIFO 就一路涨：
    // 实测 48k→44.1k 每轮喂 2048 源帧、其实只要 441 输出帧，2000 次回调后内部
    // 压了 320 万帧（约 65 秒）。听感是声音越来越滞后，环形缓冲永远被填满
    // （解码线程卡在 Enqueue 里），A/V 越拖越开。
    //
    // 也不能逐轮 ceil/floor 就算了：
    //   * 每轮 ceil（如 44.1k→48k、容量 512 时 ceil(512*44100/48000)=471，
    //     换算产出 512.65 帧）会每轮多喂 0.65 帧，2000 次后内部压了 1215 帧；
    //   * 每轮 floor 会每轮少喂，于是每轮回调都补一段静音 —— 持续的咔哒声。
    // 逐轮算零头、把余数累积到下一轮，长期平均恰是 out*src/dst（实测 2000 次
    // 回调后内部延迟稳定在滤波器固有长度 16~17 帧，欠帧只发生在第一轮）。
    size_t NextFeedFrames(size_t out_frames);

    // 重采样器内部还压着多少源帧（诊断 / 长稳守护用）。
    //
    // swr_convert 不丢输入：喂进去却没能当场产出的样本会留在内部 FIFO。
    // 这个值在稳态下应该只有滤波器固有长度（几十帧）；如果随回调次数一路涨，
    // 说明调用方喂多了，播放会越来越滞后。直通状态恒为 0。
    // 只读，不改变转换状态。
    int64_t PendingOutputFrames() const;

    // 丢弃 swr 内部残留（seek / 停止后重新打开）。
    void Reset();

private:
    SwrContext* swr_ = nullptr;
    int src_rate_ = 0;
    int src_channels_ = 0;
    int dst_rate_ = 0;
    int dst_channels_ = 0;
    PcmSampleFormat dst_format_ = PcmSampleFormat::S16;
    // 「这一轮该喂多少源帧」的余数（见 NextFeedFrames 的说明）。
    // 单位: 输出帧对应的源帧数 × dst_rate 的余量。
    int64_t feed_accum_ = 0;
};

} // namespace player
} // namespace videoeye
