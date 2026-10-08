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

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisTypes.h"

namespace videoeye {
namespace ui {

// 本地文件的版本指纹（大小 + 修改时间）。
// 网络源（http/rtmp/rtsp...）没有本地指纹，valid 一列为 false —— 于是"网络源默认
// 重新分析"是这套判定的自然结果，不需要在调用处再写一条 if。
struct FileVersionStamp {
    bool valid = false;        // 是否取到本地指纹（网络源 / 文件已删除 / 读取失败 => false）
    std::uint64_t size = 0;    // 字节数
    std::int64_t mtime_s = 0;  // 修改时间（时间点计数，仅用于同机比较是否变化）

    bool operator==(const FileVersionStamp& other) const {
        if (valid != other.valid) return false;
        return !valid || (size == other.size && mtime_s == other.mtime_s);
    }
};

// 取本地文件版本指纹。只做 stat，不读内容；任何失败（含网络 URI）都返回 valid=false。
inline FileVersionStamp QueryFileVersion(const std::string& path) {
    FileVersionStamp stamp;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return stamp;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return stamp;
    stamp.size = static_cast<std::uint64_t>(size);
    // file_time_type 的纪元与 system_clock 未必相同，但这里只做"两次取值是否相等"的比较，
    // 只要同机同 API 取值就自洽。
    stamp.mtime_s = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count());
    stamp.valid = true;
    return stamp;
}

// 一次分析结果的"身份戳"：属于哪个文件（哪个版本）、按哪个模板、用哪套分析参数。
struct ResultStamp {
    std::string path;        // 分析时对应的媒体路径
    std::string profile_id;  // 分析时使用的模板 id
    bool valid = false;      // 是否成功产出过结果（失败 / 取消 / 超时都保持 false）

    // ---- 复用判定所需的额外身份（"谨慎复用报告页结果"）----
    AnalysisOptions options;                                             // 产出这份结果所用的分析参数
    model::AnalysisStatus scan_status = model::AnalysisStatus::Failed;   // 扫描终态
    FileVersionStamp version;                                            // 分析时的本地文件版本
};

// 结果是否仍匹配当前 (文件, 模板)：有效 + 当前文件非空 + 路径与模板都一致。
inline bool IsResultFresh(const ResultStamp& stamp,
                          const std::string& current_path,
                          const std::string& current_profile_id) {
    return stamp.valid && !current_path.empty() && stamp.path == current_path &&
           stamp.profile_id == current_profile_id;
}

// 所需的分析选项是否都被缓存的选项覆盖。
//
// 模板只决定"要不要跑某个分析维度"（见 QcProfileMapper::OptionsForDepth），所以覆盖判定
// 只比这些开关：被要求的分析必须已经在缓存结果里跑过，采样粒度不能比要求的更粗。
// 各维度内部的阈值（如音频门限）不参与判定 —— 它们影响的是**判定**而非**采集**，
// 换模板时本来就由规则引擎重新套用。
inline bool AnalysisOptionsCovers(const AnalysisOptions& cached,
                                  const AnalysisOptions& required) {
    if (required.analyze_bitrate_gop && !cached.analyze_bitrate_gop) return false;
    if (required.decode_frame_types && !cached.decode_frame_types) return false;
    if (required.analyze_audio_qc && !cached.analyze_audio_qc) return false;
    if (required.analyze_color_hdr && !cached.analyze_color_hdr) return false;
    if (required.analyze_bitstream && !cached.analyze_bitstream) return false;
    if (required.analyze_mp4_sample_table && !cached.analyze_mp4_sample_table) return false;
    if (required.analyze_streaming_package && !cached.analyze_streaming_package) return false;
    if (required.analyze_subtitle && !cached.analyze_subtitle) return false;
    if (required.analyze_timecode && !cached.analyze_timecode) return false;
    if (required.analyze_aux_data && !cached.analyze_aux_data) return false;
    // 采样间隔：缓存比要求更粗（数值更大）时序列点更少，可能不足以支撑判定。
    return cached.sample_interval_seconds <= required.sample_interval_seconds;
}

// 是否可复用缓存的原始 AnalysisResult 并按新模板重算规则（不重跑 FFmpeg）。
// 三个条件缺一不可:
//   1) 同一文件版本：本地文件要求 size + mtime 一致；网络源没有指纹，直接不复用；
//   2) 所需分析选项已被缓存选项覆盖；
//   3) 上次扫描完整成功（Complete / Sampled；Cancelled / Failed 不算）。
inline bool CanReuseAnalysis(const ResultStamp& stamp,
                             const std::string& current_path,
                             const FileVersionStamp& current_version,
                             const AnalysisOptions& required_options) {
    if (!stamp.valid || current_path.empty() || stamp.path != current_path) return false;
    // 单侧取不到指纹（网络源 / 文件已变化到读不到）时保守地重新分析。
    if (!stamp.version.valid || !current_version.valid) return false;
    if (!(stamp.version == current_version)) return false;
    if (stamp.scan_status != model::AnalysisStatus::Complete &&
        stamp.scan_status != model::AnalysisStatus::Sampled) {
        return false;
    }
    return AnalysisOptionsCovers(stamp.options, required_options);
}

}  // namespace ui
}  // namespace videoeye
