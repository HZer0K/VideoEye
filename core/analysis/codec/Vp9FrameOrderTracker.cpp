#include "core/analysis/codec/Vp9FrameOrderTracker.h"

#include <algorithm>

#include "core/analysis/codec/FrameOrderStats.h"
#include "core/analysis/codec/Vp9ColorConfig.h"
#include "core/media/codec/BitReader.h"

namespace videoeye {

namespace {
// 关键帧 / intra_only 帧在 frame_sync_code 位置的固定值（规范 6.2.2）
constexpr uint32_t kFrameSyncCode = 0x498342;
} // namespace

void Vp9FrameOrderTracker::Reset(int stream_index, const Options& options) {
    stream_index_ = stream_index;
    options_ = options;
    coded_.clear();
    for (int i = 0; i < kRefFrames; ++i)
        slot_[i] = -1;
    next_rank_ = 0;
    unresolved_shows_ = 0;
    coded_observed_ = 0;
    overflow_ = false;
    first_reason_.clear();
    timeline_ = model::FrameOrderTimeline();
    finished_ = false;
}

bool Vp9FrameOrderTracker::ParseSuperframeIndex(const uint8_t* data, size_t size,
                                                uint32_t (&sizes)[kMaxFramesPerPacket], int& count) {
    count = 0;
    if (data == nullptr || size < 2)
        return false;

    // 包尾标记：高 3 位必须是 110，否则不是 superframe。
    // ❗ 这一位判定有个前提：码流最后一个字节恰好是 0xC0..0xFF 时编码器必须补
    // 一个 0 字节 —— 所以"看起来像标记但前面没有同值标记"的包要整包放弃，
    // 不能当成 superframe 硬切（切错会把两帧的位混在一起，出错的方向是
    // "POC 全乱"而不是"报错"）。
    const uint8_t marker = data[size - 1];
    if ((marker & 0xE0) != 0xC0)
        return false;

    const int frames = (marker & 0x7) + 1;
    const int mag = ((marker >> 3) & 0x3) + 1;
    const size_t index_size = 2 + static_cast<size_t>(mag) * static_cast<size_t>(frames);
    if (size < index_size)
        return false;
    if (data[size - index_size] != marker)
        return false; // 索引头标记不匹配 = 不是真的 superframe

    const uint8_t* cursor = &data[size - index_size + 1];
    uint64_t total = 0;
    for (int i = 0; i < frames; ++i) {
        uint32_t frame_size = 0;
        for (int j = 0; j < mag; ++j)
            frame_size |= static_cast<uint32_t>(*cursor++) << (j * 8);
        if (frame_size == 0)
            return false;
        sizes[i] = frame_size;
        total += frame_size;
    }
    // 各帧长度之和必须正好等于索引之前的负载长度，否则又是"标记撞车"
    if (total + index_size != size)
        return false;

    count = frames;
    return true;
}

bool Vp9FrameOrderTracker::ReadFrameHead(const uint8_t* data, size_t size, FrameHead& out) {
    // ❗ 下限只能是 1 字节：show_existing_frame 的整帧头就是 8 位
    // （frame_marker 2 + profile 2 + show_existing 1 + 槽号 3），真实码流里
    // 这种包**确实只有 1 个字节**。写 size < 4 会让所有 show_existing 被判成
    // 坏包 —— 症状是"隐藏帧永远显示不出来"，而不是报错。
    if (data == nullptr || size < 1)
        return false;

    BitReader reader;
    reader.Reset(data, size);

    if (reader.ReadBits(2) != 2)
        return false; // frame_marker 必须是 2

    const int profile_low = static_cast<int>(reader.ReadBit());
    const int profile_high = static_cast<int>(reader.ReadBit());
    out.profile = (profile_high << 1) | profile_low;
    if (out.profile > 3)
        return false;
    if (out.profile == 3 && reader.ReadBit() != 0)
        return false; // reserved_zero 必须为 0

    if (reader.ReadBit() != 0) { // show_existing_frame
        out.show_existing = true;
        out.show_slot = static_cast<int>(reader.ReadBits(3));
        return !reader.HasError();
    }

    out.key_frame = reader.ReadBit() == 0; // frame_type: 0 = KEY_FRAME
    out.show_frame = reader.ReadBit() != 0;
    out.error_resilient = reader.ReadBit() != 0;

    if (out.key_frame) {
        // 顺序是 frame_sync_code → color_config() → refresh = 全槽。
        // 后面那些字段（frame_size 等）算顺序用不上，读到同步码就够了。
        out.refresh_flags = 0xFF;
        return !reader.HasError() && reader.ReadBits(24) == kFrameSyncCode;
    }

    // ---- 帧间帧 ----
    out.intra_only = out.show_frame ? false : (reader.ReadBit() != 0);
    if (!out.error_resilient)
        reader.SkipBits(2); // reset_frame_context
    if (reader.HasError())
        return false;

    if (out.intra_only) {
        if (reader.ReadBits(24) != kFrameSyncCode)
            return false;
        // profile 0 的 intra_only 帧里没有 color_config（规范规定默认 4:2:0 8bit）
        if (out.profile > 0) {
            Vp9ColorConfigInfo color;
            if (!ParseVp9ColorConfig(reader, out.profile, color))
                return false;
        }
        out.refresh_flags = static_cast<int>(reader.ReadBits(8));
    } else {
        out.refresh_flags = static_cast<int>(reader.ReadBits(8));
    }
    return !reader.HasError();
}

void Vp9FrameOrderTracker::ShowFrame(int coded_index) {
    if (coded_index < 0 || coded_index >= static_cast<int>(coded_.size())) {
        ++unresolved_shows_;
        return;
    }
    CodedFrame& frame = coded_[static_cast<size_t>(coded_index)];
    if (frame.rank < 0)
        frame.rank = next_rank_++;
    if (frame.show_times < 255)
        ++frame.show_times;
}

bool Vp9FrameOrderTracker::HandleFrame(const uint8_t* data, size_t size, bool has_time, double time_seconds,
                                       int64_t packet_index) {
    FrameHead head;
    if (!ReadFrameHead(data, size, head)) {
        NoteReason("VP9 帧头解析失败（frame_marker / 同步码 / 保留位不符）");
        return false;
    }

    if (head.show_existing) {
        // 不是编码帧：只是把某个槽里的画面再显示一次，不产生新的解码单元。
        ShowFrame(slot_[head.show_slot]);
        return false;
    }

    if (coded_.size() >= kMaxTrackedFrames) {
        // 到这个量级（≈9 小时的 30fps 素材）继续记下去没有意义，而且槽位映射
        // 一旦错位就会把"显示顺序"整片算错 —— 宁可在明确的边界上停住并声明。
        overflow_ = true;
        ++coded_observed_;
        return false;
    }

    const int coded_index = static_cast<int>(coded_.size());
    CodedFrame frame;
    frame.packet_index = packet_index;
    frame.time_seconds = time_seconds;
    frame.has_time = has_time;
    frame.key = head.key_frame;
    frame.intra_only = head.intra_only;
    coded_.push_back(frame);
    ++coded_observed_;

    // 槽位更新：第 i 位 = 本帧写进了第 i 号参考槽
    for (int i = 0; i < kRefFrames; ++i) {
        if ((head.refresh_flags & (1 << i)) != 0)
            slot_[i] = coded_index;
    }
    if (head.show_frame)
        ShowFrame(coded_index);
    return true;
}

int Vp9FrameOrderTracker::Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds,
                               int64_t packet_index) {
    if (data == nullptr || size == 0)
        return 0;
    if (overflow_) {
        ++coded_observed_;
        return 0;
    }

    uint32_t sizes[kMaxFramesPerPacket] = {};
    int count = 0;
    if (!ParseSuperframeIndex(data, size, sizes, count)) {
        return HandleFrame(data, size, has_time, time_seconds, packet_index) ? 1 : 0;
    }

    int frames = 0;
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        if (offset + sizes[i] > size)
            break; // 索引与负载不自洽，已在 ParseSuperframeIndex 里挡过，这里兜底
        if (HandleFrame(data + offset, sizes[i], has_time, time_seconds, packet_index))
            ++frames;
        offset += sizes[i];
    }
    return frames;
}

bool Vp9FrameOrderTracker::FeedExtradata(const uint8_t* data, size_t size) {
    (void)data;
    (void)size;
    return false; // VP9 没有参数集，配置全在关键帧头里
}

void Vp9FrameOrderTracker::NoteReason(const std::string& reason) {
    if (first_reason_.empty())
        first_reason_ = reason;
}

const model::FrameOrderTimeline& Vp9FrameOrderTracker::Finish() {
    if (finished_)
        return timeline_;
    finished_ = true;

    timeline_.codec = "vp9";
    // 顺序值不是 POC，而是"第几次显示"。两者都是"显示顺序值"，差别写进
    // order_source，报告和 UI 据此换措辞（VP9 没有 pic_order_cnt_type）。
    timeline_.order_source = "display_rank";
    timeline_.poc_type = -1;
    timeline_.frames_observed = coded_observed_;

    if (coded_.empty()) {
        timeline_.available = false;
        timeline_.unavailable_reason = first_reason_.empty()
                                           ? std::string("没有解析出任何 VP9 帧（不是 VP9 码流，或所有包都被跳过）")
                                           : first_reason_;
        return timeline_;
    }

    // 按解码顺序落明细：只收"真的显示过"的帧。隐藏帧（alt-ref）自己从不显示，
    // 它只提供参考，算进 frames_observed 但不进明细 —— 否则会出现
    // "明明只看到 40 个画面，却报了 41 帧"。
    for (const CodedFrame& frame : coded_) {
        if (frame.rank < 0)
            continue;

        model::FrameOrderEntry entry;
        // VP9 没有 NAL，这里记 frame_type 的原值：0 = KEY_FRAME，1 = INTER_FRAME
        entry.nal_unit_type = frame.key ? 0 : 1;
        // ❗ 帧间帧不区分 P/B：头里没有这个信息。留 -1 让 UI 显示 '?'，
        // 不去猜（猜错的后果是"B 帧重排"结论与 I/P/B 统计互相矛盾）。
        entry.slice_type = (frame.key || frame.intra_only) ? 2 : -1;
        entry.idr = frame.key;
        entry.irap = frame.key;
        entry.frame_num = 0;
        entry.poc = frame.rank;
        entry.packet_index = frame.packet_index;
        entry.has_time = frame.has_time;
        entry.time_seconds = frame.time_seconds;

        // 分类计数在入库之前累加：统计量不该被明细上限影响
        // （H.264/HEVC 的 I/P/B 计数也是"扫到多少算多少"，不是"存了多少算多少"）。
        switch (entry.slice_type) {
        case 2:
            ++timeline_.i_count;
            break;
        case 0:
            ++timeline_.p_count;
            break;
        case 1:
            ++timeline_.b_count;
            break;
        default:
            ++timeline_.other_count;
            break;
        }
        if (entry.idr)
            ++timeline_.idr_count;
        if (entry.irap)
            ++timeline_.irap_count;

        if (timeline_.frames.size() >= options_.max_frames) {
            timeline_.truncated = true;
            continue;
        }
        timeline_.frames.push_back(entry);
    }

    if (timeline_.frames.empty()) {
        timeline_.available = false;
        timeline_.unavailable_reason =
            unresolved_shows_ > 0 ? std::string("只有 show_existing_frame，没有可用的编码帧（码流可能从中间开始）")
                                  : std::string("所有帧都是隐藏帧（alt-ref），没有任何画面被显示");
        return timeline_;
    }

    ComputeFrameOrderStatistics(timeline_);

    // 同一幅画面被显示两次：统计层只看得见"不同的帧"，看不见"同一帧放两遍"，
    // 这里补上（口径一致 —— 都是"同一画面出现了两次"）。
    for (const CodedFrame& frame : coded_) {
        if (frame.show_times > 1)
            timeline_.duplicate_poc_count += (frame.show_times - 1);
    }

    timeline_.available = true;
    return timeline_;
}

} // namespace videoeye
