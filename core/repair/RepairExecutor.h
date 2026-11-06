#pragma once

// 执行器：M1 只做"保留码流重封装 + 尾部抢救"。
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

    std::optional<double> output_start_seconds;
    std::optional<double> output_end_seconds;

    std::optional<uint64_t> input_video_payload_hash;
    uint64_t input_video_packet_count = 0;
    int output_video_stream_index = -1;

    std::vector<model::RepairActionExecution> executions;
    std::vector<model::DamageFinding> residual_findings;

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
