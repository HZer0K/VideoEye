#pragma once

// 镜头切换检测的单条结果。
//
// 下放到 domain 的原因: MediaPlayer 每解码一帧就要产出一个（它是实时帧分析的
// 输出），AnalysisPanel 又要读它做时间轴标记 —— 让这两边都为了一个三字段结构
// include 整个 SceneChangeAnalyzer（以及它依赖的直方图算法头）不划算。

namespace videoeye {
namespace model {

struct SceneChangeResult {
    int frame_index = 0;     // 视频帧序号（从 0 开始）
    double timestamp = 0.0;  // 时间戳（秒）
    double score = 0.0;      // 切换强度 0..1，越大越可能是镜头切换
};

}  // namespace model
}  // namespace videoeye
