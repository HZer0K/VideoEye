#pragma once

#include <vector>
#include <optional>

namespace videoeye {
namespace analyzer {

// 单帧场景切换检测结果
struct SceneChangeResult {
    int frame_index = 0;     // 视频帧序号 (从 0 开始)
    double timestamp = 0.0;  // 时间戳 (秒)
    double score = 0.0;      // 切换强度 0..1, 越大越可能是镜头切换
};

// 基于逐帧灰度直方图差异的镜头切换 (Shot/Scene Change) 检测器。
//
// 思路: 相邻帧的灰度直方图若发生剧烈变化 (如淡入淡出、硬切、转场),
// 其巴氏距离 (Bhattacharyya distance) 会显著增大。以 score = 1 - BC 衡量,
// score >= threshold 即判定为一次切换。
//
// 该方法对内容编辑类切换敏感, 对缓慢的光照/摄像机运动有一定鲁棒性。
class SceneChangeAnalyzer {
public:
    SceneChangeAnalyzer() = default;

    void Reset();

    // 设置切换判定阈值 (0..1)。默认 0.45, 越低越灵敏。
    void SetThreshold(double t) { threshold_ = t; }
    double GetThreshold() const { return threshold_; }

    // 喂入单帧灰度直方图 (任意量纲, 内部会归一化)。
    // 返回检测结果 (仅在检测到切换时); 首帧不会触发。
    std::optional<SceneChangeResult> Feed(int frame_index, double timestamp,
                                          const std::vector<float>& gray_hist);

    int detected_count() const { return detected_count_; }
    double last_score() const { return last_score_; }

private:
    std::vector<float> prev_hist_;
    bool has_prev_ = false;
    double threshold_ = 0.45;
    int detected_count_ = 0;
    double last_score_ = 0.0;
};

} // namespace analyzer
} // namespace videoeye
