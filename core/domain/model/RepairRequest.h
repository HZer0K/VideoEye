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

    // 产物容器。Auto = 沿用源扩展名（mov/m4v 保留，其它统一 mp4）。
    RepairOutputContainer output_container = RepairOutputContainer::Auto;

    // 取消 / 超预算时是否保留已经写入的产物前缀。
    // 默认关闭：未完成验证的前缀不能当作"修复完成"发布；开启后产物以
    // *.partial.<ext> 命名（不覆盖既有文件），并在报告里标注为未验证前缀。
    bool keep_partial_on_cancel = false;

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

    // 对源视频轨的时间轴空洞插入**合成黑帧**（切片 U）。
    //
    // 默认关闭，因为它产出的是合成内容：黑帧不是从源解出来的画面，只是把
    // S08 原本会静默压掉的那段时间补回来，让产物时长与音画对齐不因丢帧而漂移。
    // 开启后：① 每处空洞单独记账（位置 / 缺失帧数 / 插了几帧 / 为什么没插）；
    // ② 填补的秒数**不计入**恢复率分子；③ 报告会明确标出「含合成内容」。
    // 只在「救内容」模式（S08 转码链）下生效 —— 保码流路径不重新生成时间轴。
    bool fill_video_gaps_with_black_frames = false;

    // 目标容器拒绝某条音频轨的原编码时，是否解码后重编码成容器能收的编码（切片 V）。
    //
    // 默认**开启**：不转码的替代结果是把整条音轨从产物里剔除 —— 用户拿到的是一份
    // 没有声音的文件，比有损重编码更糟。开启时也必须如实报告：重编码是有损的，
    // 采样率 / 声道若被迫改变逐项写进报告；一个候选都过不了试写头时仍然 drop，
    // 并把每条候选被拒的原因列出来。关闭时该轨按「容器拒绝」剔除并写明原因。
    bool reencode_audio_to_fit_container = true;

    // 显式帧率（切片 F）：只在源侧没有帧率证据时使用 —— 典型是 SPS 里没有
    // VUI timing_info 的 H.264。写成精确分数（30000/1001）而不是 double，
    // 因为 29.97 的十进制近似会把整条时间轴按 ppm 级误差拉偏。
    //
    // 两字段都为 0 = 未指定：此时缺帧率证据的源一律拒绝重建，绝不猜。
    // 只给一个 = 参数不完整，引擎直接拒绝这次修复（不静默当成未指定）。
    uint32_t assumed_frame_rate_num = 0;
    uint32_t assumed_frame_rate_den = 0;

    bool HasAssumedFrameRate() const {
        return assumed_frame_rate_num > 0 && assumed_frame_rate_den > 0;
    }

    // 参数是否被写坏（只给了一半）。引擎在开跑前用它做快速拒绝。
    bool AssumedFrameRateIncomplete() const {
        return (assumed_frame_rate_num == 0) != (assumed_frame_rate_den == 0);
    }

    RepairLimits limits;

    bool write_report = true;
};

} // namespace model
} // namespace videoeye
