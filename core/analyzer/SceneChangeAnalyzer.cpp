#include "SceneChangeAnalyzer.h"

#include <cmath>
#include <numeric>
#include <algorithm>

namespace videoeye {
namespace analyzer {

void SceneChangeAnalyzer::Reset() {
    prev_hist_.clear();
    prev_hist_.shrink_to_fit();
    has_prev_ = false;
    detected_count_ = 0;
    last_score_ = 0.0;
}

// 巴氏距离分数: 0 = 两帧直方图完全相同, 1 = 完全不同。
// 使用归一化直方图计算 Bhattacharyya 系数 BC = sum_i sqrt(p_i * q_i), 返回 1 - BC。
static double BhattacharyyaScore(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 1.0;

    double sa = 0.0, sb = 0.0;
    for (float v : a) sa += static_cast<double>(v);
    for (float v : b) sb += static_cast<double>(v);
    if (sa <= 0.0 || sb <= 0.0) return 1.0;  // 退化为"最大变化"

    double bc = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        bc += std::sqrt((a[i] / sa) * (b[i] / sb));
    }
    if (bc > 1.0) bc = 1.0;
    if (bc < 0.0) bc = 0.0;
    return 1.0 - bc;
}

std::optional<SceneChangeResult> SceneChangeAnalyzer::Feed(int frame_index, double timestamp,
                                                          const std::vector<float>& gray_hist) {
    std::optional<SceneChangeResult> result;
    if (has_prev_) {
        const double score = BhattacharyyaScore(prev_hist_, gray_hist);
        last_score_ = score;
        if (score >= threshold_) {
            SceneChangeResult r;
            r.frame_index = frame_index;
            r.timestamp = timestamp;
            r.score = score;
            ++detected_count_;
            result = r;
        }
    }
    prev_hist_ = gray_hist;
    has_prev_ = true;
    return result;
}

} // namespace analyzer
} // namespace videoeye
