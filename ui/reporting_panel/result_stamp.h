#pragma once

// 报告页"结果新鲜度"与"结果复用"的纯逻辑（从 ReportingPanel 抽出，便于单测）。
//
// 报告页导出的单文件报告必须对应当前打开的文件与当前选中的模板：切换文件或模板后，
// 上一次的结果就不再可用（文件内容、分析深度、规则覆盖都可能不同）。把这条判定沉淀成
// 一个不依赖 Qt 的小结构，UI 与单测共用同一份实现，从根上杜绝"把上一次结果导出去"。
// 与 report_path.h 同一思路：能被测试直接调用的逻辑一律抽成自由函数。
//
// 模板切换还有第二种处置：报告页用的是"带分析深度与规则覆盖"的模板（见 QcProfile），
// 换模板只需要**重算规则**，媒体本身不用重扫。缓存里的原始 AnalysisResult 只要满足
// 三个条件就可以复用（CanReuseAnalysis）：同一文件版本、所需分析选项已被覆盖、
// 上次扫描完整成功。复用只重跑 QcRuleEngine::Evaluate，不碰 FFmpeg。

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisTypes.h"

namespace videoeye {
namespace ui {

// 本地文件的版本指纹（大小 + 修改时间）。
// 网络源（http/rtmp/rtsp...）没有本地指纹，valid 一列为 false —— 于是"网络源默认
// 重新分析"是这套判定的自然结果，不需要在调用处再写一条 if。
//
// 修改时间取**纳秒**精度而不是秒：同一秒内被替换成大小相同的另一个文件（转录工具
// 重导出、脚本覆盖、拷回旧版本）在秒级指纹下是"没变"，于是复用上一份结果导出去。
// 这里只做"两次取值是否相等"的比较，不换算成挂钟时间，所以 file_time_type 的纪元
// 与 system_clock 是否一致都无所谓 —— 同机同 API 取值就自洽。
struct FileVersionStamp {
    bool valid = false;        // 是否取到本地指纹（网络源 / 文件已删除 / 读取失败 => false）
    std::uint64_t size = 0;    // 字节数
    std::int64_t mtime_ns = 0; // 修改时间（纳秒计数，仅用于同机比较是否变化）

    bool operator==(const FileVersionStamp& other) const {
        if (valid != other.valid)
            return false;
        return !valid || (size == other.size && mtime_ns == other.mtime_ns);
    }
};

// 取本地文件版本指纹。只做 stat，不读内容；任何失败（含网络 URI）都返回 valid=false。
inline FileVersionStamp QueryFileVersion(const std::string& path) {
    FileVersionStamp stamp;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return stamp;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return stamp;
    stamp.size = static_cast<std::uint64_t>(size);
    stamp.mtime_ns = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count());
    stamp.valid = true;
    return stamp;
}

// 一次分析结果的"身份戳"：属于哪个文件（哪个版本）、按哪个模板、用哪套分析参数。
struct ResultStamp {
    std::string path;       // 分析时对应的媒体路径
    std::string profile_id; // 分析时使用的模板 id
    bool valid = false;     // 是否成功产出过结果（失败 / 取消 / 超时都保持 false）

    // ---- 复用判定所需的额外身份（"谨慎复用报告页结果"）----
    AnalysisOptions options;                                           // 产出这份结果所用的分析参数
    model::AnalysisStatus scan_status = model::AnalysisStatus::Failed; // 扫描终态
    FileVersionStamp version;                                          // 分析时的本地文件版本
};

// 一次分析**发起时**冻结的输入身份（路径 + 本地文件版本）。
//
// 为什么不"分析完了再取一次版本"：那样取到的是**完成时**的文件，而结果对应的是
// 分析期间读到的那份内容 —— 文件在被分析的过程中被替换（脚本重导出、拷回旧版本）
// 时，两者是不同的文件，却会被登记成"当前文件的结果"。冻结的版本才是这份结果的
// 真实身份；完成时只负责**复核**（IdentityStillHolds）。
struct InputIdentity {
    std::string path;
    FileVersionStamp version;

    // 与"现在的这个文件"是否还是同一个输入。
    // 两侧都取不到指纹（网络源）时退化为只比路径 —— 网络源没有版本可言，
    // 这是唯一能做的判定，也是"网络源不复用缓存"这条口径的落点（见 CanReuseAnalysis）。
    bool Matches(const std::string& current_path, const FileVersionStamp& current_version) const {
        if (path != current_path)
            return false;
        if (!version.valid && !current_version.valid)
            return true;
        return version.valid && current_version.valid && version == current_version;
    }
};

inline InputIdentity FreezeInputIdentity(const std::string& path) {
    return InputIdentity{path, QueryFileVersion(path)};
}

// 结果是否仍匹配当前 (文件, 模板)：有效 + 当前文件非空 + 路径与模板都一致 +
// **文件版本一致**。
//
// 版本这一条是评审 P2-5 补的：以前只比路径和模板 id，于是"分析完之后文件被替换成
// 另一个同名文件"仍会被判定为新鲜，导出按钮照常可点 —— 导出去的报告对不上当前文件。
inline bool IsResultFresh(const ResultStamp& stamp, const std::string& current_path,
                          const std::string& current_profile_id, const FileVersionStamp& current_version) {
    if (!stamp.valid || current_path.empty() || stamp.path != current_path || stamp.profile_id != current_profile_id)
        return false;
    // 双方都取不到指纹（网络源 / 文件已不在）时只比路径与模板 —— 没有版本可比。
    if (!stamp.version.valid && !current_version.valid)
        return true;
    return stamp.version.valid && current_version.valid && stamp.version == current_version;
}

// 便捷重载：自己取一次当前文件版本（调用方手上没有指纹时用）。
inline bool IsResultFresh(const ResultStamp& stamp, const std::string& current_path,
                          const std::string& current_profile_id) {
    return IsResultFresh(stamp, current_path, current_profile_id, QueryFileVersion(current_path));
}

// ---- 覆盖判定：采集开关表 + 采样上限 ----
//
// 参与判定的只有"决定原始 AnalysisResult 里有没有某个维度数据"的开关与上限：
// 被要求的分析必须已经在缓存结果里跑过，采样粒度不能比要求的更粗。
// 各维度内部的阈值（音频门限、GOP 上限…）不参与 —— 它们影响的是**判定**而非**采集**，
// 换模板时本来就由规则引擎重新套用。
//
// ❗ **表驱动而不是手工逐个 if**：手工比对漏过五个开关（帧顺序、音频位流、参数时间轴、
// TS 传输层、容器体检）—— 它们恰好在当前内置深度模板里取值相同，所以一直没暴露，
// 但任何新增模板/开关都可能踩到。现在"加一个采集开关 = 表里加一行"，
// 而"表里有没有漏"由单测逐字段核对（见 test_reporting_helpers.cpp）。
using AnalysisOptionFlag = bool AnalysisOptions::*;

inline const std::vector<AnalysisOptionFlag>& CoveredAnalysisFlags() {
    static const std::vector<AnalysisOptionFlag> flags = {
        &AnalysisOptions::detect_container_layout,    &AnalysisOptions::analyze_bitrate_gop,
        &AnalysisOptions::decode_frame_types,         &AnalysisOptions::analyze_audio_qc,
        &AnalysisOptions::audio_qc_all_tracks,        &AnalysisOptions::analyze_audio_bitstream,
        &AnalysisOptions::analyze_color_hdr,          &AnalysisOptions::analyze_bitstream,
        &AnalysisOptions::analyze_parameter_timeline, &AnalysisOptions::analyze_frame_order,
        &AnalysisOptions::analyze_mp4_sample_table,   &AnalysisOptions::analyze_streaming_package,
        &AnalysisOptions::analyze_subtitle,           &AnalysisOptions::analyze_timecode,
        &AnalysisOptions::analyze_aux_data,           &AnalysisOptions::analyze_ts_transport,
        &AnalysisOptions::analyze_legacy_container,
    };
    return flags;
}

// 上限类的覆盖关系：0 在很多字段里表示"不限制"，所以不能简单比大小 ——
// "缓存不限、要求有限"是覆盖，"缓存有限、要求不限"不是。
inline bool AnalysisOptionsCoversLimits(const AnalysisOptions& cached, const AnalysisOptions& required) {
    // 采样间隔：缓存比要求更粗（数值更大）时序列点更少，可能不足以支撑判定。
    if (cached.sample_interval_seconds > required.sample_interval_seconds)
        return false;
    // 包数上限（0 = 不限制）：要求全扫时缓存是抽样 -> 不覆盖。
    if (required.max_packets == 0) {
        if (cached.max_packets != 0)
            return false;
    } else if (cached.max_packets != 0 && cached.max_packets < required.max_packets) {
        return false;
    }
    // 音轨上限：缓存少测了几条轨（多音轨文件）-> 不覆盖。
    if (cached.audio_qc_max_tracks < required.audio_qc_max_tracks)
        return false;
    // TS 传输层包数上限（<=0 = 扫全文）：要求扫全文而缓存只扫了头 -> 不覆盖。
    if (required.ts_transport_max_packets <= 0) {
        if (cached.ts_transport_max_packets > 0)
            return false;
    } else if (cached.ts_transport_max_packets > 0 &&
               cached.ts_transport_max_packets < required.ts_transport_max_packets) {
        return false;
    }
    // 曲线点数上限（0 = 不限制）：缓存被降采样过 -> 曲线比要求的稀。
    if (required.timeline_max_curve_points == 0) {
        if (cached.timeline_max_curve_points != 0)
            return false;
    } else if (cached.timeline_max_curve_points != 0 &&
               cached.timeline_max_curve_points < required.timeline_max_curve_points) {
        return false;
    }
    return true;
}

inline bool AnalysisOptionsCovers(const AnalysisOptions& cached, const AnalysisOptions& required) {
    for (const AnalysisOptionFlag flag : CoveredAnalysisFlags()) {
        // ❗ 括号不能省：`.*` 的优先级低于 `!`，写成 `!cached.*flag` 会被解析成
        // `(!cached).*flag` —— 编译不过还算好，悄悄取反错对象才致命。
        if ((required.*flag) && !(cached.*flag))
            return false;
    }
    return AnalysisOptionsCoversLimits(cached, required);
}

// 是否可复用缓存的原始 AnalysisResult 并按新模板重算规则（不重跑 FFmpeg）。
// 三个条件缺一不可:
//   1) 同一文件版本：本地文件要求 size + mtime 一致；网络源没有指纹，直接不复用；
//   2) 所需分析选项已被缓存选项覆盖；
//   3) 上次扫描完整成功（Complete / Sampled；Cancelled / Failed 不算）。
inline bool CanReuseAnalysis(const ResultStamp& stamp, const std::string& current_path,
                             const FileVersionStamp& current_version, const AnalysisOptions& required_options) {
    if (!stamp.valid || current_path.empty() || stamp.path != current_path)
        return false;
    // 单侧取不到指纹（网络源 / 文件已变化到读不到）时保守地重新分析。
    if (!stamp.version.valid || !current_version.valid)
        return false;
    if (!(stamp.version == current_version))
        return false;
    if (stamp.scan_status != model::AnalysisStatus::Complete && stamp.scan_status != model::AnalysisStatus::Sampled) {
        return false;
    }
    return AnalysisOptionsCovers(stamp.options, required_options);
}

} // namespace ui
} // namespace videoeye
