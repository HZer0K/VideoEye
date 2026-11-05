#pragma once

// 产物验证：V0 容器可读 / V1 抽帧 / V2 全量解码 + 跳转 + 载荷一致性。
//
// 验证只读产物，不改任何文件。每一级都独立记账：即使 V0 通过、V2 因预算没跑完，
// 结果里也要能看出"到底验证到哪一层"。

#include <atomic>
#include <optional>

#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairProgress.h"

namespace videoeye {
namespace repair {

struct RepairVerifyOutcome {
    model::RepairVerification verification;
    model::RepairFidelity fidelity;
    bool canceled = false;
};

class RepairVerifier {
public:
    // expected_video_hash: 执行阶段对输入视频包载荷算出的哈希。
    // 有值时用"产物视频包载荷哈希"与它比对，作为"码流未被改动"的机械证据。
    static RepairVerifyOutcome Verify(const std::string& output_path, model::RepairVerificationLevel level,
                                      std::optional<uint64_t> expected_video_hash, const std::atomic<bool>* cancel,
                                      const RepairProgress& progress);
};

} // namespace repair
} // namespace videoeye
