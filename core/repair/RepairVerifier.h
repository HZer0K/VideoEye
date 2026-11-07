#pragma once

// 产物验证：V0 容器可读 / V1 抽帧 / V2 全量解码 + 跳转 + 载荷一致性。
//
// 验证只读产物，不改任何文件。每一级都独立记账：即使 V0 通过、V2 因预算没跑完，
// 结果里也要能看出"到底验证到哪一层"。
//
// ⚠️ R04：不再固定挑第一条视频轨。执行器给出的「保留轨 → 输出流」映射决定验证范围，
// 每条要求验证的轨都独立解码、独立记状态；封面图片不算视频内容轨。
// ⚠️ R05：API 返回成功不等于画面干净，帧上的 CORRUPT / decode_error_flags 单独计数。
// ⚠️ R09：V0 不解码，只能得到 Inconclusive（结构有效），不能支撑"已修复可播放"。

#include <atomic>
#include <optional>
#include <vector>

#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairProgress.h"

namespace videoeye {
namespace repair {

struct RepairVerifyOutcome {
    model::RepairVerification verification;
    model::RepairFidelity fidelity;
    bool canceled = false;
    bool timed_out = false;
};

class RepairVerifier {
public:
    // expected_video_hash / expected_audio_hash: 执行阶段对输入首条视频轨 / 首条音频轨
    // 包载荷算出的哈希；V2 全量验证时逐包比对，证明写入产物的载荷未被改码。
    // targets: 执行器建立的输入→输出轨映射；为空时枚举产物内全部音视频轨。
    // expected_duration_seconds: 源侧 sidx/tfdt 估算时长（0 = 无证据），用于逐轨时长对比。
    static RepairVerifyOutcome Verify(const std::string& output_path, model::RepairVerificationLevel level,
                                      std::optional<uint64_t> expected_video_hash, const std::atomic<bool>* cancel,
                                      const RepairProgress& progress,
                                      const std::vector<model::RepairTrackVerificationTarget>& targets = {},
                                      const RepairRunBudget* budget = nullptr, double expected_duration_seconds = 0.0,
                                      std::optional<uint64_t> expected_audio_hash = std::nullopt);
};

} // namespace repair
} // namespace videoeye
