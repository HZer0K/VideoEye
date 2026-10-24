#pragma once

// 参数集时间线：把"首次配置解析"升级成"全程配置追踪"。
//
// 背景问题（2026-10 审计，见 InBandParameterSets.h）：
//   InBandParameterSetProbe 的语义是"攒到第一次解析成功就收工"—— done_ 一置位，
//   后面所有包直接短路；AnalysisPipeline 更是成功即把 probe 从列表里 erase。
//   于是整条分析链只回答"文件开头是什么配置"，而"中途换了什么配置"永远看不到。
//   而中途换配置恰恰是拼接、编码器重启、码率/分辨率切换的直接证据。
//
// 本文件描述两条事实：
//   1. 参数集登记表 —— 按 (类型, id) 记录每一份参数集的内容摘要与首次出现位置。
//      同一 id 的内容变了 = 该 id 被**替换**（不是新增），这是最值得报的一类。
//   2. 引用统计 —— 每个 slice 引用哪份 PPS，以及引用的 PPS 是否**从未传输过**。
//      后者是"随机访问点缺失/解码失败"的硬证据，比任何启发式都可靠。
//
// 与 BitstreamAnalysisResult 的分工：
//   BitstreamAnalysisResult 回答"当前生效的配置是什么"（单份快照，下游全用它）；
//   ParameterTimeline 回答"配置怎么变过来的"（序列）。两者都需要，不要合并 ——
//   把快照改成序列会让所有只关心当前值的调用方一起改。
//
// 约束：纯 C++17，不依赖 Qt / FFmpeg。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 参数集种类（按编码归一化到这三种 + 序列头）
enum class ParameterSetKind : uint8_t {
    Unknown = 0,
    Vps, // HEVC / VVC 的 VPS（H.264 没有）
    Sps, // 序列参数集 / AV1 序列头
    Pps, // 图像参数集
    Av1SequenceHeader,
};

const char* ParameterSetKindCode(ParameterSetKind kind);
const char* ParameterSetKindName(ParameterSetKind kind);

// 参数集的"内容变化"事件。
//
// 文本形态（报告/UI 直接用）：
//   视频流 #0：00:12:35.240，1920x1080 / 8 bit -> 3840x2160 / 10 bit；
//   来源：SPS(id=0)；包号 15320；文件偏移 0x1A2B3C
struct ParameterChangeEvent {
    int stream_index = -1;
    ParameterSetKind kind = ParameterSetKind::Unknown;
    int parameter_set_id = -1;

    bool has_time = false;
    double time_seconds = 0.0; // PTS 换算成秒（无 PTS 时为 false）
    int64_t packet_index = -1;
    int64_t file_offset = -1; // 该包在文件内的起始偏移（容器给不出时为 -1）

    std::string source;     // "SPS(id=0)" / "PPS(id=1)" / "AV1 序列头"
    std::string old_config; // 变化前摘要：旧参数集的同一描述
    std::string new_config; // 变化后摘要
    std::string changes;    // 逐字段差异，例如 "分辨率 1920x1080 -> 3840x2160；位深 8 -> 10"

    // true  = 同一 id 的内容被替换（危险：之前解出的帧与之后的帧不是同一套配置）
    // false = 新增了一份不同 id 的参数集（可能只是正常的码率自适应）
    bool replaced_same_id = false;

    // 一行人类可读文本（含时间/包号/偏移）
    std::string ToString() const;
    // 简短形态，用于 UI 列表
    std::string ShortText() const;
};

// 参数集登记表的一行
struct ParameterSetEntry {
    ParameterSetKind kind = ParameterSetKind::Unknown;
    int id = -1;
    // 内容摘要（FNV-1a 64，十六进制）。同一 id 的摘要变化 = 内容被替换。
    std::string digest;
    // 该参数集承载的配置摘要，例如 "1920x1080 / 8 bit / 4:2:0"
    std::string summary;

    int64_t first_packet_index = -1;
    double first_time_seconds = 0.0;
    bool has_time = false;
    int64_t first_file_offset = -1;
    // 出现过多少次（IDR 前重复发送是正常的，这个数用于区分"重复"与"替换"）
    int64_t occurrences = 0;
    // 内容被替换过几次（同一 id 不同摘要）
    int replacements = 0;
};

// 某个 PPS id 的引用统计（由 slice header 得到）
struct ParameterReferenceStat {
    ParameterSetKind kind = ParameterSetKind::Pps;
    int id = -1;
    int64_t frame_count = 0;  // 有多少帧引用它（只数每帧首个 slice）
    int64_t slice_count = 0;  // 有多少个 slice 引用它
    bool transmitted = false; // 是否在参数集登记表里出现过
    // 首次引用早于首次传输 = 引用了从未（或过晚）到达的参数集
    bool referenced_before_transmit = false;
    int64_t first_reference_packet = -1;
    double first_reference_time = 0.0;
    bool has_time = false;
};

// 一条视频轨的完整时间线
struct ParameterTimeline {
    bool analyzed = false;
    int stream_index = -1;
    std::string codec_name;

    std::vector<ParameterSetEntry> parameter_sets;
    std::vector<ParameterChangeEvent> changes;
    std::vector<ParameterReferenceStat> references;

    int64_t packets_scanned = 0;
    int64_t parameter_set_units = 0; // 见过的参数集 NAL/OBU 总数
    // 引用了从未到达的参数集的次数（0 = 未发现；这是硬问题）
    int64_t missing_reference_count = 0;
    // 是否因为上限提前收工（true 时 changes 只覆盖前面一段）
    bool truncated = false;
    std::vector<std::string> notes;

    // 配置文本（当前生效的那份，用于"文件里到底有几套配置"这类展示）
    std::string ActiveSummary() const;
    std::string Summary() const;
};

} // namespace model
} // namespace videoeye
