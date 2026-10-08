#pragma once

// 宏块分析开关的协调状态（从 MainWindow 抽出，便于单测）。
//
// MV 叠加依赖宏块分析（要拿运动矢量），但"用户主动开启宏块分析"与"叠加需要宏块分析"
// 是两件事：实际采集状态取两者的**或**。关闭叠加时只回退到用户原先的选择，不能顺手把
// 用户主动开启的分析也关掉；反之用户主动关掉分析时，叠加失去依赖必须一并失效。
// 与 report_path.h / result_stamp.h 同一思路：能被测试直接调用的逻辑抽成不依赖 Qt 的纯结构。

namespace videoeye {
namespace ui {

struct MacroblockCoordination {
    bool user_enabled = false;     // 用户在宏块页主动开启的分析
    bool overlay_enabled = false;  // MV 叠加是否开启（依赖宏块分析）

    // 实际下发到播放器 / 宏块页的采集状态。
    bool ActualEnabled() const { return user_enabled || overlay_enabled; }

    // 用户在宏块页切换开关：关闭时叠加因失去依赖而一并失效。
    void OnUserToggle(bool enabled) {
        user_enabled = enabled;
        if (!enabled) overlay_enabled = false;
    }

    // MV 叠加开关变化。
    void OnOverlayToggle(bool enabled) { overlay_enabled = enabled; }
};

}  // namespace ui
}  // namespace videoeye
