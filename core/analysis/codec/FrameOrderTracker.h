#pragma once

// 帧级显示顺序追踪的按编码分派入口。
//
// 为什么要有这层：调用方（AnalysisPipeline）只关心"这条视频轨的显示顺序"，
// 不关心底层是 H.264 / HEVC 的 slice_header、VP9 的 uncompressed frame
// header，还是 AV1 的 OBU 帧头。没有这一层，管线里就得写 `std::variant` +
// 多处 `std::visit`，而那些细节没有一条属于编排层的职责。
//
// 与 ParameterSetTracker 的接口刻意保持一致（Reset(codec_id, stream_index)
// + enabled() + Feed/FeedExtradata/Finish），这样管线里两条追踪链路长得一样，
// 改动时不会因为"一个是类一个是 variant"而漏改一条。
//
// 当前覆盖：H.264（POC）、HEVC（POC）、VVC（PH 里的 POC）、VP9（显示事件序 ——
// 该编码没有 POC，见 Vp9FrameOrderTracker）、AV1（order_hint —— 环形量，见
// Av1FrameOrderTracker）。其余编码没实现就不建槽，上层据
// frame_order_timelines 为空把维度标成"不可用"，不静默给 0。

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

#include "core/analysis/codec/Av1FrameOrderTracker.h"
#include "core/analysis/codec/FrameOrderTypes.h"
#include "core/analysis/codec/H264PocTracker.h"
#include "core/analysis/codec/HevcPocTracker.h"
#include "core/analysis/codec/Vp9FrameOrderTracker.h"
#include "core/analysis/codec/VvcPocTracker.h"
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
    // 当前选中的实现名（"h264" / "hevc" / "vvc" / "vp9" / "av1"），只用于日志与文案。
    const char* codec_name() const;
    // 顺序值来源（"poc" / "display_rank" / ""），取值与
    // model::FrameOrderTimeline::order_source 逐字一致，只用于日志与文案。
    // ❗ 不要用"是不是 POC"来判断实现是否可用：VP9 没有 POC，顺序值来自显示事件序。
    const char* order_source_name() const;

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
    std::variant<std::monostate, H264PocTracker, HevcPocTracker, VvcPocTracker, Vp9FrameOrderTracker,
                 Av1FrameOrderTracker>
        impl_;
};

} // namespace videoeye
