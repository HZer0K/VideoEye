#include "core/analysis/codec/FrameOrderTracker.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace videoeye {

namespace {

// 没有实现时的空结果。available 为 false 且 reason 非空，
// 与"扫了但一帧都没解析到"是两种不同的情况，不能共用一条文案。
const model::FrameOrderTimeline& EmptyTimeline() {
    static const model::FrameOrderTimeline empty = [] {
        model::FrameOrderTimeline timeline;
        timeline.unavailable_reason = "该编码未实现帧级顺序（POC）追踪";
        return timeline;
    }();
    return empty;
}

} // namespace

void FrameOrderTracker::Reset(int codec_id, int stream_index, const Options& options) {
    enabled_ = false;
    disabled_reason_.clear();

    switch (static_cast<AVCodecID>(codec_id)) {
    case AV_CODEC_ID_H264:
        impl_.emplace<H264PocTracker>();
        std::get<H264PocTracker>(impl_).Reset(stream_index, options);
        enabled_ = true;
        break;
    case AV_CODEC_ID_HEVC:
        impl_.emplace<HevcPocTracker>();
        std::get<HevcPocTracker>(impl_).Reset(stream_index, options);
        enabled_ = true;
        break;
    case AV_CODEC_ID_VP9:
        impl_.emplace<Vp9FrameOrderTracker>();
        std::get<Vp9FrameOrderTracker>(impl_).Reset(stream_index, options);
        enabled_ = true;
        break;
    default:
        impl_.emplace<std::monostate>();
        disabled_reason_ = "该编码未实现帧级顺序追踪（当前覆盖 H.264 / HEVC / VP9）";
        break;
    }
}

const char* FrameOrderTracker::codec_name() const {
    if (std::holds_alternative<H264PocTracker>(impl_))
        return "h264";
    if (std::holds_alternative<HevcPocTracker>(impl_))
        return "hevc";
    if (std::holds_alternative<Vp9FrameOrderTracker>(impl_))
        return "vp9";
    return "";
}

void FrameOrderTracker::SetFraming(Framing framing) {
    if (auto* t = std::get_if<H264PocTracker>(&impl_))
        t->SetFraming(framing);
    else if (auto* t = std::get_if<HevcPocTracker>(&impl_))
        t->SetFraming(framing);
    else if (auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        t->SetFraming(framing);
}

void FrameOrderTracker::SetLengthPrefixBytes(int bytes) {
    if (auto* t = std::get_if<H264PocTracker>(&impl_))
        t->SetLengthPrefixBytes(bytes);
    else if (auto* t = std::get_if<HevcPocTracker>(&impl_))
        t->SetLengthPrefixBytes(bytes);
    else if (auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        t->SetLengthPrefixBytes(bytes);
}

int FrameOrderTracker::Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds,
                            int64_t packet_index) {
    if (auto* t = std::get_if<H264PocTracker>(&impl_))
        return t->Feed(data, size, has_time, time_seconds, packet_index);
    if (auto* t = std::get_if<HevcPocTracker>(&impl_))
        return t->Feed(data, size, has_time, time_seconds, packet_index);
    if (auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        return t->Feed(data, size, has_time, time_seconds, packet_index);
    return 0;
}

bool FrameOrderTracker::FeedExtradata(const uint8_t* data, size_t size) {
    if (auto* t = std::get_if<H264PocTracker>(&impl_))
        return t->FeedExtradata(data, size);
    if (auto* t = std::get_if<HevcPocTracker>(&impl_))
        return t->FeedExtradata(data, size);
    if (auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        return t->FeedExtradata(data, size);
    return false;
}

const model::FrameOrderTimeline& FrameOrderTracker::Finish() {
    if (auto* t = std::get_if<H264PocTracker>(&impl_))
        return t->Finish();
    if (auto* t = std::get_if<HevcPocTracker>(&impl_))
        return t->Finish();
    if (auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        return t->Finish();
    return EmptyTimeline();
}

const model::FrameOrderTimeline& FrameOrderTracker::timeline() const {
    if (const auto* t = std::get_if<H264PocTracker>(&impl_))
        return t->timeline();
    if (const auto* t = std::get_if<HevcPocTracker>(&impl_))
        return t->timeline();
    if (const auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        return t->timeline();
    return EmptyTimeline();
}

int FrameOrderTracker::stream_index() const {
    if (const auto* t = std::get_if<H264PocTracker>(&impl_))
        return t->stream_index();
    if (const auto* t = std::get_if<HevcPocTracker>(&impl_))
        return t->stream_index();
    if (const auto* t = std::get_if<Vp9FrameOrderTracker>(&impl_))
        return t->stream_index();
    return -1;
}

} // namespace videoeye
