#pragma once

#include <optional>
#include <vector>

#include "core/domain/model/SceneChangeResult.h"

namespace videoeye {

// SceneChangeResult 已下放到 core/domain/model/SceneChangeResult.h ——
// MediaPlayer 每帧都要产出一个，不该为此 include 整个检测器。别名保留。
using model::SceneChangeResult;

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
    void SetThreshold(double t) {
        threshold_ = t;
    }
    double GetThreshold() const {
        return threshold_;
    }

    // 两次切换点之间的最小间隔 (秒)。同一次转场/闪白常会在相邻两三帧都越过阈值,
    // 不去重就会把一次切换报成两条 —— 下游"场景切换处是否有关键帧"的统计也会翻倍。
    // 0 表示不做抑制。
    void SetMinInterval(double seconds) {
        min_interval_ = seconds > 0.0 ? seconds : 0.0;
    }
    double GetMinInterval() const {
        return min_interval_;
    }

    // 喂入单帧灰度直方图 (任意量纲, 内部会归一化)。
    // 返回检测结果 (仅在检测到切换时); 首帧不会触发。
    std::optional<SceneChangeResult> Feed(int frame_index, double timestamp, const std::vector<float>& gray_hist);

    int detected_count() const {
        return detected_count_;
    }
    double last_score() const {
        return last_score_;
    }

private:
    // 距上一个切换点不足最小间隔时应丢弃本次检测（同一次转场的多帧命中）。
    // 时间戳不可用（NaN）或倒退（新片段）时不抑制：那说明时间线已跳变，
    // 继续比较本来就无意义，抑制只会把真实切换一起吞掉。
    bool TooSoonAfterLastDetection(double timestamp) const;

    std::vector<float> prev_hist_;
    bool has_prev_ = false;
    double threshold_ = 0.45;
    double min_interval_ = 0.1;
    int detected_count_ = 0;
    double last_score_ = 0.0;
    double last_detected_timestamp_ = 0.0;
    bool has_detected_ = false;
};

} // namespace videoeye
