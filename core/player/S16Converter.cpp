#include "core/player/S16Converter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace videoeye {
namespace player {

namespace {

// 本模块的枚举 → FFmpeg 的采样格式。
AVSampleFormat ToAvSampleFormat(PcmSampleFormat format) {
    switch (format) {
    case PcmSampleFormat::U8:
        return AV_SAMPLE_FMT_U8;
    case PcmSampleFormat::S16:
        return AV_SAMPLE_FMT_S16;
    case PcmSampleFormat::S32:
        return AV_SAMPLE_FMT_S32;
    case PcmSampleFormat::F32:
        return AV_SAMPLE_FMT_FLT;
    case PcmSampleFormat::F64:
        return AV_SAMPLE_FMT_DBL;
    }
    return AV_SAMPLE_FMT_S16;
}

} // namespace

int PcmBytesPerSample(PcmSampleFormat format) {
    switch (format) {
    case PcmSampleFormat::U8:
        return 1;
    case PcmSampleFormat::S16:
        return 2;
    case PcmSampleFormat::S32:
    case PcmSampleFormat::F32:
        return 4;
    case PcmSampleFormat::F64:
        return 8;
    }
    return 2;
}

S16Converter::S16Converter() = default;

S16Converter::~S16Converter() {
    Reset();
}

bool S16Converter::Configure(int src_rate, int src_channels, int dst_rate, int dst_channels,
                             PcmSampleFormat dst_format) {
    Reset();
    if (src_rate <= 0 || src_channels <= 0 || dst_rate <= 0 || dst_channels <= 0)
        return false;

    src_rate_ = src_rate;
    src_channels_ = src_channels;
    dst_rate_ = dst_rate;
    dst_channels_ = dst_channels;
    dst_format_ = dst_format;

    if (src_rate == dst_rate && src_channels == dst_channels && dst_format == PcmSampleFormat::S16) {
        return true; // 完全一致: 直通, 不建 swr
    }

    swr_ = swr_alloc();
    if (swr_ == nullptr) {
        return false;
    }

    AVChannelLayout in_layout{};
    AVChannelLayout out_layout{};
    av_channel_layout_default(&in_layout, src_channels);
    av_channel_layout_default(&out_layout, dst_channels);
    const int ret = swr_alloc_set_opts2(&swr_, &out_layout, ToAvSampleFormat(dst_format), dst_rate, &in_layout,
                                        AV_SAMPLE_FMT_S16, src_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || swr_init(swr_) < 0) {
        swr_free(&swr_);
        return false;
    }
    return true;
}

size_t S16Converter::Convert(const uint8_t* src, size_t src_bytes, uint8_t* dst, size_t dst_capacity_bytes) {
    if (dst == nullptr || dst_capacity_bytes == 0)
        return 0;
    const size_t src_frame_bytes = SrcFrameBytes();
    const size_t dst_frame_bytes = DstFrameBytes();
    if (src_frame_bytes == 0 || dst_frame_bytes == 0 || src == nullptr)
        return 0;

    const size_t in_frames = src_bytes / src_frame_bytes;
    const size_t out_cap = dst_capacity_bytes / dst_frame_bytes;
    if (in_frames == 0 || out_cap == 0)
        return 0;

    if (swr_ == nullptr) {
        // 直通: 只有帧布局完全一致时才会走到这里
        const size_t n = std::min(in_frames, out_cap) * src_frame_bytes;
        std::memcpy(dst, src, n);
        return n;
    }

    uint8_t* out_planes[1] = {dst};
    const uint8_t* in_planes[1] = {src};
    const int got = swr_convert(swr_, out_planes, static_cast<int>(out_cap), in_planes, static_cast<int>(in_frames));
    if (got <= 0) {
        return 0;
    }
    return static_cast<size_t>(got) * dst_frame_bytes;
}

size_t S16Converter::Drain(uint8_t* dst, size_t dst_capacity_bytes) {
    if (swr_ == nullptr || dst == nullptr)
        return 0;
    const size_t dst_frame_bytes = DstFrameBytes();
    if (dst_frame_bytes == 0)
        return 0;
    const size_t out_cap = dst_capacity_bytes / dst_frame_bytes;
    if (out_cap == 0)
        return 0;

    uint8_t* out_planes[1] = {dst};
    const int got = swr_convert(swr_, out_planes, static_cast<int>(out_cap), nullptr, 0);
    if (got <= 0) {
        return 0;
    }
    return static_cast<size_t>(got) * dst_frame_bytes;
}

size_t S16Converter::NextFeedFrames(size_t out_frames) {
    if (swr_ == nullptr || dst_rate_ <= 0 || src_rate_ <= 0) {
        return out_frames; // 直通: 一进一出
    }
    // 把 "out_frames * src/dst" 的零头留到下一轮。注意这里**无论调用方实际
    // 读到多少帧都要把 want 结算掉**（余数清零而不是结转）：环里暂时没数据
    // 属于欠载，不该在下一轮补喂一大块，否则又变成内部 FIFO 堆积。
    feed_accum_ += static_cast<int64_t>(out_frames) * static_cast<int64_t>(src_rate_);
    const int64_t want = feed_accum_ / dst_rate_;
    feed_accum_ -= want * static_cast<int64_t>(dst_rate_);
    return want > 0 ? static_cast<size_t>(want) : 0;
}

int64_t S16Converter::PendingOutputFrames() const {
    if (swr_ == nullptr || src_rate_ <= 0)
        return 0;
    // 以源采样率为时基，得到的就是"还压着多少源帧"。用输出采样率会得到
    // 目标帧数，量纲不同，和喂入量对不上就无法做守恒核对。
    return swr_get_delay(swr_, src_rate_);
}

void S16Converter::Reset() {
    if (swr_ != nullptr) {
        swr_free(&swr_);
    }
    src_rate_ = 0;
    src_channels_ = 0;
    dst_rate_ = 0;
    dst_channels_ = 0;
    dst_format_ = PcmSampleFormat::S16;
    feed_accum_ = 0; // seek / 重新打开后余数不能带过来
}

} // namespace player
} // namespace videoeye
