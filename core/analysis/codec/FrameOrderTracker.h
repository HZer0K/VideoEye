#pragma once

// 帧级 POC 追踪的按编码分派入口。
//
// 为什么要有这层：调用方（AnalysisPipeline）只关心"这条视频轨的显示顺序"，
// 不关心底层是 H.264 还是 HEVC 的 slice_header。没有这一层，管线里就得写
// `std::variant` + 三处 `std::visit`，而那些细节没有一条属于编排层的职责。
//
// 与 ParameterSetTracker 的接口刻意保持一致（Reset(codec_id, stream_index)
// + enabled() + Feed/FeedExtradata/Finish），这样管线里两条追踪链路长得一样，
// 改动时不会因为"一个是类一个是 variant"而漏改一条。
//
// 明确不做：AV1 / VP9 / VVC。它们的显示顺序需要各自的语法
// （AV1 的 order_hint、VP9 的 show_existing_frame 语义），没实现就不建槽，
// 上层据 frame_order_timelines 为空把维度标成"不可用"，不静默给 0。

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

#include "core/analysis/codec/FrameOrderTypes.h"
#include "core/analysis/codec/H264PocTracker.h"
#include "core/analysis/codec/HevcPocTracker.h"
#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {

class FrameOrderTracker {
public:
    using Framing = NalFraming;
    using Options = FrameOrderOptions;

    // codec_id 取 FFmpeg 的 AVCodecID 数值（与 ParameterSetTracker::Reset 同口径，
    // 这样调用方不必为两条链路各转一次枚举）。
    void Reset(int codec_id, int stream_index, const Options& options = Options{});

    // false = 该编码没有实现帧级顺序追踪，调用方不要建槽。
    bool enabled() const {
        return enabled_;
    }
    // enabled() == false 时必填，说明是哪一种编码没被覆盖。
    const std::string& disabled_reason() const {
        return disabled_reason_;
    }
    // 当前选中的实现名（"h264" / "hevc" / ""），只用于日志。
    const char* codec_name() const;

    void SetFraming(Framing framing);
    void SetLengthPrefixBytes(int bytes);

    int Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);
    bool FeedExtradata(const uint8_t* data, size_t size);

    const model::FrameOrderTimeline& Finish();
    const model::FrameOrderTimeline& timeline() const;
    int stream_index() const;

private:
    bool enabled_ = false;
    std::string disabled_reason_;
    // 两个实现都不是多态基类的派生类：它们的骨架相同但状态字段完全不同，
    // 硬抽一个虚基类只会把两边各自的字段都摊到基类里。
    std::variant<std::monostate, H264PocTracker, HevcPocTracker> impl_;
};

} // namespace videoeye
