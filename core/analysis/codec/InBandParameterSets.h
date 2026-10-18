#pragma once

// 带内参数集探测（extradata 为空的封装用）。
//
// 为什么需要它：MP4/MKV 把参数集放在 extradata（avcC/hvcC/av1C）里，读一次就够；
// 而 MPEG-TS / 裸流（AnnexB ES）/ fMP4 媒体分片的 extradata **恒为空**，参数集只
// 写在码流里（每个 IDR 前重复的 SPS/PPS/VPS）。在 2026-10 之前 BitstreamAnalyzer
// 只认 extradata，于是这些封装一律落 NoExtradata，码流分析整块不可用 ——
// 而它们恰好是点播/HLS 分片与直播流的主战场。
//
// 这一层只负责两件事：
//   1. 决定"什么时候值得解一次"（逐包解析会把同一段 SPS 解 N 次）；
//   2. 攒包与上限（长 GOP 的流要等到下一个 IDR 才重复参数集）。
// 真正的语法解析仍在 BitstreamAnalyzer::AnalyzeInBand()，本类不重复实现。
//
// 判据设计：AnnexB 用一次 O(n) 的起始码扫描看有没有 SPS/PPS/VPS 类型的 NAL；
// AV1 没有起始码，按对数间隔（1,2,4,8,...）试解，序列头几乎总在最前面几个包。
// 两种判据都只在"可能成功"时才付出整段解析的代价。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/domain/model/BitstreamInfo.h"

namespace videoeye {

class InBandParameterSetProbe {
public:
    struct Options {
        // 最多攒多少字节带内数据。默认 8 MiB：默认 GOP（10 s）在 6 Mbps 下约 7.5 MB，
        // 上限给小了会在"参数集只在 IDR 出现"的流上直接判失败。
        size_t max_bytes = 8u * 1024u * 1024u;
        // 最多看多少个包（与 max_bytes 先到先算）
        int max_packets = 4000;
        // "参数集起始码出现了但没解出来"时（SPS 被 184 字节 TS 载荷切开），
        // 每多攒这么多字节再试一次；这个数字越大重试越少
        size_t retry_after_bytes = 64u * 1024u;
        // 上述重试的总次数上限（防止畸形码流把 CPU 吃满）
        int max_retries = 32;
    };

    void Reset(int codec_id, const Options& options = Options{});

    // 容器层快照，供"码流 vs 容器"一致性比对。必须在第一次 Feed() 之前设置。
    void SetContainerMetadata(const ContainerMetadata& metadata);

    // 该编码有没有参数集解析器（H.264 / HEVC / VVC / AV1 之外的编码不启用）
    bool enabled() const { return enabled_; }
    // 是否已经有结果（Feed 返回过 true）
    bool done() const { return done_; }
    // 是否已经放弃（超出字节/包上限，或重试次数耗尽）
    bool given_up() const { return given_up_; }
    // 已经喂了多少个包
    int packets() const { return packets_; }
    // 已经攒了多少字节
    size_t buffered_bytes() const { return buffer_.size(); }

    // 喂一个视频包（一个访问单元）。返回 true = 参数集已解析出来，可以收结果。
    // 返回 false 且 given_up()=true 表示别再喂了。
    bool Feed(const uint8_t* data, size_t size);

    // 仅在 done() 为 true 时有效
    const model::BitstreamAnalysisResult& result() const { return result_; }

    // 给调用方拼日志/说明用
    std::string GiveUpReason() const { return give_up_reason_; }

private:
    // AnnexB：在 [scan_pos_ - overlap, size) 里扫起始码并按 NAL 类型判参数集
    bool ScanAnnexBForParameterSet(const uint8_t* data, size_t size);
    // AV1：按对数间隔判断这次要不要试
    bool ShouldTryAv1() const;
    // 真正解一次；成功则填 result_ / done_
    bool TryParse();

    int codec_id_ = 0;
    Options options_;
    bool enabled_ = false;
    bool done_ = false;
    bool given_up_ = false;
    std::string give_up_reason_;

    int packets_ = 0;
    int retries_ = 0;
    // 增量扫描位置（下一页从 scan_pos_ - kScanOverlap 开始，起始码可能跨包）
    size_t scan_pos_ = 0;
    // "参数集起始码出现过但解析失败" —— 只在补齐更多字节后再重试
    bool pending_incomplete_ = false;
    size_t last_try_size_ = 0;

    std::vector<uint8_t> buffer_;
    ContainerMetadata container_metadata_;
    bool has_container_metadata_ = false;
    model::BitstreamAnalysisResult result_;
};

}  // namespace videoeye
