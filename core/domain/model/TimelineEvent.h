#pragma once

#include <string>

namespace videoeye {
namespace model {

/// 统一时间轴事件的来源类别。
/// 用枚举而不是文本：界面侧的分类统计/曲线分组不再依赖 tr() 文本比较，
/// 切换翻译语言时不会因文案变化导致分组错位。
enum class TimelineEventCategory {
    Event = 0,          // 异常事件（来自 AnalysisEvent）
    VideoKeyframe = 1,  // 视频关键帧
    AudioSample = 2,    // 音频采样（每 N 帧抽一条）
};

struct TimelineEvent {
    int index = 0;
    TimelineEventCategory category = TimelineEventCategory::Event;
    double timestamp_seconds = 0.0;
    std::string label;
    std::string detail;
};

} // namespace model
} // namespace videoeye