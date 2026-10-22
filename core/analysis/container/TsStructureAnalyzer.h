#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {

// MPEG-TS / M2TS / RS(204) 结构解析选项。
//
// 2026-10 重写时把三件事做成可配的：扫描量、失步重同步预算、每个 PID 的采样条数。
// 以前的版本把这三个都写死（5000 包 / 一次失步就放弃 / 8 条），
// 结果是"2 小时的广播流只看了前 0.9 MB"，而结论照样标成成功。
struct TsStructureOptions {
    // 最多扫描多少个 TS 包（<= 0 表示扫完整个文件）。
    // 默认 50 万包 ≈ 94 MB（188 字节包）：足够覆盖 PCR/PID/PES 统计，
    // 又不至于让"打开一个 20 GB 收录文件"变成分钟级操作。
    // 实际扫描量与是否跑完全文件会写进 result.metadata。
    int max_scan_packets = 500000;

    // 失步后最多往后找多少字节仍找不到同步就收手（默认 1 MiB）。
    // 广播流/录播断面里失步是常态，以前遇到一个非 0x47 字节就 break，
    // 整个文件后面的解析全部丢掉。
    int64_t max_resync_bytes = 1 << 20;

    // 每个 PID 最多保留多少条 PES 采样（树里展示 PTS/DTS 用）
    int max_pes_per_pid = 8;

    // 每个 PID 的连续计数错误最多记多少条明细（计数本身不限）
    int max_cc_error_details = 16;

    // ---- PES 载荷级检查（scan_pes_payload）----
    // 只到 PES 头的话，"这条视频一路没有 IDR"和"某个 PID 中途断了"这两类问题
    // 是看不见的：包头一切正常，画面却从头花到尾 / 后半段直接冻住。
    // 做法是在 TS 包里顺带扫 ES 载荷（H.264 / HEVC 的 NAL 起始码），代价接近一次
    // memcpy 级别的线性扫描，不额外读盘。
    bool scan_pes_payload = true;

    // 视频 PID 最长允许多久没有 IDR/CRA（秒）。<= 0 关闭该检查。
    // 只用来判"这一段视频找不到任何随机接入点"——seek 会退化成从头解码。
    double max_no_idr_interval_s = 10.0;

    // 同一 PID 相邻 PES 的最大时间间隔（秒）。<= 0 关闭该检查。
    // 超过即视为断流（PCR/PTS 上表现为该 PID 长时间没有新数据）。
    double max_pid_gap_s = 5.0;
};

/// MPEG-TS 容器结构解析器
///
/// 深度覆盖：
///   * 包长自适应 188 / 192(M2TS, 4 字节 TP_extra_header) / 204(RS 校验)
///   * 失步重同步（不再"遇到一次就整文件放弃"）
///   * PAT / PMT 多 section 组装（PMT 跨包时也能收全流表）
///   * PSI 描述符：registration(AC-3/E-AC-3/HEVC/AV1…)、ISO_639_language、
///     CA_descriptor、AC-3/E-AC-3、teletext、subtitling、stream_identifier
///   * SDT（service_descriptor）给出节目名与提供商
///   * PCR 采样（首末 PCR、最大 PCR 间隔）、连续计数错误、加扰包统计
class TsStructureAnalyzer {
public:
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr,
                 const TsStructureOptions& options = TsStructureOptions{});
};

} // namespace videoeye
