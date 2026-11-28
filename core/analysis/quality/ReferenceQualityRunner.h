#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "core/domain/model/ReferenceQualityResult.h"

namespace videoeye {

// 双视频离线参考质量评估（PSNR / SSIM）的解码执行层。
//
// 与 ReferenceQualityAnalyzer 的分工:
//   * Runner 负责两个文件的打开 / 解码 / 采样 / 取消，产出与纯逻辑层同口径的
//     ReferenceQualityResult（共用 ReferenceQualityAccumulator）
//   * 只支持本地文件 —— 双路网络流的缓冲与对齐是另一件事，不能混在离线评估里
//
// 采样与对齐:
//   * 参考侧按 target_fps 取帧（同一条时间栅格），待测侧取容差窗口内的第一帧
//   * 两侧时间戳各自减去自己首帧时间（容器起始 PTS 常不是 0），再叠加
//     compare.offset_seconds
//   * 容差窗口内找不到待测帧就记 skipped，绝不拿别处画面凑数
struct ReferenceQualityRunOptions {
    model::ReferenceQualityOptions compare;
    // 参考侧采样频率。<= 0 按 1 fps 处理；高于源帧率时退化为逐帧。
    double target_fps = 2.0;
    // 灰度样本宽度（两侧相同）。高度按各自源宽高比推算 —— 宽高比不同会得到
    // 不同尺寸，逐帧报"尺寸不一致"，而不是悄悄拉伸后给出好看的数字。
    int sample_width = 320;
};

struct ReferenceQualityRunProgress {
    double reference_seconds = 0.0; // 当前处理到的参考时间（首帧归一后）
    double distorted_seconds = 0.0; // 当前待测帧时间（首帧归一后）
    int compared_frames = 0;
    int skipped_frames = 0;
};

class ReferenceQualityRunner {
public:
    using ProgressCallback = std::function<void(const ReferenceQualityRunProgress&)>;

    // 同步执行。cancel 非空时在每个读包 / 解码循环检查一次，命中后返回
    // cancelled = true 的部分结果（已完成的帧保留）。
    static model::ReferenceQualityResult Run(const std::string& reference_path, const std::string& distorted_path,
                                             const ReferenceQualityRunOptions& options = ReferenceQualityRunOptions(),
                                             std::atomic<bool>* cancel = nullptr,
                                             const ProgressCallback& progress = ProgressCallback());
};

} // namespace videoeye
