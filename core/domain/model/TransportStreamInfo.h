#pragma once

// MPEG-TS 传输层体检结果。
//
// 为什么单独成一个值对象，而不是直接让规则去读 ContainerStructureResult：
//   * 容器结构页要的是整棵 PSI 树（几百个节点），QC 只要这十来个数；
//   * ContainerStructureResult 属于展示模型，规则层依赖它会把"报告口径"和
//     "界面结构"绑死 —— 以后改树就得改规则。
// 所以 TsStructureAnalyzer 在解析完顺手产出一份摘要，两条路径共用同一组数字。
//
// 口径提醒：所有字段都是"扫描窗口内的事实"。scan_complete=false 时必须连
// scanned_packets / estimated_packets 一起读，否则会把"前 20 万包没出错"
// 当成"整个文件没出错" —— 这正是这一层最容易骗人的地方。

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

struct TransportStreamInfo {
    bool analyzed = false; // 有没有真的跑过 TS 传输层扫描

    // 输入是不是"适用传输层体检"的：本地文件 + iformat 是 mpegts + 选项没关。
    //
    // 为什么要把 analyzed 和 applicable 分开：报告层只靠 analyzed 分不出
    // "这是 MP4，压根不该扫" 和 "这是 TS，但扫描失败了" —— 两者 analyzed 都是 false。
    // 而后者恰恰是最需要说清楚的：一条 TS 流没拿到传输层结论，上面所有
    // container.ts.* 规则的"未命中"都不代表"没问题"，只代表"没有证据"。
    // 报告里 applicable=true && analyzed=false 必须显式告警，不能静默略过。
    bool applicable = false;

    // ---- 扫描范围 ----
    int packet_size = 0; // 步长：188 / 192(M2TS) / 204(RS)
    int64_t scanned_packets = 0;
    int64_t estimated_packets = 0; // 文件大小 / 步长
    bool scan_complete = false;    // true = 扫到了文件末尾

    // ---- 节目与流表 ----
    int programs = 0; // PAT 里的节目数（0 = 没解析到，见 has_pat）
    int streams = 0;  // 各 PMT 的 ES 总数
    int services = 0; // SDT 服务数
    bool has_pat = false;
    bool has_pmt = false;
    bool has_sdt = false;

    // ---- 传输层异常 ----
    int64_t sync_loss = 0;         // 失步后重同步的次数
    int64_t continuity_errors = 0; // 连续计数错误总数（已剔除 discontinuity 标记）
    int64_t scrambled_packets = 0; // 加扰包数
    double pcr_max_gap_ms = 0.0;   // 各 PCR PID 的最大 PCR 间隔（0 = 没采到 PCR）

    // ---- PES 载荷级（TsStructureOptions::scan_pes_payload）----
    //
    // 只读包头看不出下面两件事：视频一路没有随机接入点、某个 PID 中途断了。
    // 这两个字段都是"扫描窗口内的事实"，与 pcr_max_gap_ms 同口径。
    bool pes_payload_scanned = false;
    int pes_streams_checked = 0; // 参与载荷级跟踪的 ES PID 数（视频 + 音频）

    // 最长的一段"没有任何 IDR/CRA"的视频时长。0 = 无从判定（没有视频 PID / 都只采到 1 个 PES）。
    // 只在 H.264 / HEVC 上计算 —— 其它编码的随机接入点标记不同，宁缺勿错。
    double max_no_idr_interval_ms = 0.0;
    std::string max_no_idr_pid;     // 上述间隔出在哪个 PID（已格式化成 0x....）
    int64_t idr_count = 0;          // 全窗口识别到的 IDR/CRA 总数
    int video_pids_without_idr = 0; // 整段窗口一个 IDR 都没看到的视频 PID 数

    // 同一 PID 相邻 PES 的最大时间间隔（含"该 PID 提前断流到窗口末尾"的情形）。
    double max_pid_gap_ms = 0.0;
    std::string max_pid_gap_pid;
    int stalled_pids = 0; // 间隔超过阈值的 PID 数（阈值在规则里）

    // analyzed=false 时的原因（文件打不开 / 不是 TS / 取消）
    std::string error_message;

    // 连续计数错误率（百分比）。分母用已扫包数，避免拿"窗口内 1 个错误"
    // 去和一个固定的绝对阈值比 —— 长文件的偶发丢包不该和短文件的严重损毁同罪。
    double ContinuityErrorPercent() const {
        if (scanned_packets <= 0)
            return 0.0;
        return 100.0 * static_cast<double>(continuity_errors) / static_cast<double>(scanned_packets);
    }
};

} // namespace model
} // namespace videoeye
