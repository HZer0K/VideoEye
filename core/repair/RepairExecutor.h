#pragma once

// 执行器：保留码流重封装 + 尾部抢救 + 多段恢复 + 时间轴修复 + 单轨抢救。
//
// 它不验证产物（那是 RepairVerifier 的职责），也不发布产物（那是 RepairOutputWriter）。
// 所有写操作都落在 <final>.part，执行器失败时由引擎负责清理。

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairInitSegment.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairProgress.h"

namespace videoeye {
namespace repair {

struct RepairExecutionOutcome {
    bool ok = false;
    bool canceled = false;
    bool timed_out = false;
    std::string error;

    std::string part_path;
    // 拼接"参考初始化段 + 源分片"用的临时文件（预留与清理都受保护）。
    std::string assembled_path;

    uint64_t packets_read = 0;
    uint64_t packets_written = 0;
    uint64_t bytes_written = 0;
    bool read_error = false;
    bool salvage_truncated = false;
    // 达到 max_output_bytes 后主动停写：产物是有效的可播放前缀，不是超时失败。
    bool output_size_capped = false;
    // 检测到"轨道提前结束"但跳段恢复失败：产物必然缺失中间内容，不能按完整修复上报。
    bool premature_end_unrecovered = false;

    // P1-9：moov 样本表索引不自洽（chunk 越界 / stts / ctts / stss 计数不符）时，
    // 执行前先重建索引正确的临时副本；成功时后续重封装读取该副本。
    bool index_rebuild_attempted = false;
    bool index_table_rebuilt = false;
    uint64_t index_rebuilt_chunks = 0;
    std::string index_rebuild_note;
    std::string index_rebuild_error;

    // P2-12：普通 MP4 无 moov 时，先扫描 mdat 重建样本表，再走重封装。
    bool moov_rebuild_attempted = false;
    bool moov_rebuilt = false;
    uint64_t moov_rebuilt_samples = 0;
    std::string moov_rebuild_note;
    std::string moov_rebuild_error;

    // P2-13：S08 容忍式转码的实测结果。
    bool video_reencoded = false;
    bool audio_reencoded = false;
    // 解码成功但无法稳定重编码，被丢弃的帧数（残缺/GOP 内解码失败的帧）。
    uint64_t transcoded_frames = 0;
    uint64_t dropped_video_frames = 0;
    // 抽帧 PSNR 实测（0 样本时 perceptual_fidelity 保持 unknown_without_reference）。
    uint64_t psnr_samples = 0;
    double psnr_average_db = 0.0;
    std::string transcode_note;

    std::optional<double> output_start_seconds;
    std::optional<double> output_end_seconds;
    // 源侧声明的可播放时长（容器时长或最长轨道时长），用于恢复比例估算。
    double source_duration_seconds = 0.0;

    // 输入容器确为 MPEG-TS（由 demuxer 的 format name 判定，不看扩展名）。
    // TS→MP4 会做 Annex B → AVCC 转换，视频包字节必然变化，不能再用原始包
    // 载荷哈希断言保真；扩展名不可靠：测试里的 fMP4 分片就叫 *_seg.ts。
    bool input_is_mpegts = false;

    std::optional<uint64_t> input_video_payload_hash;
    uint64_t input_video_packet_count = 0;
    // 第一条音频轨的输入包载荷哈希，用于产物音频载荷一致性核对。
    std::optional<uint64_t> input_audio_payload_hash;
    uint64_t input_audio_packet_count = 0;
    int output_video_stream_index = -1;

    std::vector<model::RepairActionExecution> executions;
    std::vector<model::DamageFinding> residual_findings;

    // S04 / S03 的实测结果，交给引擎写进最终报告。
    std::vector<model::RepairRecoveredSegment> recovered_segments;
    model::RepairTimelineRepair timeline_repair;

    // 输入 → 输出轨道映射与逐轨写入包数（R04 / R07）。
    std::vector<model::RepairTrackVerificationTarget> verification_targets;
    std::vector<model::RepairTrackConservation> track_conservation;
    // 被省略的轨道 / 元数据 / 章节，逐项列出（R11）。
    std::vector<std::string> omitted_items;
    std::string ffmpeg_version;
};

class RepairExecutor {
public:
    // reference_init：fMP4 媒体分片缺少初始化段时，由诊断阶段从参考样片提取的
    // ftyp+moov。非空时执行器会先拼一个"初始化段 + 源分片"的临时文件再重封装，
    // 临时文件在本次执行结束时删除。
    static RepairExecutionOutcome Execute(const model::RepairRequest& request, const model::RepairDiagnosis& diagnosis,
                                          const model::RepairPlan& plan, const RepairOutputPaths& paths,
                                          const RepairInitSegment* reference_init, const std::atomic<bool>* cancel,
                                          const RepairProgress& progress, const RepairRunBudget* budget = nullptr);
};

} // namespace repair
} // namespace videoeye
