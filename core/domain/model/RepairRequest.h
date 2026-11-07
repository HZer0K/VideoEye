#pragma once

// 一次修复请求。纯值对象：界面、CLI 或自动化都构造它，然后交给 RepairEngine。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace model {

struct RepairLimits {
    // 单次修复的总时间预算（秒）。0 = 不设上限。
    int max_seconds = 0;
    // 超过预算时是否允许用户确认后继续。false 时引擎按 TimedOut 返回。
    bool user_confirmed_exceed = false;
    // 最多写入多少字节。0 = 不设上限。
    uint64_t max_output_bytes = 0;
};

struct RepairRequest {
    std::string request_id;

    std::string source_path;

    // 产物目录。为空时用源文件所在目录（若不可写则修复失败，不做静默降级）。
    std::string output_directory;

    // 显式产物路径。为空时自动命名：<stem>.repaired.<runid>.<ext>。
    std::string output_path;

    // 参考样片 / 初始化段：当源文件是缺少 ftyp+moov 初始化段的 fMP4 媒体分片
    // （styp/sidx/moof/mdat）时，从这里提取初始化段并在重封装前拼接。
    std::string reference_path;

    // 目标流（FFmpeg 流下标）。空 = 自动选择第一条视频轨；没有视频则选第一条音频轨。
    std::vector<int> target_streams;

    RepairMode mode = RepairMode::PreserveBitstream;
    RepairVerificationLevel verification_level = RepairVerificationLevel::V2_FullDecode;

    // ---- 第一阶段恢复能力开关（默认全部开启，单轨抢救需显式允许）----

    // 读到损坏区间后是否继续向后寻找恢复点，把后半段一起抢救回来。
    // false 时退化为"读到错误就停"（旧行为）。
    bool allow_segment_salvage = true;

    // 跳段恢复得到多个片段时，产物时间轴如何处理。
    RepairGapPolicy gap_policy = RepairGapPolicy::PreserveOriginalTimeline;

    // 是否对有证据的时间戳重复 / 回退做显式重写（只统计缺失，不伪造）。
    bool repair_timeline = true;

    // 是否对完全缺失的时间戳做合成填充（PTS/DTS 都为 NOPTS 的包）。
    // 默认关闭：合成时间戳是按上一帧 + 标称帧时长外推的，不是源侧真实时间；
    // 开启后才能把这些包写进产物，但报告的 timeline_status 会标记为 adjusted。
    bool fill_missing_timestamps = false;

    // 允许把"全部样本都不可读"的轨道从输出里剔除（单轨抢救）。
    // 诊断层只有在拿到越界样本证据时才会给出该轨不可抢救的结论。
    bool allow_single_track = true;

    RepairLimits limits;

    bool write_report = true;
};

} // namespace model
} // namespace videoeye
