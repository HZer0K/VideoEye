#pragma once

// 参数集全程追踪器（把"首次配置解析"升级为"配置时间线"）。
//
// 与 InBandParameterSetProbe 的分工（不要合并，见两个文件头的说明）：
//   * InBandParameterSetProbe —— 攒字节直到**第一次**解析成功，产出配置快照
//     （BitstreamAnalysisResult）。它的 done_ 一旦置位就不再工作。
//   * ParameterSetTracker    —— 逐包轻量扫描，只关心"参数集的 id/内容有没有变"，
//     产出 ParameterTimeline。它**不做**完整参数集解析（只解 SPS 拿到宽高/位深，
//     PPS/VPS 只读到 id 与内容摘要），因此可以全程跑而不拖慢扫描。
//
// 为什么不用"重复调用 BitstreamAnalyzer::AnalyzeInBand"来实现：
//   那需要把从文件头到当前的所有字节都攒着（几十 MB 到几 GB），而且要重解一遍全部
//   NAL；本类只对**新出现的参数集 NAL** 做一次摘要 + 一次 SPS 解析，成本与包数线性
//   相关而不是平方。
//
// 三个产出（对应三件不同的事）：
//   1. parameter_sets  —— 按 (类型, id) 的参数集登记表（含内容摘要）
//   2. changes         —— 配置变化事件（同 id 被替换 / 新增了不同配置的 id）
//   3. references      —— slice 引用了哪份 PPS，以及是否引用了**从未传输**的 PPS
//
// 第 3 项是硬证据：一个 slice 引用了从未出现过的 PPS，解码器必然失败或报错，
// 这比任何"看起来像花屏"的启发式判据都可靠。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/ParameterTimeline.h"
// 签名里出现的是 media 层的 videoeye::NalUnit / ObuUnit（ExtradataParser 的产物）。
// ❗ 不要换成 core/domain/model/BitstreamUnits.h —— 那边是 model:: 命名空间下的
// 同名结构（结果快照），include 错了拿到的报错是"missing type specifier"，很难查。
#include "core/media/codec/ExtradataTypes.h"

namespace videoeye {

class ParameterSetTracker {
public:
    // NAL 封装方式（与 VideoEsInspector 同源问题：MP4 的媒体包没有起始码）
    enum class Framing {
        Auto = 0,       // 逐包判定：先看起始码，再看长度前缀链是否自洽
        AnnexB,         // 强制按起始码切
        LengthPrefixed, // 强制按 prefix_bytes 宽度切
    };

    struct Options {
        // 每流最多登记多少份参数集（防御畸形流把内存吃光）
        int max_parameter_sets = 256;
        // 最多记录多少个变化事件（超出后置 truncated）
        int max_changes = 256;
        // 最多扫多少个包（0 = 不限）。默认不限：本类单包成本只有一次 NAL 扫描。
        int64_t max_packets = 0;
    };

    void Reset(int codec_id, int stream_index) {
        Reset(codec_id, stream_index, Options{});
    }
    void Reset(int codec_id, int stream_index, const Options& options);

    // H.264 / HEVC / AV1 才有意义（VVC/VP9 暂未接入，理由见 .cpp）
    bool enabled() const {
        return enabled_;
    }
    // 为什么没启用（给上层写日志/note 用）
    const std::string& disabled_reason() const {
        return disabled_reason_;
    }

    // prefix_bytes: 0 = 未知（Auto 时按 4 → 2 → 1 依次试）
    void SetFraming(Framing framing, int prefix_bytes = 0);

    // 喂一个视频包。
    //   pts_seconds   : 包 PTS 换算成秒（无 PTS 传负数）
    //   packet_index  : 包序号（调用方的全局计数，用于定位）
    //   file_offset   : 包在文件内的偏移（-1 = 未知）
    void Feed(const uint8_t* data, size_t size, double pts_seconds, int64_t packet_index, int64_t file_offset);

    // 用容器 extradata 里的参数集登记"初始配置"。
    //
    // 为什么必须有这一条：MP4/MKV 把参数集写进 avcC/hvcC/av1C 之后，编码器就不再
    // 在码流里重复它们。只扫包的话，参数集登记表在主力封装上恒为空 ——
    // 表现出来是"没有检测到配置变化"，而真相是"一次参数集都没见过"。
    //
    // 这里登记的参数集是**基线**，不是变化事件：第一份登记进去的 SPS 不会被
    // 算成"从无到有的配置切换"（RegisterSet 只对"已有基线且内容不同"才产事件）。
    // 时间戳与文件偏移一律按"未知"记（extradata 既没有 PTS 也不在媒体数据区里）。
    //
    // 返回是否至少登记到了一份参数集。
    bool FeedExtradata(const uint8_t* data, size_t size);

    // 收尾：把"引用但未传输"的参数集统计出来。可重复调用（幂等）。
    void Finish();

    const model::ParameterTimeline& result() const {
        return timeline_;
    }
    model::ParameterTimeline& mutable_result() {
        return timeline_;
    }

private:
    void FeedH264(const uint8_t* data, size_t size, double pts, int64_t packet_index, int64_t file_offset);
    void FeedHevc(const uint8_t* data, size_t size, double pts, int64_t packet_index, int64_t file_offset);
    void FeedAv1(const uint8_t* data, size_t size, double pts, int64_t packet_index, int64_t file_offset);

    // 单个参数集 NAL 的登记。抽出来是因为"扫包"与"扫 extradata"两条路径
    // 必须用同一份登记逻辑 —— 各写一遍迟早会在摘要口径上分叉。
    bool RegisterH264Sps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset);
    bool RegisterH264Pps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset);
    bool RegisterHevcVps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset);
    bool RegisterHevcSps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset);
    bool RegisterHevcPps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset);

    // 登记（或更新）一份参数集；内容变化时产出变化事件
    void RegisterSet(model::ParameterSetKind kind, int id, const std::string& summary, const uint8_t* payload,
                     size_t size, double pts, int64_t packet_index, int64_t file_offset);
    // 记录一次 slice 引用
    void AddReference(model::ParameterSetKind kind, int id, bool first_slice_of_picture, double pts,
                      int64_t packet_index);

    // 找已登记的 (kind, id)
    model::ParameterSetEntry* FindEntry(model::ParameterSetKind kind, int id);

    // 该包是否是长度前缀封装（Auto 模式下用）
    bool DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const;

    int codec_id_ = 0;
    int stream_index_ = -1;
    Options options_;
    bool enabled_ = false;
    bool finished_ = false;
    std::string disabled_reason_;

    Framing framing_ = Framing::Auto;
    int prefix_bytes_ = 0;
    bool framing_decided_ = false;

    model::ParameterTimeline timeline_;
};

} // namespace videoeye
