#include "core/analysis/codec/Av1FrameOrderTracker.h"

#include "core/analysis/codec/Av1BitstreamParser.h"
#include "core/analysis/codec/FrameOrderStats.h"
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataParser.h"

namespace videoeye {

namespace {

// SELECT_SCREEN_CONTENT_TOOLS / SELECT_INTEGER_MV 的取值（规范 6.4.2）
constexpr int kSelectAdaptive = 2;

} // namespace

int Av1FrameOrderTracker::RelativeDist(int a, int b, int bits) {
    // 规范 6.10.2 get_relative_dist()：
    //   diff = a - b，m = 1 << (bits - 1)
    //   (diff & (m - 1)) - (diff & m)
    // 结果落在 [-2^(bits-1), 2^(bits-1))，正好把"环形回绕"解释成最短方向的距离。
    if (bits <= 0)
        return 0;
    const int mask = (1 << bits) - 1;
    const int diff = (a - b) & mask;
    const int m = 1 << (bits - 1);
    return (diff & (m - 1)) - (diff & m);
}

void Av1FrameOrderTracker::Reset(int stream_index, const Options& options) {
    stream_index_ = stream_index;
    options_ = options;
    seq_ = model::Av1SequenceHeaderInfo();
    have_seq_ = false;
    hint_mode_ = false;
    unsupported_ = false;

    coded_.clear();
    events_.clear();
    for (int i = 0; i < kRefFrames; ++i)
        slot_[i] = -1;
    have_last_hint_ = false;
    last_hint_ = 0;
    hint_unwrapped_ = 0;
    next_rank_ = 0;
    unresolved_shows_ = 0;
    coded_observed_ = 0;
    overflow_ = false;
    first_reason_.clear();
    timeline_ = model::FrameOrderTimeline();
    finished_ = false;
}

void Av1FrameOrderTracker::ApplySequenceHeader(const model::Av1SequenceHeaderInfo& seq) {
    if (!seq.present)
        return;
    seq_ = seq;
    have_seq_ = true;
    hint_mode_ = seq_.enable_order_hint;
    // ❗ 声明了 decoder_model_info 就整条轨放弃：帧头里紧随 primary_ref_frame
    // 之后有一串 buffer_removal_time，宽度取决于每个 operating point 的
    // decoder_model_param_present_flag，跳不过去就会把 refresh_frame_flags
    // 读错，进而让参考槽映射整片失真。宁可在明确的边界上停住。
    if (seq_.decoder_model_info_present_flag) {
        unsupported_ = true;
        NoteReason("AV1 序列头声明了 decoder_model_info（帧头含 buffer_removal_time），"
                   "暂不支持解析这类码流的帧顺序");
    }
}

bool Av1FrameOrderTracker::ReadFrameHead(const uint8_t* payload, size_t size, FrameHead& out) const {
    if (payload == nullptr || size < 1)
        return false;

    videoeye::BitReader reader;
    reader.Reset(payload, size);
    out = FrameHead();

    if (seq_.reduced_still_picture_header) {
        // ❗ 静图序列头下这三项是**定义**，不是读出来的：
        //   码流里既没有 show_existing_frame 也没有 frame_type / show_frame。
        // 照非静图的写法去读，会把 disable_cdf_update 当成 frame_type 吃掉。
        out.frame_type = kKeyFrame;
        out.show_frame = true;
        out.error_resilient = true;
    } else {
        out.show_existing = reader.ReadBit() != 0;
        if (out.show_existing) {
            // 整个分支只有这 3 位是我们需要的；decoder_model_info 已在
            // ApplySequenceHeader 里排除，frame_id 之后的字段不必再吃。
            out.show_slot = static_cast<int>(reader.ReadBits(3));
            return !reader.HasError();
        }

        out.frame_type = static_cast<int>(reader.ReadBits(2));
        out.show_frame = reader.ReadBit() != 0;
        // ❗ showable_frame 这一位**只在 show_frame == 0 时存在**。
        // 写反了会多吃一位，order_hint 从此整体错位 —— 而且不会解析失败。
        if (!out.show_frame) {
            reader.ReadBit();
        }

        const bool is_sframe = (out.frame_type == kSwitchFrame);
        if (is_sframe || (out.frame_type == kKeyFrame && out.show_frame)) {
            out.error_resilient = true; // 规范：这两个条件下恒为 1，不占位
        } else {
            out.error_resilient = reader.ReadBit() != 0;
        }
    }

    reader.ReadBit(); // disable_cdf_update（值用不上，但必须吃掉）
    int allow_screen_content_tools = 0;
    if (seq_.seq_force_screen_content_tools == kSelectAdaptive) {
        allow_screen_content_tools = static_cast<int>(reader.ReadBit());
    } else {
        allow_screen_content_tools = seq_.seq_force_screen_content_tools;
    }
    if (allow_screen_content_tools) {
        if (seq_.seq_force_integer_mv == kSelectAdaptive)
            reader.ReadBit(); // force_integer_mv（不影响顺序值）
    }

    if (seq_.reduced_still_picture_header) {
        // 静图：没有 order_hint，也没有 refresh 位；关键帧写满全部槽位。
        out.refresh_flags = 0xFF;
        return !reader.HasError();
    }

    if (seq_.frame_id_numbers_present_flag && seq_.frame_id_length > 0)
        reader.SkipBits(seq_.frame_id_length); // current_frame_id
    const bool sframe = (out.frame_type == kSwitchFrame);
    if (!sframe)
        reader.ReadBit(); // frame_size_override_flag（SWITCH 帧恒 1，不占位）

    const int hint_bits = seq_.enable_order_hint ? seq_.order_hint_bits_minus_1 + 1 : 0;
    if (hint_bits > 0) {
        out.order_hint = static_cast<int>(reader.ReadBits(hint_bits));
        out.has_order_hint = true;
    }

    const bool intra_only = (out.frame_type == kKeyFrame || out.frame_type == kIntraOnlyFrame);
    if (!out.error_resilient && !intra_only)
        reader.SkipBits(3); // primary_ref_frame

    // refresh_frame_flags：KEY 且显示的帧恒 0xFF（不占位），SWITCH 恒 0xFF。
    if (out.frame_type == kKeyFrame && out.show_frame) {
        out.refresh_flags = 0xFF;
    } else if (sframe) {
        out.refresh_flags = 0xFF;
    } else {
        out.refresh_flags = static_cast<int>(reader.ReadBits(8));
    }
    return !reader.HasError();
}

void Av1FrameOrderTracker::NoteDisplay(int coded_index, bool has_time, double time_seconds, int64_t packet_index) {
    if (coded_index < 0 || coded_index >= static_cast<int>(coded_.size())) {
        ++unresolved_shows_;
        return;
    }
    CodedFrame& frame = coded_[static_cast<size_t>(coded_index)];
    // 没有 order_hint 时，显示事件序就是顺序值的唯一来源（与 VP9 同一套）。
    if (frame.rank < 0 && !hint_mode_)
        frame.rank = next_rank_++;

    DisplayEvent event;
    event.coded_index = coded_index;
    event.packet_index = packet_index;
    event.time_seconds = time_seconds;
    event.has_time = has_time;
    events_.push_back(event);
}

bool Av1FrameOrderTracker::HandleFrame(const uint8_t* payload, size_t size, bool has_time, double time_seconds,
                                       int64_t packet_index) {
    if (!have_seq_) {
        NoteReason("还没拿到 AV1 序列头（sequence_header_obu 未出现），无法判断帧头布局");
        return false;
    }
    if (unsupported_)
        return false;

    FrameHead head;
    if (!ReadFrameHead(payload, size, head)) {
        NoteReason("AV1 帧头解析失败（showable_frame / order_hint 位序不符或包被截断）");
        return false;
    }

    if (head.show_existing) {
        // 不是编码帧：把某个槽里的画面再显示一次，不产生新的解码单元。
        if (head.show_slot < 0 || head.show_slot >= kRefFrames) {
            ++unresolved_shows_;
            return false;
        }
        NoteDisplay(slot_[head.show_slot], has_time, time_seconds, packet_index);
        return false;
    }

    if (coded_.size() >= kMaxTrackedFrames) {
        // 到这个量级（≈9 小时的 30fps 素材）继续记没有意义，而且槽位映射一旦
        // 错位就会把显示顺序整片算错 —— 宁可在明确边界上停住并声明。
        overflow_ = true;
        ++coded_observed_;
        return false;
    }

    CodedFrame frame;
    frame.packet_index = packet_index;
    frame.time_seconds = time_seconds;
    frame.has_time = has_time;
    frame.frame_type = head.frame_type;
    frame.show_frame = head.show_frame;

    if (head.has_order_hint) {
        // 解环：order_hint 是模 2^bits 的环形量，直接赋给 order_value 会在
        // 回绕处（255 → 0）被当成"回退 255 帧"。累加 get_relative_dist 得到的
        // 有符号距离，得到一条单调可比的时间轴。
        const int bits = seq_.order_hint_bits_minus_1 + 1;
        if (have_last_hint_)
            hint_unwrapped_ += RelativeDist(head.order_hint, last_hint_, bits);
        else
            hint_unwrapped_ = head.order_hint;
        last_hint_ = head.order_hint;
        have_last_hint_ = true;
        frame.order_value = hint_unwrapped_;
    }

    const int coded_index = static_cast<int>(coded_.size());
    coded_.push_back(frame);
    ++coded_observed_;

    for (int i = 0; i < kRefFrames; ++i) {
        if ((head.refresh_flags & (1 << i)) != 0)
            slot_[i] = coded_index;
    }
    if (head.show_frame)
        NoteDisplay(coded_index, has_time, time_seconds, packet_index);
    return true;
}

int Av1FrameOrderTracker::Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds,
                               int64_t packet_index) {
    if (data == nullptr || size == 0)
        return 0;
    if (overflow_) {
        ++coded_observed_;
        return 0;
    }

    int frames = 0;
    // 一包里的 OBU 顺序有意义：序列头通常在关键帧包的最前面，
    // 因此"先看序列头再看帧头"必须按遍历顺序自然地发生。
    for (const ObuUnit& obu : ExtradataParser::ExtractObuUnits(data, size)) {
        if (obu.type == static_cast<uint8_t>(Av1ObuType::SequenceHeader)) {
            ApplySequenceHeader(Av1BitstreamParser::ParseFromObuUnit(obu));
            continue;
        }
        if (obu.type == static_cast<uint8_t>(Av1ObuType::FrameHeader) ||
            obu.type == static_cast<uint8_t>(Av1ObuType::Frame)) {
            if (HandleFrame(obu.data.data(), obu.data.size(), has_time, time_seconds, packet_index))
                ++frames;
        }
        // 其它 OBU（temporal delimiter / tile group / metadata / padding）与
        // 帧顺序无关，直接跳过。
    }
    return frames;
}

bool Av1FrameOrderTracker::FeedExtradata(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return false;
    // MP4/MKV 的 av1C 里 configOBUs 可能带一份序列头；没有也没关系 ——
    // 带内关键帧一定会重写一份，这里只是让首帧之前就能判布局。
    const model::Av1SequenceHeaderInfo seq = Av1BitstreamParser::ParseSequenceHeader(data, size);
    if (seq.present) {
        ApplySequenceHeader(seq);
        return true;
    }

    // ❗ 兜底：Matroska 的 CodecPrivate 可能是一串**裸 OBU**（没有 av1C 外壳），
    // 而 av1C 的嗅探在这种输入上认不出来。判据不用"格式检测成功"，而是
    // "解析结果里真的出现了 sequence_header_obu"。
    for (const ObuUnit& obu : ExtradataParser::ExtractObuUnits(data, size)) {
        if (!obu.is_sequence_header)
            continue;
        const model::Av1SequenceHeaderInfo from_obu = Av1BitstreamParser::ParseFromObuUnit(obu);
        if (from_obu.present) {
            ApplySequenceHeader(from_obu);
            return true;
        }
    }
    return false;
}

void Av1FrameOrderTracker::NoteReason(const std::string& reason) {
    if (first_reason_.empty())
        first_reason_ = reason;
}

const model::FrameOrderTimeline& Av1FrameOrderTracker::Finish() {
    if (finished_)
        return timeline_;
    finished_ = true;

    timeline_.codec = "av1";
    // 有 order_hint 就是码流里读出来的顺序值；没有（enable_order_hint == 0
    // 或静图）就只能靠显示事件序，与 VP9 同一套口径。
    const bool has_hint = have_seq_ && hint_mode_;
    timeline_.order_source = has_hint ? "order_hint" : "display_rank";
    timeline_.poc_type = -1; // AV1 没有 pic_order_cnt_type
    timeline_.frames_observed = coded_observed_;

    if (unsupported_) {
        timeline_.available = false;
        timeline_.unavailable_reason = first_reason_;
        return timeline_;
    }

    if (coded_.empty()) {
        timeline_.available = false;
        timeline_.unavailable_reason = first_reason_.empty()
                                           ? std::string("没有解析出任何 AV1 帧（不是 AV1 码流，或所有包都被跳过）")
                                           : first_reason_;
        return timeline_;
    }

    // 分类计数按**编码帧**算（与 frames_observed 同口径）：一帧被显示两次
    // 不该让它数出两个关键帧。
    for (const CodedFrame& frame : coded_) {
        const bool intra = (frame.frame_type == kKeyFrame || frame.frame_type == kIntraOnlyFrame);
        const bool key = (frame.frame_type == kKeyFrame);
        if (intra)
            ++timeline_.i_count;
        else
            ++timeline_.other_count;
        if (key) {
            ++timeline_.idr_count;
            ++timeline_.irap_count;
        }
    }

    // 明细按**显示事件**落：解码顺序里每次"输出一帧"就是一条。隐藏帧
    // 自己从不输出，只有被 show_existing_frame 调出来时才出现在这里。
    for (const DisplayEvent& event : events_) {
        const CodedFrame& frame = coded_[static_cast<size_t>(event.coded_index)];

        model::FrameOrderEntry entry;
        // AV1 没有 NAL：这里记 frame_type 原值（0=KEY 1=INTER 2=INTRA_ONLY 3=SWITCH）。
        // 判"是不是随机接入点"请看 irap / idr，别看数字。
        entry.nal_unit_type = frame.frame_type;
        // ❗ 帧间帧不区分 P/B：AV1 的 frame_type 只有 INTER，头里没有这个概念。
        // 关键帧与 intra-only 记 I（它们确实全是帧内编码），其余留 -1 让 UI 显示 '?'。
        const bool intra = (frame.frame_type == kKeyFrame || frame.frame_type == kIntraOnlyFrame);
        entry.slice_type = intra ? 2 : -1;
        entry.idr = (frame.frame_type == kKeyFrame);
        entry.irap = entry.idr;
        entry.frame_num = 0;
        entry.poc = has_hint ? frame.order_value : static_cast<int64_t>(frame.rank);
        // 包号与时间取**显示事件**的：show_existing 的输出时刻是调出它的那个包，
        // 不是当初编码它的包。
        entry.packet_index = event.packet_index;
        entry.has_time = event.has_time;
        entry.time_seconds = event.time_seconds;

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
                                  : std::string("所有帧都是隐藏帧，没有任何画面被显示");
        return timeline_;
    }

    ComputeFrameOrderStatistics(timeline_);

    // ❗ 这里**不再**手工补"同一帧显示多次"的重复数：明细已经是显示事件，
    // 同一 order_hint 出现两次会自然被统计层算成一处重复。再补一遍就翻倍了。

    timeline_.available = true;
    return timeline_;
}

} // namespace videoeye
