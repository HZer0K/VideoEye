#include "core/analysis/codec/InBandParameterSets.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace videoeye {
namespace {

// 起始码至少 3 字节；为了还够读 VVC/HEVC 的 2 字节 NAL header，重叠留 5 字节。
// 少了这几字节，"起始码在上一包末尾、header 在下一包开头"就会被漏掉。
constexpr size_t kScanOverlap = 5;

// 这个 NAL 类型是不是参数集（SPS/PPS/VPS）。
//
// 三种封装的 header 布局不同，必须按 codec 分别读，不能统一按首字节位域猜：
//   H.264: 1 字节，type = b0 & 0x1F
//   HEVC : 2 字节，type = (b0 >> 1) & 0x3F
//   VVC  : 2 字节，type = (b1 >> 3) & 0x1F（首字节是 nuh_layer_id）
bool IsParameterSetNalType(int codec_id, uint8_t b0, uint8_t b1) {
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264: {
            const uint8_t type = static_cast<uint8_t>(b0 & 0x1F);
            return type == 7 || type == 8;  // SPS / PPS
        }
        case AV_CODEC_ID_HEVC: {
            const uint8_t type = static_cast<uint8_t>((b0 >> 1) & 0x3F);
            return type == 32 || type == 33 || type == 34;  // VPS / SPS / PPS
        }
        case AV_CODEC_ID_VVC: {
            const uint8_t type = static_cast<uint8_t>((b1 >> 3) & 0x1F);
            return type == 14 || type == 15 || type == 16;  // VPS / SPS / PPS
        }
        default:
            return false;
    }
}

}  // namespace

void InBandParameterSetProbe::Reset(int codec_id, const Options& options) {
    codec_id_ = codec_id;
    options_ = options;

    // 只有真的有参数集解析器的编码才值得攒包：其余编码攒到最后也解不出东西。
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_VVC:
        case AV_CODEC_ID_AV1:
        // VP9：**不攒包**。一个包就是一个访问单元（一张图），而帧头要么在第 1 个
        // 包里、要么只在这包所在的关键帧里。把 P 帧字节攒到一块只会让 LooksLikeKeyFrame
        // 更晚找回同步。所以 VP9 走"逐包单独试"的分支（见 Feed()）。
        case AV_CODEC_ID_VP9:
            enabled_ = true;
            break;
        default:
            enabled_ = false;
            break;
    }

    done_ = false;
    given_up_ = false;
    give_up_reason_.clear();
    packets_ = 0;
    retries_ = 0;
    scan_pos_ = 0;
    pending_incomplete_ = false;
    last_try_size_ = 0;
    buffer_.clear();
    has_container_metadata_ = false;
    container_metadata_ = ContainerMetadata();
    result_ = model::BitstreamAnalysisResult();
}

void InBandParameterSetProbe::SetContainerMetadata(const ContainerMetadata& metadata) {
    container_metadata_ = metadata;
    has_container_metadata_ = true;
}

bool InBandParameterSetProbe::ShouldTryAv1() const {
    // AV1 没有起始码可扫，按 1/2/4/8/... 的对数间隔试解：
    // 序列头 OBU 几乎总在最前面几个包里，13 次尝试足够覆盖 4000 个包。
    return packets_ > 0 && (packets_ & (packets_ - 1)) == 0;
}

bool InBandParameterSetProbe::TryParse() {
    BitstreamAnalyzer analyzer;
    if (has_container_metadata_) {
        analyzer.SetContainerMetadata(container_metadata_);
    }
    result_ = analyzer.AnalyzeInBand(buffer_.data(), buffer_.size(), codec_id_);
    // parameter_set_source 只在真的解析出参数集时才非空（见 AnalyzeInBand）：
    // 用它当成功判据，而不是 analyzed —— analyzed 只表示"跑过解析"。
    if (!result_.parameter_set_source.empty()) {
        done_ = true;
        return true;
    }
    return false;
}

bool InBandParameterSetProbe::ScanAnnexBForParameterSet(const uint8_t* data, size_t size) {
    const size_t header_len = (codec_id_ == AV_CODEC_ID_H264) ? 1u : 2u;
    size_t i = (scan_pos_ > kScanOverlap) ? scan_pos_ - kScanOverlap : 0;
    bool found = false;
    for (; i + 3 + header_len <= size; ++i) {
        if (data[i] != 0x00 || data[i + 1] != 0x00 || data[i + 2] != 0x01) continue;
        // 00 00 00 01 会在这里命中 i+1 那一组，不影响判定。
        const size_t header_at = i + 3;
        const uint8_t b0 = data[header_at];
        const uint8_t b1 = (header_len >= 2) ? data[header_at + 1] : 0;
        if (IsParameterSetNalType(codec_id_, b0, b1)) {
            found = true;
            break;
        }
    }
    // 下一包从"本次末尾 - 重叠"开始扫：起始码/NAL header 可能被包边界切开。
    scan_pos_ = size;
    return found;
}

// VP9 走另一套策略：**一个包就是一张图，攒 buffer 毫无意义**，所以直接把当前包
// 交给 analyzer（而不是把 buffer_ 喂进去）。P 帧的字节攒到一块既拼不出关键帧，
// 还会让 LooksLikeKeyFrame() 在错位的数据里徒劳地找同步码。
//
// 给包数单独设一个比 max_packets（默认 4000）小得多的上限：WebM/IVF 的第一个
// video block 必然是关键帧，正常情况第 1 个包就命中；连扫 256 个包还没命中，
// 基本可以断定"这段流找不到关键帧"（或者它根本不是 VP9）。
bool InBandParameterSetProbe::TryParseVp9(const uint8_t* data, size_t size) {
    constexpr int kMaxVp9ProbePackets = 256;

    BitstreamAnalyzer analyzer;
    if (has_container_metadata_) {
        analyzer.SetContainerMetadata(container_metadata_);
    }
    result_ = analyzer.AnalyzeInBand(data, size, codec_id_);
    if (!result_.parameter_set_source.empty()) {
        done_ = true;
        return true;
    }
    // 攒了这么多包还没碰上关键帧，继续扫下去只是白烧 CPU
    if (packets_ >= kMaxVp9ProbePackets) {
        given_up_ = true;
        give_up_reason_ = "已扫描 " + std::to_string(kMaxVp9ProbePackets) +
                          " 个 VP9 包仍未见到关键帧（序列配置只写在关键帧头部）";
    }
    return false;
}

bool InBandParameterSetProbe::Feed(const uint8_t* data, size_t size) {
    if (done_) return true;
    if (!enabled_ || given_up_) return false;
    if (data == nullptr || size == 0) return false;

    if (options_.max_packets > 0 && packets_ >= options_.max_packets) {
        given_up_ = true;
        give_up_reason_ = "已扫满 " + std::to_string(options_.max_packets) +
                          " 个包仍未见参数集（参数集可能只在流开头写过一次）";
        return false;
    }
    ++packets_;

    // VP9 与 AnnexB/AV1 两条路径不共享"攒到足够多字节再试一次"的假设，
    // 必须在接触 buffer_ 之前就把它分出去。
    if (static_cast<AVCodecID>(codec_id_) == AV_CODEC_ID_VP9) {
        return TryParseVp9(data, size);
    }

    const size_t room =
        (options_.max_bytes > buffer_.size()) ? options_.max_bytes - buffer_.size() : 0;
    if (room == 0) {
        given_up_ = true;
        give_up_reason_ = "带内数据已攒到上限 " + std::to_string(options_.max_bytes) +
                          " 字节仍未见参数集";
        return false;
    }
    buffer_.insert(buffer_.end(), data, data + (size < room ? size : room));

    const bool is_av1 = (static_cast<AVCodecID>(codec_id_) == AV_CODEC_ID_AV1);
    bool should_try = false;
    if (is_av1) {
        should_try = ShouldTryAv1();
    } else if (ScanAnnexBForParameterSet(buffer_.data(), buffer_.size())) {
        should_try = true;
    } else if (pending_incomplete_ && retries_ < options_.max_retries &&
               buffer_.size() >= last_try_size_ + options_.retry_after_bytes) {
        // 上一轮看到过参数集起始码但没解出来（SPS 被 TS 的 184 字节载荷切开）：
        // 补齐更多字节后再试，频率由 retry_after_bytes 控制，不会退化成逐包解析。
        should_try = true;
    }

    if (!should_try) return false;

    if (TryParse()) return true;

    if (!is_av1) {
        // 记下"曾经看到过参数集但没解成"，供上面的补齐重试判据使用
        pending_incomplete_ = true;
        last_try_size_ = buffer_.size();
        ++retries_;
    }
    return false;
}

}  // namespace videoeye
